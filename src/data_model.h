#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"
#include "osd_engine.h"
#include "race_core.h"

// A pilot in the roster. Pilots are identified by their VTX frequency
// (the C5 measures one frequency per slot), not by a beacon MAC.
struct PilotRoster {
    char     name[32];
    char     yomi[32];       // reading for Japanese TTS
    uint16_t freqMhz;        // VTX frequency, 0 = not set
    char     bind[33];       // ELRS bind phrase; empty = use the name (as vrxc_elrs)
    bool     osd;            // send ELRS OSD to this pilot's goggles
};

struct CalibConfig {
    int enterRssi;
    int exitRssi;
};

struct Settings {
    uint8_t  lapMode    = 0;      // 0 holeshot, 1 immediate
    uint32_t cooldownMs = 3000;
    uint8_t  sdLogMode  = 0;      // 0 always, 1 rotate, 2 off
    uint16_t totalLaps  = 0;      // 0 = unlimited
    uint8_t  gain       = C5_DEFAULT_GAIN;
};

// How the race is ranked (web UI ranking panel) and the timed race.
// Stored under its own key, so Settings' stored layout stays the same.
struct RaceRules {
    uint8_t  rankMode     = 0;     // 0 laps then total time, 1 best lap, 2 laps in a time limit
    uint16_t timeLimitSec = 120;   // rankMode 2: race length (30 .. 1800 s)
    bool     countLastLap = true;  // rankMode 2: the lap flown when time runs out counts
};

// How many browsers may connect (web UI). Admins (logged in) keep their
// connection; viewers (not logged in) get the light spectator screen and the
// oldest viewer is disconnected when there are too many. Limits per WiFi mode
// leave headroom: admins get 10 Hz RSSI telemetry, viewers only race events.
//   host mode (our access point, 10 stations at most): admins 1-3, viewers
//     0-5, so with the netpack XIAO at most 9 stations (the netpack is not
//     counted, and a full access point makes room for it: ws_handler);
//   client mode (a router): admins 1-3, viewers 0-8 (lwIP has 16 TCP
//     connections: admins + viewers + netpack + page loads).
struct ConnLimits {
    uint8_t adminMax   = 2;
    uint8_t viewHost   = 3;
    uint8_t viewClient = 6;
};
constexpr uint8_t CONN_ADMIN_MAX = 3, CONN_VIEW_HOST_MAX = 5, CONN_VIEW_CLIENT_MAX = 8;
constexpr int     AP_MAX_STATIONS = 10;   // ESP32-S3 access point

struct ElrsSettings {
    bool     enabled   = false;
    uint8_t  transport = 1;       // 1 UART straight to the ESP-NOW bridge (default), 0 TCP (elrs-netpack XIAO)
    char     host[64]  = "auto";  // "auto" = find the netpack among AP clients
    uint16_t port      = ELRS_TCP_PORT;
    char     rdBind[33] = "";     // race director's goggles (optional)
    char     eventName[33] = "";
    osd::Options opt;
};

extern PilotRoster  roster[MAX_REGISTERED];
extern CalibConfig  rosterCal[MAX_REGISTERED];
extern int          rosterCount;
extern int          activePilots[MAX_ACTIVE];
extern Settings     settings;
extern RaceRules    raceRules;
extern ConnLimits   connLimits;
extern ElrsSettings elrsCfg;
// Race director's TX switch: 0 toggle (on = start, off = stop), 1 push
// (each press starts or stops). Its own NVS key, so ElrsSettings' stored
// layout stays the same.
extern uint8_t      elrsRdSwitch;
extern String       adminPassword;
extern String       apSsid, apPass;     // host mode: our access point
extern uint8_t      netMode;            // NetMode (wifi_mgr.h)
extern String       staSsid, staPass;   // client mode: network to join
extern uint8_t      netFallbackSec;     // client mode: seconds without a link before the AP starts (15/30/60)

extern race::Core   raceCore;
extern bool         sdPresent;
extern bool         sdPollEnabled;

extern AsyncWebServer server;
extern AsyncWebSocket ws;
extern DNSServer      dnsServer;

// One recursive mutex for roster, settings and race state: they are touched
// from loop(), the async web server task and the ELRS task.
extern SemaphoreHandle_t gLock;
struct Guard {
    Guard()  { xSemaphoreTakeRecursive(gLock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGiveRecursive(gLock); }
};

inline bool rosterValid(int ri) { return ri >= 0 && ri < rosterCount; }
