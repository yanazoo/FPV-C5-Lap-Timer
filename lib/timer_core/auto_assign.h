// Automatic channel assignment from pilots' radio links.
//
// A pilot's TX backpack with Backpack > Telemetry = ESPNOW (ELRS 3.4+) sends
// the TX module's link statistics over ESP-NOW; elrs-netpack's bridge
// (v2.11+, "link watch") reports, per bind phrase UID, the uplink LQ. LQ > 0:
// the radio is linked to the aircraft, i.e. the aircraft is powered.
//
// In automatic mode, between races (never during one, or while laps are
// not yet cleared):
//   * a pilot linked for kJoinMs joins the first free channel, unless they
//     have no VTX frequency, another assigned pilot uses the same frequency,
//     every channel is taken, or their bind phrase can't tell them apart
//     (shared with another pilot, or the race director's);
//   * a pilot on a channel who was seen linked and has not been linked for
//     kLeaveMs leaves it. Pilots never seen linked (no backpack, telemetry
//     off) are left alone, so manual and automatic pilots can mix.
// Header-only; no Arduino.
#pragma once

#include <stdint.h>

namespace autoassign {

constexpr int      kSlots   = 8;
constexpr uint32_t kSeenMs  = 3000;    // a report this recent counts
constexpr uint32_t kJoinMs  = 2000;    // linked this long before joining
constexpr uint32_t kLeaveMs = 10000;   // not linked this long before leaving

// One radio, from the bridge's reports.
struct Link {
    uint32_t seenAt  = 0;   // last report (0: never)
    uint8_t  lq      = 0;   // uplink LQ in that report
    uint32_t upSince = 0;   // linked without a break since (0: not linked)
    uint32_t lastUp  = 0;   // last report with LQ > 0 (0: never linked)
};

inline void report(Link& l, uint32_t now, uint8_t lq) {
    if (!now) now = 1;
    if (l.seenAt && now - l.seenAt > kSeenMs) l.upSince = 0;   // a gap: start over
    l.seenAt = now;
    l.lq = lq;
    if (lq) {
        if (!l.upSince) l.upSince = now;
        l.lastUp = now;
    } else {
        l.upSince = 0;
    }
}

inline bool linked(const Link& l, uint32_t now) {
    return l.seenAt && l.lq && now - l.seenAt <= kSeenMs;
}

enum Wait : uint8_t {
    kNone = 0,
    kNoFreq,        // no VTX frequency set
    kSameFreq,      // another assigned pilot uses this frequency
    kFull,          // every channel is taken
    kAmbiguous,     // bind phrase shared with another pilot / the race director
};

struct Pilot {
    bool     valid = false;      // a roster entry
    uint16_t freq = 0;           // VTX MHz, 0 = not set
    bool     distinct = false;   // its UID is its own (not shared, not the RD's)
};

// What plan() did.
struct Result {
    int     joined[kSlots];   // roster indexes put on a channel
    int     nJoined = 0;
    int     left[kSlots];     // roster indexes taken off
    int     nLeft = 0;
};

// pilots / links: n roster entries. active[slot]: roster index or -1, updated
// in place. wait[i]: why linked pilot i is not on a channel (kNone otherwise).
// Returns true if active changed.
inline bool plan(uint32_t now, const Pilot* pilots, const Link* links, int n, int active[kSlots],
                 uint8_t* wait, Result& r) {
    r.nJoined = r.nLeft = 0;
    for (int i = 0; i < n; i++) wait[i] = kNone;
    // Leave: seen linked once, not linked for kLeaveMs.
    for (int s = 0; s < kSlots; s++) {
        int i = active[s];
        if (i < 0 || i >= n) continue;
        const Link& l = links[i];
        if (!l.lastUp || linked(l, now) || now - l.lastUp < kLeaveMs) continue;
        active[s] = -1;
        if (r.nLeft < kSlots) r.left[r.nLeft++] = i;
    }
    // Join: linked for kJoinMs, earliest first.
    for (;;) {
        int pick = -1;
        for (int i = 0; i < n; i++) {
            const Link& l = links[i];
            if (!pilots[i].valid || !linked(l, now) || !l.upSince || now - l.upSince < kJoinMs) continue;
            bool on = false;
            for (int s = 0; s < kSlots; s++) on |= active[s] == i;
            if (on || wait[i] != kNone) continue;
            if (pick < 0 || (int32_t)(l.upSince - links[pick].upSince) < 0) pick = i;
        }
        if (pick < 0) break;
        const Pilot& p = pilots[pick];
        int freeSlot = -1;
        bool sameFreq = false;
        for (int s = 0; s < kSlots; s++) {
            int a = active[s];
            if (a < 0 || a >= n) { if (freeSlot < 0) freeSlot = s; continue; }
            if (p.freq && pilots[a].freq == p.freq) sameFreq = true;
        }
        if (!p.distinct) wait[pick] = kAmbiguous;
        else if (!p.freq) wait[pick] = kNoFreq;
        else if (sameFreq) wait[pick] = kSameFreq;
        else if (freeSlot < 0) wait[pick] = kFull;
        else {
            active[freeSlot] = pick;
            if (r.nJoined < kSlots) r.joined[r.nJoined++] = pick;
        }
    }
    return r.nJoined || r.nLeft;
}

// During a race ("auto, also in races"): the race's laps stay with their
// slots, so nobody is taken off a slot until the race is cleared.
//   * a pilot on a slot who was seen linked and has not been linked for
//     kLeaveMs is "out": the slot stops measuring (out[slot] = true) but the
//     pilot, the slot and their laps stay; linked again (kJoinMs), back in;
//   * a pilot linked for kJoinMs joins a free slot not used in this race
//     (used[slot]: it has laps, or someone was out there), same waits as plan().
struct RaceResult {
    int joined[kSlots]; int nJoined = 0;
    int out[kSlots];    int nOut = 0;     // roster indexes that went out
    int back[kSlots];   int nBack = 0;    // roster indexes back in
};
inline bool planRace(uint32_t now, const Pilot* pilots, const Link* links, int n, int active[kSlots],
                     bool out[kSlots], const bool used[kSlots], uint8_t* wait, RaceResult& r) {
    r.nJoined = r.nOut = r.nBack = 0;
    for (int i = 0; i < n; i++) wait[i] = kNone;
    for (int s = 0; s < kSlots; s++) {
        int i = active[s];
        if (i < 0 || i >= n) continue;
        const Link& l = links[i];
        bool on = linked(l, now);
        if (!out[s] && l.lastUp && !on && now - l.lastUp >= kLeaveMs) {
            out[s] = true;
            r.out[r.nOut++] = i;
        } else if (out[s] && on && l.upSince && now - l.upSince >= kJoinMs) {
            out[s] = false;
            r.back[r.nBack++] = i;
        }
    }
    for (;;) {
        int pick = -1;
        for (int i = 0; i < n; i++) {
            const Link& l = links[i];
            if (!pilots[i].valid || !linked(l, now) || !l.upSince || now - l.upSince < kJoinMs) continue;
            bool on = false;
            for (int s = 0; s < kSlots; s++) on |= active[s] == i;
            if (on || wait[i] != kNone) continue;
            if (pick < 0 || (int32_t)(l.upSince - links[pick].upSince) < 0) pick = i;
        }
        if (pick < 0) break;
        const Pilot& p = pilots[pick];
        int freeSlot = -1;
        bool sameFreq = false;
        for (int s = 0; s < kSlots; s++) {
            int a = active[s];
            if (a < 0 || a >= n) { if (freeSlot < 0 && !used[s] && !out[s]) freeSlot = s; continue; }
            if (p.freq && pilots[a].freq == p.freq) sameFreq = true;
        }
        if (!p.distinct) wait[pick] = kAmbiguous;
        else if (!p.freq) wait[pick] = kNoFreq;
        else if (sameFreq) wait[pick] = kSameFreq;
        else if (freeSlot < 0) wait[pick] = kFull;
        else {
            active[freeSlot] = pick;
            r.joined[r.nJoined++] = pick;
        }
    }
    return r.nJoined || r.nOut || r.nBack;
}

// The first two bytes of MSP_SET_VTX_CONFIG, as ELRS's VTX Administrator
// sends them: (band - 1) * 8 + channel (0-7), bands A B E F R L (the
// Betaflight default table), or a frequency in MHz (> 63, little-endian).
// MHz, or 0 if neither.
inline uint16_t vtxConfigMhz(uint8_t b0, uint8_t b1) {
    static const uint16_t kTable[6][8] = {
        {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725},   // A
        {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866},   // B
        {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945},   // E
        {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880},   // F
        {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917},   // R
        {5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621},   // L
    };
    uint16_t v = (uint16_t)(b0 | (b1 << 8));
    if (v < 48) return kTable[v / 8][v % 8];
    if (v >= 5000 && v <= 6000) return v;
    return 0;
}

}  // namespace autoassign
