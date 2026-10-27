// Per-pilot gate pass detection on C5MK readings.
//
// Same algorithm as FPVGateC5MK's c5host::GateDetector (and FPVGate):
// smooth f = (3f + value) / 4, enter at f >= enter, leave at f <= exit,
// and time the pass at its highest f. It also keeps what the web UI and
// lap records need: the peak value, the live filtered value and when the
// pilot was last heard. The pipeline test checks it against GateDetector.
//
// Readings are on the C5's 0..1023 scale (16 counts per dB).
#pragma once

#include <stdint.h>

namespace gate {

class Tracker {
public:
    void setThresholds(uint16_t enter, uint16_t exit) { enter_ = enter, exit_ = exit; }
    uint16_t enter() const { return enter_; }
    uint16_t exit() const { return exit_; }
    // Forget the filter and any pass in progress (race start, new frequency).
    void reset() { valid_ = false, inside_ = false; }

    // Feed every reading. Returns true when a pass ends; *passUs is then
    // the time of its peak and *peak the peak's filtered value.
    bool push(uint16_t rssi, uint32_t timeUs, uint32_t* passUs, uint16_t* peak);

    uint16_t filtered() const { return f_; }
    uint16_t raw() const { return raw_; }
    bool inside() const { return inside_; }
    bool valid() const { return valid_; }

private:
    uint16_t enter_ = 600, exit_ = 500;
    uint16_t f_ = 0, raw_ = 0;
    bool valid_ = false, inside_ = false;
    uint16_t peak_ = 0;
    uint32_t peakUs_ = 0;
};

}  // namespace gate
