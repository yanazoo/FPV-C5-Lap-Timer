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
volatile uint16_t enterCounts[8], exitCounts[8];
volatile uint16_t rates[8];

void writeToC5(void*, const uint8_t* data, size_t len) { Serial1.write(data, len); }

// RSSI recording for the race (to find a missed pass afterwards): per slot
// the highest filtered reading of every C5_REC_MS of race time (pauses left
// out, as the lap times), in PSRAM. Written by the link task, read by the
// web task; a sample is one 16-bit store, so a torn read can't happen.
uint16_t*         recBuf = nullptr;    // [8][recCap]
uint32_t          recCap = 0;          // samples per channel the buffer holds
volatile bool     recOn = false;
volatile uint32_t recStart = 0;        // millis() of race time 0 (moved past pauses)
volatile uint32_t recEnd = 0;          // race time recorded so far, ms

void record(uint8_t slot, uint16_t v) {
    if (!recOn || !recBuf) return;
    uint32_t t = millis() - recStart;
    uint32_t i = t / C5_REC_MS;
    if (i >= recCap) return;
    uint16_t& cell = recBuf[slot * recCap + i];
    if (v > cell) cell = v;
    if (t > recEnd) recEnd = t;
}

void onSample(void*, const c5host::Sample& s) {
    if (s.slot >= 8) return;
    lastSampleMs[s.slot] = millis();
    counts[s.slot] = counts[s.slot] + 1;
    uint32_t passUs;
    uint16_t peak;
    bool wasInside = trackers[s.slot].inside();
    bool passEnded = trackers[s.slot].push(s.rssi, s.timeUs, &passUs, &peak);
    record(s.slot, trackers[s.slot].filtered());
    bool isInside = trackers[s.slot].inside();
    if (!wasInside && isInside) enterCounts[s.slot] = enterCounts[s.slot] + 1;
    if (wasInside && !isInside) exitCounts[s.slot] = exitCounts[s.slot] + 1;
    if (!passEnded) return;
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
            for (int i = 0; i < 8; i++) {
                rates[i] = (uint16_t)counts[i];
                counts[i] = 0;
            }
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

void c5RecStart(uint32_t raceStartMs, bool fresh) {
    if (!recBuf) return;
    recOn = false;
    if (fresh) {
        memset(recBuf, 0, 8 * recCap * sizeof(uint16_t));
        recEnd = 0;
    }
    recStart = raceStartMs;
    recOn = true;
}

void c5RecStop() { recOn = false; }

uint32_t c5RecSize(uint32_t minutes) {
    uint32_t want = minutes * 60000u / C5_REC_MS;
    if (recBuf && want == recCap) return minutes;
    recOn = false;
    if (recBuf) heap_caps_free(recBuf), recBuf = nullptr, recCap = 0;
    recEnd = 0;
    // Shorter if PSRAM can't hold it (halving, down to 5 min).
    for (uint32_t cap = want; cap >= 5u * 60000u / C5_REC_MS; cap /= 2) {
        recBuf = (uint16_t*)heap_caps_calloc(8 * cap, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (recBuf) { recCap = cap; break; }
    }
    uint32_t got = recCap * C5_REC_MS / 60000u;
    if (!recBuf) Serial.println("[C5] no PSRAM: no RSSI recording for the race");
    else Serial.printf("[C5] RSSI record: %lu min (%lu KB of PSRAM, %lu KB free)\n", (unsigned long)got,
                       (unsigned long)(8 * recCap * 2 / 1024), (unsigned long)(ESP.getFreePsram() / 1024));
    return got;
}

uint32_t c5RecCapMs() { return recCap * C5_REC_MS; }

uint32_t c5RecMs() { return recBuf ? recEnd : 0; }

const uint16_t* c5RecData(int slot, int* n) {
    *n = 0;
    if (!recBuf || slot < 0 || slot >= 8) return nullptr;
    uint32_t k = recEnd / C5_REC_MS + 1;
    *n = (int)(k < recCap ? k : recCap);
    return recBuf + slot * recCap;
}

size_t c5RecRead(int slot, size_t from, uint8_t* out, size_t maxBytes) {
    if (!recBuf || slot < 0 || slot >= 8) return 0;
    size_t total = (size_t)(recEnd / C5_REC_MS + 1) * 2;
    if (total > recCap * 2) total = recCap * 2;
    if (from >= total) return 0;
    size_t n = total - from < maxBytes ? total - from : maxBytes;
    memcpy(out, (const uint8_t*)(recBuf + slot * recCap) + from, n);   // little-endian
    return n;
}

C5SlotLive c5Live(int slot) {
    C5SlotLive l;
    const gate::Tracker& t = trackers[slot];
    l.filtered = t.filtered();
    l.inside = t.inside();
    l.enters = enterCounts[slot];
    l.exits = exitCounts[slot];
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
