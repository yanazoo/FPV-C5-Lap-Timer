#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include "elrs.h"
#include "data_model.h"
#include "msp_v2.h"
#include "wifi_mgr.h"

namespace {

enum class Ev : uint8_t { Config, Reconnect, Stage, Start, Lap, Position, Done, Stop, Clear, Announce, Test };

struct Event {
    Ev           kind;
    int8_t       slot;
    int16_t      position;
    int16_t      laps;
    uint32_t     value;
    osd::LapInfo lap;
    char         text[41];
};

QueueHandle_t evQ;
WiFiClient    client;
msp::Parser   parser;
bool          uartOpen = false;

// Snapshot of the settings the task works with (copied under gLock).
bool     enabled = false;
uint8_t  transport = 0;
char     host[64] = "auto";
uint16_t port = ELRS_TCP_PORT;

uint32_t  lastAttempt = 0;
IPAddress lastGood;

portMUX_TYPE stMux = portMUX_INITIALIZER_UNLOCKED;
ElrsStatus   st = {};

struct TransportOut : osd::Output {
    void write(const uint8_t* d, size_t n) override {
        if (!enabled) return;
        if (transport == 1) {
            if (uartOpen) Serial2.write(d, n);
        } else if (client.connected()) {
            client.write(d, n);
        } else {
            return;
        }
        portENTER_CRITICAL(&stMux);
        st.bytesSent += n;
        portEXIT_CRITICAL(&stMux);
    }
    // One recipient's burst: let the bridge switch UID (it re-inits ESP-NOW
    // on a new UID) before the next one arrives.
    void burstDone() override { vTaskDelay(pdMS_TO_TICKS(15)); }
};

TransportOut out;
osd::Engine  engine(out);

void setStatus(bool connected, const char* peer) {
    portENTER_CRITICAL(&stMux);
    st.connected = connected;
    snprintf(st.peer, sizeof st.peer, "%s", peer);
    if (!connected) st.version[0] = '\0';
    portEXIT_CRITICAL(&stMux);
}

void post(const Event& e) {
    if (evQ) xQueueSend(evQ, &e, 0);
}

Event ev(Ev k, int slot = -1) {
    Event e = {};
    e.kind = k;
    e.slot = (int8_t)slot;
    return e;
}

void refreshConfig() {
    Guard g;
    enabled = elrsCfg.enabled;
    transport = elrsCfg.transport;
    snprintf(host, sizeof host, "%s", elrsCfg.host);
    port = elrsCfg.port ? elrsCfg.port : ELRS_TCP_PORT;
    engine.options() = elrsCfg.opt;
    uint8_t uid[6];
    for (int s = 0; s < MAX_ACTIVE; s++) {
        int ri = activePilots[s];
        bool on = enabled && rosterValid(ri) && roster[ri].osd;
        if (on) msp::bindPhraseUid(roster[ri].bind[0] ? roster[ri].bind : roster[ri].name, uid);
        engine.setRecipient(s, on, uid);
    }
    bool rd = enabled && elrsCfg.rdBind[0];
    if (rd) msp::bindPhraseUid(elrsCfg.rdBind, uid);
    engine.setRecipient(osd::kRd, rd, uid);
}

void disconnect() {
    if (client.connected()) client.stop();
    if (uartOpen) Serial2.end(), uartOpen = false;
    setStatus(false, "");
}

bool tryConnect(IPAddress ip) {
    if (!client.connect(ip, port, 400)) return false;
    client.setNoDelay(true);
    lastGood = ip;
    char peer[40];
    snprintf(peer, sizeof peer, "%s:%u", ip.toString().c_str(), port);
    setStatus(true, peer);
    Serial.printf("[ELRS] connected to elrs-netpack %s\n", peer);
    uint8_t f[msp::kMaxFrame];
    size_t n = msp::encode(msp::kElrsGetBackpackVersion, nullptr, 0, f, sizeof f);
    client.write(f, n);
    return true;
}

// "auto": the netpack's XIAO is one of this AP's DHCP clients; try each on
// the netpack port (phones refuse at once). The last good address first.
bool connectAuto() {
    if (!(lastGood == IPAddress()) && tryConnect(lastGood)) return true;
    if (!netIsAp()) {
        // Client mode: the netpack is on the same network; it advertises mDNS.
        IPAddress ip = MDNS.queryHost("elrs-netpack", 1000);
        return !(ip == IPAddress()) && tryConnect(ip);
    }
    wifi_sta_list_t sl;
    if (esp_wifi_ap_get_sta_list(&sl) != ESP_OK || sl.num == 0) return false;
    esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM] = {};
    int n = sl.num < ESP_WIFI_MAX_CONN_NUM ? sl.num : ESP_WIFI_MAX_CONN_NUM;
    for (int i = 0; i < n; i++) memcpy(pairs[i].mac, sl.sta[i].mac, 6);
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap || esp_netif_dhcps_get_clients_by_mac(ap, n, pairs) != ESP_OK) return false;
    for (int i = 0; i < n; i++) {
        IPAddress ip(pairs[i].ip.addr);
        if (ip == IPAddress() || ip == lastGood) continue;
        if (tryConnect(ip)) return true;
    }
    return false;
}

bool connectHost() {
    IPAddress ip;
    if (ip.fromString(host)) return tryConnect(ip);
    String name(host);
    if (name.endsWith(".local")) name.remove(name.length() - 6);
    ip = MDNS.queryHost(name, 1000);
    if (ip == IPAddress() && WiFi.hostByName(host, ip) != 1) return false;
    return tryConnect(ip);
}

void serviceTransport(uint32_t now) {
    if (!enabled) {
        if (st.connected || uartOpen) disconnect();
        return;
    }
    if (transport == 1) {   // UART straight to the netpack ESP-NOW bridge
        if (client.connected()) client.stop();
        if (!uartOpen) {
            Serial2.setRxBufferSize(1024);
            Serial2.begin(ELRS_UART_BAUD, SERIAL_8N1, ELRS_UART_RX_PIN, ELRS_UART_TX_PIN);
            uartOpen = true;
            setStatus(true, "UART");
        }
        while (Serial2.available()) parser.feed((uint8_t)Serial2.read());
        return;
    }
    if (uartOpen) Serial2.end(), uartOpen = false;
    if (!client.connected()) {
        if (st.connected) {
            Serial.println("[ELRS] netpack connection lost");
            client.stop();
            setStatus(false, "");
        }
        if (now - lastAttempt < 5000) return;
        lastAttempt = now;
        if (strcmp(host, "auto") == 0 || !host[0]) connectAuto();
        else connectHost();
        return;
    }
    while (client.available()) {
        if (!parser.feed((uint8_t)client.read())) continue;
        if (parser.function() == msp::kElrsGetBackpackVersion && parser.size() >= 2) {
            portENTER_CRITICAL(&stMux);
            snprintf(st.version, sizeof st.version, "%u.%u", parser.payload()[0], parser.payload()[1]);
            portEXIT_CRITICAL(&stMux);
        }
    }
}

void handle(const Event& e, uint32_t now) {
    switch (e.kind) {
    case Ev::Config:    refreshConfig(); break;
    case Ev::Reconnect: refreshConfig(); disconnect(); lastAttempt = now - 5000; break;
    case Ev::Stage:     engine.onRaceStage(now, e.text); break;
    case Ev::Start:     engine.onRaceStart(now, e.value); break;
    case Ev::Lap:       engine.onLap(now, e.slot, e.lap); break;
    case Ev::Position:  engine.onPosition(e.slot, e.laps, e.position); break;
    case Ev::Done:      engine.onPilotDone(now, e.slot, e.position, e.value); break;
    case Ev::Stop:      engine.onRaceStop(now); break;
    case Ev::Clear:     engine.onLapsClear(now); break;
    case Ev::Announce:  engine.onAnnounce(now, e.text); break;
    case Ev::Test:      engine.test(now, e.slot); break;
    }
}

void elrsTask(void*) {
    refreshConfig();
    for (;;) {
        serviceTransport(millis());
        // Sleep until an event arrives (a lap goes out at once) or 20 ms
        // pass (race clock, delayed clears, transport).
        Event e;
        if (xQueueReceive(evQ, &e, pdMS_TO_TICKS(20)) == pdTRUE) {
            do handle(e, millis());
            while (xQueueReceive(evQ, &e, 0) == pdTRUE);
        }
        engine.tick(millis());
    }
}

}  // namespace

void elrsBegin() {
    evQ = xQueueCreate(48, sizeof(Event));
    xTaskCreatePinnedToCore(elrsTask, "elrs", 6144, nullptr, 2, nullptr, 0);
}

void elrsConfigChanged() { post(ev(Ev::Config)); }
void elrsReconnect()     { post(ev(Ev::Reconnect)); }

void elrsRaceStage() {
    Event e = ev(Ev::Stage);
    {
        Guard g;
        snprintf(e.text, sizeof e.text, "%s", elrsCfg.eventName);
    }
    post(e);
}

void elrsRaceStart(uint32_t elapsedMs) {
    Event e = ev(Ev::Start);
    e.value = elapsedMs;
    post(e);
}

void elrsLap(int slot, const osd::LapInfo& lap) {
    Event e = ev(Ev::Lap, slot);
    e.lap = lap;
    post(e);
}

void elrsPosition(int slot, int laps, int position) {
    Event e = ev(Ev::Position, slot);
    e.laps = (int16_t)laps;
    e.position = (int16_t)position;
    post(e);
}

void elrsPilotDone(int slot, int position, uint32_t totalMs) {
    Event e = ev(Ev::Done, slot);
    e.position = (int16_t)position;
    e.value = totalMs;
    post(e);
}

void elrsRaceStop()  { post(ev(Ev::Stop)); }
void elrsLapsClear() { post(ev(Ev::Clear)); }

void elrsAnnounce(const char* text) {
    Event e = ev(Ev::Announce);
    snprintf(e.text, sizeof e.text, "%s", text);
    post(e);
}

void elrsTest(int slot) { post(ev(Ev::Test, slot)); }

ElrsStatus elrsStatus() {
    portENTER_CRITICAL(&stMux);
    ElrsStatus s = st;
    portEXIT_CRITICAL(&stMux);
    return s;
}
