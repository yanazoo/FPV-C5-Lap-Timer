#include "gate_tracker.h"

namespace gate {

bool Tracker::push(uint16_t rssi, uint32_t timeUs, uint32_t* passUs, uint16_t* peak) {
    raw_ = rssi;
    f_ = valid_ ? (uint16_t)((3u * f_ + rssi) / 4u) : rssi;
    valid_ = true;
    if (!inside_) {
        if (f_ < enter_) return false;
        inside_ = true;
        peak_ = f_;
        peakUs_ = timeUs;
        return false;
    }
    if (f_ > peak_) peak_ = f_, peakUs_ = timeUs;
    if (f_ > exit_) return false;
    inside_ = false;
    if (passUs) *passUs = peakUs_;
    if (peak) *peak = peak_;
    return true;
}

int findPasses(const uint16_t* v, int n, uint32_t stepMs, uint16_t enter, uint16_t exit,
               uint32_t* outMs, uint16_t* outPeak, int maxOut) {
    int found = 0;
    bool inside = false;
    uint16_t peak = 0;
    int peakAt = 0;
    for (int i = 0; i < n && found < maxOut; ++i) {
        uint16_t x = v[i];
        if (!inside) {
            if (x < enter) continue;
            inside = true, peak = x, peakAt = i;
            continue;
        }
        if (x > peak) peak = x, peakAt = i;
        if (x > exit) continue;
        inside = false;
        outMs[found] = (uint32_t)peakAt * stepMs + stepMs / 2;
        if (outPeak) outPeak[found] = peak;
        ++found;
    }
    return found;
}

}  // namespace gate
