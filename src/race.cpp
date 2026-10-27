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
    }
    c5Configure(mhz, en, ex);
    c5SetGain(settings.gain);
    elrsConfigChanged();
}

void raceBegin() { applyGateConfig(); }

static void wsJson(JsonDocument& d) {
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

void raceStart() {
    uint32_t now = millis();
    {
        Guard g;
        raceCore.start(now);
    }
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
    char resp[80];
    snprintf(resp, sizeof resp, R"({"ok":true,"saved":%s,"reason":"%s"})", saved ? "true" : "false", reason);
    return String(resp);
}

static void onPass(const C5Pass& p) {
    race::PassResult r;
    int ri;
    int positions[MAX_ACTIVE], laps[MAX_ACTIVE];
    {
        Guard g;
        ri = activePilots[p.slot];
        if (!rosterValid(ri)) return;   // nobody in this slot
        if (!raceCore.pass(p.slot, ri, p.tsMs, p.peak, &r)) return;
        for (int s = 0; s < MAX_ACTIVE; s++) positions[s] = raceCore.position(s), laps[s] = raceCore.laps(s);
    }
    Serial.printf("[Race] slot %d pass  lap=%lu  lapMs=%lu  peak=%u\n", p.slot, (unsigned long)r.lapCount,
                  (unsigned long)r.lapMs, p.peak);

    JsonDocument d;
    d["type"]     = "lap";
    d["pilot"]    = p.slot;
    d["name"]     = activeName(p.slot);
    d["yomi"]     = roster[ri].yomi;
    d["lapTime"]  = r.lapMs;
    d["bestLap"]  = r.bestLapMs;
    d["lapCount"] = r.lapCount;
    d["newBest"]  = r.newBest;
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

void raceLoop() {
    C5Pass p;
    while (c5PopPass(&p)) onPass(p);
    uint32_t now = millis();
    if (now - lastRssiSend >= RSSI_INTERVAL_MS) {
        lastRssiSend = now;
        sendTelemetry();
    }
    if (now - lastStatusSend >= 1000) {
        lastStatusSend = now;
        sendC5Status();
    }
}
