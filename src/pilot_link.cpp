#include "pilot_link.h"
#include "auto_assign.h"
#include "data_model.h"
#include "elrs.h"
#include "msp_v2.h"
#include "nvs_store.h"
#include "race.h"
#include "ws_handler.h"

uint8_t assignMode = ASSIGN_MANUAL;
bool    vtxAuto = true;

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
    bool             vtxNew;    // vtx just arrived, not yet taken for a pilot (vtxAuto)
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
// When ESP-NOW reception (elrsLinkWatch) last began, 0 = off. Assignment
// waits kSettleMs after it comes back, until the radios have reported again
// (else pilots not heard during the outage would leave their channels).
uint32_t   watchSince = 0;
// Auto in races: the pilot (roster index) whose link went during this race,
// per slot; -1 none. Not measured (applyGateConfig), the pilot and laps stay
// until the race is cleared. Only while that same pilot is on the slot: a
// pilot put there by hand is measured. Under gLock.
int8_t     slotOutWho[MAX_ACTIVE] = {-1, -1, -1, -1, -1, -1, -1, -1};
inline bool outNow(int s, const int* active) { return slotOutWho[s] >= 0 && slotOutWho[s] == active[s]; }
constexpr uint32_t kSettleMs = autoassign::kSeenMs + 1000;

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
        rosterUidOf(i, uids[i]);
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
    Radio& r = radioFor(uid, now);
    r.vtx = mhz;
    r.vtxNew = true;
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

// Some radio reported lately. In a race, nobody goes out while none does:
// a bridge that answers but hears nothing would stop every slot.
static bool reportsFresh(uint32_t now) {
    portENTER_CRITICAL(&mux);
    uint32_t at = lastReportAt;
    portEXIT_CRITICAL(&mux);
    return at && now - at <= autoassign::kSeenMs;
}

static bool raceBusy() {
    if (raceActive()) return true;
    Guard g;
    return raceCore.running() || raceCore.paused() || raceCore.lapCount() > 0;
}

// vtxAuto: VTX settings just received from a registered pilot's radio become
// the pilot's VTX frequency. Not during a race or with laps not cleared (the
// channel measured would change under the race): kept until after Clear.
// Not for a UID two pilots share, nor outside the C5's 5180-5917 MHz.
static void applyRadioVtx() {
    if (!vtxAuto || !elrsLinkWatch() || raceBusy()) return;
    struct New { uint8_t uid[6]; uint16_t mhz; } got[kRadios];
    int nGot = 0;
    portENTER_CRITICAL(&mux);
    for (Radio& r : radios)
        if (r.used && r.vtxNew) {
            r.vtxNew = false;
            memcpy(got[nGot].uid, r.uid, 6);
            got[nGot++].mhz = r.vtx;
        }
    portEXIT_CRITICAL(&mux);
    if (!nGot) return;
    int changed[kRadios], nChanged = 0;
    {
        Guard g;
        uint8_t uids[MAX_REGISTERED][6], rd[6];
        bool rdOn;
        int n = rosterUids(uids, rd, rdOn);
        for (int k = 0; k < nGot; k++) {
            uint16_t mhz = got[k].mhz;
            if (mhz < 5180 || mhz > 5917) continue;
            int who = -1, count = 0;
            for (int i = 0; i < n; i++)
                if (memcmp(uids[i], got[k].uid, 6) == 0) who = i, count++;
            if (count != 1 || roster[who].freqMhz == mhz) continue;
            // Another pilot on a channel uses that frequency: both slots
            // would count each other's passes. Left to the tap (▶ 合わせる).
            bool clash = false;
            for (int s = 0; s < MAX_ACTIVE; s++) {
                int a = activePilots[s];
                if (rosterValid(a) && a != who && roster[a].freqMhz == mhz) clash = true;
            }
            if (clash) {
                Serial.printf("[Link] pilot %d VTX %u MHz: another pilot on a channel uses it\n", who, mhz);
                continue;
            }
            Serial.printf("[Link] pilot %d VTX %u -> %u MHz (from the radio)\n", who, roster[who].freqMhz, mhz);
            roster[who].freqMhz = mhz;
            saveRosterPilot(who);
            changed[nChanged++] = who;
        }
    }
    if (!nChanged) return;
    applyGateConfig();
    wsActiveUpdate();
    for (int k = 0; k < nChanged; k++) {
        JsonDocument d;
        d["type"] = "vtx_auto";
        d["id"] = changed[k];
        {
            Guard g;
            d["freq"] = roster[changed[k]].freqMhz;
        }
        String msg;
        serializeJson(d, msg);
        wsText(msg);
    }
}

void pilotLinkLoop(uint32_t now) {
    if (now - lastLoop < 500) return;
    lastLoop = now;
    bool watching = elrsLinkWatch();
    if (!watching) watchSince = 0;
    else if (!watchSince) watchSince = now ? now : 1;
    bool settled = watching && now - watchSince >= kSettleMs;
    applyRadioVtx();

    autoassign::Pilot pilots[MAX_REGISTERED];
    autoassign::Link  links[MAX_REGISTERED];
    uint16_t          vtx[MAX_REGISTERED] = {};
    uint8_t           uids[MAX_REGISTERED][6];
    bool              heard[MAX_REGISTERED] = {};
    int               active[MAX_ACTIVE], before[MAX_ACTIVE];
    int               n;
    uint8_t           mode;
    uint8_t           rd[6];
    bool              rdOn;
    {
        Guard g;
        n = rosterUids(uids, rd, rdOn);
        mode = assignMode;
        memcpy(active, activePilots, sizeof active);
        memcpy(before, activePilots, sizeof before);
        for (int i = 0; i < n; i++) {
            pilots[i].valid = true;
            pilots[i].freq = roster[i].freqMhz;
            // The race director flying with the same radio is fine: only a UID
            // two pilots share leaves the radio's pilot unknown.
            pilots[i].distinct = true;
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
    autoassign::RaceResult rr;
    bool busy = raceBusy();
    bool out[MAX_ACTIVE], used[MAX_ACTIVE];
    int  left[MAX_ACTIVE], nLeft = 0;
    {
        Guard g;
        for (int s = 0; s < MAX_ACTIVE; s++) out[s] = outNow(s, active);
        for (int s = 0; s < MAX_ACTIVE; s++) used[s] = raceCore.slot(s).lapCount > 0 || out[s];
    }
    bool anyOut = false;
    for (bool o : out) anyOut |= o;
    bool changed = false;
    if (anyOut && (!busy || mode != ASSIGN_AUTO_RACE)) {
        // The race is cleared: those out leave their channels now (auto in
        // races), or the mode changed: everyone is measured again.
        for (int s = 0; s < MAX_ACTIVE; s++) {
            if (!out[s]) continue;
            out[s] = false;
            if (!busy && active[s] >= 0) left[nLeft++] = active[s], active[s] = -1;
        }
        changed = true;
    }
    // No ESP-NOW (ELRS off, no bridge, or one too old to watch): manual.
    // Otherwise every pilot would leave their channel, their links unheard.
    if (settled && mode != ASSIGN_MANUAL) {
        if (!busy) {
            changed |= autoassign::plan(now, pilots, links, n, active, wait, res);
        } else if (mode == ASSIGN_AUTO_RACE && reportsFresh(now)) {
            changed |= autoassign::planRace(now, pilots, links, n, active, out, used, wait, rr);
        }
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
        // The roster may have changed since the copy, or a race begun or
        // ended: apply only if not.
        bool nowBusy = raceCore.running() || raceCore.paused() || raceCore.lapCount() > 0;
        // A browser changed the channels meanwhile: theirs stands, plan again.
        if (rosterCount != n || memcmp(activePilots, before, sizeof before) != 0) return;
        if (!busy && nowBusy) return;   // a race began since: plan() is for between races
        memcpy(activePilots, active, sizeof active);
        for (int s = 0; s < MAX_ACTIVE; s++) slotOutWho[s] = out[s] ? (int8_t)active[s] : (int8_t)-1;
        saveActive();
    }
    applyGateConfig();
    wsActiveUpdate();
    JsonDocument d;
    d["type"] = "auto_assign";
    JsonArray j = d["joined"].to<JsonArray>();
    for (int k = 0; k < res.nJoined; k++) j.add(res.joined[k]);
    for (int k = 0; k < rr.nJoined; k++) j.add(rr.joined[k]);
    JsonArray l = d["left"].to<JsonArray>();
    for (int k = 0; k < res.nLeft; k++) l.add(res.left[k]);
    for (int k = 0; k < nLeft; k++) l.add(left[k]);
    JsonArray o = d["out"].to<JsonArray>();
    for (int k = 0; k < rr.nOut; k++) o.add(rr.out[k]);
    JsonArray bk = d["back"].to<JsonArray>();
    for (int k = 0; k < rr.nBack; k++) bk.add(rr.back[k]);
    String msg;
    serializeJson(d, msg);
    wsText(msg);
    for (int k = 0; k < res.nJoined; k++) Serial.printf("[Link] pilot %d linked: on a channel\n", res.joined[k]);
    for (int k = 0; k < rr.nJoined; k++) Serial.printf("[Link] pilot %d linked: on a channel (race)\n", rr.joined[k]);
    for (int k = 0; k < res.nLeft; k++) Serial.printf("[Link] pilot %d unlinked: off its channel\n", res.left[k]);
    for (int k = 0; k < rr.nOut; k++) Serial.printf("[Link] pilot %d unlinked: not measured (race)\n", rr.out[k]);
    for (int k = 0; k < rr.nBack; k++) Serial.printf("[Link] pilot %d linked again: measured (race)\n", rr.back[k]);
}

bool pilotSlotOut(int slot) {   // caller holds gLock
    return slot >= 0 && slot < MAX_ACTIVE && outNow(slot, activePilots);
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
    // Slots not measured (auto in races, link gone), as a bit mask.
    {
        Guard g;
        int mask = 0;
        for (int s = 0; s < MAX_ACTIVE; s++) if (outNow(s, activePilots)) mask |= 1 << s;
        el["out"] = mask;
    }
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

bool pilotRadioByTag(const char* tag, uint8_t uid[6]) {
    bool found = false;
    portENTER_CRITICAL(&mux);
    for (const Radio& r : radios) {
        if (!r.used) continue;
        char t[7];
        uidTag(r.uid, t);
        if (strcmp(t, tag) == 0) { memcpy(uid, r.uid, 6); found = true; break; }
    }
    portEXIT_CRITICAL(&mux);
    return found;
}

String bindUidTag(const char* phrase) {
    uint8_t uid[6];
    msp::bindPhraseUid(phrase, uid);
    char tag[7];
    uidTag(uid, tag);
    return String(tag);
}
