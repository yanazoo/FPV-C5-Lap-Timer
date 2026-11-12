#include <ArduinoJson.h>
#include "race.h"
#include "c5_link.h"
#include "data_model.h"
#include "elrs.h"
#include "mic.h"
#include "sound_count.h"
#include "sd_store.h"
#include "ws_handler.h"

race::Core raceCore;

static uint32_t lastRssiSend = 0;
static uint32_t lastStatusSend = 0;
// Bumped by every start and clear: a browser that sees a different value in
// the 1 Hz status missed one and re-reads the race.
static uint32_t raceEpoch = 0;


const char* activeName(int slot) {
    int ri = activePilots[slot];
    return rosterValid(ri) ? roster[ri].name : "---";
}

int activeSlotOf(int ri) {
    for (int s = 0; s < MAX_ACTIVE; s++)
        if (activePilots[s] == ri) return s;
    return -1;
}

// Timed race rules into the race core (call under the lock, while idle).
static void applyRaceRules() {
    bool timed = raceRules.rankMode == 2;
    raceCore.timeLimitMs = timed ? (uint32_t)raceRules.timeLimitSec * 1000u : 0;
    raceCore.countLastLap = raceRules.countLastLap;
}

void applyGateConfig() {
    uint16_t mhz[8] = {}, en[8], ex[8];
    {
        Guard g;
        for (int s = 0; s < MAX_ACTIVE; s++) {
            int ri = activePilots[s];
            en[s] = DEFAULT_ENTER, ex[s] = DEFAULT_EXIT;
            if (!rosterValid(ri)) continue;
            mhz[s] = roster[ri].freqMhz;
            en[s] = (uint16_t)constrain(rosterCal[ri].enterRssi, 0, 1023);
            ex[s] = (uint16_t)constrain(rosterCal[ri].exitRssi, 0, 1023);
        }
        raceCore.lapMode = settings.lapMode == 1 ? race::LapMode::Immediate : race::LapMode::Holeshot;
        raceCore.cooldownMs = settings.cooldownMs;
        raceCore.totalLaps = settings.totalLaps;
        // A rule change mid-race would move the goalposts: it applies from
        // the next start.
        if (!raceCore.running() && !raceCore.paused()) applyRaceRules();
    }
    c5Configure(mhz, en, ex);
    c5SetGain(settings.gain);
    elrsConfigChanged();
}

void raceBegin() { applyGateConfig(); }

static void wsJson(JsonDocument& d) {
    if (!ws.count()) return;   // nobody watching: skip the serialisation
    String s;
    serializeJson(d, s);
    wsText(s);
}

void raceStage() {
    elrsRaceStage();
    JsonDocument d;
    d["type"] = "race_stage";
    wsJson(d);
}

static bool timeUpHandled = false;   // timed race: the time-up step has run

void raceStart() { raceStartAt(millis()); }

void raceStartAt(uint32_t now) {
    {
        Guard g;
        applyRaceRules();
        raceCore.start(now);
        sdNoteRaceStart();
    }
    raceEpoch++;
    timeUpHandled = false;
    c5ResetDetectors();
    elrsRaceStart(0, raceCore.timeLimitMs / 1000, false, now);   // from `now`, also for a start on a sound
    JsonDocument d;
    d["type"] = "race_start";
    d["ts"] = now;
    wsJson(d);
}

bool raceResume() {
    uint32_t elapsed, at = millis();
    {
        Guard g;
        if (!raceCore.resume(at)) return false;
        elapsed = raceCore.elapsed(at);
    }
    elrsRaceStart(elapsed, raceCore.timeLimitMs / 1000, true, at);
    JsonDocument d;
    d["type"] = "race_resume";
    d["ts"] = raceCore.startMs();
    wsJson(d);
    return true;
}

// The microphone does not start a race for a while after a stop or clear
// (the stop tone, cheering, the next pilots getting ready).
static const uint32_t MIC_QUIET_MS = 5000;
static uint32_t micQuietUntil = MIC_QUIET_MS;   // and in the first seconds after boot

void raceStop() {
    micQuietUntil = millis() + MIC_QUIET_MS;
    uint32_t elapsed;
    {
        Guard g;
        raceCore.stop(millis());
        elapsed = raceCore.elapsed(millis());
    }
    elrsRaceStop(elapsed);   // the goggles' clock stops on the same time
    JsonDocument d;
    d["type"] = "race_stop";
    d["elapsed"] = elapsed;   // every browser freezes on this time
    wsJson(d);
}

bool raceDeleteLap(int slot, int index, uint32_t lapMs) {
    int positions[MAX_ACTIVE], laps[MAX_ACTIVE];
    uint32_t best;
    bool finished;
    {
        Guard g;
        if (!raceCore.deleteLap(slot, index, lapMs)) return false;
        for (int s = 0; s < MAX_ACTIVE; s++) positions[s] = raceCore.position(s), laps[s] = raceCore.laps(s);
        best = raceCore.slot(slot).bestLapMs;
        finished = raceCore.slot(slot).finished;
    }
    Serial.printf("[Race] slot %d: pass %d (%lu ms) deleted\n", slot, index, (unsigned long)lapMs);
    elrsLapEdit(slot, laps[slot], positions[slot], best, finished);
    for (int s = 0; s < MAX_ACTIVE; s++)
        if (s != slot && positions[s] > 0) elrsPosition(s, laps[s], positions[s]);
    JsonDocument d;
    d["type"] = "laps_changed";
    d["pilot"] = slot;
    wsJson(d);
    return true;
}

String raceClear() {
    micQuietUntil = millis() + MIC_QUIET_MS;
    bool hadLaps, saved = false;
    const char* reason = "";
    {
        Guard g;
        hadLaps = raceCore.lapCount() > 0;
        if (!hadLaps) reason = "empty";
        else if (settings.sdLogMode == 2) reason = "off";
        else if (!sdPresent) reason = "nosd";
        else saved = sdSaveRace();
        if (hadLaps && !saved && !reason[0]) reason = "error";
        raceCore.clear();
    }
    raceEpoch++;
    elrsLapsClear();
    // Clear on one browser clears every browser.
    JsonDocument d;
    d["type"] = "race_clear";
    wsJson(d);
    char resp[80];
    snprintf(resp, sizeof resp, R"({"ok":true,"saved":%s,"reason":"%s"})", saved ? "true" : "false", reason);
    return String(resp);
}

static void onPass(const C5Pass& p) {
    race::PassResult r;
    int ri;
    int positions[MAX_ACTIVE], laps[MAX_ACTIVE];
    char name[sizeof roster[0].name], yomi[sizeof roster[0].yomi];
    {
        Guard g;
        ri = activePilots[p.slot];
        if (!rosterValid(ri)) return;   // nobody in this slot
        if (!raceCore.pass(p.slot, ri, p.tsMs, p.peak, &r)) return;
        for (int s = 0; s < MAX_ACTIVE; s++) positions[s] = raceCore.position(s), laps[s] = raceCore.laps(s);
        // Copied under the lock: the web task may edit or delete the pilot meanwhile.
        strlcpy(name, roster[ri].name, sizeof name);
        strlcpy(yomi, roster[ri].yomi, sizeof yomi);
    }
    Serial.printf("[Race] slot %d pass  lap=%lu  lapMs=%lu  peak=%u\n", p.slot, (unsigned long)r.lapCount,
                  (unsigned long)r.lapMs, p.peak);

    JsonDocument d;
    d["type"]     = "lap";
    d["pilot"]    = p.slot;
    d["name"]     = name;
    d["yomi"]     = yomi;
    d["lapTime"]  = r.lapMs;
    d["bestLap"]  = r.bestLapMs;
    d["lapCount"] = r.lapCount;
    d["newBest"]  = r.newBest;
    d["finished"] = r.justFinished;
    d["rssi"]     = p.peak;
    d["ts"]       = r.ts;
    wsJson(d);

    // Goggles: the pilot's own lap, everyone else's position (vrxc_elrs
    // onRaceLapRecorded), then FINISHED! when totalLaps is reached.
    osd::LapInfo li = {r.laps, r.holeshot, r.lapMs, r.bestLapMs, r.totalMs, positions[p.slot]};
    elrsLap(p.slot, li);
    for (int s = 0; s < MAX_ACTIVE; s++)
        if (s != p.slot && positions[s] > 0) elrsPosition(s, laps[s], positions[s]);
    if (r.justFinished) elrsPilotDone(p.slot, positions[p.slot], r.totalMs);
}

// All active slots in one message, 10 times a second. Sent with textAll like
// everything else: it only queues to connected clients, and the library
// closes a client whose queue fills (a dead phone), so one stale connection
// can't stop telemetry for everyone else.
static void sendTelemetry() {
    JsonDocument d;
    d["type"] = "rssi_all";
    d["ts"] = millis();
    JsonArray arr = d["p"].to<JsonArray>();
    {
        Guard g;
        for (int s = 0; s < MAX_ACTIVE; s++) {
            if (!rosterValid(activePilots[s])) continue;
            C5SlotLive l = c5Live(s);
            JsonObject o = arr.add<JsonObject>();
            o["pilot"]    = s;
            o["rssi"]     = l.filtered;
            o["crossing"] = l.inside;
            o["signal"]   = l.signal;
            o["en"]       = l.enters;   // edge counters for the gate tones
            o["ex"]       = l.exits;
        }
    }
    if (micOk()) d["mic"] = micPeak();   // loudest 20 ms since the last send (Calibration graph)
    if ((!arr.size() && !micOk()) || !ws.count()) return;
    String s;
    serializeJson(d, s);
    wsTelemetry(s);
}

static uint32_t startWaitLeft(uint32_t now);

void sendC5Status() {
    C5Status c = c5Status();
    ElrsStatus e = elrsStatus();
    JsonDocument d;
    d["type"]     = "c5";
    d["online"]   = c.online;
    d["scanning"] = c.scanning;
    d["state"]    = c.state;
    d["fw"]       = c.firmware;
    d["gain"]     = c.gain;
    d["records"]  = c.records;
    d["gaps"]     = c.seqGaps;
    d["bad"]      = c.badRecords;
    // The S3's own chip temperature (its internal sensor; runs 10-20 C above
    // the air around it), for the header.
    float t = temperatureRead();
    if (t == t && t > -40 && t < 150) d["cpuTemp"] = (int)lroundf(t);
    JsonArray rate = d["rate"].to<JsonArray>();
    for (int i = 0; i < 8; i++) rate.add(c.rate[i]);
    JsonObject el = d["elrs"].to<JsonObject>();
    el["enabled"]   = elrsCfg.enabled;
    el["connected"] = e.connected;
    el["peer"]      = e.peer;
    el["version"]   = e.version;
    el["bytes"]     = e.bytesSent;
    // The race as the timer sees it, once a second: a browser that missed a
    // stop, clear or lap puts itself right from this.
    JsonObject rs = d["race"].to<JsonObject>();
    {
        Guard g;
        uint32_t now = millis();
        rs["run"]    = raceCore.running();
        rs["paused"] = raceCore.paused();
        rs["el"]     = raceCore.elapsed(now);
        rs["now"]    = now;
        rs["laps"]   = raceCore.lapCount();
        rs["epoch"]  = raceEpoch;
        rs["wait"]   = startWaitLeft(now);   // a delayed start under way: ms to go
    }
    wsJson(d);
}

// ── Timed race ─────────────────────────────────────────────────────────────
// When the time runs out: with the last lap counted, each pilot's next pass
// ends their race (race::Core marks it, FINISHED! on the goggles); without,
// every pilot who had started is done at once. The race keeps running (the
// clock counts on below zero) until Stop is pressed.

static void serviceTimedRace(uint32_t now) {
    if (raceRules.rankMode != 2 && !timeUpHandled) return;   // not a timed race: nothing to do
    bool up, running;
    {
        Guard g;
        up = raceCore.timeUp(now);
        running = raceCore.running();
    }
    if (!running) return;
    if (up && !timeUpHandled) {
        timeUpHandled = true;
        Serial.println("[Race] time up");
        JsonDocument d;
        d["type"] = "time_up";
        wsJson(d);
        if (!raceRules.countLastLap) {
            int pos[MAX_ACTIVE];
            uint32_t tot[MAX_ACTIVE];
            bool done[MAX_ACTIVE];
            {
                Guard g;
                for (int s = 0; s < MAX_ACTIVE; s++) {
                    done[s] = raceCore.finishSlot(s);
                    pos[s] = raceCore.position(s);
                    tot[s] = raceCore.slot(s).totalMs;
                }
            }
            for (int s = 0; s < MAX_ACTIVE; s++)
                if (done[s]) elrsPilotDone(s, pos[s], tot[s]);
        }
    }
}

// ── Start / Stop (web buttons and the race director's TX switch) ───────────
// With a start delay set, a start first waits that long (race_wait: every
// browser counts it down and calls it out every 10 s), then runs the
// sequence below. Stop during the wait cancels the start.
// The firmware runs the whole start sequence, whoever asked for it, so every
// browser and every goggle follow one clock: race_ready (every browser says
// "レディ"), then the stage (ARM NOW and 3-2-1 on the goggles, the 3-2-1
// countdown on every browser), then the start 3.3 s later.
static volatile int16_t rdPending = -1;   // switch state waiting for raceLoop
static volatile int8_t  webPending = -1;  // web Start (1) / Stop (0) waiting for raceLoop
enum RdPhase : uint8_t { RD_IDLE, RD_WAIT, RD_READY, RD_COUNTDOWN };
static RdPhase  rdPhase = RD_IDLE;
static uint32_t rdNextAt = 0;
static const uint32_t RD_READY_MS     = 1500;   // time to say "レディ"
static const uint32_t RD_COUNTDOWN_MS = 3300;   // stage -> start, as the web UI

// Called from the ELRS link for each switch message.
// Toggle (a switch that stays where it is put): only a change counts
// (off -> on starts, on -> off stops). The TX may send its state again and
// again; counting a repeat restarted a race stopped from the web while the
// switch stayed on.
// Push (a button that springs back): every "on" is a press, the "off" of
// the release isn't needed. Waiting for off -> on missed every press after
// a release that didn't arrive (the bridge is on a pilot's UID for a moment
// while the OSD goes out), so a press needed doing twice. An "on" within
// RD_PRESS_MS of the last press is that same press, sent again.
static int8_t   rdLastState = -1;   // -1: nothing heard yet
static uint32_t rdLastPressAt = 0;
static bool     rdPressed = false;
static const uint32_t RD_PRESS_MS = 1500;

void raceDirectorSwitch(uint8_t state) {
    int8_t v = state ? 1 : 0;
    if (elrsRdSwitch == 1) {   // push
        if (!v) return;
        uint32_t now = millis();
        if (rdPressed && now - rdLastPressAt < RD_PRESS_MS) return;
        rdPressed = true;
        rdLastPressAt = now;
        rdPending = 1;
        return;
    }
    if (v == rdLastState) return;   // toggle
    rdLastState = v;
    rdPending = v;
}

static bool raceBusy() { return rdPhase != RD_IDLE || raceCore.running(); }
// A race is running or a start is under way (wait, レディ, 3-2-1).
bool raceActive() { return raceBusy(); }

// The start delay still to wait, ms (0: not waiting).
static uint32_t startWaitLeft(uint32_t now) {
    if (rdPhase != RD_WAIT) return 0;
    int32_t left = (int32_t)(rdNextAt - now);
    return left > 0 ? (uint32_t)left : 1;
}

static void rdReady() {
    JsonDocument d;
    d["type"] = "race_ready";
    wsJson(d);
    rdPhase = RD_READY;
    rdNextAt = millis() + RD_READY_MS;
}

static void rdStart() {
    if (raceBusy()) return;
    if (raceCore.paused()) {   // stopped mid-race: carry on, like Start in the UI
        raceResume();
        return;
    }
    uint16_t wait = startDelayOn ? startDelaySec : 0;
    if (!wait) {
        rdReady();
        return;
    }
    uint32_t now = millis();
    rdPhase = RD_WAIT;
    rdNextAt = now + (uint32_t)wait * 1000u;
    Serial.printf("[Race] start in %u s\n", wait);
    JsonDocument d;
    d["type"] = "race_wait";
    d["sec"]  = wait;
    d["now"]  = now;
    wsJson(d);
}

static void rdStop() {
    if (rdPhase == RD_IDLE && !raceCore.running()) return;
    bool goggles = rdPhase == RD_COUNTDOWN || raceCore.running();
    rdPhase = RD_IDLE;   // stopped before the start: drop it
    if (goggles) {
        raceStop();
        return;
    }
    // Cancelled during the start delay or "レディ": the goggles were told
    // nothing yet (no LAND NOW), the browsers end their countdown.
    Serial.println("[Race] start cancelled");
    JsonDocument d;
    d["type"] = "race_stop";
    d["elapsed"] = 0;
    wsJson(d);
}

void raceRequestStart() { webPending = 1; }
void raceRequestStop()  { webPending = 0; }

static void serviceRaceDirector() {
    int8_t w = webPending;
    if (w >= 0) {
        webPending = -1;
        w ? rdStart() : rdStop();
    }
    int16_t v = rdPending;
    if (v >= 0) {
        rdPending = -1;
        Serial.printf("[RD] switch %s (%s mode)\n", v ? "on" : "off", elrsRdSwitch == 1 ? "push" : "toggle");
        if (elrsRdSwitch == 1) {           // push: each press flips the race
            if (v == 1) raceBusy() ? rdStop() : rdStart();
        } else {                           // toggle: on = start, off = stop
            v ? rdStart() : rdStop();
        }
    }
    if (rdPhase != RD_IDLE && (int32_t)(millis() - rdNextAt) >= 0) {
        if (rdPhase == RD_WAIT) {
            rdReady();
        } else if (rdPhase == RD_READY) {
            raceStage();
            rdPhase = RD_COUNTDOWN;
            rdNextAt = millis() + RD_COUNTDOWN_MS;
        } else {
            rdPhase = RD_IDLE;
            raceStart();
        }
    }
}

// Manual laps (the admin's lap button): the press time per slot, taken by
// raceLoop() like a gate pass (cooldown, OSD, SD and browsers as usual).
static portMUX_TYPE manualMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t manualAt[MAX_ACTIVE];
static uint8_t  manualPending = 0;

bool raceManualLap(int slot) {
    if (slot < 0 || slot >= MAX_ACTIVE || !raceCore.running() || !rosterValid(activePilots[slot])) return false;
    portENTER_CRITICAL(&manualMux);
    manualAt[slot] = millis();
    manualPending |= (uint8_t)(1u << slot);
    portEXIT_CRITICAL(&manualMux);
    return true;
}

// ── Microphone: start on a loud sound, false start in the countdown ───────
// Each 20 ms window at or above a threshold extends a run; the run must
// last micCfg.startHoldMs / fsHoldMs. Start: only with no race at all (not running, not
// stopped mid-race, no start under way). False start: only between ARM NOW
// and GO; the race is stopped and cleared, every browser says why and the
// goggles show FALSE START.
static uint32_t micSeq = 0;
static uint16_t micFsRun = 0;

static void falseStart() {
    Serial.println("[Race] false start (microphone)");
    rdPhase = RD_IDLE;
    JsonDocument d;
    d["type"] = "false_start";
    wsJson(d);
    raceStop();
    raceClear();
    elrsAnnounce("FALSE START");
}

// Threshold crossings (sound_count.h): sounds at or above the start
// threshold for at least startHoldMs, in a series. A series of exactly
// startCount starts the race (with no race on); one of 3 clears a stopped
// race (with clapClear).
static const uint8_t CLEAR_COUNT = 3;
static sound::CrossCounter crossings;

static void micCommand(const char* cmd) {
    JsonDocument d;
    d["type"] = "mic_cmd";   // every browser says what the sound did
    d["cmd"]  = cmd;
    wsJson(d);
}

static void serviceCrossings(uint32_t now, uint8_t lvl, uint16_t add) {
    uint8_t before = crossings.count();
    uint8_t n = crossings.feed(now, lvl >= micCfg.startLvl, add, micCfg.startHoldMs);
    bool idle, stopped;
    {
        Guard g;
        idle = rdPhase == RD_IDLE && !raceCore.running() && !raceCore.paused();
        stopped = rdPhase == RD_IDLE && !raceCore.running() && (raceCore.paused() || raceCore.lapCount() > 0);
    }
    // Start on the signal itself (startNow): GO as soon as the set count is
    // reached, the clock from when that sound began; no delay, no countdown.
    bool quietOk = (int32_t)(now - micQuietUntil) >= 0;
    if (micCfg.start && micCfg.startNow && idle && quietOk && crossings.count() != before &&
        crossings.count() == micCfg.startCount) {
        uint32_t at = crossings.lastRise();
        crossings.reset();
        Serial.printf("[Mic] start now on %u crossings\n", micCfg.startCount);
        micCommand("start_now");
        raceStartAt(at);
        return;
    }
    if (!n) return;
    Serial.printf("[Mic] %u threshold crossings\n", n);
    if (n == micCfg.startCount && micCfg.start && !micCfg.startNow && idle && quietOk) {
        micCommand("start");
        rdStart();
    } else if (n == CLEAR_COUNT && micCfg.clapClear && stopped) {
        micCommand("clear");
        raceClear();
    }
}

static void serviceMic(uint32_t now) {
    if (!micOk() || (!micCfg.start && !micCfg.falseStart && !micCfg.clapClear)) return;
    uint8_t lvl;
    uint32_t seq;
    if (!micLatest(lvl, seq) || seq == micSeq) return;
    uint32_t n = seq - micSeq;   // windows since the last look (normally 1)
    micSeq = seq;
    uint16_t add = n > 50 ? 1000 : (uint16_t)(n * 20);
    serviceCrossings(now, lvl, add);
    micFsRun = (micCfg.falseStart && rdPhase == RD_COUNTDOWN && lvl >= micCfg.fsLvl) ? micFsRun + add : 0;
    if (micFsRun >= micCfg.fsHoldMs) {
        micFsRun = 0;
        falseStart();
    }
}

void raceLoop() {
    serviceRaceDirector();
    serviceMic(millis());
    C5Pass p;
    while (c5PopPass(&p)) onPass(p);
    if (manualPending) {
        uint8_t pending;
        uint32_t at[MAX_ACTIVE];
        portENTER_CRITICAL(&manualMux);
        pending = manualPending;
        memcpy(at, manualAt, sizeof at);
        manualPending = 0;
        portEXIT_CRITICAL(&manualMux);
        for (int s = 0; s < MAX_ACTIVE; s++)
            if (pending & (1u << s)) {
                Serial.printf("[Race] slot %d manual lap\n", s);
                onPass(C5Pass{(uint8_t)s, at[s], 0});
            }
    }
    uint32_t now = millis();
    serviceTimedRace(now);
    if (now - lastRssiSend >= RSSI_INTERVAL_MS) {
        lastRssiSend = now;
        sendTelemetry();
    }
    if (now - lastStatusSend >= 1000) {
        lastStatusSend = now;
        sendC5Status();
    }
}
