#include <WiFi.h>
#include "wifi_mgr.h"
#include "data_model.h"

static const uint32_t kStaTimeoutMs = 60000;

static bool     apUp = false;
static bool     staMode = false;     // trying / running as a client
static bool     staJoined = false;
static bool     fellBack = false;
static uint32_t staSinceMs = 0;      // start of the current wait for a link

static void startAp() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    WiFi.softAP(apSsid.c_str(), apPass.c_str(), AP_CHANNEL);
    // Wildcard DNS for the captive portal.
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.setTTL(30);
    dnsServer.start(53, "*", AP_IP);
    apUp = true;
    staMode = false;
    staJoined = false;
    Serial.printf("[WiFi] AP  SSID=%s  IP=%s\n", apSsid.c_str(), AP_IP.toString().c_str());
}

void netBegin() {
    if (netMode == NET_STA && staSsid.length()) {
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(MDNS_NAME);
        WiFi.setAutoReconnect(true);
        WiFi.begin(staSsid.c_str(), staPass.c_str());
        staMode = true;
        staSinceMs = millis();
        Serial.printf("[WiFi] client: joining \"%s\" (AP fallback after %lus)\n", staSsid.c_str(),
                      (unsigned long)(kStaTimeoutMs / 1000));
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
            Serial.printf("[WiFi] client: joined \"%s\"  IP=%s  (http://%s.local)\n", staSsid.c_str(),
                          WiFi.localIP().toString().c_str(), MDNS_NAME);
        }
        staSinceMs = now;
        return;
    }
    if (staJoined) {
        staJoined = false;
        staSinceMs = now;
        Serial.println("[WiFi] client: link lost, reconnecting");
    }
    if (now - staSinceMs >= kStaTimeoutMs) {
        Serial.printf("[WiFi] client: no link to \"%s\" for %lus, starting the access point\n", staSsid.c_str(),
                      (unsigned long)(kStaTimeoutMs / 1000));
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
