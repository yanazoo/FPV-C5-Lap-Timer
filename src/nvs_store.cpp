#include <Preferences.h>
#include "nvs_store.h"
#include "data_model.h"
#include "wifi_mgr.h"

PilotRoster  roster[MAX_REGISTERED];
CalibConfig  rosterCal[MAX_REGISTERED];
int          rosterCount = 0;
int          activePilots[MAX_ACTIVE];
Settings     settings;
ElrsSettings elrsCfg;
String       adminPassword = "admin";   // public default; changed from the Config tab
String       apSsid = AP_DEFAULT_SSID, apPass = AP_DEFAULT_PASS;
uint8_t      netMode = 0;   // NET_AP until configured
String       staSsid, staPass;
uint8_t      netFallbackSec = NET_FALLBACK_DEFAULT_S;

static Preferences prefs;

template <size_t N> static void terminate(char (&s)[N]) { s[N - 1] = '\0'; }

static void sanitizePos(osd::Pos& p, osd::Pos dflt) {
    if (p.row < 0 || p.row >= osd::kRows || p.col >= osd::kCols) p = dflt;
}

// Settings read back as raw bytes: every string terminated, every value in
// the range the web API would accept.
static void sanitizeElrs(ElrsSettings& e) {
    const ElrsSettings d;
    e.transport = e.transport == 1 ? 1 : 0;
    terminate(e.host);
    if (!e.host[0]) strlcpy(e.host, "auto", sizeof e.host);
    if (!e.port) e.port = ELRS_TCP_PORT;
    terminate(e.rdBind);
    terminate(e.eventName);
    osd::Options& o = e.opt;
    terminate(o.stageMsg);
    terminate(o.startMsg);
    terminate(o.pilotDoneMsg);
    terminate(o.raceStopMsg);
    for (uint16_t* u : {&o.startUptimeDs, &o.finishUptimeDs, &o.resultsUptimeDs, &o.announceUptimeDs})
        *u = constrain(*u, (uint16_t)1, (uint16_t)600);
    sanitizePos(o.eventName, d.opt.eventName);
    sanitizePos(o.announcement, d.opt.announcement);
    sanitizePos(o.status, d.opt.status);
    sanitizePos(o.currentLap, d.opt.currentLap);
    sanitizePos(o.lapResults, d.opt.lapResults);
    sanitizePos(o.bestLap, d.opt.bestLap);
    sanitizePos(o.totalTime, d.opt.totalTime);
    sanitizePos(o.raceClock, d.opt.raceClock);
    if (o.results.row < 0 || o.results.row + 1 >= osd::kRows || o.results.col >= osd::kCols) o.results = d.opt.results;
}
static const uint32_t kElrsVer = 1;   // bump when ElrsSettings' layout changes

static void putPilot(int i) {
    char key[16];
    snprintf(key, sizeof key, "p%d", i);
    prefs.putBytes(key, &roster[i], sizeof roster[i]);
    snprintf(key, sizeof key, "c%d", i);
    prefs.putBytes(key, &rosterCal[i], sizeof rosterCal[i]);
}

void saveRosterPilot(int i) {
    if (i < 0 || i >= rosterCount) return;
    prefs.begin("pilots", false);
    putPilot(i);
    prefs.putInt("count", rosterCount);
    prefs.end();
}

void saveRosterAll() {
    prefs.begin("pilots", false);
    prefs.clear();
    for (int i = 0; i < rosterCount; i++) putPilot(i);
    prefs.putInt("count", rosterCount);
    prefs.end();
}

void saveActive() {
    prefs.begin("active", false);
    prefs.putBytes("slots", activePilots, sizeof activePilots);
    prefs.end();
}

void saveSettings() {
    prefs.begin("settings", false);
    prefs.putBytes("s", &settings, sizeof settings);
    prefs.end();
}

void saveElrs() {
    prefs.begin("elrs", false);
    prefs.putUInt("ver", kElrsVer);
    prefs.putBytes("cfg", &elrsCfg, sizeof elrsCfg);
    prefs.end();
}

void saveAdminPassword(const String& pw) {
    adminPassword = pw;
    prefs.begin("auth", false);
    prefs.putString("pw", pw);
    prefs.end();
}

void saveNet(uint8_t mode, const String& apS, const String& apP, const String& staS, const String& staP,
             uint8_t fallbackSec) {
    netMode = mode;
    netFallbackSec = fallbackSec;
    apSsid = apS, apPass = apP;
    staSsid = staS, staPass = staP;
    prefs.begin("net", false);
    prefs.putUChar("mode", mode);
    prefs.putString("ssid", apS);
    prefs.putString("pass", apP);
    prefs.putString("stassid", staS);
    prefs.putString("stapass", staP);
    prefs.putUChar("fbsec", fallbackSec);
    prefs.end();
}

void loadAll() {
    prefs.begin("pilots", true);
    rosterCount = constrain(prefs.getInt("count", 0), 0, MAX_REGISTERED);
    for (int i = 0; i < rosterCount; i++) {
        char key[16];
        snprintf(key, sizeof key, "p%d", i);
        if (prefs.getBytes(key, &roster[i], sizeof roster[i]) != sizeof roster[i]) {
            memset(&roster[i], 0, sizeof roster[i]);
            snprintf(roster[i].name, sizeof roster[i].name, "Pilot %d", i + 1);
        }
        roster[i].name[sizeof roster[i].name - 1] = '\0';
        roster[i].yomi[sizeof roster[i].yomi - 1] = '\0';
        roster[i].bind[sizeof roster[i].bind - 1] = '\0';
        snprintf(key, sizeof key, "c%d", i);
        if (prefs.getBytes(key, &rosterCal[i], sizeof rosterCal[i]) != sizeof rosterCal[i])
            rosterCal[i] = {DEFAULT_ENTER, DEFAULT_EXIT};
    }
    prefs.end();

    for (int s = 0; s < MAX_ACTIVE; s++) activePilots[s] = -1;
    prefs.begin("active", true);
    int tmp[MAX_ACTIVE];
    if (prefs.getBytes("slots", tmp, sizeof tmp) == sizeof tmp)
        for (int s = 0; s < MAX_ACTIVE; s++) activePilots[s] = rosterValid(tmp[s]) ? tmp[s] : -1;
    prefs.end();

    prefs.begin("settings", true);
    Settings st;
    if (prefs.getBytes("s", &st, sizeof st) == sizeof st) settings = st;
    prefs.end();
    // Raw bytes from flash: keep them in the ranges the API enforces.
    settings.lapMode    = settings.lapMode == 1 ? 1 : 0;
    settings.cooldownMs = constrain(settings.cooldownMs, 500u, 30000u);
    settings.sdLogMode  = settings.sdLogMode <= 2 ? settings.sdLogMode : 0;
    settings.totalLaps  = settings.totalLaps <= 99 ? settings.totalLaps : 0;
    settings.gain       = settings.gain <= 89 ? settings.gain : C5_DEFAULT_GAIN;

    prefs.begin("elrs", true);
    if (prefs.getUInt("ver", 0) == kElrsVer) {
        ElrsSettings e;
        if (prefs.getBytes("cfg", &e, sizeof e) == sizeof e) elrsCfg = e;
    }
    prefs.end();
    sanitizeElrs(elrsCfg);

    prefs.begin("auth", true);
    adminPassword = prefs.getString("pw", "admin");
    prefs.end();

    prefs.begin("net", true);
    apSsid = prefs.getString("ssid", AP_DEFAULT_SSID);
    apPass = prefs.getString("pass", AP_DEFAULT_PASS);
    netMode = prefs.getUChar("mode", 0);
    staSsid = prefs.getString("stassid", "");
    staPass = prefs.getString("stapass", "");
    netFallbackSec = prefs.getUChar("fbsec", NET_FALLBACK_DEFAULT_S);
    if (!netFallbackValid(netFallbackSec)) netFallbackSec = NET_FALLBACK_DEFAULT_S;
    prefs.end();
}
