#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>
#include "sd_store.h"
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

static int findNextRaceNum() {
    char path[32];
    for (int n = 1; n < 1000; n++) {
        snprintf(path, sizeof path, "/race_%03d.csv", n);
        if (!SD.exists(path)) return n;
    }
    return 999;
}

// Rotate mode: delete the oldest race_*.csv until fewer than keep remain.
static void pruneRaceFiles(int keep) {
    char path[32];
    for (;;) {
        int count = 0, minN = 1000;
        for (int n = 1; n < 1000; n++) {
            snprintf(path, sizeof path, "/race_%03d.csv", n);
            if (SD.exists(path)) { count++; if (n < minN) minN = n; }
        }
        if (count < keep || minN == 1000) break;
        snprintf(path, sizeof path, "/race_%03d.csv", minN);
        SD.remove(path);
    }
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
    char path[32];
    snprintf(path, sizeof path, "/race_%03d.csv", findNextRaceNum());
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    f.print("\xEF\xBB\xBF");   // UTF-8 BOM so Excel shows Japanese names
    f.println("Slot,Name,FreqMHz,Lap,LapTime_ms,PeakRSSI,Timestamp_ms");
    int perSlot[MAX_ACTIVE] = {0};
    for (int i = 0; i < raceCore.lapCount(); i++) {
        const race::Lap& l = raceCore.lap(i);
        int lapNo = (l.slot >= 0 && l.slot < MAX_ACTIVE) ? ++perSlot[l.slot] : 0;
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

bool sdDeleteFile(const String& path) {
    if (!safePath(path)) return false;
    SdGuard g;
    return sdPresent && SD.remove(path);
}
