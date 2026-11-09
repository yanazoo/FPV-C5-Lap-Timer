#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include "elrs.h"
#include "data_model.h"
#include "msp_v2.h"
#include "wifi_mgr.h"
#include "race.h"

namespace {

enum class Ev : uint8_t { Config, Reconnect, Stage, Start, Lap, Position, Done, Stop, Clear, Announce, Test, LapEdit };

struct Event {
    Ev           kind;
    int8_t       slot;
    int16_t      position;
    int16_t      laps;
    uint32_t     value;
    uint32_t     at;      // millis() when posted: the OSD clock is set from
                          // this, not from when the task got round to it
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
volatile uint32_t netpackAddr = 0;   // lastGood, for the web task

// Race director: the bridge only hears what is sent to the MAC it has at
// the moment, and a TX backpack sends its DVR-record switch to the
// bind-phrase UID. So once a round of OSD bursts is done, the bridge is
// switched to the race director's UID and left there while idle, where it
// can hear the RD's switch (the way a RotorHazard timer's own backpack,
// bound to the RD's phrase, does).
bool    rdValid = false;
uint8_t rdUid[6];
bool    rdListenDue = false;   // OSD went out since the bridge last listened
bool    rdListening = false;   // the bridge is on the RD's UID right now
uint32_t rdListenAt = 0;       // when the bridge was last pointed at the RD
// The bridge said it passes on only frames from the RD's UID (MSP
// kBridgeSetListen answered). Then a switch frame is the RD's whenever it
// arrives: over netpack it can take tens of ms to come back, by which time
// the next OSD burst has already moved the bridge to a pilot's UID.
bool    bridgeFilter = false;
bool     listenCfgDue = false;
uint32_t listenCfgAt = 0;
const uint32_t RD_RELISTEN_MS = 2000;   // idle: point the bridge at the RD again
const uint32_t LISTEN_CFG_MS  = 5000;   // resend the filter (a rebooted bridge)
uint32_t uartParkAt = 0;
bool     uartMode = false;   // the UART bridge is the selected transport

portMUX_TYPE stMux = portMUX_INITIALIZER_UNLOCKED;
ElrsStatus   st = {};

// Send over TCP without giving up the connection when lwIP is short of
// buffers. WiFiClient::write() closes the socket on any error but EAGAIN,
// and ENOMEM (the send queue full of small segments) is what a burst of OSD
// frames used to hit during a race: the netpack connection dropped and came
// back every few seconds. Here a full queue is waited out (up to 1 s, then
// the rest of this burst is dropped and the connection kept).
bool tcpSend(const uint8_t* d, size_t n) {
    int fd = client.fd();
    if (fd < 0) return false;
    uint32_t start = millis();
    while (n) {
        int r = send(fd, d, n, MSG_DONTWAIT);
        if (r > 0) {
            d += r;
            n -= (size_t)r;
            continue;
        }
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOMEM) {
            Serial.printf("[ELRS] netpack send error %d\n", errno);
            client.stop();
            return false;
        }
        if (millis() - start > 1000) return false;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return true;
}

// Frames are collected and sent once per burst: one TCP segment instead of
// a dozen tiny ones (or one UART write).
struct TransportOut : osd::Output {
    uint8_t buf[1536];
    size_t  len = 0;

    void write(const uint8_t* d, size_t n) override {
        if (!enabled) return;
        rdListenDue = true;
        rdListening = false;
        if (len + n > sizeof buf) flush();
        if (n > sizeof buf) return;
        memcpy(buf + len, d, n);
        len += n;
    }
    void flush() {
        if (!len) return;
        size_t n = len;
        len = 0;
        if (transport == 1) {
            if (!uartOpen) return;
            Serial2.write(buf, n);
        } else if (!client.connected() || !tcpSend(buf, n)) {
            return;
        }
        portENTER_CRITICAL(&stMux);
        st.bytesSent += n;
        portEXIT_CRITICAL(&stMux);
    }
    // One recipient's burst: send it, and let the bridge switch UID (it
    // re-inits ESP-NOW on a new UID) before the next one arrives.
    void burstDone() override {
        flush();
        vTaskDelay(pdMS_TO_TICKS(15));
    }
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
    e.at = millis();
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
    rdValid = rd;
    if (rd) memcpy(rdUid, uid, 6);
    rdListenDue = rd;
    listenCfgDue = true;   // the RD's UID may have changed
}

// Point the bridge at the race director's UID (see rdValid). Also again
// every RD_RELISTEN_MS while idle: a bridge that was rebooted or powered up
// after the timer would otherwise stay deaf to the switch until the next OSD.
// And tell the bridge to pass on only frames from the RD (kBridgeSetListen).
void listenForRd(uint32_t now) {
    if (!st.connected) return;
    if (listenCfgDue || now - listenCfgAt >= LISTEN_CFG_MS) {
        uint8_t f[msp::kMaxFrame];
        size_t n = msp::encode(msp::kBridgeSetListen, rdUid, rdValid ? 6 : 0, f, sizeof f);
        out.write(f, n);
        out.flush();
        listenCfgDue = false;
        listenCfgAt = now;
        rdListenDue = rdValid;   // write() cleared listening; point it back
    }
    if (rdValid && rdListening && now - rdListenAt >= RD_RELISTEN_MS) rdListenDue = true;
    if (!rdValid || !rdListenDue) return;
    uint8_t p[7] = {0x01};
    memcpy(p + 1, rdUid, 6);
    uint8_t f[msp::kMaxFrame];
    size_t n = msp::encode(msp::kElrsSetSendUid, p, sizeof p, f, sizeof f);
    out.write(f, n);
    out.flush();
    rdListenDue = false;
    rdListening = true;
    rdListenAt = now;
}

// A bridge wired to the UART while the netpack (WiFi) is selected: park it,
// so it doesn't sit on the RD's or a pilot's UID next to the netpack's own
// bridge, and passes nothing on (it would hear every broadcast OSD). Every
// LISTEN_CFG_MS, in case it reboots. Its output is read and dropped.
const uint8_t kParkUid[6] = {0x02, 0x00, 0x46, 0x43, 0x35, 0x4C};   // locally administered
void parkUartBridge(uint32_t now) {
    if (!uartOpen) {
        Serial2.setRxBufferSize(1024);
        Serial2.begin(ELRS_UART_BAUD, SERIAL_8N1, ELRS_UART_RX_PIN, ELRS_UART_TX_PIN);
        uartOpen = true;
        uartParkAt = now - LISTEN_CFG_MS;
    }
    while (Serial2.available()) Serial2.read();
    if (now - uartParkAt < LISTEN_CFG_MS) return;
    uartParkAt = now;
    uint8_t f[msp::kMaxFrame];
    size_t n = msp::encode(msp::kBridgeSetListen, kParkUid, 6, f, sizeof f);
    Serial2.write(f, n);
    uint8_t p[7] = {0x01};
    memcpy(p + 1, kParkUid, 6);
    n = msp::encode(msp::kElrsSetSendUid, p, sizeof p, f, sizeof f);
    Serial2.write(f, n);
}

// A frame from the bridge / netpack: the backpack version, or the race
// director's TX switch.
void onFrame() {
    if (parser.function() == msp::kElrsGetBackpackVersion && parser.size() >= 2) {
        portENTER_CRITICAL(&stMux);
        snprintf(st.version, sizeof st.version, "%u.%u", parser.payload()[0], parser.payload()[1]);
        portEXIT_CRITICAL(&stMux);
    } else if (parser.function() == msp::kBridgeSetListen) {
        bridgeFilter = parser.size() >= 1 && parser.payload()[0] == 1;
        Serial.printf("[ELRS] bridge listen filter %s\n", bridgeFilter ? "on" : "off");
    } else if (parser.function() == msp::kElrsBackpackSetRecordingState && parser.size() >= 1) {
        // A filtering bridge passes on only the RD's frames. An older one
        // passes on everything it hears: then only while it is on the RD's
        // UID, since on a pilot's UID it would hear that pilot's DVR switch.
        if (rdValid && (bridgeFilter || rdListening)) raceDirectorSwitch(parser.payload()[0]);
    }
}

void disconnect() {
    if (client.connected()) client.stop();
    if (uartOpen) Serial2.end(), uartOpen = false;
    uartMode = false;
    bridgeFilter = false;
    setStatus(false, "");
}

bool tryConnect(IPAddress ip) {
    if (!client.connect(ip, port, 400)) return false;
    client.setNoDelay(true);
    // TCP keepalive (5 s idle, 3 probes 2 s apart): if the netpack reboots
    // or drops off WiFi between races, notice within ~11 s and reconnect,
    // instead of holding a dead connection until the next OSD write.
    int fd = client.fd(), on = 1, idle = 5, intvl = 2, cnt = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    lastGood = ip;
    netpackAddr = (uint32_t)ip;
    char peer[40];
    snprintf(peer, sizeof peer, "%s:%u", ip.toString().c_str(), port);
    setStatus(true, peer);
    rdListenDue = rdValid;
    listenCfgDue = true;
    bridgeFilter = false;
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
        }
        if (!uartMode) {   // just opened, or switched over from the netpack
            uartMode = true;
            setStatus(true, "UART");
            rdListenDue = rdValid;
            listenCfgDue = true;
            bridgeFilter = false;
            out.len = 0;
        }
        while (Serial2.available())
            if (parser.feed((uint8_t)Serial2.read())) onFrame();
        return;
    }
    if (uartMode) {   // switched over from the UART bridge: park it now
        uartMode = false;
        setStatus(false, "");
        uartParkAt = now - LISTEN_CFG_MS;
    }
    parkUartBridge(now);
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
    while (client.available())
        if (parser.feed((uint8_t)client.read())) onFrame();
}

void handle(const Event& e, uint32_t now) {
    switch (e.kind) {
    case Ev::Config:    refreshConfig(); break;
    case Ev::Reconnect: refreshConfig(); disconnect(); lastAttempt = now - 5000; break;
    // Stage: 3-2-1 on the race clock row, with the browsers' countdown.
    case Ev::Stage:     engine.onRaceStage(now, e.text, 3); break;
    // The race has been running since e.at: start the clock that far in.
    case Ev::Start:     engine.onRaceStart(now, e.value + (now - e.at), (uint16_t)e.laps); break;
    case Ev::Lap:       engine.onLap(now, e.slot, e.lap); break;
    case Ev::Position:  engine.onPosition(now, e.slot, e.laps, e.position); break;
    case Ev::Done:      engine.onPilotDone(now, e.slot, e.position, e.value); break;
    case Ev::Stop:      engine.onRaceStop(now, (int32_t)e.value); break;
    case Ev::Clear:     engine.onLapsClear(now); break;
    case Ev::Announce:  engine.onAnnounce(now, e.text); break;
    case Ev::Test:      engine.test(now, e.slot); break;
    case Ev::LapEdit:   engine.onLapEdit(now, e.slot, e.laps, e.position, e.value, e.lap.holeshot); break;
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
        listenForRd(millis());
        out.flush();
    }
}

}  // namespace

void elrsBegin() {
    evQ = xQueueCreate(48, sizeof(Event));
    xTaskCreatePinnedToCore(elrsTask, "elrs", 6144, nullptr, 2, nullptr, 0);
}

void elrsConfigChanged() { post(ev(Ev::Config)); }

bool elrsWantsNetpack() { return enabled && transport == 0 && !elrsStatus().connected; }
uint32_t elrsNetpackIp() { return netpackAddr; }
void elrsReconnect()     { post(ev(Ev::Reconnect)); }

void elrsRaceStage() {
    Event e = ev(Ev::Stage);
    {
        Guard g;
        snprintf(e.text, sizeof e.text, "%s", elrsCfg.eventName);
    }
    post(e);
}

void elrsRaceStart(uint32_t elapsedMs, uint32_t countdownSec) {
    Event e = ev(Ev::Start);
    e.value = elapsedMs;
    e.laps = (int16_t)countdownSec;   // timed race length (<= 1800 s)
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

void elrsRaceStop(uint32_t elapsedMs) {
    Event e = ev(Ev::Stop);
    e.value = elapsedMs;
    post(e);
}
void elrsLapsClear() { post(ev(Ev::Clear)); }

void elrsAnnounce(const char* text) {
    Event e = ev(Ev::Announce);
    snprintf(e.text, sizeof e.text, "%s", text);
    post(e);
}

void elrsTest(int slot) { post(ev(Ev::Test, slot)); }

void elrsLapEdit(int slot, int laps, int position, uint32_t bestMs, bool finished) {
    Event e = ev(Ev::LapEdit, slot);
    e.laps = (int16_t)laps;
    e.position = (int16_t)position;
    e.value = bestMs;
    e.lap.holeshot = finished;   // "still finished"
    post(e);
}

ElrsStatus elrsStatus() {
    portENTER_CRITICAL(&stMux);
    ElrsStatus s = st;
    portEXIT_CRITICAL(&stMux);
    return s;
}
