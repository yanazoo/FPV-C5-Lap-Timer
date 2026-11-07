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
    // A pilot who completed totalLaps is done: flying through the gate to
    // land must not add laps, best laps or change the placings.
    if (s.finished) return false;
    // Cooldown on pass (peak) times. Signed difference: an out-of-order or
    // repeated ts is inside the cooldown, never a ~49-day "gap".
    if (s.lastLapTs && (int32_t)(ts - s.lastLapTs) < (int32_t)cooldownMs) return false;
    // Timed race over: the pass either ends the pilot's race or doesn't count.
    bool overtime = timeLimitMs && (int32_t)(ts - startMs_) >= (int32_t)timeLimitMs;
    if (overtime && !countLastLap) return false;

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
    s.totalMs = total;

    if (lapCount_ < kMaxLaps) laps_[lapCount_++] = {(int8_t)slot, (int16_t)rosterIdx, lapMs, ts, (int16_t)rssi};

    int done = laps(slot);
    bool justFinished = false;
    if ((totalLaps > 0 && done >= (int)totalLaps) || overtime) {
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

bool Core::finishSlot(int slot) {
    if (slot < 0 || slot >= kSlots) return false;
    SlotState& s = slots_[slot];
    if (s.finished || s.lapCount == 0) return false;
    s.finished = true;
    return true;
}

bool Core::deleteLap(int slot, int index, uint32_t expectLapMs) {
    if (slot < 0 || slot >= kSlots || index < 0) return false;
    // Where the slot's passes are in laps_.
    static int at[kMaxLaps];
    int n = 0;
    for (int i = 0; i < lapCount_; ++i)
        if (laps_[i].slot == slot) at[n++] = i;
    if (index >= n) return false;
    const Lap& del = laps_[at[index]];
    if (expectLapMs && del.lapMs != expectLapMs) return false;
    SlotState& s = slots_[slot];
    uint32_t delMs = del.lapMs;
    if (index < n - 1) {
        // The next lap now runs from the pass before the deleted one.
        Lap& next = laps_[at[index + 1]];
        uint32_t nextMs = next.lapMs;
        if (delMs && nextMs) next.lapMs = nextMs + delMs;
        // Holeshot mode: the next pass becomes the holeshot, later by its lap.
        if (lapMode == LapMode::Holeshot && index == 0 && s.hsTs) s.hsTs += nextMs;
    } else {
        // The last pass: the cooldown and the next lap count from the pass
        // before (these times are already moved past any pause, as lapMs).
        s.lastLapTs = (n > 1 && s.lastLapTs) ? s.lastLapTs - delMs : 0;
    }
    for (int i = at[index]; i + 1 < lapCount_; ++i) laps_[i] = laps_[i + 1];
    --lapCount_;
    s.lapCount = (uint32_t)(n - 1);
    if (s.lapCount == 0) s.hsTs = s.lastLapTs = 0;
    // Best lap and total from the laps that are left (holeshot mode: the
    // holeshot is no lap, and the total counts from it).
    s.bestLapMs = 0;
    uint32_t total = 0;
    int k = 0;
    for (int i = 0; i < lapCount_; ++i) {
        if (laps_[i].slot != slot) continue;
        bool hs = lapMode == LapMode::Holeshot && k++ == 0;
        if (hs) continue;
        uint32_t ms = laps_[i].lapMs;
        total += ms;
        if (ms && (s.bestLapMs == 0 || ms < s.bestLapMs)) s.bestLapMs = ms;
    }
    s.totalMs = total;
    // Back under the total laps: racing again (a timed race's finish stays).
    if (s.finished && totalLaps > 0 && !timeLimitMs && laps(slot) < (int)totalLaps) s.finished = false;
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
