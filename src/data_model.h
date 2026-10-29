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

struct ElrsSettings {
    bool     enabled   = false;
    uint8_t  transport = 0;       // 0 TCP (elrs-netpack XIAO), 1 UART (netpack Wrover direct)
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
extern ElrsSettings elrsCfg;
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
