#include <ArduinoJson.h>
#include <LittleFS.h>
#include <freertos/timers.h>
#include <functional>
#include "http_routes.h"
#include "c5_link.h"
#include "data_model.h"
#include "elrs.h"
#include "nvs_store.h"
#include "race.h"
#include "sd_store.h"
#include "wifi_mgr.h"
#include "ws_handler.h"

namespace {

using JsonHandler = std::function<void(AsyncWebServerRequest*, JsonDocument&)>;

// POST with a JSON body. The body can arrive in several chunks.
void onJson(const char* path, JsonHandler h) {
    server.on(path, HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
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

void posFromJson(JsonObject o, const char* key, osd::Pos& p) {
    const char* v = o[key] | (const char*)nullptr;
    int row, col;
    if (v && sscanf(v, "%d,%d", &row, &col) == 2 && row >= 0 && row < osd::kRows && col < osd::kCols)
        p = {(int8_t)row, (int8_t)(col < 0 ? -1 : col)};
}

void copyStr(JsonDocument& d, const char* key, char* dst, size_t cap) {
    if (d[key].is<const char*>()) strlcpy(dst, d[key].as<const char*>(), cap);
}

}  // namespace

void registerHttpRoutes() {
    // ── Captive portal probes (first, so they take priority) ───────────────
    for (const char* p : {"/generate_204", "/gen_204", "/connecttest.txt", "/hotspot-detect.html",
                          "/library/test/success.html", "/success.txt", "/canonical.html", "/ncsi.txt",
                          "/redirect", "/fwlink", "/wpad.dat", "/bag", "/kindle-wifi/wifistub.html", "/chat"})
        server.on(p, HTTP_ANY, [](AsyncWebServerRequest* r) { cpRedirect(r); });
    for (const char* p : {"/favicon.ico", "/apple-touch-icon.png", "/robots.txt"})
        server.on(p, HTTP_ANY, [](AsyncWebServerRequest* r) { r->send(204); });

    // ── Roster ─────────────────────────────────────────────────────────────
    server.on("/api/pilots", HTTP_GET, [](AsyncWebServerRequest* req) {
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
    server.on("/api/active", HTTP_GET, [](AsyncWebServerRequest* req) {
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
        {
            Guard g;
            if (!rosterValid(id)) { bad(req, "bad id"); return; }
            rosterCal[id].enterRssi = constrain((int)(doc["enter"] | rosterCal[id].enterRssi), 0, 1023);
            rosterCal[id].exitRssi  = constrain((int)(doc["exit"] | rosterCal[id].exitRssi), 0, 1023);
            saveRosterPilot(id);
        }
        applyGateConfig();
        ok(req);
    });

    // ── Race ───────────────────────────────────────────────────────────────
    server.on("/api/race/stage", HTTP_POST, [](AsyncWebServerRequest* req) { raceStage(); ok(req); });
    server.on("/api/race/start", HTTP_POST, [](AsyncWebServerRequest* req) { raceStart(); ok(req); });
    server.on("/api/race/stop", HTTP_POST, [](AsyncWebServerRequest* req) { raceStop(); ok(req); });
    // Continue a paused race (Stop -> Start) without resetting laps.
    server.on("/api/race/resume", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (raceResume()) ok(req);
        else req->send(200, "application/json", R"({"ok":false,"reason":"not_paused"})");
    });
    // The Clear button: save to SD (per SD log mode), then forget the race.
    server.on("/api/race/save", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", raceClear());
    });

    server.on("/api/laps", HTTP_GET, [](AsyncWebServerRequest* req) {
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

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        Guard g;
        doc["raceRunning"] = raceCore.running();
        doc["raceStartMs"] = raceCore.startMs();
        doc["lapCount"]    = raceCore.lapCount();
        doc["rosterCount"] = rosterCount;
        doc["uptime"]      = millis();
        doc["sdPresent"]   = sdPresent;
        sendJson(req, doc);
    });

    // ── Settings ───────────────────────────────────────────────────────────
    server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        Guard g;
        doc["lapMode"]    = settings.lapMode;
        doc["cooldownMs"] = settings.cooldownMs;
        doc["sdLogMode"]  = settings.sdLogMode;
        doc["totalLaps"]  = settings.totalLaps;
        doc["gain"]       = settings.gain;
        sendJson(req, doc);
    });
    onJson("/api/settings", [](AsyncWebServerRequest* req, JsonDocument& doc) {
        {
            Guard g;
            // Changing detection settings mid-race would change how laps
            // are counted; the UI only sends them while idle, the gain and
            // total laps excepted.
            if (!doc["lapMode"].isNull()) settings.lapMode = (int)doc["lapMode"] == 1 ? 1 : 0;
            if (!doc["sdLogMode"].isNull()) settings.sdLogMode = (uint8_t)constrain((int)doc["sdLogMode"], 0, 2);
            if (!doc["cooldownMs"].isNull()) settings.cooldownMs = constrain((int)doc["cooldownMs"], 500, 30000);
            if (!doc["totalLaps"].isNull()) settings.totalLaps = (uint16_t)constrain((int)doc["totalLaps"], 0, 99);
            if (!doc["gain"].isNull()) settings.gain = (uint8_t)constrain((int)doc["gain"], 0, 89);
            saveSettings();
        }
        applyGateConfig();
        ok(req);
    });

    // ── C5 receiver ────────────────────────────────────────────────────────
    server.on("/api/c5", HTTP_GET, [](AsyncWebServerRequest* req) {
        C5Status c = c5Status();
        JsonDocument d;
        d["online"] = c.online, d["scanning"] = c.scanning, d["state"] = c.state;
        d["fw"] = c.firmware, d["gain"] = c.gain, d["records"] = c.records;
        d["gaps"] = c.seqGaps, d["bad"] = c.badRecords, d["badLines"] = c.badLines;
        JsonArray rate = d["rate"].to<JsonArray>();
        for (int i = 0; i < 8; i++) rate.add(c.rate[i]);
        sendJson(req, d);
    });

    // ── ELRS OSD (vrxc_elrs settings) ──────────────────────────────────────
    server.on("/api/elrs", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument d;
        Guard g;
        const osd::Options& o = elrsCfg.opt;
        d["enabled"]       = elrsCfg.enabled;
        d["transport"]     = elrsCfg.transport;
        d["host"]          = elrsCfg.host;
        d["port"]          = elrsCfg.port;
        d["rdBind"]        = elrsCfg.rdBind;
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
        sendJson(req, d);
    });

    onJson("/api/elrs", [](AsyncWebServerRequest* req, JsonDocument& d) {
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
            JsonObject p = d["pos"].as<JsonObject>();
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
        ok(req);
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
    server.on("/api/sd/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", sdPresent ? R"({"present":true})" : R"({"present":false})");
    });
    onJson("/api/sd/poll", [](AsyncWebServerRequest* req, JsonDocument& d) {
        sdPollEnabled = d["enable"] | false;
        ok(req);
    });
    server.on("/api/sd/pilots/backup", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (sdBackupRoster()) ok(req);
        else req->send(503, "application/json", R"({"error":"no sd card"})");
    });
    server.on("/api/sd/pilots/restore", HTTP_POST, [](AsyncWebServerRequest* req) {
        int n = sdRestoreRoster();
        if (n < 0) { req->send(503, "application/json", R"({"error":"no /pilots.csv"})"); return; }
        applyGateConfig();
        wsActiveUpdate();
        char b[48];
        snprintf(b, sizeof b, R"({"ok":true,"pilots":%d})", n);
        req->send(200, "application/json", b);
    });
    server.on("/api/sd/files", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", sdListJson());
    });
    server.on("/api/sd/file", HTTP_GET, [](AsyncWebServerRequest* req) {
        String path = req->hasParam("path") ? req->getParam("path")->value() : "";
        String body;
        if (!sdReadFile(path, body)) { req->send(404, "application/json", R"({"error":"not found"})"); return; }
        AsyncWebServerResponse* res = req->beginResponse(200, "text/csv; charset=utf-8", body);
        String name = path.substring(1);
        res->addHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
        req->send(res);
    });
    onJson("/api/sd/files/delete", [](AsyncWebServerRequest* req, JsonDocument& d) {
        bool done = sdDeleteFile(d["path"] | "");
        req->send(200, "application/json", done ? R"({"ok":true})" : R"({"ok":false})");
    });

    // ── Auth (soft: protects against accidental changes, not attacks) ──────
    server.on("/api/auth/password", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument d;
        d["password"] = adminPassword;
        sendJson(req, d);
    });
    onJson("/api/auth/login", [](AsyncWebServerRequest* req, JsonDocument& d) {
        if (adminPassword == (d["password"] | "")) ok(req);
        else req->send(401, "application/json", R"({"ok":false,"error":"bad password"})");
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
    server.on("/api/net", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument d;
        d["mode"]      = netMode;
        d["apSsid"]    = apSsid;
        d["apPass"]    = apPass;
        d["staSsid"]   = staSsid;
        d["staPassSet"] = staPass.length() > 0;
        d["fallbackSec"] = netFallbackSec;
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
        uint8_t mode = (int)(d["mode"] | 0) == NET_STA ? NET_STA : NET_AP;
        String apS = d["apSsid"] | "", apP = d["apPass"] | "";
        String staS = d["staSsid"] | "", staP = d["staPass"] | "";
        if (apS.length() < 1 || apS.length() > 31) { bad(req, "AP SSID 1..31"); return; }
        if (apP.length() < 8 || apP.length() > 63) { bad(req, "AP password 8..63"); return; }
        if (mode == NET_STA && (staS.length() < 1 || staS.length() > 32)) { bad(req, "client SSID 1..32"); return; }
        int fbSec = d["fallbackSec"] | (int)netFallbackSec;
        if (!netFallbackValid(fbSec)) { bad(req, "fallbackSec 15/30/60"); return; }
        // Empty client password keeps the stored one (the UI never shows it);
        // "staPassClear" switches to an open network.
        if (!staP.length() && !(d["staPassClear"] | false)) staP = staPass;
        if (staP.length() && (staP.length() < 8 || staP.length() > 63)) { bad(req, "client password 8..63"); return; }
        saveNet(mode, apS, apP, staS, staP, (uint8_t)fbSec);
        ok(req);
    });
    server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
        ok(req);
        static TimerHandle_t t = xTimerCreate("reboot", pdMS_TO_TICKS(500), pdFALSE, nullptr,
                                              [](TimerHandle_t) { ESP.restart(); });
        xTimerStart(t, 0);
    });

    // ── Static files, then the captive portal catch-all ────────────────────
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html").setCacheControl("no-cache");
    server.onNotFound([](AsyncWebServerRequest* req) {
        if (req->method() == HTTP_OPTIONS) { req->send(204); return; }
        if (req->url().startsWith("/api")) req->send(404, "application/json", R"({"error":"not found"})");
        else cpRedirect(req);
    });
}
