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

#include "msp_v2.h"
#include "osd_engine.h"
#include "race_core.h"

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
            seed = seed * 1103515245u + 12345u;
            if (dropPct > 0 && (int)((seed >> 16) % 100) < dropPct) continue;
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
    e.tick(1400);   // the start burst is repeated once 0.3 s later
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
    e.onRaceStage(0, "");
    e.onRaceStart(1000, 0);
    for (uint32_t t = 1000; t <= 62000; t += 20) e.tick(t);
    g.feed(cap.bytes);
    cap.bytes.clear();
    CHECK(g.shows(2, "01:01"));
    e.onRaceStop(62500);
    g.feed(cap.bytes);
    cap.bytes.clear();
    CHECK(g.shows(5, "LAND NOW!") && !g.shows(2, ":"));
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
        bool ok = !g.shows(2, ":") && !g.shows(8, "0:09.0") && !g.shows(5, "FINISHED") && g.shows(12, "BEST");
        if (!ok) printf("seed %u: clock %d lap %d finished %d best %d\n", seed, g.shows(2, ":"), g.shows(8, "0:09.0"),
                        g.shows(5, "FINISHED"), g.shows(12, "BEST"));
        CHECK(ok);
    }
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

int runTests() {
    testMsp();
    testUid();
    testRaceHoleshot();
    testRaceImmediatePauseFinish();
    testPositions();
    testOsdLapFlow();
    testOsdRdAndJapanese();
    testOsdWhatThePilotSees();
    testOsdClockAndStop();
    testOsdFinishWithoutResults();
    testOsdClearsSurviveLoss();
    testFormat();
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
