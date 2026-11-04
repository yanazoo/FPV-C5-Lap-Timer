#include <ArduinoJson.h>
#include "race.h"
#include "c5_link.h"
#include "data_model.h"
#include "elrs.h"
#include "sd_store.h"
#include "ws_handler.h"

race::Core raceCore;

static uint32_t lastRssiSend = 0;
static uint32_t lastStatusSend = 0;

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
static uint32_t timeUpAt = 0;

void raceStart() {
    uint32_t now = millis();
    {
        Guard g;
        applyRaceRules();
        raceCore.start(now);
    }
    timeUpHandled = false;
    c5ResetDetectors();
    elrsRaceStart(0);
    JsonDocument d;
    d["type"] = "race_start";
    d["ts"] = now;
    wsJson(d);
}

bool raceResume() {
    uint32_t elapsed;
    {
        Guard g;
        if (!raceCore.resume(millis())) return false;
        elapsed = raceCore.elapsed(millis());
    }
    elrsRaceStart(elapsed);
    JsonDocument d;
    d["type"] = "race_resume";
    d["ts"] = raceCore.startMs();
    wsJson(d);
    return true;
}

void raceStop() {
    {
        Guard g;
        raceCore.stop(millis());
    }
    elrsRaceStop();
    JsonDocument d;
    d["type"] = "race_stop";
    wsJson(d);
}

String raceClear() {
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
            o["name"]     = activeName(s);
            o["rssi"]     = l.filtered;
            o["raw"]      = l.raw;
            o["crossing"] = l.inside;
            o["signal"]   = l.signal;
            o["en"]       = l.enters;   // edge counters for the gate tones
            o["ex"]       = l.exits;
        }
    }
    if (arr.size()) wsJson(d);
}

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
    JsonArray rate = d["rate"].to<JsonArray>();
    for (int i = 0; i < 8; i++) rate.add(c.rate[i]);
    JsonObject el = d["elrs"].to<JsonObject>();
    el["enabled"]   = elrsCfg.enabled;
    el["connected"] = e.connected;
    el["peer"]      = e.peer;
    el["version"]   = e.version;
    el["bytes"]     = e.bytesSent;
    wsJson(d);
}

// ── Timed race ─────────────────────────────────────────────────────────────
// When the time runs out: with the last lap counted, each pilot's next pass
// ends their race (race::Core marks it, FINISHED! on the goggles); without,
// every pilot who had started is done at once. The race stops by itself
// once every pilot who started is done, or 60 s after the time ran out
// (someone crashed on the last lap).
static const uint32_t TIME_UP_GRACE_MS = 60000;

static void serviceTimedRace(uint32_t now) {
    bool up, running;
    {
        Guard g;
        up = raceCore.timeUp(now);
        running = raceCore.running();
    }
    if (!running) return;
    if (up && !timeUpHandled) {
        timeUpHandled = true;
        timeUpAt = now;
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
    if (!timeUpHandled) return;
    bool allDone = true;
    {
        Guard g;
        for (int s = 0; s < MAX_ACTIVE; s++) {
            const race::SlotState& st = raceCore.slot(s);
            if (rosterValid(activePilots[s]) && st.lapCount > 0 && !st.finished) allDone = false;
        }
    }
    if (allDone || now - timeUpAt >= TIME_UP_GRACE_MS) raceStop();
}

// ── Race director (ELRS TX switch) ─────────────────────────────────────────
// Countdown run by the firmware itself when the race director starts the
// race (no browser drives it): stage now, start 3.3 s later, the same
// timing as the web UI's Start button.
static volatile int16_t rdPending = -1;   // switch state waiting for raceLoop
static bool     rdCountdown = false;
static uint32_t rdStartAt = 0;
static const uint32_t RD_COUNTDOWN_MS = 3300;

void raceDirectorSwitch(uint8_t state) { rdPending = state ? 1 : 0; }

static bool raceBusy() { return rdCountdown || raceCore.running(); }

static void rdStart() {
    if (raceBusy()) return;
    if (raceCore.paused()) {   // stopped mid-race: carry on, like Start in the UI
        raceResume();
        return;
    }
    raceStage();
    rdCountdown = true;
    rdStartAt = millis() + RD_COUNTDOWN_MS;
}

static void rdStop() {
    if (rdCountdown) rdCountdown = false;   // stopped during the countdown
    else if (!raceCore.running()) return;
    raceStop();
}

static void serviceRaceDirector() {
    int16_t v = rdPending;
    if (v >= 0) {
        rdPending = -1;
        Serial.printf("[RD] switch %s\n", v ? "on" : "off");
        if (elrsRdSwitch == 1) {           // push: each press flips the race
            if (v == 1) raceBusy() ? rdStop() : rdStart();
        } else {                           // toggle: on = start, off = stop
            v ? rdStart() : rdStop();
        }
    }
    if (rdCountdown && (int32_t)(millis() - rdStartAt) >= 0) {
        rdCountdown = false;
        raceStart();
    }
}

void raceLoop() {
    serviceRaceDirector();
    C5Pass p;
    while (c5PopPass(&p)) onPass(p);
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
