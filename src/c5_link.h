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
    uint16_t raw;
    bool     inside;
    bool     signal;    // readings arriving for this slot
};

void c5Begin();
// Frequencies (0 = off) and thresholds per slot. Thread safe; applied by
// the link task. A slot whose frequency changes restarts its detector.
void c5Configure(const uint16_t mhz[8], const uint16_t enter[8], const uint16_t exit[8]);
void c5SetGain(int gain);
void c5ResetDetectors();             // race start
bool c5PopPass(C5Pass* out);
C5SlotLive c5Live(int slot);
C5Status c5Status();
