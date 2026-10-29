#include <WiFi.h>
#include <esp_system.h>
#include "wifi_mgr.h"
#include "data_model.h"

// Set while a client-mode attempt hasn't joined yet; survives a crash reset
// (not a power cycle). If the timer crashes during the attempt or the switch
// to the access point, the next boot goes straight to the access point
// instead of trying the same network and crashing again.
static const uint32_t kStaTrying = 0x57A7C11E;
static RTC_NOINIT_ATTR uint32_t rtcStaTrying;

static bool     apUp = false;
static bool     staMode = false;     // trying / running as a client
static bool     staJoined = false;
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
    ok = WiFi.softAP(apSsid.c_str(), apPass.c_str(), AP_CHANNEL) && ok;
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
        if (crashedWhileTrying()) {
            Serial.println("[WiFi] client: crashed while joining last time, starting the access point");
            fellBack = true;
            startAp();
            return;
        }
        WiFi.setHostname(MDNS_NAME);   // before the STA interface comes up
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(staSsid.c_str(), staPass.c_str());
        staMode = true;
        staSinceMs = millis();
        rtcStaTrying = kStaTrying;
        Serial.printf("[WiFi] client: joining \"%s\" (AP fallback after %us)\n", staSsid.c_str(), netFallbackSec);
    } else {
        startAp();
    }
}

void netLoop() {
    if (apUp) {
        dnsServer.processNextRequest();
        return;
    }
    if (!staMode) return;
    uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED) {
        if (!staJoined) {
            staJoined = true;
            rtcStaTrying = 0;
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
    if (now - staSinceMs >= netFallbackSec * 1000UL) {
        Serial.printf("[WiFi] client: no link to \"%s\" for %us, starting the access point\n", staSsid.c_str(),
                      netFallbackSec);
        fellBack = true;
        startAp();
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
    return s;
}
