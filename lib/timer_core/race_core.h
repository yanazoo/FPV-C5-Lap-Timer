// Race logic: turns gate passes into laps.
//
// This is what ESP-NOW-Lap-Timer split between gate_node (cooldown) and
// web_node (gate_comm.cpp: lap times, holeshot, best lap, pause/resume).
// With the C5 receiver both live on the XIAO ESP32-S3, so they are one
// platform-independent class here (host-tested in test/).
//
// All times are milliseconds on one clock (the S3's millis()). Not thread
// safe: the firmware guards it with its race mutex.
#pragma once

#include <stdint.h>

namespace race {

constexpr int kSlots = 8;
constexpr int kMaxLaps = 500;
constexpr uint32_t kMaxLapMs = 3600000UL;   // longer laps are treated as corrupt

enum class LapMode : uint8_t { Holeshot = 0, Immediate = 1 };

struct Lap {
    int8_t slot;
    int16_t rosterIdx;
    uint32_t lapMs;
    uint32_t ts;
    int16_t rssi;
};

struct SlotState {
    uint32_t lapCount = 0;    // passes recorded, holeshot included
    uint32_t bestLapMs = 0;   // holeshot excluded
    uint32_t lastLapTs = 0;   // ts of the last recorded pass (0 = none)
    uint32_t hsTs = 0;        // holeshot ts (holeshot mode)
    bool finished = false;    // reached totalLaps, or done when the time ran out
    uint32_t totalMs = 0;     // race time at the last pass (from holeshot in holeshot mode)
};

// What one recorded pass means.
struct PassResult {
    int slot;
    uint32_t ts;
    uint32_t lapMs;      // 0 if unknown
    uint32_t lapCount;   // including holeshot (what the web UI counts)
    int laps;            // completed laps, holeshot excluded
    bool holeshot;       // the first pass in holeshot mode
    bool newBest;
    uint32_t bestLapMs;
    uint32_t totalMs;    // race time so far (from holeshot in holeshot mode)
    bool justFinished;   // this pass completed totalLaps (or the timed race)
};

class Core {
public:
    LapMode lapMode = LapMode::Holeshot;
    uint32_t cooldownMs = 3000;   // minimum time between two passes of a pilot
    uint16_t totalLaps = 0;       // 0 = unlimited
    // Timed race: 0 = off. Once timeLimitMs of race time has passed, a
    // pilot's next pass ends their race if countLastLap (the lap they were
    // on counts), otherwise passes no longer count at all.
    uint32_t timeLimitMs = 0;
    bool countLastLap = true;

    // Fresh race: clears laps and per-slot state.
    void start(uint32_t now);
    // Pause. Passes are ignored until resume().
    void stop(uint32_t now);
    // Continue a paused race; the paused time is excluded from lap times.
    bool resume(uint32_t now);
    // Forget the race (laps and state).
    void clear();

    bool running() const { return running_; }
    bool paused() const { return !running_ && pauseStart_ != 0; }
    uint32_t startMs() const { return startMs_; }
    // Race time excluding pauses.
    uint32_t elapsed(uint32_t now) const;

    // A gate pass at ts (its peak). Returns true and fills out if it was
    // recorded; false if no race is running or it is within cooldownMs of
    // the pilot's previous pass.
    bool pass(int slot, int rosterIdx, uint32_t ts, int rssi, PassResult* out);

    // Timed race: has the time run out (race time, pauses excluded)?
    bool timeUp(uint32_t now) const { return timeLimitMs && running_ && elapsed(now) >= timeLimitMs; }
    // End a pilot's race without a pass (time up, last lap not counted).
    // Returns true if they had started and weren't finished yet.
    bool finishSlot(int slot);

    // Delete a wrong pass (a shortcut, a pass picked up by mistake): the
    // pilot's pass number `index` (0 = their first pass, the holeshot in
    // holeshot mode). Its time joins the next lap, so the total race time
    // stays as it was; deleting the last pass just drops it. Best lap,
    // total, finish and the cooldown reference are worked out again.
    // expectLapMs != 0: only if that pass's lap time still is this (a
    // browser that showed a stale list must not delete another pass).
    // Returns false if there is no such pass.
    bool deleteLap(int slot, int index, uint32_t expectLapMs = 0);

    // Completed laps (holeshot excluded).
    int laps(int slot) const;
    // 1-based race position by laps then time of the last pass; 0 if the
    // pilot has no pass yet.
    int position(int slot) const;

    const SlotState& slot(int s) const { return slots_[s]; }
    int lapCount() const { return lapCount_; }
    const Lap& lap(int i) const { return laps_[i]; }
    void clearLaps() { lapCount_ = 0; }

private:
    bool running_ = false;
    uint32_t startMs_ = 0;
    uint32_t pauseStart_ = 0;
    SlotState slots_[kSlots];
    Lap laps_[kMaxLaps];
    int lapCount_ = 0;
};

}  // namespace race
