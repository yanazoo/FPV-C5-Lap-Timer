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

}  // namespace autoassign
