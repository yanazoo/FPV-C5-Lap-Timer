// WiFi: host (own access point) or client (joins an existing network).
//
//   * Nothing configured, or mode = host: start the access point.
//   * Mode = client: join the configured network. If it isn't joined within
//     netFallbackSec (15 / 30 / 60 s) of boot, or the link is lost that long
//     later on, fall back to the access point (until the next reboot) so the
//     timer is always reachable.
#pragma once
#include <Arduino.h>
#include <IPAddress.h>

enum NetMode : uint8_t { NET_AP = 0, NET_STA = 1 };

// Client-mode wait before falling back to the access point.
constexpr uint8_t NET_FALLBACK_DEFAULT_S = 30;
inline bool netFallbackValid(int s) { return s == 15 || s == 30 || s == 60; }

struct NetStatus {
    bool      ap;          // our access point is up
    bool      connected;   // joined the client network
    bool      fallback;    // client mode failed, running the access point instead
    IPAddress ip;          // address to open the UI on
    int       rssi;        // client mode signal, dBm
};

void      netBegin();
void      netLoop();       // call from loop()
bool      netIsAp();
IPAddress netIp();
NetStatus netStatus();
