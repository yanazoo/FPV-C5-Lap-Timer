#include "fw_update.h"
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <WiFi.h>
#include <time.h>
#include "data_model.h"
#include "github_ca.h"
#include "race.h"
#include "sys_diag.h"
#include "wifi_mgr.h"
#include "ws_handler.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
#ifndef FW_BUILD
#define FW_BUILD "?"
#endif

const char* fwVersion() { return FW_VERSION; }

namespace {

// Release assets (publish-release.yaml): the same names in every release,
// so ".../releases/latest/download/<name>" is always the newest.
const char* const kReleases  = "https://github.com/yanazoo/FPV-C5-Lap-Timer/releases";
const char* const kVerAsset  = "fpv-c5-lap-timer-version.txt";
const char* const kFwAsset   = "fpv-c5-lap-timer-xiao_esp32s3-firmware.bin";
const char* const kFsAsset   = "fpv-c5-lap-timer-xiao_esp32s3-littlefs.bin";

enum State : uint8_t { kIdle, kChecking, kChecked, kUpdating, kDone, kFailed };
const char* const kStateNames[] = {"idle", "checking", "checked", "updating", "done", "failed"};

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
State   state = kIdle;
uint8_t phase = 0;        // updating: 1 firmware, 2 web UI
uint8_t pct = 0;
char    latest[24] = "";  // the newest release's tag, from the check
char    error[80] = "";
bool    taskRunning = false;

void setState(State s, const char* err = nullptr) {
    portENTER_CRITICAL(&mux);
    state = s;
    if (err) {
        strlcpy(error, err, sizeof error);
        for (char* c = error; *c; c++)   // goes into JSON as is
            if (*c == '"' || *c == '\\' || (uint8_t)*c < 0x20) *c = ' ';
    }
    else if (s != kFailed) error[0] = '\0';
    portEXIT_CRITICAL(&mux);
}

// A race on or not cleared: writing flash and restarting would lose it.
bool raceInTheWay() {
    if (raceActive()) return true;
    Guard g;
    return raceCore.running() || raceCore.paused() || raceCore.lapCount() > 0;
}

bool online() {
    NetStatus n = netStatus();
    return n.connected && !n.fallback;
}

// TLS checks certificate dates: set the clock first (NTP) if nobody has.
void syncClock() {
    if (time(nullptr) > 1700000000) return;
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    for (int i = 0; i < 40 && time(nullptr) < 1700000000; i++) delay(250);
}

void checkTask(void*) {
    syncClock();
    NetworkClientSecure tls;
    tls.setCACert(kGithubRootCAs);
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    String url = String(kReleases) + "/latest/download/" + kVerAsset;
    int code = http.begin(tls, url) ? http.GET() : -1;
    if (code == 200) {
        String v = http.getString();
        v.trim();
        if (v.length() > 1 && v.length() < sizeof latest && v[0] == 'v') {
            portENTER_CRITICAL(&mux);
            strlcpy(latest, v.c_str(), sizeof latest);
            portEXIT_CRITICAL(&mux);
            setState(kChecked);
            Serial.printf("[Update] latest release %s (this firmware %s)\n", latest, FW_VERSION);
        } else {
            setState(kFailed, "バージョンを読めませんでした");
        }
    } else {
        char e[80];
        snprintf(e, sizeof e, "GitHub に接続できませんでした（%d）", code);
        setState(kFailed, e);
    }
    http.end();
    taskRunning = false;
    vTaskDelete(nullptr);
}

void updateTask(void*) {
    syncClock();
    char tag[24];
    portENTER_CRITICAL(&mux);
    strlcpy(tag, latest, sizeof tag);
    portEXIT_CRITICAL(&mux);
    String base = String(kReleases) + "/download/" + tag + "/";
    NetworkClientSecure tls;
    tls.setCACert(kGithubRootCAs);
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(false);
    httpUpdate.onProgress([](int cur, int total) {
        portENTER_CRITICAL(&mux);
        pct = total > 0 ? (uint8_t)((int64_t)cur * 100 / total) : 0;
        portEXIT_CRITICAL(&mux);
    });
    // Firmware first (into the other app slot: a failure leaves the running
    // one as it is), then the web UI.
    phase = 1, pct = 0;
    Serial.printf("[Update] firmware %s\n", tag);
    t_httpUpdate_return r = httpUpdate.update(tls, base + kFwAsset);
    if (r != HTTP_UPDATE_OK) {
        char e[80];
        snprintf(e, sizeof e, "ファームの書き込みに失敗しました：%s", httpUpdate.getLastErrorString().c_str());
        setState(kFailed, e);
        taskRunning = false;
        vTaskDelete(nullptr);
        return;
    }
    phase = 2, pct = 0;
    Serial.printf("[Update] web UI %s\n", tag);
    LittleFS.end();
    r = httpUpdate.updateSpiffs(tls, base + kFsAsset);
    if (r != HTTP_UPDATE_OK) {
        // The new firmware is in; its rescue page can bring the web UI back.
        char e[80];
        snprintf(e, sizeof e, "Web 画面の書き込みに失敗しました：%s", httpUpdate.getLastErrorString().c_str());
        setState(kFailed, e);
        Serial.println("[Update] web UI failed: restarting into the new firmware (rescue page)");
        sysRequestRestart(3000);
        taskRunning = false;
        vTaskDelete(nullptr);
        return;
    }
    setState(kDone);
    Serial.println("[Update] done, restarting");
    sysRequestRestart(2000);
    taskRunning = false;
    vTaskDelete(nullptr);
}

bool startTask(TaskFunction_t fn, const char* name) {
    if (taskRunning) return false;
    taskRunning = true;
    if (xTaskCreatePinnedToCore(fn, name, 12288, nullptr, 1, nullptr, 0) != pdPASS) {
        taskRunning = false;
        return false;
    }
    return true;
}

String statusJson() {
    char buf[320];
    portENTER_CRITICAL(&mux);
    State s = state;
    char lt[24], er[80];
    strlcpy(lt, latest, sizeof lt);
    strlcpy(er, error, sizeof er);
    uint8_t ph = phase, pc = pct;
    portEXIT_CRITICAL(&mux);
    snprintf(buf, sizeof buf,
             R"({"version":"%s","build":"%s","latest":"%s","state":"%s","phase":%u,"pct":%u,"error":"%s","online":%s,"race":%s})",
             FW_VERSION, FW_BUILD, lt, kStateNames[s], ph, pc, er, online() ? "true" : "false",
             raceInTheWay() ? "true" : "false");
    return String(buf);
}

// Refusals shared by every write.
const char* refusal(AsyncWebServerRequest* req) {
    String token = req->hasParam("token") ? req->getParam("token")->value() : "";
    if (!wsTokenValid(token)) return R"({"ok":false,"reason":"login"})";
    if (raceInTheWay()) return R"({"ok":false,"reason":"race"})";
    if (taskRunning || Update.isRunning()) return R"({"ok":false,"reason":"busy"})";
    return nullptr;
}

// A firmware or web UI file in the request body (raw, not a form).
struct Upload {
    bool        began;
    bool        fs;
    size_t      got;
    int         code;
    const char* why;
};

const char kRescuePage[] PROGMEM = R"HTML(<!doctype html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>FPV C5 Lap Timer - 復旧</title>
<style>body{font-family:sans-serif;background:#111;color:#ddd;max-width:560px;margin:24px auto;padding:0 16px}
h1{font-size:20px}button{padding:8px 14px;margin:6px 0}input{margin:6px 0}#st{margin-top:12px;color:#fc6}</style></head>
<body><h1>FPV C5 Lap Timer：Web 画面がありません</h1>
<p>Web 画面のデータ（littlefs.bin）が見つかりません。更新が途中で止まった可能性があります。どちらかで書き込んでください。</p>
<p>管理者パスワード：<input id="pw" type="password" value=""></p>
<h3>ネットから（クライアントモードでインターネットにつながっているとき）</h3>
<button onclick="online()">最新のリリースを書き込む</button>
<h3>ファイルから</h3>
<p>リリースの zip の中の <b>littlefs.bin</b>：<input id="fs" type="file" accept=".bin"></p>
<button onclick="upload()">書き込む</button>
<div id="st"></div>
<script>
const st=t=>document.getElementById('st').textContent=t;
async function login(){const r=await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:document.getElementById('pw').value})});
 const j=await r.json();if(!j.token)throw 'ログインできません';return j.token;}
async function online(){try{const t=await login();st('最新のリリースを確認しています…');
 let r=await fetch('/api/update/check?token='+t,{method:'POST'});
 for(;;){await new Promise(x=>setTimeout(x,1000));const s=await (await fetch('/api/update')).json();
  if(s.state==='checked'){r=await fetch('/api/update/online?token='+t,{method:'POST'});break;}
  if(s.state==='failed')throw s.error;}
 for(;;){await new Promise(x=>setTimeout(x,1000));let s;try{s=await (await fetch('/api/update')).json();}catch(e){st('再起動しています…');setTimeout(()=>location.reload(),8000);return;}
  if(s.state==='failed')throw s.error;st((s.phase===1?'ファーム':'Web 画面')+' '+s.pct+'%');}
}catch(e){st('失敗：'+e);}}
async function upload(){try{const f=document.getElementById('fs').files[0];if(!f)throw 'ファイルを選んでください';const t=await login();st('書き込んでいます…');
 const r=await fetch('/api/update/upload?part=fs&restart=1&token='+t,{method:'POST',body:f});const j=await r.json();
 if(!j.ok)throw j.reason;st('書き込みました。再起動しています…');setTimeout(()=>location.reload(),8000);}catch(e){st('失敗：'+e);}}
</script></body></html>)HTML";

}  // namespace

bool fwWebUiMissing() { return !LittleFS.exists("/index.html.gz") && !LittleFS.exists("/index.html"); }

void fwSendRescuePage(AsyncWebServerRequest* req) { req->send(200, "text/html; charset=utf-8", kRescuePage); }

void fwUpdateRoutes() {
    server.on(AsyncURIMatcher::exact("/api/update"), HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", statusJson());
    });
    // Read the newest release's version (takes a few seconds: poll /api/update).
    server.on(AsyncURIMatcher::exact("/api/update/check"), HTTP_POST, [](AsyncWebServerRequest* req) {
        String token = req->hasParam("token") ? req->getParam("token")->value() : "";
        if (!wsTokenValid(token)) { req->send(403, "application/json", R"({"ok":false,"reason":"login"})"); return; }
        if (!online()) { req->send(409, "application/json", R"({"ok":false,"reason":"offline"})"); return; }
        if (taskRunning) { req->send(409, "application/json", R"({"ok":false,"reason":"busy"})"); return; }
        setState(kChecking);
        if (!startTask(checkTask, "fwcheck")) { setState(kFailed, "開始できませんでした"); req->send(409, "application/json", R"({"ok":false,"reason":"busy"})"); return; }
        req->send(200, "application/json", R"({"ok":true})");
    });
    // Download and write the release the check found, then restart.
    server.on(AsyncURIMatcher::exact("/api/update/online"), HTTP_POST, [](AsyncWebServerRequest* req) {
        if (const char* why = refusal(req)) { req->send(409, "application/json", why); return; }
        if (!online()) { req->send(409, "application/json", R"({"ok":false,"reason":"offline"})"); return; }
        portENTER_CRITICAL(&mux);
        bool have = latest[0];
        portEXIT_CRITICAL(&mux);
        if (!have) { req->send(409, "application/json", R"({"ok":false,"reason":"nocheck"})"); return; }
        setState(kUpdating);
        if (!startTask(updateTask, "fwupdate")) { setState(kFailed, "開始できませんでした"); req->send(409, "application/json", R"({"ok":false,"reason":"busy"})"); return; }
        req->send(200, "application/json", R"({"ok":true})");
    });
    // A file from the release zip: ?part=fw (firmware.bin) or fs
    // (littlefs.bin), &restart=1 to restart when written.
    server.on(AsyncURIMatcher::exact("/api/update/upload"), HTTP_POST,
        [](AsyncWebServerRequest* req) {
            Upload* u = reinterpret_cast<Upload*>(req->_tempObject);
            if (!u) { req->send(400, "application/json", R"({"ok":false,"reason":"empty"})"); return; }
            if (u->why) { req->send(u->code, "application/json", u->why); return; }
            bool restart = req->hasParam("restart") && req->getParam("restart")->value() == "1";
            Serial.printf("[Update] %s written from the browser (%u bytes)\n", u->fs ? "web UI" : "firmware", (unsigned)u->got);
            req->send(200, "application/json", R"({"ok":true})");
            if (restart) sysRequestRestart(1500);
        },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index == 0) {
                Upload* n = reinterpret_cast<Upload*>(malloc(sizeof(Upload)));
                if (!n) return;
                *n = Upload{};
                req->_tempObject = n;
                // The browser went away half way: free the update for the next try.
                req->onDisconnect([]() {
                    if (Update.isRunning()) Update.abort();
                });
                String part = req->hasParam("part") ? req->getParam("part")->value() : "";
                n->fs = part == "fs";
                if (const char* why = refusal(req)) n->code = 409, n->why = why;
                else if (part != "fw" && part != "fs") n->code = 400, n->why = R"({"ok":false,"reason":"part"})";
                else if (!total) n->code = 400, n->why = R"({"ok":false,"reason":"empty"})";
                else if (!n->fs && data[0] != 0xE9) n->code = 400, n->why = R"({"ok":false,"reason":"notfirmware"})";
                else {
                    if (n->fs) LittleFS.end();
                    if (!Update.begin(total, n->fs ? U_SPIFFS : U_FLASH)) n->code = 400, n->why = R"({"ok":false,"reason":"size"})";
                    else n->began = true;
                }
            }
            Upload* u = reinterpret_cast<Upload*>(req->_tempObject);
            if (!u || u->why || index != u->got) return;
            if (Update.write(data, len) != len) {
                Update.abort();
                u->code = 500, u->why = R"({"ok":false,"reason":"write"})";
                return;
            }
            u->got = index + len;
            if (u->got == total && !Update.end(true)) u->code = 500, u->why = R"({"ok":false,"reason":"verify"})";
        });
}
