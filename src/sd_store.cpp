#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>
#include <sys/time.h>
#include <time.h>
#include "sd_store.h"
#include "race_files.h"
#include "data_model.h"
#include "nvs_store.h"
#include "race.h"
#include "ws_handler.h"

bool sdPresent     = false;
bool sdPollEnabled = false;

// SD is used from the web server task and loop(). Lock order: gLock, then
// sdMutex; code holding sdMutex never takes gLock.
static SemaphoreHandle_t sdMutex;
struct SdGuard {
    SdGuard()  { xSemaphoreTakeRecursive(sdMutex, portMAX_DELAY); }
    ~SdGuard() { xSemaphoreGiveRecursive(sdMutex); }
};

static SPIClass sdSpi(FSPI);

static bool mount() {
    SD.end();
    return SD.begin(SD_CS_PIN, sdSpi, 4000000);
}

static void sendStatus() {
    char buf[48];
    snprintf(buf, sizeof buf, R"({"type":"sd_status","present":%s})", sdPresent ? "true" : "false");
    wsText(buf);
}

void sdInit() {
    sdMutex = xSemaphoreCreateRecursiveMutex();
    sdSpi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    SdGuard g;
    sdPresent = mount();
    if (sdPresent) Serial.printf("[SD] card OK  %lluMB\n", SD.cardSize() / (1024ULL * 1024ULL));
    else Serial.println("[SD] no card");
}

void sdCheckHotplug(uint32_t now) {
    static uint32_t last = 0;
    if (now - last < (sdPresent ? 3000UL : 500UL)) return;
    last = now;
    SdGuard g;
    bool present = mount();
    if (present != sdPresent) {
        sdPresent = present;
        Serial.printf("[SD] %s\n", present ? "inserted" : "removed");
        sendStatus();
    }
}

// Race files: race_NNN.csv, or race_NNN_YYYYMMDD_HHMM.csv once the timer
// knew the date (racefiles:: in timer_core). The oldest is found by the date
// in the name, so numbers can start again at 000 after 999.
static int raceNum(const char* name) { return racefiles::number(name); }

// One pass over the card's root: how many race files, the newest one's
// number and the oldest one's path.
struct RaceFiles {
    int count = 0, newestN = -1;
    String newest, oldest;
};
static RaceFiles scanRaceFiles() {
    RaceFiles r;
    File root = SD.open("/");
    for (File e = root ? root.openNextFile() : File(); e; e = root.openNextFile()) {
        const char* name = e.name();
        if (!e.isDirectory() && raceNum(name) >= 0) {
            r.count++;
            if (r.newest.isEmpty() || racefiles::older(r.newest.c_str(), name)) r.newest = name, r.newestN = raceNum(name);
            if (r.oldest.isEmpty() || racefiles::older(name, r.oldest.c_str() + 1)) r.oldest = String(e.path());
        }
        e.close();
    }
    if (root) root.close();
    return r;
}

// Next number: one past the newest race's, 999 -> 000.
static int findNextRaceNum() {
    RaceFiles r = scanRaceFiles();
    return (r.newestN + 1) % racefiles::kNumbers;
}

// Rotate mode: delete the oldest race (by the date in its name) until fewer
// than keep remain.
static void pruneRaceFiles(int keep) {
    for (int guard = 0; guard < 1000; guard++) {
        RaceFiles r = scanRaceFiles();
        if (r.count < keep || r.oldest.isEmpty()) break;
        Serial.printf("[SD] rotate: delete %s\n", r.oldest.c_str());
        if (!SD.remove(r.oldest)) break;
    }
}

// ── Wall clock ─────────────────────────────────────────────────────────────
static bool   clockValid = false;
static int    clockTzMin = 0;
static time_t raceStartUtc = 0;    // 0 = clock unknown when the race started
static int    raceStartTz = 0;
static uint8_t raceStartRank = 0, raceStartLastLap = 1;

void sdSetClock(int64_t epochMs, int tzMin) {
    if (epochMs < 1600000000000LL) return;   // not a real date
    struct timeval tv;
    tv.tv_sec  = (time_t)(epochMs / 1000);
    tv.tv_usec = (suseconds_t)((epochMs % 1000) * 1000);
    settimeofday(&tv, nullptr);
    clockTzMin = constrain(tzMin, -14 * 60, 14 * 60);
    if (!clockValid) Serial.printf("[Clock] set from browser (UTC%+d min)\n", clockTzMin);
    clockValid = true;
}

void sdNoteRaceStart() {
    raceStartUtc     = clockValid ? time(nullptr) : 0;
    raceStartTz      = clockTzMin;
    raceStartRank    = raceRules.rankMode;
    raceStartLastLap = raceRules.countLastLap ? 1 : 0;
}

// CSV cells: commas and line breaks would break the columns.
static String cell(const char* s) {
    String o(s);
    o.replace(",", " ");
    o.replace("\n", " ");
    o.replace("\r", " ");
    return o;
}

bool sdSaveRace() {
    SdGuard g;
    if (!sdPresent) return false;
    if (settings.sdLogMode == 1) pruneRaceFiles(SD_MAX_RACE_FILES);
    // The race's local start time, when the timer knew the date: in the
    // file name (race_012_20261108_1432.csv) and on the #FPVC5 line.
    struct tm lt = {};
    if (raceStartUtc) {
        time_t local = raceStartUtc + (time_t)raceStartTz * 60;
        gmtime_r(&local, &lt);
    }
    char path[48];
    int no = findNextRaceNum();
    if (raceStartUtc)
        snprintf(path, sizeof path, "/race_%03d_%04d%02d%02d_%02d%02d.csv", no, lt.tm_year + 1900, lt.tm_mon + 1,
                 lt.tm_mday, lt.tm_hour, lt.tm_min);
    else
        snprintf(path, sizeof path, "/race_%03d.csv", no);
    // A file of that name already there (numbers restart at 000): take the
    // next free number instead of overwriting it.
    for (int i = 0; i < racefiles::kNumbers && SD.exists(path); i++) {
        no = (no + 1) % racefiles::kNumbers;
        if (raceStartUtc)
            snprintf(path, sizeof path, "/race_%03d_%04d%02d%02d_%02d%02d.csv", no, lt.tm_year + 1900,
                     lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
        else
            snprintf(path, sizeof path, "/race_%03d.csv", no);
    }
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    f.print("\xEF\xBB\xBF");   // UTF-8 BOM so Excel shows Japanese names
    // One "#FPVC5" line with when the race started and its rules, for the
    // results viewer (older files have none; readers skip lines with "#").
    char when[40] = "";
    if (raceStartUtc) {
        const struct tm& t = lt;
        int tz = raceStartTz < 0 ? -raceStartTz : raceStartTz;
        snprintf(when, sizeof when, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
                 t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, raceStartTz < 0 ? '-' : '+', tz / 60, tz % 60);
    }
    f.printf("#FPVC5,v=1,start=%s,mode=%s,laps=%u,rank=%u,limit_s=%lu,lastlap=%u,elapsed_ms=%lu,event=%s\n", when,
             raceCore.lapMode == race::LapMode::Holeshot ? "holeshot" : "immediate", raceCore.totalLaps,
             raceStartRank, (unsigned long)(raceCore.timeLimitMs / 1000), raceStartLastLap,
             (unsigned long)raceCore.elapsed(millis()), cell(elrsCfg.eventName).c_str());
    f.println("Slot,Name,FreqMHz,Lap,LapTime_ms,PeakRSSI,Timestamp_ms");
    int perSlot[MAX_ACTIVE] = {0};
    // Holeshot mode: the first pass is the holeshot, lap 0 (as on the race tab).
    int first = raceCore.lapMode == race::LapMode::Holeshot ? 0 : 1;
    for (int i = 0; i < raceCore.lapCount(); i++) {
        const race::Lap& l = raceCore.lap(i);
        int lapNo = (l.slot >= 0 && l.slot < MAX_ACTIVE) ? first + perSlot[l.slot]++ : 0;
        bool known = rosterValid(l.rosterIdx);
        f.printf("%d,%s,%u,%d,%lu,%d,%lu\n", l.slot, known ? cell(roster[l.rosterIdx].name).c_str() : "---",
                 known ? roster[l.rosterIdx].freqMhz : 0, lapNo, (unsigned long)l.lapMs, l.rssi,
                 (unsigned long)l.ts);
    }
    f.close();
    Serial.printf("[SD] saved %s\n", path);
    return true;
}

bool sdBackupRoster() {
    String csv = "\xEF\xBB\xBFname,yomi,freq,enter,exit,slot,bind,osd\n";
    {
        Guard g;
        for (int i = 0; i < rosterCount; i++) {
            char line[200];
            snprintf(line, sizeof line, "%s,%s,%u,%d,%d,%d,%s,%d\n", cell(roster[i].name).c_str(),
                     cell(roster[i].yomi).c_str(), roster[i].freqMhz, rosterCal[i].enterRssi,
                     rosterCal[i].exitRssi, activeSlotOf(i), cell(roster[i].bind).c_str(), roster[i].osd ? 1 : 0);
            csv += line;
        }
    }
    SdGuard g;
    if (!sdPresent) return false;
    File f = SD.open("/pilots.csv", FILE_WRITE);
    if (!f) return false;
    f.print(csv);
    f.close();
    return true;
}

static String field(const String& line, int idx) {
    int start = 0;
    for (int i = 0; i < idx; i++) {
        start = line.indexOf(',', start);
        if (start < 0) return "";
        start++;
    }
    int end = line.indexOf(',', start);
    return line.substring(start, end < 0 ? line.length() : end);
}

int sdRestoreRoster() {
    String csv;
    if (!sdReadFile("/pilots.csv", csv)) return -1;
    Guard g;
    rosterCount = 0;
    for (int s = 0; s < MAX_ACTIVE; s++) activePilots[s] = -1;
    int pos = 0;
    bool header = true;
    while (pos < (int)csv.length() && rosterCount < MAX_REGISTERED) {
        int nl = csv.indexOf('\n', pos);
        String line = csv.substring(pos, nl < 0 ? csv.length() : nl);
        pos = nl < 0 ? csv.length() : nl + 1;
        line.trim();
        if (!line.length()) continue;
        if (header) { header = false; continue; }
        if (line.indexOf(',') < 0) continue;
        int id = rosterCount++;
        PilotRoster& p = roster[id];
        memset(&p, 0, sizeof p);
        strlcpy(p.name, field(line, 0).c_str(), sizeof p.name);
        strlcpy(p.yomi, field(line, 1).c_str(), sizeof p.yomi);
        p.freqMhz = (uint16_t)field(line, 2).toInt();
        rosterCal[id].enterRssi = field(line, 3).length() ? field(line, 3).toInt() : DEFAULT_ENTER;
        rosterCal[id].exitRssi  = field(line, 4).length() ? field(line, 4).toInt() : DEFAULT_EXIT;
        int slot = field(line, 5).length() ? field(line, 5).toInt() : -1;
        if (slot >= 0 && slot < MAX_ACTIVE) activePilots[slot] = id;
        strlcpy(p.bind, field(line, 6).c_str(), sizeof p.bind);
        p.osd = field(line, 7).toInt() != 0;
    }
    saveRosterAll();
    saveActive();
    return rosterCount;
}

String sdListJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    {
        SdGuard g;
        if (sdPresent) {
            File root = SD.open("/");
            for (File e = root ? root.openNextFile() : File(); e; e = root.openNextFile()) {
                if (!e.isDirectory()) {
                    JsonObject o = arr.add<JsonObject>();
                    o["name"] = String(e.name());
                    o["size"] = (uint32_t)e.size();
                    time_t t = e.getLastWrite();
                    if (t > 1600000000) o["t"] = (uint32_t)t;   // only with the clock set
                }
                e.close();
            }
            if (root) root.close();
        }
    }
    String s;
    serializeJson(doc, s);
    return s;
}

static bool safePath(const String& p) { return p.startsWith("/") && p.indexOf("..") < 0 && p.length() < 64; }

bool sdReadFile(const String& path, String& out) {
    if (!safePath(path)) return false;
    SdGuard g;
    if (!sdPresent) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    if (f.size() > 200000) { f.close(); return false; }   // race files are a few tens of KB
    out.reserve(f.size());
    out = f.readString();
    f.close();
    return true;
}

bool sdAppendFile(const String& path, const uint8_t* data, size_t len, bool first) {
    if (!safePath(path)) return false;
    SdGuard g;
    if (!sdPresent) return false;
    File f = SD.open(path, first ? FILE_WRITE : FILE_APPEND);
    if (!f) return false;
    size_t put = f.write(data, len);
    f.close();
    return put == len;
}

size_t sdReadAt(const String& path, size_t offset, uint8_t* out, size_t len) {
    if (!safePath(path)) return 0;
    SdGuard g;
    if (!sdPresent) return 0;
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    size_t n = f.seek(offset) ? f.read(out, len) : 0;
    f.close();
    return n;
}

int32_t sdFileSize(const String& path) {
    if (!safePath(path)) return -1;
    SdGuard g;
    if (!sdPresent) return -1;
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) { if (f) f.close(); return -1; }
    int32_t n = (int32_t)f.size();
    f.close();
    return n;
}

bool sdReplaceFile(const String& from, const String& to) {
    if (!safePath(from) || !safePath(to)) return false;
    SdGuard g;
    if (!sdPresent) return false;
    if (SD.exists(to)) SD.remove(to);
    bool ok = SD.rename(from, to);
    if (ok) Serial.printf("[SD] wrote %s\n", to.c_str());
    return ok;
}

bool sdWriteFile(const String& path, const uint8_t* data, size_t len) {
    if (!safePath(path)) return false;
    SdGuard g;
    if (!sdPresent) return false;
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    size_t put = f.write(data, len);
    f.close();
    if (put != len) { SD.remove(path); return false; }
    Serial.printf("[SD] wrote %s (%u bytes)\n", path.c_str(), (unsigned)len);
    return true;
}

bool sdDeleteFile(const String& path) {
    if (!safePath(path)) return false;
    SdGuard g;
    return sdPresent && SD.remove(path);
}

// Remove a folder's contents (and the folder, unless it is the root).
// Names are collected a few at a time with the folder closed before
// deleting: removing entries while iterating skips some on FAT.
static int eraseDir(const String& dir) {
    int n = 0;
    for (;;) {
        String names[16];
        bool isDir[16];
        int k = 0;
        File d = SD.open(dir);
        if (!d) break;
        for (File e = d.openNextFile(); e && k < 16; e = d.openNextFile()) {
            names[k] = String(e.path());
            isDir[k] = e.isDirectory();
            e.close();
            k++;
        }
        d.close();
        if (!k) break;
        int removed = 0;
        for (int i = 0; i < k; i++) {
            int before = n;
            if (isDir[i]) n += eraseDir(names[i]);
            else if (SD.remove(names[i])) n++;
            if (!SD.exists(names[i])) removed++;
            else if (n == before) Serial.printf("[SD] could not delete %s\n", names[i].c_str());
        }
        if (!removed) break;   // stuck (read-only card?): stop instead of looping
    }
    if (dir != "/" && SD.rmdir(dir)) n++;
    return n;
}

int sdEraseAll() {
    SdGuard g;
    if (!sdPresent) return -1;
    int n = eraseDir("/");
    Serial.printf("[SD] erased %d entries\n", n);
    return n;
}
