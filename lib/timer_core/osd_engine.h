// ELRS backpack OSD for the lap timer: a port of vrxc_elrs
// (custom_plugins/vrxc_elrs/elrs_backpack.py) from a RotorHazard plugin to
// firmware.
//
// What carries over, so goggles show the same thing as under RotorHazard:
//   * per-pilot UID from the ELRS bind phrase (callsign if none)
//   * the OSD frame model: every DISPLAY resends the recipient's whole
//     screen, so goggles that render each DISPLAY from scratch still show
//     every row
//   * ARM NOW / GO! / race clock / LAP + position / lap time / best lap /
//     total time / FINISHED! + placement / LAND NOW! and their timings
//   * lap rows redrawn twice 0.8 s apart so one lost packet can't blank them
//   * an optional race-director recipient that gets the broadcast messages
//
// What differs: a burst never starts with the OSD "clear" command. HDZero
// keeps a canvas that only "clear" blanks; with clear first, any write lost
// on the ESP-NOW hop between clear and display shows a screen with that row
// missing, and the next second's burst brings it back, so rows vanished and
// flickered. Instead each row is written at its full width (spaces included)
// and a row that became empty is overwritten with spaces, then resent blank
// on the next bursts too. A lost write now leaves the previous text up for
// a second instead of blanking it. "clear" is only sent when the whole
// screen is meant to go blank (staging, clearing laps).
//
// Every burst that changes what a recipient should see (other than the race
// clock ticking) is sent again kRefreshRepeats times, kRefreshMs apart. Once
// the race clock stops nothing else redraws the screen, so without these a
// single lost write left LAND NOW!'s clock row, an expired lap time or a
// cleared FINISHED! up for good.
//
// Gevent greenlets with sleeps become timed actions run from tick(). One
// task owns the Engine; nothing here is thread safe.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace osd {

constexpr int kPilots = 8;
constexpr int kRd = 8;            // recipient index of the race director
constexpr int kRecipients = 9;
constexpr int kRows = 18;         // HDZero OSD rows
constexpr int kCols = 50;         // vrxc_elrs centres on 50 columns
constexpr int kCellsPerRow = 3;
constexpr int kTextMax = 40;
constexpr int kMaxActions = 48;
constexpr int kBlankResends = 2;     // extra bursts that repeat a row's blanking
constexpr int kRefreshRepeats = 2;   // extra bursts after a change
constexpr uint32_t kRefreshMs = 300;
constexpr int kLapRepeats = 2;            // LAP_OSD_REPEATS
constexpr uint32_t kLapRepeatMs = 800;    // LAP_OSD_REPEAT_INTERVAL

struct Pos {
    int8_t row;
    int8_t col;   // < 0: centre the text
};

// Defaults are vrxc_elrs's option defaults (custom_plugins/vrxc_elrs/__init__.py).
struct Options {
    bool showRaceClock = true;
    bool showBestLap = true;
    bool positionMode = true;    // "POSN: 1 | LAP: 2" instead of "LAP: 2"
    bool showTotalTime = false;
    bool resultsMode = true;     // placement + total after FINISHED!
    char stageMsg[24] = "w ARM NOW x";
    char startMsg[24] = "w   GO!   x";
    char pilotDoneMsg[24] = "w FINISHED! x";
    char raceStopMsg[24] = "w  LAND NOW!  x";
    uint16_t startUptimeDs = 5;      // deciseconds, as in vrxc_elrs
    uint16_t finishUptimeDs = 100;
    uint16_t resultsUptimeDs = 50;
    uint16_t announceUptimeDs = 50;
    Pos eventName = {0, -1};
    Pos announcement = {3, -1};
    Pos status = {5, -1};
    Pos currentLap = {1, -1};
    Pos lapResults = {8, -1};
    Pos bestLap = {12, 0};
    Pos totalTime = {3, -1};
    Pos results = {10, -1};      // uses two rows
    Pos raceClock = {2, -1};
};

// Receives finished MSP frames (bytes ready for the TCP socket or UART).
class Output {
public:
    virtual ~Output() {}
    virtual void write(const uint8_t* data, size_t len) = 0;
    // One recipient's burst is complete; a transport may pace here.
    virtual void burstDone() {}
};

struct LapInfo {
    int laps;           // completed laps (0 for a holeshot)
    bool holeshot;
    uint32_t lapMs;
    uint32_t bestMs;
    uint32_t totalMs;
    int position;
};

class Engine {
public:
    explicit Engine(Output& out) : out_(out) {}

    Options& options() { return opt_; }

    // Recipients 0..7 are the timer's slots, 8 the race director. Disabled
    // recipients get nothing.
    void setRecipient(int idx, bool enabled, const uint8_t uid[6]);
    bool recipientEnabled(int idx) const { return rcpt_[idx].enabled; }

    void onRaceStage(uint32_t now, const char* eventName);
    // elapsedMs: race time already run (non-zero when resuming a pause).
    void onRaceStart(uint32_t now, uint32_t elapsedMs);
    void onLap(uint32_t now, int slot, const LapInfo& lap);
    // Another pilot's position/lap count changed (vrxc_elrs update_pos).
    void onPosition(uint32_t now, int slot, int laps, int position);
    void onPilotDone(uint32_t now, int slot, int position, uint32_t totalMs);
    void onRaceStop(uint32_t now);
    void onLapsClear(uint32_t now);
    void onAnnounce(uint32_t now, const char* text);
    // Shows "OSD TEST" on one recipient (or all, idx < 0) for 5 s.
    void test(uint32_t now, int idx);
    // Clears every enabled recipient's screen now.
    void clearAll();

    // Call every few tens of ms: race clock and delayed actions.
    void tick(uint32_t now);

    bool clockRunning() const { return clockRunning_; }

    // Exposed for tests.
    static void formatTime(uint32_t ms, char* buf, size_t cap);   // M:SS.T
    static int centerCol(size_t len);

private:
    struct Cell {
        int8_t col;
        char text[kTextMax + 1];
    };
    struct Row {
        uint8_t n;
        Cell cells[kCellsPerRow];
    };
    struct Frame {
        Row rows[kRows];
    };
    struct Recipient {
        bool enabled;
        uint8_t uid[6];
    };
    enum class Act : uint8_t { ClearRows, ClearStart, LapRedraw, ClearLapRow, Refresh };
    struct Action {
        bool used;
        uint32_t due;
        Act kind;
        int8_t rcpt;
        int8_t rows[3];
        uint32_t token;   // LapRedraw / ClearLapRow: the slot's lap token
        uint8_t repeat;
    };
    struct SlotLap {
        uint32_t token;
        LapInfo info;
        uint32_t drawnAt;
    };

    Output& out_;
    Options opt_;
    Recipient rcpt_[kRecipients] = {};
    Frame frames_[kRecipients] = {};
    Action actions_[kMaxActions] = {};
    SlotLap lap_[kPilots] = {};
    bool finished_[kPilots] = {};
    char lastPos_[kPilots][kTextMax + 1] = {};
    // What each recipient's goggles were last sent per row, for overwriting
    // rows that became empty: how many more bursts resend it as spaces.
    bool shown_[kRecipients][kRows] = {};
    uint8_t blankResends_[kRecipients][kRows] = {};
    // Hash of each row as last sent, to tell a change from a redraw, and the
    // refresh bursts still to come per recipient.
    uint32_t sentHash_[kRecipients][kRows] = {};
    uint8_t refreshLeft_[kRecipients] = {};
    uint32_t now_ = 0;   // time of the event being handled
    bool clockRunning_ = false;
    uint32_t clockBase_ = 0;
    uint32_t lastClockSec_ = 0;

    // Frame editing (vrxc_elrs send_clear_osd / send_osd_text / ...).
    void clearFrame(int r);
    void clearRow(int r, int row);
    void setText(int r, Pos pos, const char* text);
    // Sends the recipient's whole frame wrapped in SET_SEND_UID. hard: start
    // with "clear" (only when the screen is meant to go blank).
    // clockTick: only the race clock moved (no refresh bursts for that).
    void flush(int r, bool hard = false, bool clockTick = false);
    void composeRow(int r, int row, char* line) const;
    void sendMsp(uint16_t func, const uint8_t* payload, size_t len);

    bool isBroadcast(int r) const;
    void schedule(uint32_t due, Act kind, int rcpt, int8_t r0, int8_t r1, int8_t r2, uint32_t token = 0, uint8_t repeat = 0);
    void runAction(uint32_t now, Action& a);
    void posMessage(int laps, int position, char* buf, size_t cap) const;
    void drawLap(int slot, bool clearFirst);
};

}  // namespace osd
