#include "race_core.h"

namespace race {

void Core::start(uint32_t now) {
    running_ = true;
    startMs_ = now;
    pauseStart_ = 0;
    lapCount_ = 0;
    for (auto& s : slots_) s = SlotState();
}

void Core::stop(uint32_t now) {
    if (running_) pauseStart_ = now ? now : 1;
    running_ = false;
}

bool Core::resume(uint32_t now) {
    if (running_ || pauseStart_ == 0) return false;
    // Shift every stored reference forward by the pause, so the lap that
    // straddles the pause is measured as if the race never stopped.
    uint32_t dur = now - pauseStart_;
    startMs_ += dur;
    for (auto& s : slots_) {
        if (s.lastLapTs) s.lastLapTs += dur;
        if (s.hsTs) s.hsTs += dur;
    }
    pauseStart_ = 0;
    running_ = true;
    return true;
}

void Core::clear() {
    running_ = false;
    pauseStart_ = 0;
    lapCount_ = 0;
    for (auto& s : slots_) s = SlotState();
}

uint32_t Core::elapsed(uint32_t now) const {
    if (running_) return now - startMs_;
    if (pauseStart_) return pauseStart_ - startMs_;
    return 0;
}

bool Core::pass(int slot, int rosterIdx, uint32_t ts, int rssi, PassResult* out) {
    if (!running_ || slot < 0 || slot >= kSlots) return false;
    SlotState& s = slots_[slot];
    // Cooldown on pass (peak) times. Signed difference: an out-of-order or
    // repeated ts is inside the cooldown, never a ~49-day "gap".
    if (s.lastLapTs && (int32_t)(ts - s.lastLapTs) < (int32_t)cooldownMs) return false;

    // Guard the unsigned subtraction: a peak timed before the reference
    // (pilot already in the gate at the start) gives 0, not ~2^32.
    uint32_t ref = s.lastLapTs ? s.lastLapTs : startMs_;
    uint32_t lapMs = ((int32_t)(ts - ref) > 0) ? ts - ref : 0;
    if (lapMs > kMaxLapMs) lapMs = 0;

    s.lapCount++;
    s.lastLapTs = ts;
    bool holeshot = lapMode == LapMode::Holeshot && s.lapCount == 1;
    if (holeshot) s.hsTs = ts;

    bool newBest = false;
    if (!holeshot && lapMs > 0 && (s.bestLapMs == 0 || lapMs < s.bestLapMs)) {
        s.bestLapMs = lapMs;
        newBest = true;
    }

    uint32_t base = lapMode == LapMode::Holeshot ? s.hsTs : startMs_;
    uint32_t total = ((int32_t)(ts - base) > 0) ? ts - base : 0;

    if (lapCount_ < kMaxLaps) laps_[lapCount_++] = {(int8_t)slot, (int16_t)rosterIdx, lapMs, ts, (int16_t)rssi};

    int done = laps(slot);
    bool justFinished = false;
    if (totalLaps > 0 && !s.finished && done >= (int)totalLaps) {
        s.finished = true;
        justFinished = true;
    }

    if (out) {
        out->slot = slot;
        out->ts = ts;
        out->lapMs = lapMs;
        out->lapCount = s.lapCount;
        out->laps = done;
        out->holeshot = holeshot;
        out->newBest = newBest;
        out->bestLapMs = s.bestLapMs;
        out->totalMs = total;
        out->justFinished = justFinished;
    }
    return true;
}

int Core::laps(int slot) const {
    const SlotState& s = slots_[slot];
    if (lapMode == LapMode::Holeshot) return s.lapCount ? (int)s.lapCount - 1 : 0;
    return (int)s.lapCount;
}

int Core::position(int slot) const {
    if (slots_[slot].lapCount == 0) return 0;
    int mine = laps(slot);
    int pos = 1;
    for (int i = 0; i < kSlots; ++i) {
        if (i == slot || slots_[i].lapCount == 0) continue;
        int other = laps(i);
        if (other > mine) ++pos;
        else if (other == mine && (int32_t)(slots_[i].lastLapTs - slots_[slot].lastLapTs) < 0) ++pos;
    }
    return pos;
}

}  // namespace race
