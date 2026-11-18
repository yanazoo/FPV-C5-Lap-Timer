#include "pilot_link.h"
#include "auto_assign.h"
#include "data_model.h"
#include "msp_v2.h"
#include "nvs_store.h"
#include "race.h"
#include "ws_handler.h"

uint8_t assignMode = ASSIGN_MANUAL;

static_assert(autoassign::kSlots == MAX_ACTIVE, "slot count");

namespace {

// Radios heard, by UID. Written by the ELRS task, read by raceLoop.
constexpr int kRadios = 24;
struct Radio {
    bool             used;
    uint8_t          uid[6];
    autoassign::Link link;
};
Radio        radios[kRadios];
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t     lastReportAt = 0;   // any report (0: none since boot)

// Per roster entry, from the last pilotLinkLoop (for the status).
struct PilotState {
    bool    heard;    // a radio with this pilot's UID reported
    bool    linked;
    uint8_t lq;
    uint8_t wait;     // autoassign::Wait
    bool    shared;   // UID not distinct
};
PilotState pstate[MAX_REGISTERED];
uint32_t   lastLoop = 0;

int findRadio(const uint8_t uid[6]) {
    for (int i = 0; i < kRadios; i++)
        if (radios[i].used && memcmp(radios[i].uid, uid, 6) == 0) return i;
    return -1;
}

}  // namespace

void pilotLinkReport(const uint8_t uid[6], uint8_t lq) {
    uint32_t now = millis();
    portENTER_CRITICAL(&mux);
    int i = findRadio(uid);
    if (i < 0) {
        // A free entry, else the one heard longest ago.
        for (int k = 0; k < kRadios; k++) {
            if (!radios[k].used) { i = k; break; }
            if (i < 0 || (int32_t)(radios[k].link.seenAt - radios[i].link.seenAt) < 0) i = k;
        }
        radios[i] = Radio{};
        radios[i].used = true;
        memcpy(radios[i].uid, uid, 6);
    }
    autoassign::report(radios[i].link, now, lq);
    lastReportAt = now ? now : 1;
    portEXIT_CRITICAL(&mux);
}

static bool raceBusy() {
    if (raceActive()) return true;
    Guard g;
    return raceCore.running() || raceCore.paused() || raceCore.lapCount() > 0;
}

void pilotLinkLoop(uint32_t now) {
    if (now - lastLoop < 500) return;
    lastLoop = now;

    autoassign::Pilot pilots[MAX_REGISTERED];
    autoassign::Link  links[MAX_REGISTERED];
    uint8_t           uids[MAX_REGISTERED][6];
    bool              heard[MAX_REGISTERED] = {};
    int               active[MAX_ACTIVE];
    int               n;
    uint8_t           mode;
    {
        Guard g;
        n = rosterCount;
        mode = assignMode;
        memcpy(active, activePilots, sizeof active);
        uint8_t rd[6];
        bool rdOn = elrsCfg.rdBind[0];
        if (rdOn) msp::bindPhraseUid(elrsCfg.rdBind, rd);
        for (int i = 0; i < n; i++) {
            msp::bindPhraseUid(roster[i].bind[0] ? roster[i].bind : roster[i].name, uids[i]);
            pilots[i].valid = true;
            pilots[i].freq = roster[i].freqMhz;
            pilots[i].distinct = !(rdOn && memcmp(uids[i], rd, 6) == 0);
        }
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            if (i != j && memcmp(uids[i], uids[j], 6) == 0) pilots[i].distinct = false;
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < n; i++) {
        int r = findRadio(uids[i]);
        if (r >= 0) links[i] = radios[r].link, heard[i] = true;
    }
    portEXIT_CRITICAL(&mux);

    uint8_t wait[MAX_REGISTERED] = {};
    autoassign::Result res;
    bool changed = false;
    if (mode == ASSIGN_AUTO && !raceBusy()) {
        changed = autoassign::plan(now, pilots, links, n, active, wait, res);
    }
    for (int i = 0; i < n; i++) {
        pstate[i].heard = heard[i];
        pstate[i].linked = autoassign::linked(links[i], now);
        pstate[i].lq = links[i].lq;
        pstate[i].wait = wait[i];
        pstate[i].shared = !pilots[i].distinct;
    }
    if (!changed) return;
    {
        Guard g;
        // The roster may have changed since the copy: apply only if not.
        if (rosterCount != n || raceCore.running() || raceCore.paused() || raceCore.lapCount() > 0) return;
        memcpy(activePilots, active, sizeof active);
        saveActive();
    }
    applyGateConfig();
    wsActiveUpdate();
    JsonDocument d;
    d["type"] = "auto_assign";
    JsonArray j = d["joined"].to<JsonArray>();
    for (int k = 0; k < res.nJoined; k++) j.add(res.joined[k]);
    JsonArray l = d["left"].to<JsonArray>();
    for (int k = 0; k < res.nLeft; k++) l.add(res.left[k]);
    String msg;
    serializeJson(d, msg);
    wsText(msg);
    for (int k = 0; k < res.nJoined; k++) Serial.printf("[Link] pilot %d linked: on a channel\n", res.joined[k]);
    for (int k = 0; k < res.nLeft; k++) Serial.printf("[Link] pilot %d unlinked: off its channel\n", res.left[k]);
}

void pilotLinkToJson(JsonObject el, JsonArray links) {
    uint32_t at;
    portENTER_CRITICAL(&mux);
    at = lastReportAt;
    portEXIT_CRITICAL(&mux);
    // Seconds since the bridge last reported a radio (-1: never).
    el["linkAge"] = at ? (int)((millis() - at) / 1000) : -1;
    Guard g;
    for (int i = 0; i < rosterCount; i++) {
        const PilotState& p = pstate[i];
        if (!p.heard) continue;
        JsonObject o = links.add<JsonObject>();
        o["id"] = i;
        o["lq"] = p.linked ? p.lq : 0;
        o["on"] = p.linked;
        if (p.wait) o["wait"] = p.wait;
    }
}
