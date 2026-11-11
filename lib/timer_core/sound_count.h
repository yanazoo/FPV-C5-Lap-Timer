// Counting sounds over a threshold, for the microphone's start / clear
// signal (platform independent; the firmware feeds it one 20 ms loudness
// window at a time).
//
// A sound that stays at or above the threshold for at least holdMs counts
// once. Sounds that begin within kMaxGapMs of the previous counted one's
// end make a series; the series is finished kEndMs after the last one
// ended with nothing new, and feed() then returns how many it had.
#pragma once
#include <stdint.h>

namespace sound {

class CrossCounter {
public:
    static constexpr uint16_t kMaxGapMs = 800, kEndMs = 900;

    // One window ending at `now` (ms), `loud` = at or above the threshold,
    // `addMs` = the window's length (more if windows were missed). Returns
    // the size of a series that just finished, else 0.
    uint8_t feed(uint32_t now, bool loud, uint16_t addMs, uint16_t holdMs) {
        if (loud) {
            if (!loud_) {   // a new sound; too long after the last one starts a new series
                if (count_ && now - end_ > kMaxGapMs) count_ = 0;
                above_ = 0;
                counted_ = false;
                rise_ = now - addMs;   // the start of this window
            }
            above_ = above_ + addMs > 60000 ? 60000 : above_ + addMs;
            if (!counted_ && above_ >= holdMs) {
                counted_ = true;
                if (count_ < 99) count_++;
            }
        } else if (loud_ && counted_) {
            end_ = now;
        }
        loud_ = loud;
        if (!count_ || loud_ || now - end_ < kEndMs) return 0;
        uint8_t n = count_;
        count_ = 0;
        return n;
    }
    void reset() { count_ = 0, loud_ = false, counted_ = false, above_ = 0; }
    // The series so far (before it is finished), and when its latest sound
    // began: for starting the clock on the signal itself.
    uint8_t  count() const { return count_; }
    uint32_t lastRise() const { return rise_; }

private:
    uint8_t  count_ = 0;
    uint32_t end_ = 0, rise_ = 0;
    uint16_t above_ = 0;
    bool     loud_ = false, counted_ = false;
};

}  // namespace sound
