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

// Radios heard, by UID. Written by the ELRS task, read by raceLoop / web.
constexpr int      kRadios      = 24;
constexpr uint32_t kCandidateMs = 60000;   // an unregistered radio is listed this long after last heard
constexpr uint32_t kVtxAskMs    = 20000;   // ask a linked radio for its VTX this often
constexpr uint8_t  kVtxAsks     = 3;       // and at most this many times
struct Radio {
    bool             used;
    uint8_t          uid[6];
    uint32_t         heardAt;   // any report
    autoassign::Link link;
    uint16_t         vtx;       // MHz from its VTX Administrator, 0 = not heard
    uint32_t         vtxAskAt;
    uint8_t          vtxAsks;
};
Radio        radios[kRadios];
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t     lastReportAt = 0;   // any link report (0: none since boot)

// Per roster entry, from the last pilotLinkLoop (for the status).
struct PilotState {
    bool     heard;    // a radio with this pilot's UID reported
    bool     linked;
    uint8_t  lq;
    uint8_t  wait;     // autoassign::Wait
    uint16_t vtx;      // the radio's VTX setting (0: not heard)
};
PilotState pstate[MAX_REGISTERED];
uint32_t   lastLoop = 0;

int findRadio(const uint8_t uid[6]) {
    for (int i = 0; i < kRadios; i++)
        if (radios[i].used && memcmp(radios[i].uid, uid, 6) == 0) return i;
    return -1;
}

// Under mux: the entry for this UID, made if new (a free one, else the one
// heard longest ago).
Radio& radioFor(const uint8_t uid[6], uint32_t now) {
    int i = findRadio(uid);
    if (i < 0) {
        for (int k = 0; k < kRadios; k++) {
            if (!radios[k].used) { i = k; break; }
            if (i < 0 || (int32_t)(radios[k].heardAt - radios[i].heardAt) < 0) i = k;
        }
        radios[i] = Radio{};
        radios[i].used = true;
        memcpy(radios[i].uid, uid, 6);
    }
    radios[i].heardAt = now;
    return radios[i];
}

// Roster UIDs and the race director's (caller holds gLock).
int rosterUids(uint8_t uids[][6], uint8_t rd[6], bool& rdOn) {
    rdOn = elrsCfg.rdBind[0];
    if (rdOn) msp::bindPhraseUid(elrsCfg.rdBind, rd);
    for (int i = 0; i < rosterCount; i++)
        msp::bindPhraseUid(roster[i].bind[0] ? roster[i].bind : roster[i].name, uids[i]);
    return rosterCount;
}

// A UID's first 3 bytes as 6 hex digits: enough to tell radios apart and
// to check a typed bind phrase. Browsers (viewers too) get only this: the
// whole UID is what an ELRS receiver binds to.
void uidTag(const uint8_t uid[6], char out[7]) {
    for (int k = 0; k < 3; k++) snprintf(out + 2 * k, 3, "%02x", uid[k]);
}

}  // namespace

void pilotLinkReport(const uint8_t uid[6], uint8_t lq) {
    uint32_t now = millis();
    if (!now) now = 1;
    portENTER_CRITICAL(&mux);
    autoassign::report(radioFor(uid, now).link, now, lq);
    lastReportAt = now;
    portEXIT_CRITICAL(&mux);
}

void pilotVtxReport(const uint8_t uid[6], uint16_t mhz) {
    if (!mhz) return;
    uint32_t now = millis();
    if (!now) now = 1;
    portENTER_CRITICAL(&mux);
    radioFor(uid, now).vtx = mhz;
    portEXIT_CRITICAL(&mux);
}

bool pilotVtxWanted(uint8_t uid[6], uint32_t now) {
    bool found = false;
    portENTER_CRITICAL(&mux);
    for (Radio& r : radios) {
        if (!r.used || r.vtx || r.vtxAsks >= kVtxAsks || !autoassign::linked(r.link, now)) continue;
        if (r.vtxAsks && now - r.vtxAskAt < kVtxAskMs) continue;
        r.vtxAsks++;
        r.vtxAskAt = now;
        memcpy(uid, r.uid, 6);
        found = true;
        break;
    }
    portEXIT_CRITICAL(&mux);
    return found;
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
    uint16_t          vtx[MAX_REGISTERED] = {};
    uint8_t           uids[MAX_REGISTERED][6];
    bool              heard[MAX_REGISTERED] = {};
    int               active[MAX_ACTIVE];
    int               n;
    uint8_t           mode;
    uint8_t           rd[6];
    bool              rdOn;
    {
        Guard g;
        n = rosterUids(uids, rd, rdOn);
        mode = assignMode;
        memcpy(active, activePilots, sizeof active);
        for (int i = 0; i < n; i++) {
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
        if (r >= 0) links[i] = radios[r].link, vtx[i] = radios[r].vtx, heard[i] = true;
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
        pstate[i].vtx = vtx[i];
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

void pilotLinkToJson(JsonObject el, JsonArray links, JsonArray cand) {
    uint32_t now = millis();
    uint8_t uids[MAX_REGISTERED][6], rd[6];
    bool rdOn;
    int n;
    {
        Guard g;
        n = rosterUids(uids, rd, rdOn);
        for (int i = 0; i < n; i++) {
            const PilotState& p = pstate[i];
            if (!p.heard) continue;
            JsonObject o = links.add<JsonObject>();
            o["id"] = i;
            o["lq"] = p.linked ? p.lq : 0;
            o["on"] = p.linked;
            if (p.wait) o["wait"] = p.wait;
            if (p.vtx) o["vtx"] = p.vtx;
        }
    }
    // Radios heard lately that are nobody's on the roster (nor the race
    // director's): candidates for a new pilot. Linked ones first.
    Radio copy[kRadios];
    uint32_t at;
    portENTER_CRITICAL(&mux);
    memcpy(copy, radios, sizeof copy);
    at = lastReportAt;
    portEXIT_CRITICAL(&mux);
    // Seconds since the bridge last reported a radio (-1: never).
    el["linkAge"] = at ? (int)((now - at) / 1000) : -1;
    int listed = 0;
    for (int pass = 0; pass < 2 && listed < 8; pass++) {
        for (const Radio& r : copy) {
            if (!r.used || now - r.heardAt > kCandidateMs) continue;
            bool on = autoassign::linked(r.link, now);
            if (on != (pass == 0)) continue;
            bool known = rdOn && memcmp(r.uid, rd, 6) == 0;
            for (int i = 0; i < n && !known; i++) known = memcmp(r.uid, uids[i], 6) == 0;
            if (known) continue;
            char tag[7];
            uidTag(r.uid, tag);
            JsonObject o = cand.add<JsonObject>();
            o["tag"] = tag;
            o["on"] = on;
            o["lq"] = on ? r.link.lq : 0;
            if (r.vtx) o["vtx"] = r.vtx;
            o["age"] = (now - r.heardAt) / 1000;
            if (++listed >= 8) break;
        }
    }
}

String bindUidTag(const char* phrase) {
    uint8_t uid[6];
    msp::bindPhraseUid(phrase, uid);
    char tag[7];
    uidTag(uid, tag);
    return String(tag);
}
