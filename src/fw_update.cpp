#include "fw_update.h"
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <time.h>
#include "data_model.h"
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
char    error[160] = "";
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

// The full Mozilla root store built into the core (esp_crt_bundle), so a
// change of CA at GitHub or its download hosts doesn't break the update.
extern const uint8_t crtBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t crtBundleEnd[]   asm("_binary_x509_crt_bundle_end");

void useRootStore(NetworkClientSecure& tls) {
    tls.setCACertBundle(crtBundleStart, crtBundleEnd - crtBundleStart);
}

// Before talking TLS: the clock (certificate dates, NTP) and the name
// lookup. False with the reason in `why`.
bool netReady(char* why, size_t n) {
    IPAddress ip;
    if (!WiFi.hostByName("github.com", ip)) {
        snprintf(why, n, "github.com の名前を引けませんでした（DNS）");
        return false;
    }
    if (time(nullptr) < 1700000000) {
        configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
        for (int i = 0; i < 60 && time(nullptr) < 1700000000; i++) delay(250);
    }
    if (time(nullptr) < 1700000000) {
        snprintf(why, n, "時計を合わせられませんでした（NTP）");
        return false;
    }
    return true;
}

// "（code：TLS の理由）" for a failed request.
void failText(char* out, size_t n, const char* what, int code, NetworkClientSecure& tls) {
    char t[96] = "";
    tls.lastError(t, sizeof t);
    if (t[0]) snprintf(out, n, "%s（%d：%s）", what, code, t);
    else      snprintf(out, n, "%s（%d）", what, code);
    Serial.printf("[Update] %s, free heap %u (largest %u)\n", out, (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// GETs `url`, following redirects one hop at a time with a new TLS client
// for each: the core's client drops its root store when a connection closes
// (stop() clears it), so a redirect followed on the same client fails with
// "No CA chain". Tries the whole chain up to 3 times. On 200 `url` becomes
// the final address and, with `body`, the response is read into it;
// otherwise `why` says what failed.
int getFinal(String& url, String* body, char* why, size_t n) {
    int code = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) delay(1500);
        String u = url;
        for (int hop = 0; hop < 6; hop++) {
            NetworkClientSecure tls;
            useRootStore(tls);
            HTTPClient http;
            http.setReuse(false);
            http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
            http.setTimeout(15000);
            code = http.begin(tls, u) ? http.GET() : -1;
            String next = http.getLocation();
            if (code >= 300 && code < 400 && next.startsWith("https://")) {
                http.end();
                u = next;
                continue;
            }
            if (code == 200) {
                if (body) *body = http.getString();
                http.end();
                url = u;
                return 200;
            }
            failText(why, n, "GitHub に接続できませんでした", code, tls);
            http.end();
            break;
        }
    }
    return code;
}

void checkTask(void*) {
    char e[160];
    String url = String(kReleases) + "/latest/download/" + kVerAsset, v;
    if (!netReady(e, sizeof e)) {
        setState(kFailed, e);
    } else if (getFinal(url, &v, e, sizeof e) != 200) {
        setState(kFailed, e);
    } else {
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
    }
    taskRunning = false;
    vTaskDelete(nullptr);
}

void updateTask(void*) {
    char e[160];
    if (!netReady(e, sizeof e)) {
        setState(kFailed, e);
        taskRunning = false;
        vTaskDelete(nullptr);
        return;
    }
    char tag[24];
    portENTER_CRITICAL(&mux);
    strlcpy(tag, latest, sizeof tag);
    portEXIT_CRITICAL(&mux);
    String base = String(kReleases) + "/download/" + tag + "/";
    httpUpdate.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(false);
    httpUpdate.onProgress([](int cur, int total) {
        portENTER_CRITICAL(&mux);
        pct = total > 0 ? (uint8_t)((int64_t)cur * 100 / total) : 0;
        portEXIT_CRITICAL(&mux);
    });
    // Firmware first (into the other app slot: a failure leaves the running
    // one as it is), then the web UI. Each download address is resolved
    // first (redirects, see getFinal) and then fetched on a fresh client.
    phase = 1, pct = 0;
    Serial.printf("[Update] firmware %s\n", tag);
    String url = base + kFwAsset;
    t_httpUpdate_return r = HTTP_UPDATE_FAILED;
    if (getFinal(url, nullptr, e, sizeof e) == 200) {
        NetworkClientSecure tls;
        useRootStore(tls);
        r = httpUpdate.update(tls, url);
        if (r != HTTP_UPDATE_OK)
            failText(e, sizeof e, ("ファームの書き込みに失敗しました：" + httpUpdate.getLastErrorString()).c_str(),
                     httpUpdate.getLastError(), tls);
    }
    if (r != HTTP_UPDATE_OK) {
        setState(kFailed, e);
        taskRunning = false;
        vTaskDelete(nullptr);
        return;
    }
    phase = 2, pct = 0;
    Serial.printf("[Update] web UI %s\n", tag);
    url = base + kFsAsset;
    r = HTTP_UPDATE_FAILED;
    if (getFinal(url, nullptr, e, sizeof e) == 200) {
        LittleFS.end();
        NetworkClientSecure tls;
        useRootStore(tls);
        r = httpUpdate.updateSpiffs(tls, url);
        if (r != HTTP_UPDATE_OK)
            failText(e, sizeof e, ("Web 画面の書き込みに失敗しました：" + httpUpdate.getLastErrorString()).c_str(),
                     httpUpdate.getLastError(), tls);
    }
    if (r != HTTP_UPDATE_OK) {
        // The new firmware is in; its rescue page can bring the web UI back.
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
    char buf[480];
    portENTER_CRITICAL(&mux);
    State s = state;
    char lt[24], er[160];
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
    bool        done;     // Update.end() ran (or the update was aborted)
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
            else if (u->fs) LittleFS.begin();   // the new web UI, until the restart
        },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index == 0) {
                Upload* n = reinterpret_cast<Upload*>(malloc(sizeof(Upload)));
                if (!n) return;
                *n = Upload{};
                req->_tempObject = n;
                // The browser went away half way: free the update for the next
                // try. Only this request's own unfinished update: the previous
                // file's connection can close while the next file is being
                // written (web UI then firmware), and must not abort it.
                req->onDisconnect([req]() {
                    Upload* u = reinterpret_cast<Upload*>(req->_tempObject);
                    if (u && u->began && !u->done && Update.isRunning()) Update.abort();
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
                u->done = true;
                u->code = 500, u->why = R"({"ok":false,"reason":"write"})";
                return;
            }
            u->got = index + len;
            if (u->got == total) {
                u->done = true;
                if (!Update.end(true)) u->code = 500, u->why = R"({"ok":false,"reason":"verify"})";
            }
        });
}
