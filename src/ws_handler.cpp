#include <ArduinoJson.h>
#include <memory>
#include <vector>
#include "ws_handler.h"
#include "c5_link.h"
#include "data_model.h"
#include "race.h"
#include "sd_store.h"
#include "wifi_mgr.h"

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer      dnsServer;

void wsText(const String& msg) { ws.textAll(msg); }

// ── Clients and admin logins ──────────────────────────────────────────────
// Every WebSocket client is a viewer until it says hello with the token of a
// logged-in admin. Admins get the 10 Hz telemetry and are never disconnected
// for space; viewers get race events only, and when there are more viewers
// than allowed the oldest one is closed (code 4001, so it does not reconnect
// by itself). Admin logins are limited too: a login past the limit is
// refused. A login stays held while one of its browsers is connected and for
// kSessionGraceMs after (a page reload keeps it).
static constexpr int      kMaxClients     = 24;
static constexpr int      kMaxSessions    = CONN_ADMIN_MAX;
static constexpr uint32_t kSessionGraceMs = 60000;
static constexpr uint32_t kHelloGraceMs   = 3000;   // time a new client has to say who it is

struct WsClient {
    uint32_t id;
    uint32_t since;
    int8_t   session;   // -1 viewer, else index into sessions
};
struct Session {
    bool     used;
    char     token[17];
    uint32_t lastSeen;
};
static portMUX_TYPE idMux = portMUX_INITIALIZER_UNLOCKED;
static WsClient wsClients[kMaxClients];
static int      wsIdCount = 0;
static Session  sessions[kMaxSessions];

static void rememberClient(uint32_t id, bool on) {
    portENTER_CRITICAL(&idMux);
    int i = 0;
    while (i < wsIdCount && wsClients[i].id != id) ++i;
    if (on && i == wsIdCount && wsIdCount < kMaxClients) wsClients[wsIdCount++] = {id, millis(), -1};
    if (!on && i < wsIdCount) {
        int8_t sess = wsClients[i].session;
        if (sess >= 0) sessions[sess].lastSeen = millis();   // grace starts now
        wsClients[i] = wsClients[--wsIdCount];
    }
    portEXIT_CRITICAL(&idMux);
}

static int findSession(const char* token) {
    if (!token || strlen(token) != 16) return -1;
    for (int i = 0; i < kMaxSessions; i++)
        if (sessions[i].used && strcmp(sessions[i].token, token) == 0) return i;
    return -1;
}

// A session counts while one of its browsers is connected or within the grace.
static bool sessionLive(int i, uint32_t now) {
    if (!sessions[i].used) return false;
    for (int c = 0; c < wsIdCount; c++)
        if (wsClients[c].session == i) return true;
    return now - sessions[i].lastSeen < kSessionGraceMs;
}

static int liveSessions(uint32_t now) {
    int n = 0;
    for (int i = 0; i < kMaxSessions; i++) n += sessionLive(i, now);
    return n;
}

bool wsLogin(String& token) {
    uint32_t now = millis();
    char t[17];
    snprintf(t, sizeof t, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    bool ok = false;
    portENTER_CRITICAL(&idMux);
    if (liveSessions(now) < connLimits.adminMax) {
        for (int i = 0; i < kMaxSessions; i++) {
            if (!sessionLive(i, now)) {
                sessions[i].used = true;
                memcpy(sessions[i].token, t, sizeof t);
                sessions[i].lastSeen = now;
                ok = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&idMux);
    if (ok) token = t;
    return ok;
}

void wsLogout(const String& token) {
    uint32_t ids[kMaxClients];
    int n = 0;
    portENTER_CRITICAL(&idMux);
    int s = findSession(token.c_str());
    if (s >= 0) {
        sessions[s].used = false;
        for (int c = 0; c < wsIdCount; c++)
            if (wsClients[c].session == s) wsClients[c].session = -1, ids[n++] = wsClients[c].id;
    }
    portEXIT_CRITICAL(&idMux);
    for (int i = 0; i < n; i++) {
        AsyncWebSocketClient* c = ws.client(ids[i]);
        if (c) c->text(R"({"type":"hello","admin":false})");
    }
}

bool wsTokenValid(const String& token) {
    portENTER_CRITICAL(&idMux);
    bool ok = findSession(token.c_str()) >= 0 && sessionLive(findSession(token.c_str()), millis());
    portEXIT_CRITICAL(&idMux);
    return ok;
}

void wsCounts(int& admins, int& viewers) {
    admins = viewers = 0;
    portENTER_CRITICAL(&idMux);
    for (int c = 0; c < wsIdCount; c++) (wsClients[c].session >= 0 ? admins : viewers)++;
    portEXIT_CRITICAL(&idMux);
}

int wsAdminLogins() {
    portENTER_CRITICAL(&idMux);
    int n = liveSessions(millis());
    portEXIT_CRITICAL(&idMux);
    return n;
}

int wsViewerLimit() { return netIsAp() ? connLimits.viewHost : connLimits.viewClient; }

// {"type":"hello","token":"...","epoch":ms,"tz":min}: a browser says whether
// it is logged in; an admin's clock sets the timer's date.
static void onHello(AsyncWebSocketClient* client, JsonDocument& d) {
    const char* token = d["token"] | "";
    bool admin = false;
    portENTER_CRITICAL(&idMux);
    int s = findSession(token);
    if (s >= 0 && !sessionLive(s, millis())) s = -1;
    for (int c = 0; c < wsIdCount; c++)
        if (wsClients[c].id == client->id()) {
            wsClients[c].session = s;
            admin = s >= 0;
            if (admin) sessions[s].lastSeen = millis();
        }
    portEXIT_CRITICAL(&idMux);
    if (admin) sdSetClock(d["epoch"] | (int64_t)0, d["tz"] | 0);
    client->text(admin ? R"({"type":"hello","admin":true})" : R"({"type":"hello","admin":false})");
}

// Called from loop(): keep logins of connected admins fresh and close the
// oldest viewers over the limit.
void wsLoop(uint32_t now) {
    static uint32_t last = 0;
    if (now - last < 500) return;
    last = now;
    int limit = wsViewerLimit();
    uint32_t kick[kMaxClients];
    int nKick = 0;
    portENTER_CRITICAL(&idMux);
    for (int c = 0; c < wsIdCount; c++)
        if (wsClients[c].session >= 0) sessions[wsClients[c].session].lastSeen = now;
    // Viewers that had time to say hello, oldest first.
    int viewers = 0;
    for (int c = 0; c < wsIdCount; c++)
        if (wsClients[c].session < 0 && now - wsClients[c].since >= kHelloGraceMs) viewers++;
    while (viewers > limit) {
        int oldest = -1;
        for (int c = 0; c < wsIdCount; c++) {
            const WsClient& w = wsClients[c];
            if (w.session >= 0 || now - w.since < kHelloGraceMs) continue;
            bool taken = false;
            for (int k = 0; k < nKick; k++) taken |= kick[k] == w.id;
            if (!taken && (oldest < 0 || (int32_t)(w.since - wsClients[oldest].since) < 0)) oldest = c;
        }
        if (oldest < 0) break;
        kick[nKick++] = wsClients[oldest].id;
        viewers--;
    }
    portEXIT_CRITICAL(&idMux);
    for (int i = 0; i < nKick; i++) {
        AsyncWebSocketClient* c = ws.client(kick[i]);
        if (c) {
            Serial.printf("[WS] viewer #%lu closed (more than %d viewers)\n", (unsigned long)kick[i], limit);
            c->close(4001, "viewers full");
        }
    }
}

// The 10 Hz telemetry goes only to admins (viewers see the light screen)
// and only to browsers that are keeping up. A client's queue holds 32
// messages and, once full, drops whatever comes next; a phone that fell
// behind for a few seconds had its queue filled with telemetry, so
// race_stop / race_clear / laps were the ones dropped and that browser fell
// out of step. Telemetry is the one message that can be skipped.
void wsTelemetry(const String& msg) {
    uint32_t ids[kMaxClients];
    int n = 0;
    portENTER_CRITICAL(&idMux);
    for (int c = 0; c < wsIdCount; c++)
        if (wsClients[c].session >= 0) ids[n++] = wsClients[c].id;
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

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType evType, void* arg, uint8_t* data,
                      size_t len) {
    if (evType == WS_EVT_DISCONNECT) rememberClient(client->id(), false);
    if (evType == WS_EVT_DATA) {
        // Only small single-frame text messages (hello) come from browsers.
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT && len < 256) {
            JsonDocument d;
            if (!deserializeJson(d, data, len) && strcmp(d["type"] | "", "hello") == 0) onHello(client, d);
        }
        return;
    }
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
