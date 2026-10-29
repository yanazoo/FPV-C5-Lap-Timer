#include "osd_engine.h"

#include <stdio.h>
#include <string.h>

#include "msp_v2.h"

namespace osd {

namespace {

// Goggle fonts are ASCII; lower case letters are symbols (vrxc_elrs uses
// "w"/"x" as flags in its messages), so user text is upper-cased.
void sanitize(const char* in, char* out, size_t cap, bool upper) {
    size_t n = 0;
    for (const char* p = in; *p && n + 1 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c >= 0x7F) continue;   // drop UTF-8 (e.g. Japanese names) and controls
        if (upper && c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        out[n++] = (char)c;
    }
    out[n] = '\0';
}

}  // namespace

void Engine::formatTime(uint32_t ms, char* buf, size_t cap) {
    snprintf(buf, cap, "%lu:%02lu.%lu", (unsigned long)(ms / 60000), (unsigned long)((ms / 1000) % 60),
             (unsigned long)((ms % 1000) / 100));
}

int Engine::centerCol(size_t len) {
    int c = kCols / 2 - (int)(len / 2);
    return c > 0 ? c : 0;
}

void Engine::setRecipient(int idx, bool enabled, const uint8_t uid[6]) {
    if (idx < 0 || idx >= kRecipients) return;
    bool changed = enabled != rcpt_[idx].enabled || (enabled && memcmp(uid, rcpt_[idx].uid, 6) != 0);
    rcpt_[idx].enabled = enabled;
    if (enabled) memcpy(rcpt_[idx].uid, uid, 6);
    // A different recipient (new pilot in the slot) starts from a blank screen.
    if (changed) {
        clearFrame(idx);
        memset(shown_[idx], 0, sizeof shown_[idx]);
        memset(blankResends_[idx], 0, sizeof blankResends_[idx]);
        memset(sentHash_[idx], 0, sizeof sentHash_[idx]);
        refreshLeft_[idx] = 0;
    }
}

bool Engine::isBroadcast(int r) const { return rcpt_[r].enabled; }

// ── frame editing ──────────────────────────────────────────────────────────

void Engine::clearFrame(int r) { memset(&frames_[r], 0, sizeof frames_[r]); }

void Engine::clearRow(int r, int row) {
    if (row < 0 || row >= kRows) return;
    frames_[r].rows[row].n = 0;
}

void Engine::setText(int r, Pos pos, const char* text) {
    if (pos.row < 0 || pos.row >= kRows) return;
    char clean[kTextMax + 1];
    sanitize(text, clean, sizeof clean, false);
    int col = pos.col < 0 ? centerCol(strlen(clean)) : pos.col;
    Row& row = frames_[r].rows[pos.row];
    Cell* cell = nullptr;
    for (int i = 0; i < row.n; ++i)
        if (row.cells[i].col == col) cell = &row.cells[i];
    if (!cell) cell = row.n < kCellsPerRow ? &row.cells[row.n++] : &row.cells[kCellsPerRow - 1];
    cell->col = (int8_t)col;
    strncpy(cell->text, clean, kTextMax);
    cell->text[kTextMax] = '\0';
}

void Engine::sendMsp(uint16_t func, const uint8_t* payload, size_t len) {
    uint8_t frame[msp::kMaxFrame];
    size_t n = msp::encode(func, payload, len, frame, sizeof frame);
    if (n) out_.write(frame, n);
}

void Engine::composeRow(int r, int row, char* line) const {
    memset(line, ' ', kCols);
    line[kCols] = '\0';
    const Row& rw = frames_[r].rows[row];
    for (int i = 0; i < rw.n; ++i) {
        int col = rw.cells[i].col;
        for (const char* c = rw.cells[i].text; *c && col < kCols; ++c, ++col) line[col] = *c;
    }
}

static uint32_t rowHash(const char* line) {
    uint32_t h = 2166136261u;   // FNV-1a
    for (; *line; ++line) h = (h ^ (uint8_t)*line) * 16777619u;
    return h;
}

void Engine::flush(int r, bool hard, bool clockTick) {
    if (!rcpt_[r].enabled) return;
    bool changed = hard;
    uint8_t p[msp::kMaxPayload];
    p[0] = 0x01;
    memcpy(p + 1, rcpt_[r].uid, 6);
    sendMsp(msp::kElrsSetSendUid, p, 7);
    if (hard) {
        p[0] = 0x02;   // clear
        sendMsp(msp::kElrsSetOsd, p, 1);
    }
    for (int row = 0; row < kRows; ++row) {
        char line[kCols + 1];
        composeRow(r, row, line);
        uint32_t h = rowHash(line);
        if (h != sentHash_[r][row]) {
            sentHash_[r][row] = h;
            if (!(clockTick && row == opt_.raceClock.row)) changed = true;
        }
        bool empty = frames_[r].rows[row].n == 0;
        if (empty) {
            if (shown_[r][row]) blankResends_[r][row] = kBlankResends + 1;
            shown_[r][row] = false;
            if (blankResends_[r][row] == 0) continue;
            --blankResends_[r][row];
            if (hard) continue;   // "clear" already blanks it this time
        } else {
            shown_[r][row] = true;
            blankResends_[r][row] = 0;
        }
        p[0] = 0x03;   // write text: the whole row, so it replaces what was there
        p[1] = (uint8_t)row;
        p[2] = 0;
        p[3] = 0;
        memcpy(p + 4, line, kCols);
        sendMsp(msp::kElrsSetOsd, p, 4 + kCols);
    }
    p[0] = 0x04;   // display
    sendMsp(msp::kElrsSetOsd, p, 1);
    p[0] = 0x00;   // back to the bind-phrase UID
    sendMsp(msp::kElrsSetSendUid, p, 1);
    out_.burstDone();
    if (changed) {
        bool pending = refreshLeft_[r] > 0;
        refreshLeft_[r] = kRefreshRepeats;
        if (!pending) schedule(now_ + kRefreshMs, Act::Refresh, r, -1, -1, -1);
    }
}

// ── timed actions ──────────────────────────────────────────────────────────

void Engine::schedule(uint32_t due, Act kind, int rcpt, int8_t r0, int8_t r1, int8_t r2, uint32_t token,
                      uint8_t repeat) {
    Action* slot = nullptr;
    for (auto& a : actions_)
        if (!a.used) { slot = &a; break; }
    if (!slot) {   // full: replace the one due soonest (it is the most overdue)
        slot = &actions_[0];
        for (auto& a : actions_)
            if ((int32_t)(a.due - slot->due) < 0) slot = &a;
    }
    *slot = {true, due, kind, (int8_t)rcpt, {r0, r1, r2}, token, repeat};
}

void Engine::runAction(uint32_t now, Action& a) {
    a.used = false;
    int r = a.rcpt;
    switch (a.kind) {
    case Act::ClearRows:
    case Act::ClearStart:
        for (int8_t row : a.rows) clearRow(r, row);
        flush(r);
        break;
    case Act::LapRedraw: {
        if (lap_[r].token != a.token) break;   // a newer lap took over
        drawLap(r, false);
        uint32_t uptime = opt_.resultsUptimeDs * 100u;
        uint32_t next = (uint32_t)(a.repeat + 1) * kLapRepeatMs;
        if (a.repeat < kLapRepeats && next < uptime)
            schedule(now + kLapRepeatMs, Act::LapRedraw, r, -1, -1, -1, a.token, (uint8_t)(a.repeat + 1));
        break;
    }
    case Act::Refresh:
        if (refreshLeft_[r] == 0) break;
        --refreshLeft_[r];
        flush(r);   // unchanged, so it schedules nothing itself
        if (refreshLeft_[r] > 0) schedule(now + kRefreshMs, Act::Refresh, r, -1, -1, -1);
        break;
    case Act::ClearLapRow:
        // A newer lap has already redrawn the row: don't wipe it.
        if (lap_[r].token != a.token) break;
        clearRow(r, opt_.lapResults.row);
        flush(r);
        break;
    }
}

void Engine::tick(uint32_t now) {
    now_ = now;
    if (clockRunning_) {
        uint32_t sec = (now - clockBase_) / 1000;
        if (sec != lastClockSec_) {
            lastClockSec_ = sec;
            char msg[12];
            snprintf(msg, sizeof msg, "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
            for (int r = 0; r < kRecipients; ++r) {
                if (!isBroadcast(r)) continue;
                clearRow(r, opt_.raceClock.row);   // "100:00" centres one column left
                setText(r, opt_.raceClock, msg);
                flush(r, false, true);
            }
        }
    }
    // Earliest first, so redraws and clears of one recipient keep their order.
    for (;;) {
        Action* next = nullptr;
        for (auto& a : actions_)
            if (a.used && (int32_t)(now - a.due) >= 0 && (!next || (int32_t)(a.due - next->due) < 0)) next = &a;
        if (!next) break;
        now_ = next->due;   // refreshes scheduled from here count from when it was due
        runAction(now, *next);
        now_ = now;
    }
}

// ── race events ────────────────────────────────────────────────────────────

void Engine::posMessage(int laps, int position, char* buf, size_t cap) const {
    if (opt_.positionMode && position > 0) snprintf(buf, cap, "POSN: %d | LAP: %d", position, laps);
    else snprintf(buf, cap, "LAP: %d", laps);
}

void Engine::onRaceStage(uint32_t now, const char* eventName) {
    now_ = now;
    // Fresh race: drop pending clears and every per-pilot cache.
    for (auto& a : actions_) a.used = false;
    memset(refreshLeft_, 0, sizeof refreshLeft_);
    for (int s = 0; s < kPilots; ++s) {
        lap_[s].token++;
        finished_[s] = false;
        lastPos_[s][0] = '\0';
    }
    clockRunning_ = false;
    char ev[kTextMax + 1] = "";
    if (eventName) sanitize(eventName, ev, sizeof ev, true);
    for (int r = 0; r < kRecipients; ++r) {
        if (!isBroadcast(r)) continue;
        clearFrame(r);
        setText(r, opt_.status, opt_.stageMsg);
        if (ev[0]) setText(r, opt_.eventName, ev);
        flush(r, true);
    }
}

void Engine::onRaceStart(uint32_t now, uint32_t elapsedMs) {
    now_ = now;
    if (opt_.showRaceClock) {
        clockRunning_ = true;
        clockBase_ = now - elapsedMs;
        lastClockSec_ = elapsedMs / 1000;   // first tick at the next whole second
    }
    // A pending "GO!" clear from an earlier start must not cut this one short.
    for (auto& a : actions_)
        if (a.used && a.kind == Act::ClearStart) a.used = false;
    char clock[12] = "";
    if (opt_.showRaceClock) {
        uint32_t sec = elapsedMs / 1000;
        snprintf(clock, sizeof clock, "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
    }
    for (int r = 0; r < kRecipients; ++r) {
        if (!isBroadcast(r)) continue;
        if (elapsedMs == 0) clearFrame(r);   // a resumed race keeps its lap rows
        clearRow(r, opt_.status.row);
        setText(r, opt_.status, opt_.startMsg);
        if (clock[0]) {
            clearRow(r, opt_.raceClock.row);
            setText(r, opt_.raceClock, clock);
        }
        flush(r);
        schedule(now + opt_.startUptimeDs * 100u, Act::ClearStart, r, opt_.status.row, -1, -1);
    }
}

void Engine::drawLap(int slot, bool clearFirst) {
    const LapInfo& li = lap_[slot].info;
    bool showPos = clearFirst || !finished_[slot];
    if (clearFirst) clearRow(slot, opt_.currentLap.row);
    if (showPos) setText(slot, opt_.currentLap, lastPos_[slot]);
    char t[16], msg[kTextMax + 1];
    formatTime(li.lapMs, t, sizeof t);
    setText(slot, opt_.lapResults, t);
    if (opt_.showBestLap && li.bestMs) {
        formatTime(li.bestMs, t, sizeof t);
        snprintf(msg, sizeof msg, "BEST: %s", t);
        setText(slot, opt_.bestLap, msg);
    }
    if (opt_.showTotalTime) {
        formatTime(li.totalMs, t, sizeof t);
        snprintf(msg, sizeof msg, "TOTAL: %s", t);
        setText(slot, opt_.totalTime, msg);
    }
    flush(slot);
}

void Engine::onLap(uint32_t now, int slot, const LapInfo& lap) {
    now_ = now;
    if (slot < 0 || slot >= kPilots || !rcpt_[slot].enabled || finished_[slot]) return;
    if (lap.holeshot || lap.laps <= 0) {   // RotorHazard: holeshot is lap 0, position only
        onPosition(now, slot, 0, lap.position);
        return;
    }
    SlotLap& sl = lap_[slot];
    sl.token++;
    sl.info = lap;
    sl.drawnAt = now;
    posMessage(lap.laps, lap.position, lastPos_[slot], sizeof lastPos_[slot]);
    drawLap(slot, true);
    uint32_t uptime = opt_.resultsUptimeDs * 100u;
    if (kLapRepeats > 0 && kLapRepeatMs < uptime)
        schedule(now + kLapRepeatMs, Act::LapRedraw, slot, -1, -1, -1, sl.token, 1);
    schedule(now + uptime, Act::ClearLapRow, slot, -1, -1, -1, sl.token);
}

void Engine::onPosition(uint32_t now, int slot, int laps, int position) {
    now_ = now;
    if (slot < 0 || slot >= kPilots || !rcpt_[slot].enabled || finished_[slot]) return;
    char msg[kTextMax + 1];
    posMessage(laps, position, msg, sizeof msg);
    if (strcmp(msg, lastPos_[slot]) == 0) return;   // unchanged: skip the RF traffic
    strcpy(lastPos_[slot], msg);
    clearRow(slot, opt_.currentLap.row);
    setText(slot, opt_.currentLap, msg);
    flush(slot);
}

void Engine::onPilotDone(uint32_t now, int slot, int position, uint32_t totalMs) {
    now_ = now;
    if (slot < 0 || slot >= kPilots || !rcpt_[slot].enabled) return;
    finished_[slot] = true;
    clearRow(slot, opt_.currentLap.row);
    clearRow(slot, opt_.status.row);
    setText(slot, opt_.status, opt_.pilotDoneMsg);
    int8_t r1 = -1, r2 = -1;
    if (opt_.resultsMode) {
        char msg[kTextMax + 1], t[16];
        snprintf(msg, sizeof msg, "PLACEMENT: %d", position);
        setText(slot, opt_.results, msg);
        formatTime(totalMs, t, sizeof t);
        snprintf(msg, sizeof msg, "TOTAL TIME: %s", t);
        Pos second = {(int8_t)(opt_.results.row + 1), opt_.results.col};
        setText(slot, second, msg);
        r1 = opt_.results.row;
        r2 = second.row;
    }
    flush(slot);
    schedule(now + opt_.finishUptimeDs * 100u, Act::ClearRows, slot, opt_.status.row, r1, r2);
}

void Engine::onRaceStop(uint32_t now) {
    now_ = now;
    bool hadClock = clockRunning_;
    clockRunning_ = false;
    // A pending "GO!" clear would wipe LAND NOW! off the status row.
    for (auto& a : actions_)
        if (a.used && a.kind == Act::ClearStart) a.used = false;
    for (int r = 0; r < kRecipients; ++r) {
        if (!isBroadcast(r)) continue;
        if (hadClock) clearRow(r, opt_.raceClock.row);
        if (r == kRd || !finished_[r]) setText(r, opt_.status, opt_.raceStopMsg);
        flush(r);
    }
}

void Engine::onLapsClear(uint32_t now) {
    now_ = now;
    for (auto& a : actions_) a.used = false;
    memset(refreshLeft_, 0, sizeof refreshLeft_);
    for (int s = 0; s < kPilots; ++s) {
        lap_[s].token++;
        finished_[s] = false;
        lastPos_[s][0] = '\0';
    }
    clockRunning_ = false;
    clearAll();
}

void Engine::onAnnounce(uint32_t now, const char* text) {
    now_ = now;
    char msg[kTextMax + 1];
    sanitize(text, msg, sizeof msg, true);
    if (!msg[0]) return;
    for (int r = 0; r < kRecipients; ++r) {
        if (!isBroadcast(r)) continue;
        setText(r, opt_.announcement, msg);
        flush(r);
        schedule(now + opt_.announceUptimeDs * 100u, Act::ClearRows, r, opt_.announcement.row, -1, -1);
    }
}

void Engine::test(uint32_t now, int idx) {
    now_ = now;
    for (int r = 0; r < kRecipients; ++r) {
        if (!rcpt_[r].enabled || (idx >= 0 && r != idx)) continue;
        Pos a = {5, -1}, b = {7, -1};
        setText(r, a, "OSD TEST");
        setText(r, b, "FPV C5 LAP TIMER");
        flush(r);
        schedule(now + 5000, Act::ClearRows, r, 5, 7, -1);
    }
}

void Engine::clearAll() {
    for (int r = 0; r < kRecipients; ++r) {
        if (!rcpt_[r].enabled) continue;
        clearFrame(r);
        flush(r, true);
    }
}

}  // namespace osd
