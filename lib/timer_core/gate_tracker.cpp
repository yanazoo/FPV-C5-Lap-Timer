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

}  // namespace gate
