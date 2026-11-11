#include <ArduinoJson.h>
#include <LittleFS.h>
#include <algorithm>
#include <functional>
#include <time.h>
#include "http_routes.h"
#include "c5_link.h"
#include "data_model.h"
#include "elrs.h"
#include "mic.h"
#include "nvs_store.h"
#include "race.h"
#include "sd_store.h"
#include "sys_diag.h"
#include "wifi_mgr.h"
#include "ws_handler.h"

namespace {

using JsonHandler = std::function<void(AsyncWebServerRequest*, JsonDocument&)>;

// POST with a JSON body. The body can arrive in several chunks.
void onJson(const char* path, JsonHandler h) {
    server.on(AsyncURIMatcher::exact(path), HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
        [h](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (total > 4096) { if (index == 0) req->send(413); return; }
            // malloc, not new: the server free()s _tempObject if the request is aborted.
            if (index == 0) req->_tempObject = malloc(total + 1);
            char* body = reinterpret_cast<char*>(req->_tempObject);
            if (!body) return;
            memcpy(body + index, data, len);
            if (index + len < total) return;
            body[total] = '\0';
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, (const char*)body);
            free(body);
            req->_tempObject = nullptr;
            if (err) { req->send(400, "application/json", R"({"error":"bad json"})"); return; }
            h(req, doc);
        });
}

void sendJson(AsyncWebServerRequest* req, JsonDocument& d) {
    String s;
    serializeJson(d, s);
    req->send(200, "application/json", s);
}

void ok(AsyncWebServerRequest* req) { req->send(200, "application/json", R"({"ok":true})"); }
void bad(AsyncWebServerRequest* req, const char* why) {
    char b[80];
    snprintf(b, sizeof b, R"({"ok":false,"error":"%s"})", why);
    req->send(400, "application/json", b);
}

void cpRedirect(AsyncWebServerRequest* req) {
    AsyncWebServerResponse* res = req->beginResponse(302);
    res->addHeader("Location", "http://" + netIp().toString() + "/");
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

String rosterJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    Guard g;
    for (int i = 0; i < rosterCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]         = i;
        o["name"]       = roster[i].name;
        o["yomi"]       = roster[i].yomi;
        o["freq"]       = roster[i].freqMhz;
        o["bind"]       = roster[i].bind;
        o["osd"]        = roster[i].osd;
        o["activeSlot"] = activeSlotOf(i);
        o["enter"]      = rosterCal[i].enterRssi;
        o["exit"]       = rosterCal[i].exitRssi;
    }
    String s;
    serializeJson(doc, s);
    return s;
}

void posToJson(JsonObject o, const char* key, osd::Pos p) {
    char b[12];
    snprintf(b, sizeof b, "%d,%d", p.row, p.col);
    o[key] = b;
}

void posFromJson(JsonObjectConst o, const char* key, osd::Pos& p) {
    const char* v = o[key] | (const char*)nullptr;
    int row, col;
    if (v && sscanf(v, "%d,%d", &row, &col) == 2 && row >= 0 && row < osd::kRows && col < osd::kCols)
        p = {(int8_t)row, (int8_t)(col < 0 ? -1 : col)};
}

void copyStr(JsonVariantConst d, const char* key, char* dst, size_t cap) {
    if (d[key].is<const char*>()) strlcpy(dst, d[key].as<const char*>(), cap);
}

// ── Settings <-> JSON: the API and the settings backup on the SD card ─────
// Same keys as GET/POST, so a backup restores through the same checks.

// Race content and detection (caller holds gLock).
void settingsToJson(JsonObject doc) {
    doc["lapMode"]    = settings.lapMode;
    doc["cooldownMs"] = settings.cooldownMs;
    doc["sdLogMode"]  = settings.sdLogMode;
    doc["totalLaps"]  = settings.totalLaps;
    doc["gain"]       = settings.gain;
    doc["rankMode"]     = raceRules.rankMode;
    doc["timeLimitSec"] = raceRules.timeLimitSec;
    doc["countLastLap"] = raceRules.countLastLap;
    doc["startDelayOn"]  = startDelayOn;
    doc["startDelaySec"] = startDelaySec;
}
// Takes gLock and saves; the caller applies (applyGateConfig) and tells
// the browsers (wsSettings).
void settingsFromJson(JsonVariantConst doc) {
    Guard g;
    // Changing detection settings mid-race would change how laps are
    // counted; the UI only sends them while idle, the gain and total laps
    // excepted.
    if (!doc["lapMode"].isNull()) settings.lapMode = (int)doc["lapMode"] == 1 ? 1 : 0;
    if (!doc["sdLogMode"].isNull()) settings.sdLogMode = (uint8_t)constrain((int)doc["sdLogMode"], 0, 2);
    if (!doc["cooldownMs"].isNull()) settings.cooldownMs = constrain((int)doc["cooldownMs"], 500, 30000);
    if (!doc["totalLaps"].isNull()) settings.totalLaps = (uint16_t)constrain((int)doc["totalLaps"], 0, 99);
    if (!doc["gain"].isNull()) settings.gain = (uint8_t)constrain((int)doc["gain"], 0, 89);
    if (!doc["rankMode"].isNull()) raceRules.rankMode = (uint8_t)constrain((int)doc["rankMode"], 0, 2);
    if (!doc["timeLimitSec"].isNull())
        raceRules.timeLimitSec = (uint16_t)constrain((int)doc["timeLimitSec"], 30, 1800);
    if (!doc["countLastLap"].isNull()) raceRules.countLastLap = doc["countLastLap"] | true;
    if (!doc["startDelayOn"].isNull()) startDelayOn = doc["startDelayOn"] | false;
    if (!doc["startDelaySec"].isNull()) {
        int sd = doc["startDelaySec"] | 0;
        if (sd > 0) startDelaySec = (uint16_t)min(sd, (int)START_DELAY_MAX);
        else if (doc["startDelayOn"].isNull()) startDelayOn = false;   // an old backup's "none"
    }
    saveSettings();
}

// ELRS OSD (caller holds gLock).
void elrsToJson(JsonObject d) {
    const osd::Options& o = elrsCfg.opt;
    d["enabled"]       = elrsCfg.enabled;
    d["transport"]     = elrsCfg.transport;
    d["host"]          = elrsCfg.host;
    d["port"]          = elrsCfg.port;
    d["rdBind"]        = elrsCfg.rdBind;
    d["rdSwitch"]      = elrsRdSwitch;
    d["eventName"]     = elrsCfg.eventName;
    d["showRaceClock"] = o.showRaceClock;
    d["showBestLap"]   = o.showBestLap;
    d["positionMode"]  = o.positionMode;
    d["showTotalTime"] = o.showTotalTime;
    d["resultsMode"]   = o.resultsMode;
    d["stageMsg"]      = o.stageMsg;
    d["startMsg"]      = o.startMsg;
    d["pilotDoneMsg"]  = o.pilotDoneMsg;
    d["raceStopMsg"]   = o.raceStopMsg;
    d["startUptime"]   = o.startUptimeDs;
    d["finishUptime"]  = o.finishUptimeDs;
    d["resultsUptime"] = o.resultsUptimeDs;
    d["announceUptime"] = o.announceUptimeDs;
    JsonObject p = d["pos"].to<JsonObject>();
    posToJson(p, "eventName", o.eventName);
    posToJson(p, "announcement", o.announcement);
    posToJson(p, "status", o.status);
    posToJson(p, "currentLap", o.currentLap);
    posToJson(p, "lapResults", o.lapResults);
    posToJson(p, "bestLap", o.bestLap);
    posToJson(p, "totalTime", o.totalTime);
    posToJson(p, "results", o.results);
    posToJson(p, "raceClock", o.raceClock);
}
// Takes gLock, saves, and brings the OSD link in line with the change.
void elrsFromJson(JsonVariantConst d) {
    bool reconnect = false;
    {
        Guard g;
        ElrsSettings& e = elrsCfg;
        osd::Options& o = e.opt;
        uint8_t oldTransport = e.transport;
        uint16_t oldPort = e.port;
        String oldHost = e.host;
        bool oldEnabled = e.enabled;
        if (!d["enabled"].isNull()) e.enabled = d["enabled"] | false;
        if (!d["transport"].isNull()) e.transport = (int)d["transport"] == 1 ? 1 : 0;
        copyStr(d, "host", e.host, sizeof e.host);
        if (!e.host[0]) strlcpy(e.host, "auto", sizeof e.host);
        if (!d["port"].isNull()) e.port = (uint16_t)constrain((int)d["port"], 1, 65535);
        copyStr(d, "rdBind", e.rdBind, sizeof e.rdBind);
        if (!d["rdSwitch"].isNull()) elrsRdSwitch = (int)d["rdSwitch"] == 1 ? 1 : 0;
        copyStr(d, "eventName", e.eventName, sizeof e.eventName);
        if (!d["showRaceClock"].isNull()) o.showRaceClock = d["showRaceClock"] | true;
        if (!d["showBestLap"].isNull())   o.showBestLap   = d["showBestLap"] | true;
        if (!d["positionMode"].isNull())  o.positionMode  = d["positionMode"] | true;
        if (!d["showTotalTime"].isNull()) o.showTotalTime = d["showTotalTime"] | false;
        if (!d["resultsMode"].isNull())   o.resultsMode   = d["resultsMode"] | true;
        copyStr(d, "stageMsg", o.stageMsg, sizeof o.stageMsg);
        copyStr(d, "startMsg", o.startMsg, sizeof o.startMsg);
        copyStr(d, "pilotDoneMsg", o.pilotDoneMsg, sizeof o.pilotDoneMsg);
        copyStr(d, "raceStopMsg", o.raceStopMsg, sizeof o.raceStopMsg);
        if (!d["startUptime"].isNull())    o.startUptimeDs    = (uint16_t)constrain((int)d["startUptime"], 1, 600);
        if (!d["finishUptime"].isNull())   o.finishUptimeDs   = (uint16_t)constrain((int)d["finishUptime"], 1, 600);
        if (!d["resultsUptime"].isNull())  o.resultsUptimeDs  = (uint16_t)constrain((int)d["resultsUptime"], 1, 600);
        if (!d["announceUptime"].isNull()) o.announceUptimeDs = (uint16_t)constrain((int)d["announceUptime"], 1, 600);
        JsonObjectConst p = d["pos"].as<JsonObjectConst>();
        if (p) {
            posFromJson(p, "eventName", o.eventName);
            posFromJson(p, "announcement", o.announcement);
            posFromJson(p, "status", o.status);
            posFromJson(p, "currentLap", o.currentLap);
            posFromJson(p, "lapResults", o.lapResults);
            posFromJson(p, "bestLap", o.bestLap);
            posFromJson(p, "totalTime", o.totalTime);
            posFromJson(p, "results", o.results);
            posFromJson(p, "raceClock", o.raceClock);
        }
        reconnect = e.transport != oldTransport || e.port != oldPort || oldHost != e.host || e.enabled != oldEnabled;
        saveElrs();
    }
    if (reconnect) elrsReconnect();
    else elrsConfigChanged();
}

// Microphone start / false start (mic.h); saves.
void micToJson(JsonObject d) {
    d["start"]      = micCfg.start != 0;
    d["falseStart"] = micCfg.falseStart != 0;
    d["startClap"]  = micCfg.startClap != 0;
    d["clapClear"]  = micCfg.clapClear != 0;
    d["startLvl"]   = micCfg.startLvl;
    d["fsLvl"]      = micCfg.fsLvl;
    d["startHoldMs"] = micCfg.startHoldMs;
    d["fsHoldMs"]    = micCfg.fsHoldMs;
}
void micFromJson(JsonVariantConst d) {
    if (!d["start"].isNull()) micCfg.start = (d["start"] | false) ? 1 : 0;
    if (!d["falseStart"].isNull()) micCfg.falseStart = (d["falseStart"] | false) ? 1 : 0;
    if (!d["startClap"].isNull()) micCfg.startClap = (d["startClap"] | false) ? 1 : 0;
    if (!d["clapClear"].isNull()) micCfg.clapClear = (d["clapClear"] | false) ? 1 : 0;
    if (!d["startLvl"].isNull()) micCfg.startLvl = (uint8_t)constrain((int)d["startLvl"], 0, 100);
    if (!d["fsLvl"].isNull()) micCfg.fsLvl = (uint8_t)constrain((int)d["fsLvl"], 0, 100);
    if (!d["startHoldMs"].isNull() && micHoldValid(d["startHoldMs"] | 0)) micCfg.startHoldMs = d["startHoldMs"] | 0;
    if (!d["fsHoldMs"].isNull() && micHoldValid(d["fsHoldMs"] | 0)) micCfg.fsHoldMs = d["fsHoldMs"] | 0;
    saveMic();
}

// Connection limits and the login hold time; saves.
void connFromJson(JsonVariantConst d) {
    if (!d["adminMax"].isNull()) connLimits.adminMax = constrain((int)d["adminMax"], 1, (int)CONN_ADMIN_MAX);
    if (!d["viewHost"].isNull()) connLimits.viewHost = constrain((int)d["viewHost"], 1, (int)CONN_VIEW_HOST_MAX);
    if (!d["viewClient"].isNull())
        connLimits.viewClient = constrain((int)d["viewClient"], 1, (int)CONN_VIEW_CLIENT_MAX);
    if (!d["holdSec"].isNull() && loginHoldValid(d["holdSec"] | 0)) loginHoldSec = d["holdSec"] | 0;
    // Host mode: admins + viewers within our access point (one for the netpack).
    connLimits.viewHost = min(connLimits.viewHost, viewHostLimitFor(connLimits.adminMax));
    saveConnLimits();
}

// WiFi host / client and the client address; saves (applied after a
// reboot). Returns why it was refused, or nullptr.
const char* netFromJson(JsonVariantConst d) {
    uint8_t mode = (int)(d["mode"] | 0) == NET_STA ? NET_STA : NET_AP;
    String apS = d["apSsid"] | "", apP = d["apPass"] | "";
    String staS = d["staSsid"] | "", staP = d["staPass"] | "";
    if (apS.length() < 1 || apS.length() > 31) return "AP SSID 1..31";
    if (apP.length() < 8 || apP.length() > 63) return "AP password 8..63";
    if (mode == NET_STA && (staS.length() < 1 || staS.length() > 32)) return "client SSID 1..32";
    int fbSec = d["fallbackSec"] | (int)netFallbackSec;
    if (!netFallbackValid(fbSec)) return "fallbackSec 15/30/60";
    // Empty client password keeps the stored one (the UI never shows it);
    // "staPassClear" switches to an open network.
    if (!staP.length() && !(d["staPassClear"] | false)) staP = staPass;
    if (staP.length() && (staP.length() < 8 || staP.length() > 63)) return "client password 8..63";
    // Client mode address: DHCP, or fixed (address, mask, gateway; DNS
    // defaults to the gateway).
    StaIpConfig ipc = staIp;
    if (!d["ipFixed"].isNull()) {
        ipc.fixed = (d["ipFixed"] | false) ? 1 : 0;
        if (ipc.fixed) {
            IPAddress ip, mask, gw, dns;
            if (!ip.fromString(d["ip"] | "") || (uint32_t)ip == 0) return "IP address";
            if (!mask.fromString(d["mask"] | "") || (uint32_t)mask == 0) return "subnet mask";
            if (!gw.fromString(d["gw"] | "")) return "gateway";
            const char* dnsS = d["dns"] | "";
            if (*dnsS && !dns.fromString(dnsS)) return "DNS";
            ipc.ip = (uint32_t)ip, ipc.mask = (uint32_t)mask, ipc.gw = (uint32_t)gw;
            ipc.dns = *dnsS ? (uint32_t)dns : 0;
        }
    }
    saveNet(mode, apS, apP, staS, staP, (uint8_t)fbSec);
    staIp = ipc;
    saveStaIp();
    return nullptr;
}

void ipToJson(JsonObject d) {
    d["ipFixed"] = staIp.fixed != 0;
    d["ip"]      = staIp.ip ? IPAddress(staIp.ip).toString() : String();
    d["mask"]    = staIp.mask ? IPAddress(staIp.mask).toString() : String();
    d["gw"]      = staIp.gw ? IPAddress(staIp.gw).toString() : String();
    d["dns"]     = staIp.dns ? IPAddress(staIp.dns).toString() : String();
}

// The settings backup (everything but the pilots), with the WiFi and admin
// passwords: only a logged-in admin may read it back.

String settingsBackupJson() {
    JsonDocument doc;
    doc["kind"] = "fpv-c5-lt-settings";
    doc["ver"]  = 1;
    time_t t = time(nullptr);
    if (t > 1600000000) doc["saved"] = (uint32_t)t;
    {
        Guard g;
        settingsToJson(doc["settings"].to<JsonObject>());
        elrsToJson(doc["elrs"].to<JsonObject>());
    }
    micToJson(doc["mic"].to<JsonObject>());
    JsonObject c = doc["conn"].to<JsonObject>();
    c["adminMax"]   = connLimits.adminMax;
    c["viewHost"]   = connLimits.viewHost;
    c["viewClient"] = connLimits.viewClient;
    c["holdSec"]    = loginHoldSec;
    JsonObject n = doc["net"].to<JsonObject>();
    n["mode"]        = netMode;
    n["apSsid"]      = apSsid;
    n["apPass"]      = apPass;
    n["staSsid"]     = staSsid;
    n["staPass"]     = staPass;
    n["staPassClear"] = staPass.length() == 0;
    n["fallbackSec"] = netFallbackSec;
    ipToJson(n);
    doc["auth"]["password"] = adminPassword;
    String s;
    serializeJsonPretty(doc, s);
    return s;
}

// Files on the card that hold passwords: the settings backup and the
// connection QR images (the WiFi QR carries the password).
// By the name alone, in lower case (FAT ignores case and "//" or "/./" reach
// the same file).
bool sdSecretFile(const String& path) {
    String n = path.substring(path.lastIndexOf('/') + 1);
    n.toLowerCase();
    return (n.startsWith("settings") && n.endsWith(".json")) || (n.startsWith("qr_") && n.endsWith(".png"));
}

// A connection QR image the browser drew: /qr_<a-z0-9_->.png only.
bool qrImagePath(const String& path) {
    if (!path.startsWith("/qr_") || !path.endsWith(".png") || path.length() > 40) return false;
    for (size_t i = 4; i < path.length() - 4; i++) {
        char ch = path[i];
        if (!(isalnum((unsigned char)ch) || ch == '_' || ch == '-')) return false;
    }
    return true;
}
const size_t kQrImageMax = 300 * 1024;
const char* const kQrUploadTmp = "/qr_upload_tmp.png";   // until the last piece is in
struct QrUpload {
    size_t total, got;
    bool started;
    int code;
    const char* why;   // refused: the reply
};

const char* mimeOf(const String& path) {
    if (path.endsWith(".csv")) return "text/csv; charset=utf-8";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

bool raceOn() {
    Guard g;
    return raceCore.running() || raceCore.paused();
}

}  // namespace

void registerHttpRoutes() {
    // ── Opened as http://fpv-c5-lt.local: go to the IP address ─────────────
    // Chrome's Local Network Access treats a page on a .local name over plain
    // http as public and blocks its requests to .local (scripts, API,
    // WebSocket): the page loads but nothing runs. Opened by IP address the
    // page and its requests are both on the local network. So any request
    // for a .local host is sent on to the same path on our IP address.
    server.addMiddleware([](AsyncWebServerRequest* req, ArMiddlewareNext next) {
        String host = req->host();
        int colon = host.indexOf(':');
        if (colon >= 0) host.remove(colon);
        host.toLowerCase();
        if (host.endsWith(".local")) {
            String to = "http://" + netIp().toString() + req->url();
            char sep = '?';
            for (size_t i = 0; i < req->params(); i++) {
                const AsyncWebParameter* p = req->getParam(i);
                if (p->isPost() || p->isFile()) continue;
                to += sep + p->name() + "=" + p->value();
                sep = '&';
            }
            AsyncWebServerResponse* res = req->beginResponse(302);
            res->addHeader("Location", to);
            res->addHeader("Cache-Control", "no-store");
            req->send(res);
            return;
        }
        next();
    });

    // ── Captive portal probes (first, so they take priority) ───────────────
    for (const char* p : {"/generate_204", "/gen_204", "/connecttest.txt", "/hotspot-detect.html",
                          "/library/test/success.html", "/success.txt", "/canonical.html", "/ncsi.txt",
                          "/redirect", "/fwlink", "/wpad.dat", "/bag", "/kindle-wifi/wifistub.html", "/chat"})
        server.on(p, HTTP_ANY, [](AsyncWebServerRequest* r) { cpRedirect(r); });
    for (const char* p : {"/favicon.ico", "/apple-touch-icon.png", "/robots.txt"})
        server.on(p, HTTP_ANY, [](AsyncWebServerRequest* r) { r->send(204); });

    // ── Roster ─────────────────────────────────────────────────────────────
    server.on(AsyncURIMatcher::exact("/api/pilots"), HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", rosterJson());
    });

    onJson("/api/pilots/delete", [](AsyncWebServerRequest* req, JsonDocument& doc) {
        int id = doc["id"] | -1;
        {
            Guard g;
            if (!rosterValid(id)) { bad(req, "bad id"); return; }
            for (int s = 0; s < MAX_ACTIVE; s++) {
                if (activePilots[s] == id) activePilots[s] = -1;
                else if (activePilots[s] > id) activePilots[s]--;
            }
            for (int i = id; i < rosterCount - 1; i++) roster[i] = roster[i + 1], rosterCal[i] = rosterCal[i + 1];
            rosterCount--;
            saveRosterAll();
            saveActive();
        }
        applyGateConfig();
        wsActiveUpdate();
        ok(req);
    });

    onJson("/api/pilots", [](AsyncWebServerRequest* req, JsonDocument& doc) {
        int id = doc["id"] | -1;
        int freq = doc["freq"] | 0;
        if (freq && (freq < 5180 || freq > 5917)) { bad(req, "freq 5180-5917"); return; }
        {
            Guard g;
            if (id < -1 || id >= rosterCount) { bad(req, "bad id"); return; }
            if (id == -1) {
                if (rosterCount >= MAX_REGISTERED) { bad(req, "roster full"); return; }
                id = rosterCount++;
                memset(&roster[id], 0, sizeof roster[id]);
                rosterCal[id] = {DEFAULT_ENTER, DEFAULT_EXIT};
                roster[id].osd = true;
            }
            PilotRoster& p = roster[id];
            strlcpy(p.name, doc["name"] | "", sizeof p.name);
            strlcpy(p.yomi, doc["yomi"] | "", sizeof p.yomi);
            p.freqMhz = (uint16_t)freq;
            if (doc["bind"].is<const char*>()) strlcpy(p.bind, doc["bind"].as<const char*>(), sizeof p.bind);
            if (!doc["osd"].isNull()) p.osd = doc["osd"] | true;
            if (!doc["enter"].isNull()) rosterCal[id].enterRssi = constrain((int)doc["enter"], 0, 1023);
            if (!doc["exit"].isNull())  rosterCal[id].exitRssi  = constrain((int)doc["exit"], 0, 1023);
            saveRosterPilot(id);
        }
        applyGateConfig();
        wsActiveUpdate();
        JsonDocument resp;
        resp["ok"] = true;
        resp["id"] = id;
        sendJson(req, resp);
    });

    // ── Slots ──────────────────────────────────────────────────────────────
    server.on(AsyncURIMatcher::exact("/api/active"), HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        Guard g;
        for (int s = 0; s < MAX_ACTIVE; s++) {
            JsonObject o = arr.add<JsonObject>();
            int ri = activePilots[s];
            o["slot"] = s;
            o["rosterIdx"] = ri;
            o["name"] = activeName(s);
            o["yomi"] = rosterValid(ri) ? roster[ri].yomi : "";
            o["freq"] = rosterValid(ri) ? roster[ri].freqMhz : 0;
            if (rosterValid(ri)) o["enter"] = rosterCal[ri].enterRssi, o["exit"] = rosterCal[ri].exitRssi;
        }
        sendJson(req, doc);
    });

    onJson("/api/active", [](AsyncWebServerRequest* req, JsonDocument& doc) {
        JsonArray slots = doc["slots"].as<JsonArray>();
        if (!slots) { bad(req, "no slots"); return; }
        {
            Guard g;
            for (int s = 0; s < MAX_ACTIVE && s < (int)slots.size(); s++) {
                int ri = slots[s].as<int>();
                activePilots[s] = rosterValid(ri) ? ri : -1;
            }
            saveActive();
        }
        applyGateConfig();
        wsActiveUpdate();
        ok(req);
    });

    onJson("/api/calib", [](AsyncWebServerRequest* req, JsonDocument& doc) {
        int id = doc["id"] | -1;
        int en, ex;
        {
            Guard g;
            if (!rosterValid(id)) { bad(req, "bad id"); return; }
            rosterCal[id].enterRssi = constrain((int)(doc["enter"] | rosterCal[id].enterRssi), 0, 1023);
            rosterCal[id].exitRssi  = constrain((int)(doc["exit"] | rosterCal[id].exitRssi), 0, 1023);
            en = rosterCal[id].enterRssi, ex = rosterCal[id].exitRssi;
            saveRosterPilot(id);
        }
        applyGateConfig();
        ok(req);
        char b[72];   // the new thresholds on every browser
        snprintf(b, sizeof b, R"({"type":"calib","id":%d,"enter":%d,"exit":%d})", id, en, ex);
        wsText(b);
    });

    // ── Race ───────────────────────────────────────────────────────────────
    // Start: the timer runs ready -> stage -> start (or resumes a stopped
    // race) and every browser and goggle follows it.
    server.on(AsyncURIMatcher::exact("/api/race/start"), HTTP_POST, [](AsyncWebServerRequest* req) { raceRequestStart(); ok(req); });
    server.on(AsyncURIMatcher::exact("/api/race/stop"), HTTP_POST, [](AsyncWebServerRequest* req) { raceRequestStop(); ok(req); });
    // The Clear button: save to SD (per SD log mode), then forget the race.
    server.on(AsyncURIMatcher::exact("/api/race/save"), HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", raceClear());
    });

    // Delete a wrong pass (index = the pilot's pass number from 0, lapTime
    // = its lap time as the browser showed it, so a stale list can't delete
    // another pass).
    onJson("/api/laps/delete", [](AsyncWebServerRequest* req, JsonDocument& d) {
        int slot = d["slot"] | -1, index = d["index"] | -1;
        uint32_t lapMs = d["lapTime"] | 0u;
        if (raceDeleteLap(slot, index, lapMs)) ok(req);
        else req->send(409, "application/json", R"({"ok":false,"reason":"no such lap"})");
    });

    // The lap button on a race card (admins): a pass for that slot now.
    onJson("/api/laps/manual", [](AsyncWebServerRequest* req, JsonDocument& d) {
        if (raceManualLap(d["slot"] | -1)) ok(req);
        else req->send(409, "application/json", R"({"ok":false,"reason":"not racing"})");
    });

    server.on(AsyncURIMatcher::exact("/api/laps"), HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        Guard g;
        for (int i = 0; i < raceCore.lapCount(); i++) {
            const race::Lap& l = raceCore.lap(i);
            JsonObject o = arr.add<JsonObject>();
            o["slot"]    = l.slot;
            o["name"]    = rosterValid(l.rosterIdx) ? roster[l.rosterIdx].name : "---";
            o["lapTime"] = l.lapMs;
            o["ts"]      = l.ts;
        }
        sendJson(req, doc);
    });


    // ── Settings ───────────────────────────────────────────────────────────
    server.on(AsyncURIMatcher::exact("/api/settings"), HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        {
            Guard g;
            settingsToJson(doc.to<JsonObject>());
        }
        sendJson(req, doc);
    });
    onJson("/api/settings", [](AsyncWebServerRequest* req, JsonDocument& doc) {
        settingsFromJson(doc.as<JsonVariantConst>());
        applyGateConfig();
        ok(req);
        wsSettings();   // every browser shows the same settings
    });


    // ── ELRS OSD (vrxc_elrs settings) ──────────────────────────────────────
    server.on(AsyncURIMatcher::exact("/api/elrs"), HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument d;
        {
            Guard g;
            elrsToJson(d.to<JsonObject>());
        }
        sendJson(req, d);
    });

    onJson("/api/elrs", [](AsyncWebServerRequest* req, JsonDocument& d) {
        elrsFromJson(d.as<JsonVariantConst>());
        ok(req);
        wsText(R"({"type":"config_changed","what":"elrs"})");
    });

    onJson("/api/elrs/test", [](AsyncWebServerRequest* req, JsonDocument& d) {
        elrsTest(d["slot"] | -1);
        ok(req);
    });
    onJson("/api/elrs/announce", [](AsyncWebServerRequest* req, JsonDocument& d) {
        elrsAnnounce(d["text"] | "");
        ok(req);
    });

    // ── SD card ────────────────────────────────────────────────────────────
    server.on(AsyncURIMatcher::exact("/api/sd/status"), HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", sdPresent ? R"({"present":true})" : R"({"present":false})");
    });
    onJson("/api/sd/poll", [](AsyncWebServerRequest* req, JsonDocument& d) {
        sdPollEnabled = d["enable"] | false;
        ok(req);
    });
    // Pilot backup to /pilots_<date>_<time>.csv; restore from the file the
    // browser picked ({"path"}; /pilots.csv without one).
    server.on(AsyncURIMatcher::exact("/api/sd/pilots/backup"), HTTP_POST, [](AsyncWebServerRequest* req) {
        String path;
        if (!sdBackupRoster(path)) { req->send(503, "application/json", R"({"error":"no sd card"})"); return; }
        JsonDocument o;
        o["ok"]   = true;
        o["file"] = path.substring(1);
        sendJson(req, o);
    });
    onJson("/api/sd/pilots/restore", [](AsyncWebServerRequest* req, JsonDocument& d) {
        int n = sdRestoreRoster(String(d["path"] | "/pilots.csv"));
        if (n < 0) { req->send(503, "application/json", R"({"error":"no such backup"})"); return; }
        applyGateConfig();
        wsActiveUpdate();
        char b[48];
        snprintf(b, sizeof b, R"({"ok":true,"pilots":%d})", n);
        req->send(200, "application/json", b);
    });
    // Settings backup (everything but the pilots) to /settings_<date>_<time>.json, and
    // back. Admin only (token): the file holds the WiFi and admin passwords.
    onJson("/api/sd/settings/backup", [](AsyncWebServerRequest* req, JsonDocument& d) {
        if (!wsTokenValid(String(d["token"] | ""))) {
            req->send(403, "application/json", R"({"ok":false,"reason":"login"})");
            return;
        }
        String s = settingsBackupJson();
        String path = sdStampedName("settings", ".json");
        if (!sdWriteFile(path, (const uint8_t*)s.c_str(), s.length())) {
            req->send(503, "application/json", R"({"ok":false,"reason":"nosd"})");
            return;
        }
        JsonDocument o;
        o["ok"]   = true;
        o["file"] = path.substring(1);
        sendJson(req, o);
    });
    onJson("/api/sd/settings/restore", [](AsyncWebServerRequest* req, JsonDocument& d) {
        if (!wsTokenValid(String(d["token"] | ""))) {
            req->send(403, "application/json", R"({"ok":false,"reason":"login"})");
            return;
        }
        if (raceOn()) { req->send(409, "application/json", R"({"ok":false,"reason":"race"})"); return; }
        String path = d["path"] | "/settings.json";
        String body;
        if (!sdBackupName(path, "settings", ".json") || !sdReadFile(path, body)) {
            req->send(404, "application/json", R"({"ok":false,"reason":"nofile"})");
            return;
        }
        JsonDocument b;
        if (deserializeJson(b, body) || String(b["kind"] | "") != "fpv-c5-lt-settings") {
            req->send(422, "application/json", R"({"ok":false,"reason":"badfile"})");
            return;
        }
        JsonVariantConst v = b.as<JsonVariantConst>();
        if (v["settings"].is<JsonObjectConst>()) {
            settingsFromJson(v["settings"]);
            applyGateConfig();
            wsSettings();
        }
        if (v["elrs"].is<JsonObjectConst>()) {
            elrsFromJson(v["elrs"]);
            wsText(R"({"type":"config_changed","what":"elrs"})");
        }
        if (v["conn"].is<JsonObjectConst>()) connFromJson(v["conn"]);
        if (v["mic"].is<JsonObjectConst>()) {
            micFromJson(v["mic"]);
            wsText(R"({"type":"config_changed","what":"mic"})");
        }
        const char* pw = v["auth"]["password"] | "";
        if (strlen(pw) >= 1 && strlen(pw) <= 32) saveAdminPassword(String(pw));
        const char* netWhy = "missing";
        if (v["net"].is<JsonObjectConst>()) {
            netWhy = netFromJson(v["net"]);
            if (!netWhy) wsText(R"({"type":"config_changed","what":"net"})");
        }
        JsonDocument o;
        o["ok"]  = true;
        o["net"] = netWhy == nullptr;   // WiFi settings: applied after a restart
        if (netWhy) o["netError"] = netWhy;
        if (b["saved"].is<uint32_t>()) o["saved"] = b["saved"].as<uint32_t>();
        sendJson(req, o);
    });

    // A connection QR image drawn by the browser (PNG body), saved to the
    // card as /qr_*.png. Admin only: the WiFi QR carries the password.
    server.on(AsyncURIMatcher::exact("/api/sd/upload"), HTTP_POST,
        [](AsyncWebServerRequest* req) {
            QrUpload* u = reinterpret_cast<QrUpload*>(req->_tempObject);
            String path = req->hasParam("path") ? req->getParam("path")->value() : "";
            int code = 200;
            const char* msg = R"({"ok":true})";
            if (!u) code = 400, msg = R"({"ok":false,"reason":"size"})";
            else if (u->why) code = u->code, msg = u->why;
            else if (!u->total || u->got != u->total) code = 400, msg = R"({"ok":false,"reason":"size"})";
            else if (!sdReplaceFile(kQrUploadTmp, path)) code = 503, msg = R"({"ok":false,"reason":"nosd"})";
            if (code != 200 && u && u->started) sdDeleteFile(kQrUploadTmp);
            req->send(code, "application/json", msg);
        },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index == 0) {
                // malloc: the server free()s _tempObject when the request ends.
                QrUpload* n = reinterpret_cast<QrUpload*>(malloc(sizeof(QrUpload)));
                if (!n) return;
                *n = QrUpload{};
                n->total = total;
                String path = req->hasParam("path") ? req->getParam("path")->value() : "";
                String token = req->hasParam("token") ? req->getParam("token")->value() : "";
                if (!wsTokenValid(token)) n->code = 403, n->why = R"({"ok":false,"reason":"login"})";
                else if (!qrImagePath(path)) n->code = 400, n->why = R"({"ok":false,"reason":"path"})";
                else if (!total || total > kQrImageMax) n->code = 413, n->why = R"({"ok":false,"reason":"size"})";
                req->_tempObject = n;
            }
            QrUpload* u = reinterpret_cast<QrUpload*>(req->_tempObject);
            if (!u || u->why || index != u->got) return;
            // Each piece straight to a temporary file on the card.
            if (!sdAppendFile(kQrUploadTmp, data, len, index == 0)) {
                u->code = 503, u->why = R"({"ok":false,"reason":"nosd"})";
                return;
            }
            u->started = true;
            u->got = index + len;
        });

    server.on(AsyncURIMatcher::exact("/api/sd/files"), HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", sdListJson());
    });
    // Download a file. The settings backup and the QR images hold
    // passwords: only for a logged-in admin (token).
    server.on(AsyncURIMatcher::exact("/api/sd/file"), HTTP_GET, [](AsyncWebServerRequest* req) {
        String path = req->hasParam("path") ? req->getParam("path")->value() : "";
        String token = req->hasParam("token") ? req->getParam("token")->value() : "";
        if (sdSecretFile(path) && !wsTokenValid(token)) {
            req->send(403, "application/json", R"({"error":"login"})");
            return;
        }
        // Read a piece at a time (a QR image is ~80 KB): no big buffer.
        int32_t size = sdFileSize(path);
        if (size < 0 || size > 2000000) {
            req->send(404, "application/json", R"({"error":"not found"})");
            return;
        }
        AsyncWebServerResponse* res = req->beginResponse(mimeOf(path), (size_t)size,
            [path, size](uint8_t* out, size_t maxLen, size_t index) -> size_t {
                if (index >= (size_t)size) return 0;
                return sdReadAt(path, index, out, std::min(maxLen, (size_t)size - index));
            });
        String name = path.substring(1);
        res->addHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
        req->send(res);
    });
    onJson("/api/sd/files/delete", [](AsyncWebServerRequest* req, JsonDocument& d) {
        bool done = sdDeleteFile(d["path"] | "");
        req->send(200, "application/json", done ? R"({"ok":true})" : R"({"ok":false})");
    });
    // Format: delete every file on the card. Asks for the admin password
    // again (it can't be undone) and refuses while a race is on.
    onJson("/api/sd/erase", [](AsyncWebServerRequest* req, JsonDocument& d) {
        if (adminPassword != (d["password"] | "")) {
            req->send(401, "application/json", R"({"ok":false,"reason":"password"})");
            return;
        }
        bool busy;
        {
            Guard g;
            busy = raceCore.running() || raceCore.paused() || raceCore.lapCount() > 0;
        }
        if (busy) { req->send(409, "application/json", R"({"ok":false,"reason":"race"})"); return; }
        int n = sdEraseAll();
        if (n < 0) { req->send(503, "application/json", R"({"ok":false,"reason":"nosd"})"); return; }
        char b[48];
        snprintf(b, sizeof b, R"({"ok":true,"removed":%d})", n);
        req->send(200, "application/json", b);
    });

    // The clock of a logged-in browser (the S3 has no RTC): race files get
    // their date. Browsers also send it in their WebSocket hello.
    onJson("/api/clock", [](AsyncWebServerRequest* req, JsonDocument& d) {
        if (!wsTokenValid(String(d["token"] | ""))) {
            req->send(403, "application/json", R"({"ok":false,"reason":"login"})");
            return;
        }
        sdSetClock(d["epoch"] | (int64_t)0, d["tz"] | 0);
        ok(req);
    });

    // ── Auth (soft: protects against accidental changes, not attacks) ──────
    // Login: a token for this browser, unless as many admins as allowed are
    // logged in already (the browser says so and stays a viewer).
    onJson("/api/auth/login", [](AsyncWebServerRequest* req, JsonDocument& d) {
        if (adminPassword != (d["password"] | "")) {
            req->send(401, "application/json", R"({"ok":false,"error":"bad password"})");
            return;
        }
        String token;
        char b[96];
        if (!wsLogin(token)) {
            snprintf(b, sizeof b, R"({"ok":false,"reason":"full","max":%u})", connLimits.adminMax);
            req->send(409, "application/json", b);
            return;
        }
        snprintf(b, sizeof b, R"({"ok":true,"token":"%s"})", token.c_str());
        req->send(200, "application/json", b);
    });
    onJson("/api/auth/logout", [](AsyncWebServerRequest* req, JsonDocument& d) {
        wsLogout(String(d["token"] | ""));
        ok(req);
    });

    // Connection limits (settings) and who is connected now.
    server.on(AsyncURIMatcher::exact("/api/conn"), HTTP_GET, [](AsyncWebServerRequest* req) {
        int admins, viewers;
        wsCounts(admins, viewers);
        char b[256];
        snprintf(b, sizeof b,
                 R"({"adminMax":%u,"viewHost":%u,"viewClient":%u,"adminLimit":%u,"viewHostLimit":%u,)"
                 R"("viewClientLimit":%u,"hostTotal":%u,"ap":%s,"admins":%d,"logins":%d,"viewers":%d,"viewerLimit":%d,"holdSec":%u})",
                 connLimits.adminMax, connLimits.viewHost, connLimits.viewClient, CONN_ADMIN_MAX, CONN_VIEW_HOST_MAX,
                 CONN_VIEW_CLIENT_MAX, CONN_HOST_TOTAL, netIsAp() ? "true" : "false", admins, wsAdminLogins(), viewers,
                 wsViewerLimit(), loginHoldSec);
        req->send(200, "application/json", b);
    });
    onJson("/api/conn", [](AsyncWebServerRequest* req, JsonDocument& d) {
        connFromJson(d.as<JsonVariantConst>());
        ok(req);
    });
    onJson("/api/auth/password", [](AsyncWebServerRequest* req, JsonDocument& d) {
        const char* pw = d["password"] | "";
        size_t n = strlen(pw);
        if (n < 1 || n > 32) { bad(req, "length 1..32"); return; }
        saveAdminPassword(String(pw));
        ok(req);
    });

    // ── WiFi: host / client (applied after a reboot) ───────────────────────
    // The client network's password is never sent back to the browser.
    server.on(AsyncURIMatcher::exact("/api/net"), HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument d;
        d["mode"]      = netMode;
        d["apSsid"]    = apSsid;
        d["apPass"]    = apPass;
        d["staSsid"]   = staSsid;
        d["staPassSet"] = staPass.length() > 0;
        d["fallbackSec"] = netFallbackSec;
        ipToJson(d.as<JsonObject>());
        NetStatus st = netStatus();
        JsonObject o = d["status"].to<JsonObject>();
        o["ap"]        = st.ap;
        o["connected"] = st.connected;
        o["fallback"]  = st.fallback;
        o["ip"]        = st.ip.toString();
        o["rssi"]      = st.rssi;
        sendJson(req, d);
    });
    onJson("/api/net", [](AsyncWebServerRequest* req, JsonDocument& d) {
        if (const char* why = netFromJson(d.as<JsonVariantConst>())) { bad(req, why); return; }
        ok(req);
        wsText(R"({"type":"config_changed","what":"net"})");
    });
    // Client mode's network for the connection QR code. The password is not
    // in GET /api/net; only a logged-in admin (token) gets it here.
    onJson("/api/net/wifi-qr", [](AsyncWebServerRequest* req, JsonDocument& d) {
        if (!wsTokenValid(String(d["token"] | ""))) {
            req->send(403, "application/json", R"({"ok":false,"reason":"login"})");
            return;
        }
        JsonDocument o;
        o["ssid"] = staSsid;
        o["pass"] = staPass;
        sendJson(req, o);
    });

    // Restart button, and the reboot after a WiFi change: tidy up, then a
    // power-on-like restart (sys_diag). The answer goes out first.
    server.on(AsyncURIMatcher::exact("/api/restart"), HTTP_POST, [](AsyncWebServerRequest* req) {
        ok(req);
        Serial.println("[Sys] restart requested");
        sysRequestRestart(1000);
    });
    server.on(AsyncURIMatcher::exact("/api/reboot"), HTTP_POST, [](AsyncWebServerRequest* req) {
        ok(req);
        sysRequestRestart(500);
    });
    // The Sense board's microphone: start / false start settings.
    server.on(AsyncURIMatcher::exact("/api/mic"), HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument d;
        micToJson(d.to<JsonObject>());
        d["ok"] = micOk();   // the microphone is there and running
        sendJson(req, d);
    });
    onJson("/api/mic", [](AsyncWebServerRequest* req, JsonDocument& d) {
        micFromJson(d.as<JsonVariantConst>());
        ok(req);
        wsText(R"({"type":"config_changed","what":"mic"})");
    });

    // Reload every open page (admins and viewers). Admin only (token).
    onJson("/api/reload-all", [](AsyncWebServerRequest* req, JsonDocument& d) {
        if (!wsTokenValid(String(d["token"] | ""))) {
            req->send(403, "application/json", R"({"ok":false,"reason":"login"})");
            return;
        }
        ok(req);
        Serial.println("[Web] every page asked to reload");
        wsText(R"({"type":"reload"})");
    });
    // Why the timer last started, crashes since, and free memory.
    server.on(AsyncURIMatcher::exact("/api/sys"), HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", sysJson());
    });

    // ── Static files, then the captive portal catch-all ────────────────────
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html").setCacheControl("no-cache");
    server.onNotFound([](AsyncWebServerRequest* req) {
        if (req->method() == HTTP_OPTIONS) { req->send(204); return; }
        if (req->url().startsWith("/api")) req->send(404, "application/json", R"({"error":"not found"})");
        else cpRedirect(req);
    });
}
