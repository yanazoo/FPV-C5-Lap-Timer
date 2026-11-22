// FPVGate C5MK receiver on UART1: c5host::Link (from the FPVGateC5MK
// submodule) in its own task, plus one gate::Tracker per slot.
//
// Detection runs on every reading (~1 kHz per pilot) in the link task;
// loop() collects finished passes with c5PopPass().
#pragma once
#include <Arduino.h>

struct C5Pass {
    uint8_t  slot;
    uint32_t tsMs;    // peak time on millis()
    uint16_t peak;
};

struct C5Status {
    bool     online;
    bool     scanning;
    char     state[12];
    int      firmware;
    int      gain;
    uint32_t records, seqGaps, badRecords, badLines;
    uint16_t rate[8];   // readings per second per slot
};

struct C5SlotLive {
    uint16_t filtered;
    bool     inside;
    bool     signal;    // readings arriving for this slot
    uint16_t enters;    // gate entries / exits seen so far (wrap around):
    uint16_t exits;     // the web UI plays its tones from these, so a
                        // crossing shorter than the telemetry interval
                        // still beeps
};

void c5Begin();
// Frequencies (0 = off) and thresholds per slot. Thread safe; applied by
// the link task. A slot whose frequency changes restarts its detector.
void c5Configure(const uint16_t mhz[8], const uint16_t enter[8], const uint16_t exit[8]);
void c5SetGain(int gain);
void c5ResetDetectors();             // race start
bool c5PopPass(C5Pass* out);
// RSSI recorded during the race (see c5_link.cpp): start (fresh: a new race,
// else a resume) with the millis() of race time 0, stop on a pause. Read:
// the race time recorded (ms), and a slot's samples as bytes (uint16 LE,
// one per C5_REC_MS) from byte `from`; returns the bytes copied.
void     c5RecStart(uint32_t raceStartMs, bool fresh);
void     c5RecStop();
// Size the record for this many minutes of race time (drops what is in
// it): the minutes it holds (less if PSRAM is short, 0 without PSRAM).
uint32_t c5RecSize(uint32_t minutes);
uint32_t c5RecCapMs();   // race time the record holds, ms (0: none)
uint32_t c5RecMs();
// A slot's record (racing: still being written; samples are single 16-bit
// stores, so reading it is safe).
const uint16_t* c5RecData(int slot, int* n);
size_t   c5RecRead(int slot, size_t from, uint8_t* out, size_t maxBytes);
C5SlotLive c5Live(int slot);
C5Status c5Status();
