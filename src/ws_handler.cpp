#include <ArduinoJson.h>
#include "ws_handler.h"
#include "c5_link.h"
#include "data_model.h"
#include "race.h"

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer      dnsServer;

void wsText(const String& msg) { ws.textAll(msg); }
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
    if (evType != WS_EVT_CONNECT) return;
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
        doc["sdPresent"]     = sdPresent;
        doc["totalLaps"]     = settings.totalLaps;
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
