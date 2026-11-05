#include <ArduinoJson.h>
#include <memory>
#include <vector>
#include "ws_handler.h"
#include "c5_link.h"
#include "data_model.h"
#include "race.h"

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer      dnsServer;

void wsText(const String& msg) { ws.textAll(msg); }

// Connected client ids, for the telemetry below (kept from the WebSocket
// events; the library's own client list is not ours to walk).
static portMUX_TYPE idMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t wsIds[16];
static int      wsIdCount = 0;

static void rememberClient(uint32_t id, bool on) {
    portENTER_CRITICAL(&idMux);
    int i = 0;
    while (i < wsIdCount && wsIds[i] != id) ++i;
    if (on && i == wsIdCount && wsIdCount < 16) wsIds[wsIdCount++] = id;
    if (!on && i < wsIdCount) wsIds[i] = wsIds[--wsIdCount];
    portEXIT_CRITICAL(&idMux);
}

// The 10 Hz telemetry goes only to browsers that are keeping up. A client's
// queue holds 32 messages and, once full, drops whatever comes next; a phone
// that fell behind for a few seconds had its queue filled with telemetry, so
// race_stop / race_clear / laps were the ones dropped and that browser fell
// out of step. Telemetry is the one message that can be skipped.
void wsTelemetry(const String& msg) {
    uint32_t ids[16];
    int n;
    portENTER_CRITICAL(&idMux);
    n = wsIdCount;
    memcpy(ids, wsIds, n * sizeof ids[0]);
    portEXIT_CRITICAL(&idMux);
    if (!n) return;
    auto buf = std::make_shared<std::vector<uint8_t>>(msg.begin(), msg.end());
    for (int i = 0; i < n; ++i) {
        AsyncWebSocketClient* c = ws.client(ids[i]);
        if (c && c->status() == WS_CONNECTED && c->queueLen() < 4) c->text(buf);
    }
}
void wsText(const char* msg)   { ws.textAll(msg); }

static void addSlots(JsonArray pa, bool withRace) {
    for (int s = 0; s < MAX_ACTIVE; s++) {
        JsonObject o = pa.add<JsonObject>();
        int ri = activePilots[s];
        bool ok = rosterValid(ri);
        o["slot"]      = s;
        o["rosterIdx"] = ri;
        o["name"]      = activeName(s);
        o["yomi"]      = ok ? roster[ri].yomi : "";
        o["freq"]      = ok ? roster[ri].freqMhz : 0;
        o["enter"]     = ok ? rosterCal[ri].enterRssi : DEFAULT_ENTER;
        o["exit"]      = ok ? rosterCal[ri].exitRssi : DEFAULT_EXIT;
        if (withRace) {
            const race::SlotState& st = raceCore.slot(s);
            C5SlotLive l = c5Live(s);
            o["lapCount"] = st.lapCount;
            o["bestLap"]  = st.bestLapMs;
            o["rssi"]     = l.filtered;
            o["crossing"] = l.inside;
        }
    }
}

// Race settings, as in GET /api/settings (call under the lock).
static void addSettings(JsonDocument& d) {
    d["lapMode"]    = settings.lapMode;
    d["cooldownMs"] = settings.cooldownMs;
    d["sdLogMode"]  = settings.sdLogMode;
    d["totalLaps"]  = settings.totalLaps;
    d["gain"]       = settings.gain;
    d["rankMode"]     = raceRules.rankMode;
    d["timeLimitSec"] = raceRules.timeLimitSec;
    d["countLastLap"] = raceRules.countLastLap;
}

void wsSettings() {
    JsonDocument d;
    d["type"] = "settings";
    {
        Guard g;
        addSettings(d);
    }
    String s;
    serializeJson(d, s);
    wsText(s);
}

void wsActiveUpdate() {
    JsonDocument d;
    d["type"] = "active_update";
    {
        Guard g;
        addSlots(d["pilots"].to<JsonArray>(), false);
    }
    String s;
    serializeJson(d, s);
    wsText(s);
}

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType evType, void*, uint8_t*, size_t) {
    if (evType == WS_EVT_DISCONNECT) rememberClient(client->id(), false);
    if (evType != WS_EVT_CONNECT) return;
    rememberClient(client->id(), true);
    JsonDocument doc;
    {
        Guard g;
        uint32_t now = millis();
        doc["type"]          = "init";
        doc["raceRunning"]   = raceCore.running();
        doc["racePaused"]    = raceCore.paused();
        doc["raceStartMs"]   = raceCore.startMs();
        // Authoritative elapsed time so a reloaded client restores the timer
        // (running: keeps counting; paused: frozen at the pause point).
        doc["raceElapsedMs"] = raceCore.elapsed(now);
        doc["now"]           = now;   // the browser's race clock follows this clock
        doc["sdPresent"]     = sdPresent;
        addSettings(doc);
        addSlots(doc["pilots"].to<JsonArray>(), true);
    }
    String msg;
    serializeJson(doc, msg);
    client->text(msg);
}

void initWsHandler() {
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
}
