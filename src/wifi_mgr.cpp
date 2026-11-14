#include <WiFi.h>
#include <esp_system.h>
#include "wifi_mgr.h"
#include "data_model.h"
#include "race.h"
#include "sys_diag.h"

// Set while a client-mode attempt hasn't joined yet; survives a crash reset
// (not a power cycle). If the timer crashes during the attempt or the switch
// to the access point, the next boot goes straight to the access point
// instead of trying the same network and crashing again.
static const uint32_t kStaTrying = 0x57A7C11E;
static RTC_NOINIT_ATTR uint32_t rtcStaTrying;

// Back to the client network after a fallback. The access point stays up
// until the timer restarts, so a fallback used to last until someone power
// cycled it: a router that took longer to start than the timer (both
// switched on together at the field), or that was off for a while, left the
// timer on its own access point for good. Now, while nobody is on the
// access point, it looks for the network now and then, and restarts into
// client mode once it is there. Not after a crash while joining, not when
// the network refused the password (seeing it again changes nothing), not
// during a race or with laps not yet cleared, and at most kProbeRestarts
// times in a row (a network that is there but can't be joined).
struct ProbeState {
    uint32_t magic;
    uint8_t  restarts;   // restarts for a network that was seen, since the last join
};
static const uint32_t kProbeMagic = 0x50524F42;
static const uint8_t  kProbeRestarts = 3;
static RTC_NOINIT_ATTR ProbeState rtcProbe;   // survives the restart (deep sleep), not a power cycle
static bool     probing = false;
static uint32_t probeAt = 0;
static uint8_t  fallbackReason = 0;   // the disconnect reason that made it fall back
static bool     crashFallback = false;   // fell back because joining crashed the timer

// Why the last attempt to join failed (WiFi disconnect reason, 0: none yet).
static volatile uint8_t lastReason = 0;
static uint8_t loggedReason = 0;

static void onStaDisconnected(arduino_event_id_t, arduino_event_info_t info) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    lastReason = r ? r : WIFI_REASON_UNSPECIFIED;
}

static const char* reasonText(uint8_t r) {
    switch (r) {
        case WIFI_REASON_NO_AP_FOUND:                   return "network not found";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "network found, security not supported";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:                   return "wrong password?";
        case WIFI_REASON_BEACON_TIMEOUT:                return "signal lost";
        case WIFI_REASON_ASSOC_TOOMANY:                 return "the router has too many devices";
        default:                                        return "";
    }
}

// The network turned the password down: finding it again won't help.
static bool passwordRefused(uint8_t r) {
    return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_MIC_FAILURE;
}

static bool     apUp = false;
static bool     staMode = false;     // trying / running as a client
static bool     staJoined = false;
static bool     everJoined = false;  // joined the network since this boot
static bool     waitRacing = false;  // the current wait for the link is a race's (netRaceHoldSec)
static bool     fellBack = false;
static uint32_t staSinceMs = 0;      // start of the current wait for a link

static void startAp() {
    if (staMode) {
        // Leave client mode without turning the radio off: WiFi.disconnect(true)
        // stops WiFi entirely, and arduino-esp32 3.x then deinitialises it and
        // destroys the network interfaces that mDNS and the ELRS task (on the
        // other core) still use, so the fallback crashed instead of starting
        // the access point. STA -> AP is a plain mode change.
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false);
    }
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    // All 10 stations: browsers are limited on the WebSocket (ws_handler),
    // and the netpack XIAO must always find a place (wsLoop frees one).
    ok = WiFi.softAP(apSsid.c_str(), apPass.c_str(), AP_CHANNEL, 0, AP_MAX_STATIONS) && ok;
    if (!ok) Serial.println("[WiFi] AP start failed");
    // Wildcard DNS for the captive portal.
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.setTTL(30);
    dnsServer.start(53, "*", AP_IP);
    apUp = true;
    staMode = false;
    staJoined = false;
    rtcStaTrying = 0;
    Serial.printf("[WiFi] AP  SSID=%s  IP=%s\n", apSsid.c_str(), AP_IP.toString().c_str());
}

static bool crashedWhileTrying() {
    esp_reset_reason_t r = esp_reset_reason();
    bool crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    return crash && rtcStaTrying == kStaTrying;
}

void netBegin() {
    // Settings live in our own NVS keys; don't let the WiFi driver write them too.
    WiFi.persistent(false);
    if (netMode == NET_STA && staSsid.length()) {
        if (rtcProbe.magic != kProbeMagic) rtcProbe = {kProbeMagic, 0};   // power on
        if (crashedWhileTrying()) {
            Serial.println("[WiFi] client: crashed while joining last time, starting the access point");
            crashFallback = true;   // don't go back to it by itself (it would crash again)
            fellBack = true;
            startAp();
            return;
        }
        WiFi.onEvent(onStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        WiFi.setHostname(MDNS_NAME);   // before the STA interface comes up
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        // Look on every channel and take the strongest access point of the
        // network: the library's default joins the first one it hears, the
        // far one of a mesh or a repeater as often as the near one.
        WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
        WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
        // Older routers (WPA only, or WPA/WPA2 set to WPA): the library
        // refuses anything below WPA2 by default, and the network then
        // looks missing.
        WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
        if (staIp.fixed) {
            IPAddress ip(staIp.ip), gw(staIp.gw), mask(staIp.mask), dns(staIp.dns ? staIp.dns : staIp.gw);
            if (!WiFi.config(ip, gw, mask, dns)) Serial.println("[WiFi] fixed address rejected, using DHCP");
            else Serial.printf("[WiFi] client: fixed address %s\n", ip.toString().c_str());
        }
        WiFi.begin(staSsid.c_str(), staPass.c_str());
        // No modem sleep as a client: with it the radio dozes between beacons,
        // and the burst of requests for the web UI's page and scripts stalls
        // or drops (the page shows, but its scripts don't load). The access
        // point never sleeps, which is why host mode was fine.
        WiFi.setSleep(false);
        staMode = true;
        staSinceMs = millis();
        rtcStaTrying = kStaTrying;
        Serial.printf("[WiFi] client: joining \"%s\" (AP fallback after %us)\n", staSsid.c_str(), netFallbackSec);
    } else {
        startAp();
    }
}

// A race is on or not yet cleared: a restart would lose it.
static bool raceKeepsUs() {
    if (raceActive()) return true;
    Guard g;
    return raceCore.running() || raceCore.paused() || raceCore.lapCount() > 0;
}

// Fallen back to the access point: look for the client network now and
// then (see ProbeState), and restart into client mode once it is there.
static void probeForNetwork(uint32_t now) {
    if (!fellBack || netMode != NET_STA || !staSsid.length()) return;
    if (crashFallback || passwordRefused(fallbackReason) || rtcProbe.restarts >= kProbeRestarts) return;
    if (probing) {
        int16_t n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) return;
        probing = false;
        bool seen = false;
        for (int i = 0; i < n; i++)
            if (WiFi.SSID(i) == staSsid) seen = true;
        WiFi.scanDelete();
        if (!seen) return;
        if (raceKeepsUs() || WiFi.softAPgetStationNum() > 0) return;   // next look
        rtcProbe.restarts++;
        Serial.printf("[WiFi] \"%s\" is there again: restarting into client mode\n", staSsid.c_str());
        sysRequestRestart(200);
        return;
    }
    // Every 30 s, a little longer after each restart that didn't join.
    uint32_t every = 30000u << rtcProbe.restarts;
    if (now - probeAt < every) return;
    probeAt = now;
    // Somebody is on the access point: a scan would take its channel away
    // for a moment, and the restart would cut them off.
    if (WiFi.softAPgetStationNum() > 0 || raceKeepsUs()) return;
    if (WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING) probing = true;
}

void netLoop() {
    uint8_t r = lastReason;
    if (r != loggedReason) {
        loggedReason = r;
        Serial.printf("[WiFi] client: not joined, reason %u %s\n", r, reasonText(r));
    }
    if (apUp) {
        dnsServer.processNextRequest();
        probeForNetwork(millis());
        return;
    }
    if (!staMode) return;
    uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED) {
        if (!staJoined) {
            staJoined = true;
            everJoined = true;
            rtcStaTrying = 0;
            rtcProbe.restarts = 0;
            lastReason = loggedReason = 0;
            Serial.printf("[WiFi] client: joined \"%s\"  IP=%s  (http://%s.local)\n", staSsid.c_str(),
                          WiFi.localIP().toString().c_str(), MDNS_NAME);
        }
        staSinceMs = now;
        return;
    }
    if (staJoined) {
        staJoined = false;
        staSinceMs = now;
        rtcStaTrying = kStaTrying;
        Serial.println("[WiFi] client: link lost, reconnecting");
    }
    // Link lost during a race (or with laps not yet cleared): wait longer
    // for the router, netRaceHoldSec (0: as long as it takes). Every phone
    // is on the router, so the access point would reach nobody at first,
    // and the timer can't go back to the router until the race is cleared;
    // the router coming back brings everyone back at once. The wait starts
    // afresh when a race starts or is cleared during the outage.
    bool racing = everJoined && raceKeepsUs();
    if (racing != waitRacing) {
        waitRacing = racing;
        staSinceMs = now;
    }
    uint32_t waitS = racing ? netRaceHoldSec : netFallbackSec;
    if (racing && !waitS) {
        staSinceMs = now;
        return;
    }
    if (now - staSinceMs >= waitS * 1000UL) {
        Serial.printf("[WiFi] client: no link to \"%s\" for %lus%s, starting the access point\n", staSsid.c_str(),
                      (unsigned long)waitS, racing ? " (race on)" : "");
        fellBack = true;
        fallbackReason = lastReason;
        startAp();
        probeAt = now;
    }
}

bool netIsAp() { return apUp; }

IPAddress netIp() { return apUp ? AP_IP : WiFi.localIP(); }

NetStatus netStatus() {
    NetStatus s;
    s.ap = apUp;
    s.connected = staMode && staJoined;
    s.fallback = fellBack;
    s.ip = netIp();
    s.rssi = s.connected ? WiFi.RSSI() : 0;
    s.reason = s.connected ? 0 : (fellBack ? fallbackReason : lastReason);
    s.retrying = fellBack && !crashFallback && netMode == NET_STA && staSsid.length() && !passwordRefused(fallbackReason) &&
                 rtcProbe.restarts < kProbeRestarts;
    return s;
}
