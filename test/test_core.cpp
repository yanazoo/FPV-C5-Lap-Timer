// PC tests for lib/timer_core (no hardware, no Arduino). Run test/run.sh.
//
//   test_core                      run the unit tests
//   test_core uid <phrase>         print the backpack UID for a bind phrase
//   test_core center <len>           print the centred OSD column
//   test_core frame <phrase> <row> <col> <text> [<row> <col> <text> ...]
//                                  print the MSP bytes of one OSD burst
//                                  (used by crosscheck_vrxc.py)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "utf8.h"
#include "msp_v2.h"
#include "osd_engine.h"
#include "race_core.h"
#include "gate_tracker.h"
#include "race_files.h"
#include "sound_count.h"
#include "auto_assign.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

struct Capture : osd::Output {
    std::vector<uint8_t> bytes;
    int bursts = 0;
    void write(const uint8_t* d, size_t n) override { bytes.insert(bytes.end(), d, d + n); }
    void burstDone() override { ++bursts; }
};

struct Packet {
    uint16_t func;
    std::vector<uint8_t> payload;
};

std::vector<Packet> parse(const std::vector<uint8_t>& bytes) {
    msp::Parser p;
    std::vector<Packet> out;
    for (uint8_t b : bytes)
        if (p.feed(b)) out.push_back({p.function(), std::vector<uint8_t>(p.payload(), p.payload() + p.size())});
    return out;
}

// Text of all 0x03 writes in a capture, "row,col:text" each. The engine
// writes whole rows; col/text are the row with its padding trimmed, and rows
// written blank (to erase them) are left out.
std::vector<std::string> texts(const std::vector<Packet>& pk) {
    std::vector<std::string> out;
    for (auto& p : pk) {
        if (p.func != msp::kElrsSetOsd || p.payload.size() < 4 || p.payload[0] != 0x03) continue;
        std::string t(p.payload.begin() + 4, p.payload.end());
        size_t a = t.find_first_not_of(' ');
        if (a == std::string::npos) continue;
        size_t b = t.find_last_not_of(' ');
        out.push_back(std::to_string(p.payload[1]) + "," + std::to_string(p.payload[2] + a) + ":" +
                      t.substr(a, b - a + 1));
    }
    return out;
}

// Goggles at the end of the ESP-NOW hop. HDZero keeps a canvas that "clear"
// blanks and "display" shows (hdzero-goggle src/core/elrs.c handle_osd);
// scratch=true models goggles that show only what was written since the
// previous display. dropPct > 0 loses that share of OSD packets on the way
// (pseudo-random, seeded).
struct Goggles {
    uint8_t uid[6];
    bool scratch = false;
    int dropPct = 0;
    uint32_t seed = 1;
    // A radio outage: every OSD packet in [outFrom, outTo) ms is lost.
    int outFrom = -1, outTo = -1;
    uint32_t now = 0;
    bool mine = false;
    char canvas[osd::kRows][osd::kCols + 1];
    char screen[osd::kRows][osd::kCols + 1];
    msp::Parser parser;

    Goggles(const char* phrase, bool scratchModel = false) : scratch(scratchModel) {
        msp::bindPhraseUid(phrase, uid);
        blank(canvas);
        blank(screen);
    }
    static void blank(char (&c)[osd::kRows][osd::kCols + 1]) {
        for (auto& row : c) memset(row, ' ', osd::kCols), row[osd::kCols] = '\0';
    }
    void feed(const std::vector<uint8_t>& bytes) {
        for (uint8_t b : bytes) {
            if (!parser.feed(b)) continue;
            const uint8_t* p = parser.payload();
            size_t n = parser.size();
            if (parser.function() == msp::kElrsSetSendUid) {
                mine = n == 7 && p[0] == 1 && memcmp(p + 1, uid, 6) == 0;
                if (n == 1) mine = false;
                continue;
            }
            if (!mine || parser.function() != msp::kElrsSetOsd || n < 1) continue;
            // splitmix32: an LCG's lattice correlates packets a fixed burst
            // length apart, which made the same row drop burst after burst.
            seed += 0x9E3779B9u;
            uint32_t z = seed;
            z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
            z = (z ^ (z >> 13)) * 0xC2B2AE35u;
            z ^= z >> 16;
            if (dropPct > 0 && (int)(z % 100) < dropPct) continue;
            if (outFrom >= 0 && (int)now >= outFrom && (int)now < outTo) continue;
            if (p[0] == 0x02) {
                blank(canvas);
            } else if (p[0] == 0x03 && n >= 4) {
                for (size_t i = 4; i < n && p[2] + (i - 4) < (size_t)osd::kCols; ++i) canvas[p[1]][p[2] + i - 4] = (char)p[i];
            } else if (p[0] == 0x04) {
                memcpy(screen, canvas, sizeof screen);
                if (scratch) blank(canvas);
            }
        }
    }
    bool shows(int row, const char* text) const { return strstr(screen[row], text) != nullptr; }
};

// Packets of the last recipient burst (from its SET_SEND_UID with a UID).
std::vector<Packet> lastBurst(const std::vector<Packet>& pk) {
    size_t start = 0;
    for (size_t i = 0; i < pk.size(); ++i)
        if (pk[i].func == msp::kElrsSetSendUid && pk[i].payload.size() == 7) start = i;
    return std::vector<Packet>(pk.begin() + (long)start, pk.end());
}

bool has(const std::vector<std::string>& v, const std::string& suffix) {
    for (auto& s : v)
        if (s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
    return false;
}

std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; ++i) snprintf(b, sizeof b, "%02x", p[i]), s += b;
    return s;
}

void testMsp() {
    // vrxc_elrs MSPPacket(SET_SEND_UID, [0]).get_packet()
    uint8_t out[msp::kMaxFrame];
    uint8_t zero = 0;
    size_t n = msp::encode(msp::kElrsSetSendUid, &zero, 1, out, sizeof out);
    CHECK(n == 10);
    CHECK(out[0] == '$' && out[1] == 'X' && out[2] == '<' && out[3] == 0 && out[4] == 0xB5 && out[5] == 0);
    CHECK(out[6] == 1 && out[7] == 0 && out[8] == 0);
    msp::Parser p;
    int frames = 0;
    for (size_t i = 0; i < n; ++i) frames += p.feed(out[i]);
    CHECK(frames == 1 && p.function() == msp::kElrsSetSendUid && p.size() == 1);
    out[n - 1] ^= 0xFF;   // corrupt CRC
    frames = 0;
    for (size_t i = 0; i < n; ++i) frames += p.feed(out[i]);
    CHECK(frames == 0);
    uint8_t big[65] = {};
    CHECK(msp::encode(msp::kElrsSetOsd, big, sizeof big, out, sizeof out) == 0);
}

void testUid() {
    uint8_t uid[6];
    // MD5('-DMY_BINDING_PHRASE="test"'), checked against Python hashlib.
    msp::bindPhraseUid("test", uid);
    CHECK((uid[0] & 1) == 0);
    uint8_t again[6];
    msp::bindPhraseUid("test", again);
    CHECK(memcmp(uid, again, 6) == 0);
    msp::bindPhraseUid("other", again);
    CHECK(memcmp(uid, again, 6) != 0);
}

// Deleting a wrong pass (a shortcut): its time joins the next lap.
void testRaceDeleteLap() {
    race::PassResult r;
    {   // holeshot mode: HS 2 s, laps 20 s, 8 s (shortcut), 22 s
        race::Core c;
        c.start(0);
        for (uint32_t t : {2000u, 22000u, 30000u, 52000u}) CHECK(c.pass(0, 0, t, 600, &r));
        CHECK(c.laps(0) == 3 && c.slot(0).bestLapMs == 8000);
        CHECK(!c.deleteLap(0, 2, 9999));            // stale view: not that lap
        CHECK(!c.deleteLap(0, 9) && !c.deleteLap(1, 0));
        CHECK(c.deleteLap(0, 2, 8000));
        CHECK(c.laps(0) == 2 && c.lapCount() == 3);
        CHECK(c.lap(2).lapMs == 30000 && c.lap(2).ts == 52000);   // 8 + 22 s
        CHECK(c.slot(0).bestLapMs == 20000 && c.slot(0).totalMs == 50000);
        // Delete the last pass: the next lap counts from the pass before.
        CHECK(c.deleteLap(0, 2));
        CHECK(c.laps(0) == 1 && c.slot(0).lastLapTs == 22000 && c.slot(0).totalMs == 20000);
        CHECK(c.pass(0, 0, 45000, 600, &r) && r.lapMs == 23000 && r.laps == 2);
        // Delete the holeshot: the next pass becomes the holeshot.
        CHECK(c.deleteLap(0, 0));
        CHECK(c.laps(0) == 1 && c.lap(0).lapMs == 22000 && c.slot(0).hsTs == 22000);
        CHECK(c.slot(0).totalMs == 23000 && c.slot(0).bestLapMs == 23000);
    }
    {   // a finished pilot back under the total laps races on
        race::Core c;
        c.totalLaps = 2;
        c.start(0);
        for (uint32_t t : {1000u, 11000u, 15000u}) CHECK(c.pass(0, 0, t, 600, &r));
        CHECK(r.justFinished && c.slot(0).finished);
        CHECK(c.deleteLap(0, 2));
        CHECK(!c.slot(0).finished && c.laps(0) == 1);
        CHECK(c.pass(0, 0, 21000, 600, &r) && r.lapMs == 10000 && r.justFinished);
    }
    {   // a pause in between: times already moved past it
        race::Core c;
        c.lapMode = race::LapMode::Immediate;
        c.start(0);
        CHECK(c.pass(0, 0, 10000, 600, &r));
        c.stop(15000);
        c.resume(25000);   // 10 s pause
        CHECK(c.pass(0, 0, 32000, 600, &r) && r.lapMs == 12000);
        CHECK(c.deleteLap(0, 1));
        CHECK(c.pass(0, 0, 40000, 600, &r) && r.lapMs == 20000);   // 40 - 10 - 10 s pause
        CHECK(c.slot(0).totalMs == 30000);
    }
}

// Passes found again in a recorded RSSI trace with other thresholds.
void testFindPasses() {
    // 40 ms steps: a pass peaking at 500 (step 10), a weak one at 290
    // (step 40), noise at 120, nothing heard (0) at the end.
    uint16_t v[60];
    for (int i = 0; i < 60; i++) v[i] = 120;
    v[8] = 320, v[9] = 450, v[10] = 500, v[11] = 400, v[12] = 250;
    v[39] = 270, v[40] = 290, v[41] = 260;
    v[55] = 0, v[56] = 0;
    uint32_t t[8];
    uint16_t pk[8];
    // Enter 300 / exit 280: only the strong one.
    CHECK(gate::findPasses(v, 60, 40, 300, 280, t, pk, 8) == 1 && t[0] == 420 && pk[0] == 500);
    // Enter 285 / exit 265: the weak one too, timed at its peak.
    CHECK(gate::findPasses(v, 60, 40, 285, 265, t, pk, 8) == 2 && t[1] == 1620 && pk[1] == 290);
    // A pass still open at the end isn't one; maxOut is kept.
    v[58] = 600, v[59] = 600;
    CHECK(gate::findPasses(v, 60, 40, 285, 265, t, pk, 8) == 2);
    CHECK(gate::findPasses(v, 60, 40, 285, 265, t, pk, 1) == 1);
}

// A pilot's passes replaced by those found again (race stopped).
void testRaceReplaceSlot() {
    race::PassResult r;
    {   // holeshot mode: HS 2 s, a 40 s lap (one pass missed at 22 s), 20 s
        race::Core c;
        c.cooldownMs = 3000;
        c.start(0);
        for (uint32_t t : {2000u, 42000u, 62000u}) CHECK(c.pass(0, 0, t, 600, &r));
        CHECK(c.pass(1, 1, 30000, 600, &r));
        uint32_t again[] = {2000, 22000, 42000, 62000};
        CHECK(c.replaceSlot(0, 0, again, nullptr, 4, 50000) == 3);   // racing: up to race time 50 s
        CHECK(c.laps(0) == 2 && c.slot(0).lastLapTs == 42000);
        CHECK(!c.pass(0, 0, 43000, 600, &r));   // the cooldown counts from the pass found
        c.stop(70000);
        CHECK(c.replaceSlot(0, 0, again, nullptr, 4, 0) == 4);
        CHECK(c.laps(0) == 3 && c.slot(0).bestLapMs == 20000 && c.slot(0).totalMs == 60000);
        CHECK(c.slot(0).lastLapTs == 62000 && c.slot(0).hsTs == 2000);
        // laps_ in race-time order, the other pilot's pass where it was.
        CHECK(c.lapCount() == 5 && c.lap(1).slot == 0 && c.lap(2).slot == 1 && c.lap(2).lapMs == 30000 && c.lap(3).slot == 0);
        CHECK(c.passTime(0, 1) == 22000 && c.passTime(1, 0) == 30000);
        // Cooldown, and times at / past the end, as pass() would.
        uint32_t noisy[] = {2000, 3000, 22000, 69000, 75000};
        CHECK(c.replaceSlot(0, 0, noisy, nullptr, 5, 0) == 3 && c.laps(0) == 2);
        // Racing on after it: the next lap counts from the last pass found.
        CHECK(c.resume(80000) && c.pass(0, 0, 91000, 600, &r) && r.lapMs == 12000);   // race time 81 s - 69 s
    }
    {   // a pause: race times leave it out
        race::Core c;
        c.lapMode = race::LapMode::Immediate;
        c.start(0);
        CHECK(c.pass(0, 0, 10000, 600, &r));
        c.stop(20000);
        c.resume(30000);   // 10 s pause
        CHECK(c.pass(0, 0, 40000, 600, &r) && r.lapMs == 20000);
        c.stop(50000);     // race time 40 s
        uint32_t again[] = {10000, 20000, 30000};
        CHECK(c.replaceSlot(0, 0, again, nullptr, 3, 0) == 3 && c.laps(0) == 3);
        CHECK(c.lap(1).lapMs == 10000 && c.slot(0).totalMs == 30000 && c.slot(0).lastLapTs == 40000);
    }
    {   // no race: nothing to replace
        race::Core c;
        uint32_t t[] = {1000};
        CHECK(c.replaceSlot(0, 0, t, nullptr, 1, 0) == -1);
    }
    {   // total laps: the passes after the finish are dropped
        race::Core c;
        c.totalLaps = 2;
        c.lapMode = race::LapMode::Immediate;
        c.start(0);
        CHECK(c.pass(0, 0, 10000, 600, &r));
        c.stop(40000);
        uint32_t again[] = {10000, 20000, 30000};
        CHECK(c.replaceSlot(0, 0, again, nullptr, 3, 0) == 2 && c.slot(0).finished && c.laps(0) == 2);
    }
}

void testRaceHoleshot() {
    race::Core c;
    c.cooldownMs = 3000;
    race::PassResult r;
    CHECK(!c.pass(0, 0, 500, 600, &r));   // no race running
    c.start(1000);
    CHECK(c.pass(0, 0, 3000, 600, &r));
    CHECK(r.holeshot && r.lapCount == 1 && r.laps == 0 && r.lapMs == 2000 && !r.newBest && r.bestLapMs == 0);
    CHECK(!c.pass(0, 0, 4000, 600, &r));   // inside cooldown
    CHECK(c.pass(0, 0, 13000, 600, &r));
    CHECK(!r.holeshot && r.laps == 1 && r.lapMs == 10000 && r.newBest && r.totalMs == 10000);
    CHECK(c.pass(0, 0, 21000, 600, &r));
    CHECK(r.laps == 2 && r.lapMs == 8000 && r.newBest && r.totalMs == 18000);
    CHECK(c.pass(0, 0, 30000, 600, &r));
    CHECK(r.lapMs == 9000 && !r.newBest && r.bestLapMs == 8000);
    CHECK(c.lapCount() == 4);
}

void testRaceImmediatePauseFinish() {
    race::Core c;
    c.lapMode = race::LapMode::Immediate;
    c.cooldownMs = 1000;
    c.totalLaps = 2;
    race::PassResult r;
    c.start(0);
    CHECK(c.pass(1, 3, 5000, 700, &r));
    CHECK(!r.holeshot && r.laps == 1 && r.lapMs == 5000 && r.newBest && !r.justFinished);
    c.stop(6000);
    CHECK(c.paused() && !c.running());
    CHECK(!c.pass(1, 3, 7000, 700, &r));
    CHECK(c.elapsed(9000) == 6000);
    CHECK(c.resume(10000));   // 4 s pause
    CHECK(c.elapsed(10000) == 6000);
    CHECK(c.pass(1, 3, 13000, 700, &r));
    CHECK(r.lapMs == 4000 && r.totalMs == 9000 && r.justFinished);
    CHECK(!c.pass(1, 3, 20000, 700, &r));   // finished: later passes don't count
    CHECK(c.laps(1) == 2 && c.lapCount() == 2);
    // Peak before the race start gives 0, not a wrapped time.
    c.start(50000);
    CHECK(c.pass(2, 0, 49990, 700, &r));
    CHECK(r.lapMs == 0);
}

void testRaceTimed() {
    race::PassResult r;
    // Last lap counts: the first pass after the time is up ends the race.
    race::Core c;
    c.lapMode = race::LapMode::Holeshot;
    c.cooldownMs = 1000;
    c.timeLimitMs = 60000;
    c.countLastLap = true;
    c.start(0);
    CHECK(c.pass(0, 0, 2000, 0, &r) && r.holeshot);
    CHECK(c.pass(0, 0, 30000, 0, &r) && !r.justFinished);
    CHECK(!c.timeUp(59000) && c.timeUp(60000));
    CHECK(c.pass(0, 0, 65000, 0, &r) && r.justFinished && r.laps == 2 && r.totalMs == 63000);
    CHECK(!c.pass(0, 0, 90000, 0, &r));   // done
    CHECK(c.slot(0).finished && c.slot(0).totalMs == 63000);
    // Pauses don't eat into the time.
    c.start(0);
    c.pass(1, 1, 1000, 0, &r);
    c.stop(10000);
    c.resume(30000);   // 20 s pause
    CHECK(!c.timeUp(75000) && c.timeUp(80000));
    // Last lap not counted: passes after the time don't count; finishSlot
    // ends the race of pilots who had started.
    race::Core d;
    d.lapMode = race::LapMode::Immediate;
    d.cooldownMs = 1000;
    d.timeLimitMs = 60000;
    d.countLastLap = false;
    d.start(0);
    CHECK(d.pass(0, 0, 20000, 0, &r) && r.laps == 1);
    CHECK(!d.pass(0, 0, 61000, 0, &r));
    CHECK(d.laps(0) == 1);
    CHECK(d.finishSlot(0) && d.slot(0).finished && !d.finishSlot(0));
    CHECK(!d.finishSlot(3));   // never started
}

void testPositions() {
    race::Core c;
    c.lapMode = race::LapMode::Immediate;
    c.cooldownMs = 0;
    race::PassResult r;
    c.start(0);
    c.pass(0, 0, 1000, 0, &r);
    c.pass(1, 1, 1500, 0, &r);
    CHECK(c.position(0) == 1 && c.position(1) == 2 && c.position(2) == 0);
    c.pass(1, 1, 3000, 0, &r);
    CHECK(c.position(1) == 1 && c.position(0) == 2);
}

void testOsdLapFlow() {
    Capture cap;
    osd::Engine e(cap);
    uint8_t uid[6];
    msp::bindPhraseUid("pilot-a", uid);
    e.setRecipient(0, true, uid);

    e.onRaceStage(0, "Kanto Cup");
    auto pk = parse(cap.bytes);
    CHECK(pk.size() == 6);   // uid, clear (staging blanks the screen), 2 rows, display, reset
    CHECK(pk[1].func == msp::kElrsSetOsd && pk[1].payload[0] == 0x02);
    CHECK(pk[0].func == msp::kElrsSetSendUid && pk[0].payload.size() == 7 && memcmp(&pk[0].payload[1], uid, 6) == 0);
    auto tx = texts(pk);
    CHECK(has(tx, ":w ARM NOW x") && has(tx, "0,21:KANTO CUP"));
    CHECK(pk.back().func == msp::kElrsSetSendUid && pk.back().payload.size() == 1 && pk.back().payload[0] == 0);

    cap.bytes.clear();
    e.onRaceStart(1000, 0);
    pk = parse(cap.bytes);
    for (auto& p : pk) CHECK(!(p.func == msp::kElrsSetOsd && p.payload[0] == 0x02));   // no clear mid-race
    tx = texts(pk);
    CHECK(has(tx, ":w   GO!   x") && has(tx, "2,23:00:00"));
    cap.bytes.clear();
    e.tick(1400);   // the start burst is repeated 0.2 s and 0.4 s later
    CHECK(has(texts(parse(cap.bytes)), ":w   GO!   x"));
    cap.bytes.clear();
    e.tick(1500);   // GO! cleared after 0.5 s: its row is written blank, the clock stays
    tx = texts(parse(cap.bytes));
    CHECK(!has(tx, "GO!") && has(tx, "2,23:00:00"));
    cap.bytes.clear();
    e.tick(2000);   // race clock, first second
    CHECK(has(texts(parse(cap.bytes)), "2,23:00:01"));

    // A lap: position, lap time, best, then two redraws and a clear at 5 s.
    cap.bytes.clear();
    e.onLap(12000, 0, {1, false, 11000, 11000, 11000, 1});
    tx = texts(parse(cap.bytes));
    CHECK(has(tx, "1,17:POSN: 1 | LAP: 1"));
    CHECK(has(tx, "8,22:0:11.0"));
    CHECK(has(tx, "12,0:BEST: 0:11.0"));
    CHECK(has(tx, ":00:01"));   // full frame: the clock row is resent too
    cap.bytes.clear();
    e.tick(12800);
    CHECK(has(texts(parse(cap.bytes)), "8,22:0:11.0"));
    cap.bytes.clear();
    e.tick(13600);
    CHECK(has(texts(parse(cap.bytes)), "8,22:0:11.0"));
    cap.bytes.clear();
    cap.bursts = 0;
    e.tick(14400);   // only two redraws: just the clock tick (14 s) flushes
    CHECK(cap.bursts == 1);
    cap.bytes.clear();
    e.tick(17000);   // clock tick, then the lap row is cleared at 5 s
    tx = texts(lastBurst(parse(cap.bytes)));
    CHECK(!has(tx, "8,22:0:11.0"));
    CHECK(has(tx, "12,0:BEST: 0:11.0"));   // best stays

    // A newer lap cancels the older lap's clear.
    cap.bytes.clear();
    e.onLap(20000, 0, {2, false, 8000, 8000, 19000, 1});
    e.onLap(22000, 0, {3, false, 2000, 2000, 21000, 1});
    e.tick(25000);   // first lap's clear time: must not wipe lap 3
    cap.bytes.clear();
    e.tick(25500);
    CHECK(has(texts(parse(cap.bytes)), ":0:02.0") || cap.bytes.empty());

    // Finish and stop.
    cap.bytes.clear();
    e.onPilotDone(30000, 0, 1, 29000);
    tx = texts(parse(cap.bytes));
    CHECK(has(tx, ":w FINISHED! x") && has(tx, ":PLACEMENT: 1") && has(tx, ":TOTAL TIME: 0:29.0"));
    CHECK(!has(tx, "POSN"));
    cap.bytes.clear();
    e.onRaceStop(31000);
    tx = texts(parse(cap.bytes));
    CHECK(!has(tx, "LAND NOW"));   // finished pilots aren't told to land
    CHECK(!e.clockRunning());
}

void testOsdRdAndJapanese() {
    Capture cap;
    osd::Engine e(cap);
    uint8_t a[6], rd[6];
    msp::bindPhraseUid("a", a);
    msp::bindPhraseUid("rd", rd);
    e.setRecipient(2, true, a);
    e.setRecipient(osd::kRd, true, rd);
    e.onRaceStage(0, "大会 2026");
    CHECK(cap.bursts == 2);
    CHECK(has(texts(parse(cap.bytes)), ":2026"));   // non-ASCII dropped
    cap.bytes.clear();
    e.onRaceStop(1000);
    auto tx = texts(parse(cap.bytes));
    int lands = 0;
    for (auto& t : tx) lands += t.find("LAND NOW") != std::string::npos;
    CHECK(lands == 2);
    // Laps go to the pilot only.
    cap.bursts = 0;
    e.onLap(2000, 2, {1, false, 9000, 9000, 9000, 1});
    CHECK(cap.bursts == 1);
    // Holeshot: position row only, dedup on repeat.
    cap.bursts = 0;
    e.onLapsClear(3000);
    cap.bursts = 0;
    e.onLap(4000, 2, {0, true, 0, 0, 0, 1});
    e.onPosition(4000, 2, 0, 1);
    CHECK(cap.bursts == 1);
}

// What the pilot sees over a race, sampled every 20 ms.
struct Watch {
    int lapShownAt = -1, lapGoneAt = -1, flickers = 0;
};

Watch watchRace(Goggles& g, bool withRd) {
    Capture cap;
    osd::Engine e(cap);
    e.setRecipient(0, true, g.uid);
    e.setLightClock(!g.scratch);   // scratch goggles need the whole screen each tick
    if (withRd) {
        uint8_t rd[6];
        msp::bindPhraseUid("rd", rd);
        e.setRecipient(osd::kRd, true, rd);
    }
    Watch w;
    bool bestSeen = false, posSeen = false, clockSeen = false;
    auto step = [&](uint32_t now) {
        g.feed(cap.bytes);
        cap.bytes.clear();
        bool lap = g.shows(8, "0:12.3");
        if (lap && w.lapShownAt < 0) w.lapShownAt = (int)now;
        if (!lap && w.lapShownAt >= 0 && w.lapGoneAt < 0) w.lapGoneAt = (int)now;
        // Once up, these rows stay up for the whole race.
        if (bestSeen && !g.shows(12, "BEST: 0:12.3")) ++w.flickers;
        if (posSeen && !g.shows(1, "POSN: 1 | LAP: 1")) ++w.flickers;
        if (clockSeen && !g.shows(2, ":")) ++w.flickers;
        bestSeen |= g.shows(12, "BEST: 0:12.3");
        posSeen |= g.shows(1, "POSN: 1 | LAP: 1");
        clockSeen |= g.shows(2, "00:0");
    };
    e.onRaceStage(0, "");
    step(0);
    e.onRaceStart(3000, 0);
    for (uint32_t t = 3000; t <= 30000; t += 20) {
        if (t == 15300) e.onLap(t, 0, {1, false, 12300, 12300, 12300, 1});
        e.tick(t);
        step(t);
    }
    return w;
}

void testOsdWhatThePilotSees() {
    // Lap time up at the lap, down 5 s later (results uptime); best, position
    // and clock never blink.
    Goggles hd("pilot-a");
    Watch w = watchRace(hd, true);
    CHECK(w.lapShownAt == 15300);
    CHECK(w.lapGoneAt >= 20300 && w.lapGoneAt <= 20320);
    CHECK(w.flickers == 0);

    // Goggles that render each display from scratch see the same.
    Goggles scratch("pilot-a", true);
    w = watchRace(scratch, false);
    CHECK(w.lapShownAt == 15300 && w.lapGoneAt >= 20300 && w.lapGoneAt <= 20320 && w.flickers == 0);

    // Lossy ESP-NOW hop (15 % of packets lost): a lost write leaves the old
    // row up, so nothing blanks; the lap time still appears (first burst or
    // one of the two redraws) and goes within a second of its time.
    for (uint32_t n = 1; n <= 20; ++n) {
        Goggles lossy("pilot-a");
        lossy.dropPct = 15;
        lossy.seed = n;
        w = watchRace(lossy, true);
        CHECK(w.flickers == 0);
        if (!(w.lapShownAt >= 15300 && w.lapShownAt <= 16100 && w.lapGoneAt >= 20300 && w.lapGoneAt <= 21400)) printf("seed %u: shown %d gone %d\n", n, w.lapShownAt, w.lapGoneAt);
        CHECK(w.lapShownAt >= 15300 && w.lapShownAt <= 16100);
        CHECK(w.lapGoneAt >= 20300 && w.lapGoneAt <= 21400);
    }
}

void testOsdClockAndStop() {
    Capture cap;
    osd::Engine e(cap);
    Goggles g("p");
    e.setRecipient(0, true, g.uid);
    e.setLightClock(!g.scratch);   // scratch goggles need the whole screen each tick
    e.onRaceStage(0, "");
    e.onRaceStart(1000, 0);
    for (uint32_t t = 1000; t <= 62000; t += 20) e.tick(t);
    g.feed(cap.bytes);
    cap.bytes.clear();
    CHECK(g.shows(2, "01:01"));
    e.onRaceStop(62500);
    g.feed(cap.bytes);
    cap.bytes.clear();
    // The clock stays up on the time it stopped on, as in the web UI.
    CHECK(g.shows(5, "LAND NOW!") && g.shows(2, "01:01"));
    // Resume: GO! for 0.5 s, the clock continues from the paused time.
    e.onRaceStart(70000, 61500);
    g.feed(cap.bytes);
    cap.bytes.clear();
    CHECK(g.shows(5, "GO!") && g.shows(2, "01:01"));
    for (uint32_t t = 70000; t <= 71000; t += 20) e.tick(t);
    g.feed(cap.bytes);
    CHECK(!g.shows(5, "GO!") && !g.shows(5, "LAND") && g.shows(2, "01:02"));
    // A second start within 0.5 s keeps its GO! for the full time.
    cap.bytes.clear();
    e.onRaceStop(71100);
    e.onRaceStart(71300, 62000);
    e.tick(71500);   // the first start's clear time
    g.feed(cap.bytes);
    CHECK(g.shows(5, "GO!"));
}

void testOsdFinishWithoutResults() {
    Capture cap;
    osd::Engine e(cap);
    Goggles g("p");
    e.setRecipient(0, true, g.uid);
    e.setLightClock(!g.scratch);   // scratch goggles need the whole screen each tick
    e.options().resultsMode = false;
    e.onRaceStage(0, "");
    e.onRaceStart(0, 0);
    e.onLap(5000, 0, {1, false, 5000, 5000, 5000, 1});
    e.onPilotDone(5000, 0, 1, 5000);
    e.onRaceStop(6000);   // must not drop FINISHED!'s own clear
    for (uint32_t t = 6000; t <= 16000; t += 20) e.tick(t);
    g.feed(cap.bytes);
    CHECK(!g.shows(5, "FINISHED"));
}

// After the race clock stops nothing redraws the screen, so a lost write
// used to leave rows up for good: the clock after LAND NOW!, the lap time
// after its 5 s, FINISHED! after its 10 s. Refresh bursts repair that.
void testOsdClearsSurviveLoss() {
    for (uint32_t seed = 1; seed <= 30; ++seed) {
        Capture cap;
        osd::Engine e(cap);
        Goggles g("p");
        g.dropPct = 15;
        g.seed = seed;
        e.setRecipient(0, true, g.uid);
        e.setLightClock(!g.scratch);   // scratch goggles need the whole screen each tick
        e.onRaceStage(0, "");
        e.onRaceStart(1000, 0);
        uint32_t t = 1000;
        auto run = [&](uint32_t until) {
            for (; t <= until; t += 20) e.tick(t);
            g.feed(cap.bytes);
            cap.bytes.clear();
        };
        run(10000);
        e.onLap(10000, 0, {1, false, 9000, 9000, 9000, 1});
        e.onPilotDone(10000, 0, 1, 9000);
        run(12000);
        e.onRaceStop(12000);
        run(25000);   // lap row (5 s) and FINISHED! (10 s) have expired
        bool ok = g.shows(2, "00:11") && !g.shows(8, "0:09.0") && !g.shows(5, "FINISHED") && g.shows(12, "BEST");
        if (!ok) printf("seed %u: clock %d lap %d finished %d best %d\n", seed, g.shows(2, ":"), g.shows(8, "0:09.0"),
                        g.shows(5, "FINISHED"), g.shows(12, "BEST"));
        CHECK(ok);
    }
}

// When does each element appear and disappear? One race with every timed
// element, the goggles' screen sampled every 20 ms. Each element must be up
// in exactly one stretch, from its event until its due time - not gone early
// (flicker), not left up late. With packet loss the start and the end may
// slip by the redraws/refreshes that repair a lost burst.
struct Span {
    int first = -1, last = -1, stretches = 0;
};

struct Timeline {
    static constexpr int kItems = 11;
    Span s[kItems];
};

// Event times (ms).
constexpr uint32_t kStage = 0, kStart = 3000, kLap = 15000, kDone = 25000, kAnn = 30000, kStop = 40000,
                   kClear = 60000, kEnd = 63000;

Timeline raceTimeline(Goggles& g, bool finish = true) {
    Capture cap;
    osd::Engine e(cap);
    e.setRecipient(0, true, g.uid);
    e.setLightClock(!g.scratch);   // scratch goggles need the whole screen each tick
    e.options().eventName = {0, -1};
    Timeline tl;
    struct { int row; const char* text; } items[Timeline::kItems] = {
        {5, "ARM NOW"}, {0, "CUP"}, {5, "GO!"}, {2, ":"}, {8, "0:12.0"}, {12, "BEST"}, {1, "POSN"},
        {5, "FINISHED"}, {10, "PLACEMENT"}, {3, "HELLO"}, {5, "LAND NOW"}};
    bool up[Timeline::kItems] = {};
    for (uint32_t t = 0; t <= kEnd; t += 20) {
        if (t == kStage) e.onRaceStage(t, "Cup");
        if (t == kStart) e.onRaceStart(t, 0);
        if (t == kLap) e.onLap(t, 0, {1, false, 12000, 12000, 12000, 1});
        if (t == kDone && finish) e.onPilotDone(t, 0, 1, 22000);
        if (t == kAnn) e.onAnnounce(t, "hello");
        if (t == kStop) e.onRaceStop(t);
        if (t == kClear) e.onLapsClear(t);
        e.tick(t);
        g.now = t;
        g.feed(cap.bytes);
        cap.bytes.clear();
        for (int i = 0; i < Timeline::kItems; ++i) {
            bool now = g.shows(items[i].row, items[i].text);
            Span& sp = tl.s[i];
            if (now && !up[i]) ++sp.stretches;
            if (now) {
                if (sp.first < 0) sp.first = (int)t;
                sp.last = (int)t;
            }
            up[i] = now;
        }
    }
    return tl;
}

// Expected [appear, disappear) per element, in item order.
struct Want { int from, until; };
const Want kWant[Timeline::kItems] = {
    {kStage, kStart},               // ARM NOW until GO!
    {kStage, kStart},               // event name until GO! (start clears the frame)
    {kStart, kStart + 500},         // GO! for startUptime 0.5 s
    {kStart, kClear},               // race clock until cleared (final time after the stop)
    {kLap, kLap + 5000},            // lap time for resultsUptime 5 s
    {kLap, kClear},                 // best lap until the laps are cleared
    {kLap, kDone},                  // position until the pilot finishes
    {kDone, kDone + 10000},         // FINISHED! for finishUptime 10 s
    {kDone, kDone + 10000},         // placement with it
    {kAnn, kAnn + 5000},            // announcement for announceUptime 5 s
    {-1, -1},                       // LAND NOW!: never - a finished pilot isn't told to land
};

bool checkTimeline(const Timeline& tl, int slack, const char* what) {
    static const char* names[Timeline::kItems] = {"ARM NOW", "event", "GO!", "clock", "lap", "best", "posn",
                                                  "FINISHED", "placement", "announce", "LAND NOW"};
    bool ok = true;
    for (int i = 0; i < Timeline::kItems; ++i) {
        const Span& sp = tl.s[i];
        int gone = sp.last + 20;   // first sample without it
        if (kWant[i].from < 0) {
            if (sp.stretches) printf("%s: %s up %d..%d, want never\n", what, names[i], sp.first, gone), ok = false;
            continue;
        }
        bool good = sp.stretches == 1 && sp.first >= kWant[i].from && sp.first <= kWant[i].from + slack &&
                    gone >= kWant[i].until && gone <= kWant[i].until + slack;
        if (!good) {
            printf("%s: %s up %d..%d in %d stretch(es), want %d..%d\n", what, names[i], sp.first, gone,
                   sp.stretches, kWant[i].from, kWant[i].until);
            ok = false;
        }
    }
    return ok;
}

// A pilot still racing at the stop: LAND NOW! from the stop until the laps
// are cleared, the clock on its final time until then, position and lap up
// as usual.
bool checkUnfinished(Goggles& g, int slack, const char* what) {
    Timeline tl = raceTimeline(g, false);
    struct { int i; Want w; const char* name; } checks[] = {
        {10, {kStop, kClear}, "LAND NOW"}, {3, {kStart, kClear}, "clock"}, {6, {kLap, kClear}, "posn"},
        {4, {kLap, kLap + 5000}, "lap"}, {7, {-1, -1}, "FINISHED"}};
    bool ok = true;
    for (auto& c : checks) {
        const Span& sp = tl.s[c.i];
        int gone = sp.last + 20;
        bool good = c.w.from < 0 ? sp.stretches == 0
                                 : sp.stretches == 1 && sp.first >= c.w.from && sp.first <= c.w.from + slack &&
                                       gone >= c.w.until && gone <= c.w.until + slack;
        if (!good) printf("%s: %s up %d..%d in %d stretch(es)\n", what, c.name, sp.first, gone, sp.stretches), ok = false;
    }
    return ok;
}

void testOsdAppearsAndClearsOnTime() {
    {
        Goggles a("p"), b("p", true);
        CHECK(checkUnfinished(a, 20, "unfinished hdzero"));
        CHECK(checkUnfinished(b, 20, "unfinished scratch"));
        for (uint32_t seed = 1; seed <= 20; ++seed) {
            Goggles l("p");
            l.dropPct = 10;
            l.seed = seed;
            CHECK(checkUnfinished(l, 1300, "unfinished lossy"));
        }
    }
    // No loss: every element up and down within one 20 ms sample.
    Goggles hd("p");
    CHECK(checkTimeline(raceTimeline(hd), 20, "hdzero"));
    Goggles scratch("p", true);
    CHECK(checkTimeline(raceTimeline(scratch), 20, "scratch"));
    // 10 % loss: a lost burst is repaired by the next refresh or redraw, so
    // each change lands within 1.3 s (draws are repeated at +0.2/+0.4 s, lap
    // times also at +0.8/+1.6 s; clears at +0.2 ... +0.8 s).
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        Goggles lossy("p");
        lossy.dropPct = 10;
        lossy.seed = seed;
        char what[24];
        snprintf(what, sizeof what, "loss seed %u", seed);
        CHECK(checkTimeline(raceTimeline(lossy), 1300, what));
    }
}

// A 1.5 s outage starting exactly when something changes swallows the
// change and all its quick repeats (they fall within ~1 s). The slow
// settle repeats, or the race clock's per-second redraw while it runs,
// must still land it within ~2.6 s.
void testOsdSurvivesOutage() {
    const int kOut = 1500, kLate = 2600;
    struct Case { uint32_t at; bool finish; int item; bool appear; const char* what; };
    const Case cases[] = {
        {kLap + 5000, true, 4, false, "lap time clear (clock running)"},
        {kDone + 10000, true, 7, false, "FINISHED clear (clock running)"},
        {kAnn + 5000, true, 9, false, "announcement clear"},
        {kClear, true, 3, false, "clock row clear at the clear"},
        {kStop, false, 10, true, "LAND NOW after the stop"},
        {kDone, true, 7, true, "FINISHED draw"},
    };
    for (const Case& c : cases) {
        Goggles g("p");
        g.outFrom = (int)c.at;
        g.outTo = (int)c.at + kOut;
        Timeline tl = raceTimeline(g, c.finish);
        const Span& sp = tl.s[c.item];
        bool ok;
        if (c.appear) ok = sp.first >= (int)c.at && sp.first <= (int)c.at + kLate;
        else {
            int gone = sp.last + 20;
            ok = gone <= (int)c.at + kLate;
        }
        if (!ok) printf("outage: %s: up %d..%d\n", c.what, sp.first, sp.last + 20);
        CHECK(ok);
    }
}

// Timed race: the goggles' race clock counts down from the limit to 00:00,
// then shows the time over the limit as -mm:ss.
void testOsdCountdownClock() {
    Goggles g("p");
    Capture cap;
    osd::Engine e(cap);
    e.setRecipient(0, true, g.uid);
    e.setLightClock(!g.scratch);   // scratch goggles need the whole screen each tick
    int row = e.options().raceClock.row;
    auto at = [&](uint32_t t) { e.tick(t); g.feed(cap.bytes); cap.bytes.clear(); };
    e.onRaceStart(0, 0, 90);
    at(0);
    CHECK(g.shows(row, "01:30"));
    // Same seconds as the web UI, which shows 01:29.50 here.
    at(500);
    CHECK(g.shows(row, "01:29"));
    at(1000);
    CHECK(g.shows(row, "01:29"));
    at(61000);
    CHECK(g.shows(row, "00:29"));
    at(90000);
    CHECK(g.shows(row, "00:00"));
    at(95000);
    CHECK(g.shows(row, "-00:05"));
    at(161000);
    CHECK(g.shows(row, "-01:11"));
    // A resumed timed race continues from where it was.
    e.onRaceStart(100000, 30000, 90);
    at(100000);
    CHECK(g.shows(row, "01:00"));
    // Without a limit it counts up as before.
    e.onRaceStart(200000, 0);
    at(201000);
    CHECK(g.shows(row, "00:01"));
}

// The stage counts 3, 2, 1 on the clock row with the browsers; GO! takes
// over; a stop during it removes the digit. Stop keeps the timer's time.
void testOsdStageCountAndStopTime() {
    Goggles g("p");
    Capture cap;
    osd::Engine e(cap);
    e.setRecipient(0, true, g.uid);
    e.setLightClock(!g.scratch);   // scratch goggles need the whole screen each tick
    int row = e.options().raceClock.row;
    auto at = [&](uint32_t t) { e.tick(t); g.feed(cap.bytes); cap.bytes.clear(); };
    e.onRaceStage(0, "", 3);
    at(0);
    CHECK(g.shows(5, "ARM NOW") && g.shows(row, "3"));
    at(1000);
    CHECK(g.shows(row, "2"));
    at(2000);
    CHECK(g.shows(row, "1"));
    e.onRaceStart(3300, 0);
    at(3300);
    CHECK(g.shows(5, "GO!") && g.shows(row, "00:00"));
    at(4300);
    CHECK(g.shows(row, "00:01"));
    // The goggles show the time the timer stopped on, even if this task is late.
    e.onRaceStop(9000, 4999);
    at(9000);
    CHECK(g.shows(5, "LAND NOW") && g.shows(row, "00:04"));
    // Stopped during the 3-2-1: the digit goes, no more follow.
    e.onLapsClear(10000);
    e.onRaceStage(11000, "", 3);
    at(11000);
    e.onRaceStop(11500);
    at(11500);
    at(13000);
    CHECK(!g.shows(row, "1") && !g.shows(row, "2") && !g.shows(row, "3"));
}

// A deleted lap: the goggles show the new lap count, position and best,
// and the (maybe deleted) last lap time goes; FINISHED! goes if unfinished.
// A start the OSD task got to a few ms late is still a fresh start: the
// staging screen (the event name) goes. Only a resumed race keeps its rows.
void testOsdStartClearsStage() {
    Capture cap;
    osd::Engine e(cap);
    Goggles g("p");
    e.setRecipient(0, true, g.uid);
    auto at = [&](uint32_t t) { e.tick(t); g.feed(cap.bytes); cap.bytes.clear(); };
    e.onRaceStage(0, "Cup", 3);
    at(3000);
    CHECK(g.shows(0, "CUP"));
    e.onRaceStart(3304, 4, 0, false);   // 4 ms into the race
    at(3320);
    CHECK(!g.shows(0, "CUP") && g.shows(5, "GO!"));
    // Stopped and resumed: the lap rows stay.
    e.onLap(10000, 0, {1, false, 6700, 6700, 6700, 1});
    at(10020);
    e.onRaceStop(11000);
    e.onRaceStart(15000, 7700, 0, true);
    at(15020);
    CHECK(g.shows(1, "POSN: 1 | LAP: 1") && g.shows(5, "GO!"));
}

void testUtf8() {
    char b[32];
    // 11 three-byte characters are 33 bytes: 10 fit, none is cut in half.
    const char* eleven = "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86\xE3\x81\x88\xE3\x81\x8A"
                         "\xE3\x81\x8B\xE3\x81\x8D\xE3\x81\x8F\xE3\x81\x91\xE3\x81\x93\xE3\x81\x95";
    CHECK(utf8::copy(b, eleven, sizeof b) == 30 && strlen(b) == 30 && memcmp(b, eleven, 30) == 0);
    CHECK(utf8::copy(b, "Yana", sizeof b) == 4 && strcmp(b, "Yana") == 0);
    // ASCII then a character that no longer fits whole.
    char c[6];
    CHECK(utf8::copy(c, "abcd\xE3\x81\x82", sizeof c) == 4 && strcmp(c, "abcd") == 0);
    // Bytes that are not UTF-8 (Shift-JIS, a lone continuation, an overlong form) are left out.
    CHECK(utf8::copy(b, "A\x83\x70" "B\x80\xC0\xAF" "C", sizeof b) == 4 && strcmp(b, "ApBC") == 0);
    // A string already cut in half is repaired in place.
    char d[8] = "ab\xE3\x81";
    CHECK(utf8::copy(d, d, sizeof d) == 2 && strcmp(d, "ab") == 0);
    // Four-byte characters (emoji) stay whole.
    CHECK(utf8::copy(b, "\xF0\x9F\x8F\x81", sizeof b) == 4);
}

void testOsdLapEdit() {
    Goggles g("p");
    Capture cap;
    osd::Engine e(cap);
    e.setRecipient(0, true, g.uid);
    auto at = [&](uint32_t t) { e.tick(t); g.feed(cap.bytes); cap.bytes.clear(); };
    e.onRaceStage(0, "");
    e.onRaceStart(3000, 0);
    at(3600);   // GO! gone
    e.onLap(10000, 0, {1, false, 7000, 7000, 7000, 1});
    e.onLap(12000, 0, {2, false, 2000, 2000, 9000, 1});   // the shortcut
    e.onPilotDone(12000, 0, 1, 9000);
    at(12000);
    CHECK(g.shows(12, "BEST: 0:02.0") && g.shows(5, "FINISHED"));
    e.onLapEdit(13000, 0, 1, 1, 7000, false);
    at(13000);
    CHECK(g.shows(1, "POSN: 1 | LAP: 1") && g.shows(12, "BEST: 0:07.0"));
    CHECK(!g.shows(8, "0:02.0") && !g.shows(5, "FINISHED"));
    at(20000);   // the old lap's pending clear/redraw must not bring it back
    CHECK(!g.shows(8, "0:02.0") && g.shows(12, "BEST: 0:07.0"));
}

void testFormat() {
    char b[16];
    osd::Engine::formatTime(1700, b, sizeof b);
    CHECK(strcmp(b, "0:01.7") == 0);
    osd::Engine::formatTime(65432, b, sizeof b);
    CHECK(strcmp(b, "1:05.4") == 0);
    CHECK(osd::Engine::centerCol(11) == 20);
    CHECK(osd::Engine::centerCol(80) == 0);
}

// Race file names: number, date, which is older (rotate mode).
void testRaceFiles() {
    using namespace racefiles;
    CHECK(number("race_001.csv") == 1);
    CHECK(number("race_000_20261108_1432.csv") == 0);
    CHECK(number("race_012_20261108_1432.csv") == 12);
    CHECK(number("race_.csv") == -1);
    CHECK(number("race_12x.csv") == -1);
    CHECK(number("pilots.csv") == -1);
    CHECK(number("race_001.txt") == -1);
    CHECK(dateOf("race_012_20261108_1432.csv") == "20261108_1432");
    CHECK(dateOf("race_012.csv") == "");
    CHECK(dateOf("race_012_2026110_1432.csv") == "");
    // By date, whatever the numbers (999 -> 000 restart).
    CHECK(older("race_999_20261108_1432.csv", "race_000_20261108_1435.csv"));
    CHECK(!older("race_000_20261108_1435.csv", "race_999_20261108_1432.csv"));
    CHECK(older("race_500_20261107_2359.csv", "race_001_20261108_0000.csv"));
    // No date (before the clock was set) counts as older.
    CHECK(older("race_998.csv", "race_001_20261108_0900.csv"));
    // Same minute or no dates: by number.
    CHECK(older("race_004_20261108_1432.csv", "race_005_20261108_1432.csv"));
    CHECK(older("race_003.csv", "race_004.csv"));
}

// The microphone's sound signal: feed 20 ms windows from a script of
// (loud?, ms) pieces; collect what feed() reports.
std::vector<int> soundSeries(std::vector<std::pair<bool, int>> script, uint16_t holdMs) {
    sound::CrossCounter c;
    std::vector<int> got;
    uint32_t t = 1000;
    script.push_back({false, 3000});   // and quiet after
    for (auto& piece : script)
        for (int ms = 0; ms < piece.second; ms += 20) {
            t += 20;
            uint8_t n = c.feed(t, piece.first, 20, holdMs);
            if (n) got.push_back(n);
        }
    return got;
}

void testAutoAssign() {
    using namespace autoassign;
    const int n = 5;
    Pilot pilots[n];
    Link links[n];
    for (int i = 0; i < n; i++) pilots[i] = {true, (uint16_t)(5658 + 37 * i), true};
    int active[kSlots];
    for (int& a : active) a = -1;
    uint8_t wait[n];
    Result r;
    uint32_t t = 1000;
    auto tick = [&](uint32_t dt, std::initializer_list<int> up, std::initializer_list<int> down = {}) {
        for (uint32_t x = 0; x < dt; x += 500) {
            t += 500;
            for (int i : up) report(links[i], t, 90);
            for (int i : down) report(links[i], t, 0);
        }
        return plan(t, pilots, links, n, active, wait, r);
    };
    // Not linked long enough yet, then pilot 2 then 0 join, in that order.
    report(links[2], t, 80);
    CHECK(!tick(500, {2}));
    CHECK(active[0] == -1);
    CHECK(tick(1500, {2}) && r.nJoined == 1 && active[0] == 2);
    CHECK(!tick(500, {0, 2}));
    CHECK(tick(2000, {0, 2}) && active[1] == 0 && r.joined[0] == 0);
    // LQ 0 (aircraft off, radio on): leaves after kLeaveMs, not before.
    CHECK(!tick(5000, {0}, {2}));
    CHECK(active[0] == 2);
    CHECK(tick(5500, {0}, {2}) && r.nLeft == 1 && r.left[0] == 2 && active[0] == -1 && active[1] == 0);
    // Reports stop altogether (radio off): leaves too.
    CHECK(!tick(9000, {}));
    CHECK(tick(1500, {}) && active[1] == -1);
    // A pilot put on a channel by hand, never heard: stays.
    active[5] = 4;
    CHECK(!tick(20000, {}));
    CHECK(active[5] == 4);
    // Same frequency as an assigned pilot: waits; shared UID: waits.
    pilots[1].freq = pilots[4].freq;
    pilots[3].distinct = false;
    CHECK(!tick(3000, {1, 3}));
    CHECK(wait[1] == kSameFreq && wait[3] == kAmbiguous);
    pilots[1].freq = 0;
    CHECK(!tick(500, {1, 3}) && wait[1] == kNoFreq);
    // Earliest linked first when several are ready; a full house waits.
    pilots[1].freq = 5800;
    pilots[3].distinct = true;
    for (int& a : active) a = 4;   // every channel taken (manual, never heard)
    CHECK(!tick(500, {1, 3}) && wait[1] == kFull && wait[3] == kFull);
    for (int& a : active) a = -1;
    active[5] = 4;
    CHECK(tick(500, {1, 3}) && r.nJoined == 2 && active[0] == 1 && active[1] == 3);
    // A gap in reports longer than kSeenMs restarts the join clock.
    Link l;
    report(l, 100, 50);
    report(l, 100 + kSeenMs + 1, 50);
    CHECK(l.upSince == 100 + kSeenMs + 1);
    CHECK(linked(l, 100 + 2 * kSeenMs + 1) && !linked(l, 100 + 2 * kSeenMs + 2));
    // During a race: leave (the slot stays theirs), back to their own slot,
    // newcomers only into slots nobody had in this race.
    {
        Link rl[n];
        int act[kSlots], own[kSlots];
        for (int& a : act) a = -1;
        for (int& o : own) o = -1;
        act[0] = 0; act[1] = 1;
        own[2] = kBlocked;   // slot 2 has laps from someone unknown
        for (int i = 0; i < n; i++) pilots[i] = {true, (uint16_t)(5658 + 37 * i), true};
        RaceResult rr;
        uint32_t u = 1000;
        auto rtick = [&](uint32_t dt, std::initializer_list<int> up) {
            for (uint32_t x = 0; x < dt; x += 500) { u += 500; for (int i : up) report(rl[i], u, 90); }
            return planRace(u, pilots, rl, n, act, own, wait, rr);
        };
        CHECK(!rtick(3000, {0, 1}));
        CHECK(own[0] == 0 && own[1] == 1);
        CHECK(!rtick(9500, {1}));                 // 0 off for 9.5 s: still on
        CHECK(rtick(1000, {1}) && rr.nLeft == 1 && rr.left[0] == 0 && act[0] == -1 && own[0] == 0);   // left, slot kept for them
        CHECK(rtick(2500, {1, 3}) && act[3] == 3 && act[0] == -1 && act[2] == -1);   // newcomer: not 0 (0's), not 2 (blocked)
        CHECK(rtick(2500, {0, 1, 3}) && act[0] == 0);   // 0 back to their own slot
        rtick(11000, {1, 3});                     // 0 leaves again
        CHECK(act[0] == -1 && own[0] == 0);
        CHECK(rtick(2500, {1, 3, 4}) && act[4] == 4);   // 4 not into slot 0
    }
    // VTX Administrator: R3 = (5-1)*8+2, A1 = 0, L8 = 47, a frequency, junk.
    CHECK(vtxConfigMhz(34, 0) == 5732 && vtxConfigMhz(0, 0) == 5865 && vtxConfigMhz(47, 0) == 5621);
    CHECK(vtxConfigMhz(5800 & 0xFF, 5800 >> 8) == 5800);
    CHECK(vtxConfigMhz(50, 0) == 0 && vtxConfigMhz(0x10, 0x27) == 0);
}

void testSoundCount() {
    // Two claps (40 ms, 300 ms apart), 0.02 s detection: one series of 2.
    CHECK((soundSeries({{true, 40}, {false, 300}, {true, 40}}, 20) == std::vector<int>{2}));
    // Three claps: 3.
    CHECK((soundSeries({{true, 40}, {false, 300}, {true, 40}, {false, 300}, {true, 40}}, 20) == std::vector<int>{3}));
    // A clap shorter than the detection time does not count.
    CHECK((soundSeries({{true, 40}}, 200).empty()));
    // An air horn of 0.6 s with 0.2 s detection: 1.
    CHECK((soundSeries({{true, 600}}, 200) == std::vector<int>{1}));
    // Two sounds 1.2 s apart are two series of 1.
    CHECK((soundSeries({{true, 40}, {false, 1200}, {true, 40}}, 20) == std::vector<int>{1, 1}));
    // A short blip under the detection time between two counted sounds
    // neither counts nor breaks the series.
    CHECK((soundSeries({{true, 100}, {false, 200}, {true, 20}, {false, 200}, {true, 100}}, 60) == std::vector<int>{2}));
    // Continuous noise: nothing while it lasts, then one when it stops.
    CHECK((soundSeries({{true, 5000}}, 20) == std::vector<int>{1}));
    // Starting on the signal: the count is known as each sound counts, and
    // lastRise() is when that sound began (not when it was recognised).
    {
        sound::CrossCounter c;
        uint32_t t = 1000;
        auto win = [&](bool loud) { t += 20; return c.feed(t, loud, 20, 60); };
        for (int i = 0; i < 10; i++) win(false);
        win(true);   // the window 1200..1220 is loud
        CHECK(c.count() == 0);   // not yet 60 ms
        win(true);
        win(true);
        CHECK(c.count() == 1);
        CHECK(c.lastRise() == 1200);
    }
}

int runTests() {
    testMsp();
    testUid();
    testRaceHoleshot();
    testRaceDeleteLap();
    testFindPasses();
    testRaceReplaceSlot();
    testRaceImmediatePauseFinish();
    testRaceTimed();
    testPositions();
    testOsdLapFlow();
    testOsdRdAndJapanese();
    testOsdWhatThePilotSees();
    testOsdClockAndStop();
    testOsdFinishWithoutResults();
    testOsdClearsSurviveLoss();
    testOsdAppearsAndClearsOnTime();
    testOsdSurvivesOutage();
    testOsdCountdownClock();
    testOsdStageCountAndStopTime();
    testOsdLapEdit();
    testOsdStartClearsStage();
    testUtf8();
    testFormat();
    testRaceFiles();
    testSoundCount();
    testAutoAssign();
    printf(failures ? "%d FAILED\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return runTests();
    if (argc == 3 && strcmp(argv[1], "uid") == 0) {
        uint8_t uid[6];
        msp::bindPhraseUid(argv[2], uid);
        printf("%s\n", hex(uid, 6).c_str());
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "center") == 0) {
        printf("%d\n", osd::Engine::centerCol((size_t)atoi(argv[2])));
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "frame") == 0 && (argc - 3) % 3 == 0) {
        Capture cap;
        osd::Engine e(cap);
        uint8_t uid[6];
        msp::bindPhraseUid(argv[2], uid);
        e.setRecipient(0, true, uid);
        // Each announcement adds one cell at the given position to the frame
        // (texts are upper-cased, so pass upper case); the last burst holds
        // the whole frame.
        for (int i = 3; i < argc; i += 3) {
            osd::Options& o = e.options();
            o.announcement = {(int8_t)atoi(argv[i]), (int8_t)atoi(argv[i + 1])};
            o.announceUptimeDs = 0;
            cap.bytes.clear();
            e.onAnnounce(0, argv[i + 2]);
        }
        printf("%s\n", hex(cap.bytes.data(), cap.bytes.size()).c_str());
        return 0;
    }
    fprintf(stderr, "usage: test_core [uid <phrase> | frame <phrase> (<row> <col> <text>)...]\n");
    return 2;
}
