#include "c5_link.h"
#include "c5host.h"
#include "config.h"
#include "gate_tracker.h"

namespace {

QueueHandle_t passQ;
portMUX_TYPE  mux = portMUX_INITIALIZER_UNLOCKED;

// Requested configuration, written by any task under mux, applied by the link task.
uint16_t wantMhz[8], wantEnter[8], wantExit[8];
int      wantGain = C5_DEFAULT_GAIN;
bool     cfgDirty = false, gainDirty = true, resetReq = false;

// Owned by the link task; read (racily but harmlessly) for telemetry.
gate::Tracker     trackers[8];
uint16_t          curMhz[8];
volatile uint32_t lastSampleMs[8];
volatile uint32_t counts[8];
volatile uint16_t rates[8];

void writeToC5(void*, const uint8_t* data, size_t len) { Serial1.write(data, len); }

void onSample(void*, const c5host::Sample& s) {
    if (s.slot >= 8) return;
    lastSampleMs[s.slot] = millis();
    counts[s.slot] = counts[s.slot] + 1;
    uint32_t passUs;
    uint16_t peak;
    if (!trackers[s.slot].push(s.rssi, s.timeUs, &passUs, &peak)) return;
    // The peak's time on the C5's clock, mapped by c5host onto micros();
    // move it onto millis() so the race logic works on one clock.
    uint32_t agoMs = (micros() - passUs) / 1000;
    C5Pass p = {s.slot, millis() - agoMs, peak};
    xQueueSend(passQ, &p, 0);   // never block the UART reader
}

c5host::Link c5(writeToC5, onSample, nullptr);

void applyConfig() {
    uint16_t mhz[8], en[8], ex[8];
    bool dirty, gDirty, reset;
    int gain;
    portENTER_CRITICAL(&mux);
    dirty = cfgDirty, gDirty = gainDirty, reset = resetReq, gain = wantGain;
    memcpy(mhz, wantMhz, sizeof mhz);
    memcpy(en, wantEnter, sizeof en);
    memcpy(ex, wantExit, sizeof ex);
    cfgDirty = gainDirty = resetReq = false;
    portEXIT_CRITICAL(&mux);

    if (dirty) {
        for (int i = 0; i < 8; i++) {
            trackers[i].setThresholds(en[i], ex[i]);
            if (mhz[i] != curMhz[i]) trackers[i].reset();
        }
        if (c5.setSlots(mhz)) memcpy(curMhz, mhz, sizeof curMhz);
        else Serial.println("[C5] slot list rejected (frequency out of 5180-5917 MHz)");
    }
    if (gDirty) c5.setGain(gain);
    if (reset)
        for (auto& t : trackers) t.reset();
}

void linkTask(void*) {
    uint8_t buf[256];
    uint32_t lastRate = millis();
    for (;;) {
        applyConfig();
        int avail = Serial1.available();
        while (avail > 0) {
            size_t got = Serial1.read(buf, avail < (int)sizeof buf ? avail : sizeof buf);
            if (!got) break;
            c5.feed(buf, got, micros());
            avail -= (int)got;
        }
        c5.tick(micros());
        uint32_t now = millis();
        if (now - lastRate >= 1000) {
            for (int i = 0; i < 8; i++) rates[i] = (uint16_t)counts[i], counts[i] = 0;
            lastRate = now;
        }
        vTaskDelay(1);
    }
}

}  // namespace

void c5Begin() {
    passQ = xQueueCreate(32, sizeof(C5Pass));
    for (int i = 0; i < 8; i++) wantEnter[i] = 600, wantExit[i] = 500;
    Serial1.setRxBufferSize(C5_RX_BUFFER);   // before begin()
    Serial1.begin(c5host::kBaud, SERIAL_8N1, C5_RX_PIN, C5_TX_PIN);
    c5.begin(micros());
    // Above loop()'s priority so a busy loop() can't overflow the UART buffer.
    xTaskCreatePinnedToCore(linkTask, "c5link", 4096, nullptr, 3, nullptr, 1);
    Serial.printf("[C5] UART1 %lu baud  TX=GPIO%d RX=GPIO%d\n", (unsigned long)c5host::kBaud, C5_TX_PIN, C5_RX_PIN);
}

void c5Configure(const uint16_t mhz[8], const uint16_t enter[8], const uint16_t exit[8]) {
    portENTER_CRITICAL(&mux);
    memcpy(wantMhz, mhz, sizeof wantMhz);
    memcpy(wantEnter, enter, sizeof wantEnter);
    memcpy(wantExit, exit, sizeof wantExit);
    cfgDirty = true;
    portEXIT_CRITICAL(&mux);
}

void c5SetGain(int gain) {
    portENTER_CRITICAL(&mux);
    wantGain = constrain(gain, 0, c5host::kMaxGain);
    gainDirty = true;
    portEXIT_CRITICAL(&mux);
}

void c5ResetDetectors() {
    portENTER_CRITICAL(&mux);
    resetReq = true;
    portEXIT_CRITICAL(&mux);
}

bool c5PopPass(C5Pass* out) { return xQueueReceive(passQ, out, 0) == pdTRUE; }

C5SlotLive c5Live(int slot) {
    C5SlotLive l;
    const gate::Tracker& t = trackers[slot];
    l.filtered = t.filtered();
    l.raw = t.raw();
    l.inside = t.inside();
    uint32_t last = lastSampleMs[slot];   // read before millis(): never "in the future"
    l.signal = curMhz[slot] && t.valid() && millis() - last < SIGNAL_LOST_MS;
    return l;
}

C5Status c5Status() {
    C5Status s;
    s.online = c5.online(micros());
    s.scanning = c5.scanning();
    snprintf(s.state, sizeof s.state, "%s", s.online ? c5.state() : "OFFLINE");
    s.firmware = c5.firmware();
    s.gain = c5.reportedGain();
    const c5host::Counters& n = c5.counters();
    s.records = n.records, s.seqGaps = n.seqGaps, s.badRecords = n.badRecords, s.badLines = n.badLines;
    for (int i = 0; i < 8; i++) s.rate[i] = rates[i];
    return s;
}
