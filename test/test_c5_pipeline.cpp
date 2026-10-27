// End to end on the PC: simulated C5MK UART bytes -> c5host::Link (from the
// FPVGateC5MK submodule) -> gate::Tracker -> race::Core, the same chain as
// src/c5_link.cpp and src/race.cpp. Also checks gate::Tracker gives the same
// passes as c5host::GateDetector.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "c5host.h"
#include "gate_tracker.h"
#include "race_core.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                            \
        }                                                          \
    } while (0)

std::string sent;   // bytes the host wrote to the C5

uint8_t crc8(const uint8_t* p, size_t n) {
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; ++i) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

std::vector<uint8_t> statusLine(const char* payload) {
    uint8_t x = 0;
    for (const char* p = payload; *p; ++p) x ^= (uint8_t)*p;
    char line[64];
    snprintf(line, sizeof line, "%s*%02X\n", payload, x);
    return std::vector<uint8_t>(line, line + strlen(line));
}

// One scan record with readings for slots 0 and 1.
std::vector<uint8_t> record(uint8_t seq, uint32_t t0, uint16_t v0, uint16_t v1) {
    std::vector<uint8_t> r = {0xA5, 15, 'M', seq, (uint8_t)t0, (uint8_t)(t0 >> 8), (uint8_t)(t0 >> 16),
                              (uint8_t)(t0 >> 24), 0x03};
    auto add16 = [&](uint16_t v) { r.push_back((uint8_t)v), r.push_back((uint8_t)(v >> 8)); };
    add16(v0), add16(0), add16(v1), add16(120);
    r.push_back(crc8(r.data() + 1, r.size() - 1));
    return r;
}

struct Rx {
    gate::Tracker tr[2];
    c5host::GateDetector ref[2];
    std::vector<uint32_t> passes[2];
    std::vector<uint16_t> peaks[2];
    std::vector<uint32_t> refPasses[2];
};
Rx rx;

void onSample(void*, const c5host::Sample& s) {
    if (s.slot > 1) return;
    uint32_t at;
    uint16_t peak;
    if (rx.tr[s.slot].push(s.rssi, s.timeUs, &at, &peak)) rx.passes[s.slot].push_back(at), rx.peaks[s.slot].push_back(peak);
    if (rx.ref[s.slot].push(s.rssi, s.timeUs, &at)) rx.refPasses[s.slot].push_back(at);
}

void writeToC5(void*, const uint8_t* d, size_t n) { sent.append((const char*)d, n); }

// RSSI of a quad flying through the gate: floor 116, peak ~ +40 dB.
uint16_t passProfile(double tSec, const std::vector<double>& peaks) {
    double v = 116;
    for (double p : peaks) v += 640 * exp(-((tSec - p) * (tSec - p)) / (2 * 0.12 * 0.12));
    return (uint16_t)(v > 1023 ? 1023 : v);
}

}  // namespace

int main() {
    c5host::Link link(writeToC5, onSample, nullptr);
    for (auto& t : rx.tr) t.setThresholds(400, 300);
    for (auto& d : rx.ref) d.setThresholds(400, 300);
    const uint16_t slots[8] = {5658, 5695, 0, 0, 0, 0, 0, 0};
    CHECK(link.setSlots(slots));
    link.setGain(30);

    const uint32_t latencyUs = 1800;   // UART + host delay, constant here
    const uint32_t c5Start = 4000000000u;   // C5 clock near its wrap
    uint32_t hostUs = 123456;
    link.begin(hostUs);
    CHECK(sent.find("Q*51\n") != std::string::npos);

    auto st = statusLine("S,0,30,ERR_FREQ,2");
    link.feed(st.data(), st.size(), hostUs);
    link.tick(hostUs);
    CHECK(sent.find("P,5658,5695,0,0,0,0,0,0*") != std::string::npos);
    CHECK(sent.find("G,30*68\n") != std::string::npos);
    st = statusLine("S,0,30,SCAN,2");
    link.feed(st.data(), st.size(), hostUs);
    CHECK(link.scanning());

    // 40 s at one record per ms. Slot 0 passes at 5, 17, 28 s; slot 1 at 9 s.
    const std::vector<double> p0 = {5.0, 17.0, 28.0}, p1 = {9.0};
    uint8_t seq = 0;
    for (uint32_t ms = 0; ms < 40000; ++ms) {
        double t = ms / 1000.0;
        uint32_t c5t = c5Start + ms * 1000u;
        auto r = record(seq++, c5t, passProfile(t, p0), passProfile(t, p1));
        uint32_t now = hostUs + ms * 1000u + latencyUs;
        link.feed(r.data(), r.size(), now);
        if (ms % 5 == 0) link.tick(now);
    }
    const c5host::Counters& n = link.counters();
    CHECK(n.records == 40000 && n.seqGaps == 0 && n.badRecords == 0);

    CHECK(rx.passes[0].size() == 3 && rx.passes[1].size() == 1);
    CHECK(rx.passes[0] == rx.refPasses[0] && rx.passes[1] == rx.refPasses[1]);
    // Pass times sit on the host clock at the true peak (+ mapping latency,
    // + up to a few ms of filter lag).
    for (size_t i = 0; i < rx.passes[0].size(); ++i) {
        uint32_t truth = hostUs + (uint32_t)(p0[i] * 1e6) + latencyUs;
        int32_t err = (int32_t)(rx.passes[0][i] - truth);
        CHECK(err >= -1000 && err <= 6000);
        CHECK(rx.peaks[0][i] > 700);
    }

    // Into the race core as millisecond passes.
    race::Core race;
    race.cooldownMs = 3000;
    race.start(hostUs / 1000);
    race::PassResult r;
    std::vector<uint32_t> lapMs;
    for (uint32_t us : rx.passes[0])
        if (race.pass(0, 0, us / 1000, 0, &r)) lapMs.push_back(r.lapMs);
    CHECK(lapMs.size() == 3);
    if (lapMs.size() == 3) {
        CHECK(lapMs[1] >= 11995 && lapMs[1] <= 12005);
        CHECK(lapMs[2] >= 10995 && lapMs[2] <= 11005);
    }
    CHECK(race.slot(0).bestLapMs >= 10995 && race.slot(0).bestLapMs <= 11005);

    printf(failures ? "%d FAILED\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
