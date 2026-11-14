// WiFi: host (own access point) or client (joins an existing network).
//
//   * Nothing configured, or mode = host: start the access point.
//   * Mode = client: join the configured network. If it isn't joined within
//     netFallbackSec (15 / 30 / 60 s) of boot, or the link is lost that long
//     later on, fall back to the access point so the timer is always
//     reachable. While nobody is on it, the timer looks for the network and
//     restarts into client mode once it is back (not during a race).
#pragma once
#include <Arduino.h>
#include <IPAddress.h>

enum NetMode : uint8_t { NET_AP = 0, NET_STA = 1 };

// Client-mode wait before falling back to the access point.
constexpr uint8_t NET_FALLBACK_DEFAULT_S = 30;
inline bool netFallbackValid(int s) { return s == 15 || s == 30 || s == 60; }
// Client mode, the link lost during a race (or with laps not yet cleared):
// seconds to wait for the router before starting the access point. 0: wait
// as long as it takes (every phone is on the router).
constexpr uint16_t NET_RACE_HOLD_DEFAULT_S = 60;
inline bool netRaceHoldValid(int s) {
    return s == 0 || s == 15 || s == 30 || s == 60 || s == 180 || s == 300;
}

struct NetStatus {
    bool      ap;          // our access point is up
    bool      connected;   // joined the client network
    bool      fallback;    // client mode failed, running the access point instead
    IPAddress ip;          // address to open the UI on
    int       rssi;        // client mode signal, dBm
    uint8_t   reason;      // why joining failed (WiFi disconnect reason), 0: none
    bool      retrying;    // fallen back, looking for the network to go back to it
};

void      netBegin();
void      netLoop();       // call from loop()
bool      netIsAp();
IPAddress netIp();
NetStatus netStatus();
