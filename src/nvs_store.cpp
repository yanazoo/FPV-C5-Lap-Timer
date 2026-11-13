#include <Preferences.h>
#include "utf8.h"
#include <nvs.h>
#include <nvs_flash.h>
#include "event_log.h"
#include "sys_diag.h"
#include "nvs_store.h"
#include "data_model.h"
#include "wifi_mgr.h"

PilotRoster  roster[MAX_REGISTERED];
CalibConfig  rosterCal[MAX_REGISTERED];
int          rosterCount = 0;
int          activePilots[MAX_ACTIVE];
Settings     settings;
RaceRules    raceRules;
ConnLimits   connLimits;
bool         startDelayOn = false;
uint16_t     startDelaySec = 10;
uint16_t     loginHoldSec = LOGIN_HOLD_DEFAULT;
StaIpConfig  staIp;
MicSettings  micCfg;
ElrsSettings elrsCfg;
uint8_t      elrsRdSwitch = 0;
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
    // Text stored cut in the middle of a character: drop the half character.
    utf8::copy(e.rdBind, e.rdBind, sizeof e.rdBind);
    utf8::copy(e.eventName, e.eventName, sizeof e.eventName);
    utf8::copy(o.stageMsg, o.stageMsg, sizeof o.stageMsg);
    utf8::copy(o.startMsg, o.startMsg, sizeof o.startMsg);
    utf8::copy(o.pilotDoneMsg, o.pilotDoneMsg, sizeof o.pilotDoneMsg);
    utf8::copy(o.raceStopMsg, o.raceStopMsg, sizeof o.raceStopMsg);
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
    prefs.putBytes("r", &raceRules, sizeof raceRules);
    prefs.putUShort("sdly", startDelaySec);
    prefs.putBool("sdlyon", startDelayOn);
    prefs.end();
}

void saveConnLimits() {
    prefs.begin("settings", false);
    prefs.putBytes("c", &connLimits, sizeof connLimits);
    prefs.putUShort("hold", loginHoldSec);
    prefs.end();
}

void saveMic() {
    prefs.begin("mic", false);
    prefs.putBytes("cfg", &micCfg, sizeof micCfg);
    prefs.end();
}

void saveElrs() {
    prefs.begin("elrs", false);
    prefs.putUInt("ver", kElrsVer);
    prefs.putBytes("cfg", &elrsCfg, sizeof elrsCfg);
    prefs.putUChar("rdsw", elrsRdSwitch);
    prefs.end();
}

void saveAdminPassword(const String& pw) {
    adminPassword = pw;
    prefs.begin("auth", false);
    prefs.putString("pw", pw);
    prefs.end();
}

void saveStaIp() {
    prefs.begin("net", false);
    prefs.putBytes("ipcfg", &staIp, sizeof staIp);
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
    // The certificate an earlier build kept for its https:// redirect: gone
    // with that feature, free its NVS space.
    prefs.begin("tls", false);
    if (prefs.isKey("crt")) prefs.clear();
    prefs.end();
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
        // A name stored cut in the middle of a character (older firmware):
        // drop the half character, or no browser can keep its WebSocket.
        utf8::copy(roster[i].name, roster[i].name, sizeof roster[i].name);
        utf8::copy(roster[i].yomi, roster[i].yomi, sizeof roster[i].yomi);
        utf8::copy(roster[i].bind, roster[i].bind, sizeof roster[i].bind);
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
    RaceRules rr;
    if (prefs.getBytes("r", &rr, sizeof rr) == sizeof rr) raceRules = rr;
    // Before the switch, 0 s meant off.
    uint16_t sd = min(prefs.getUShort("sdly", 0), START_DELAY_MAX);
    startDelayOn = prefs.isKey("sdlyon") ? prefs.getBool("sdlyon", false) : sd > 0;
    startDelaySec = sd ? sd : 10;
    ConnLimits cl;
    if (prefs.getBytes("c", &cl, sizeof cl) == sizeof cl) connLimits = cl;
    loginHoldSec = prefs.getUShort("hold", LOGIN_HOLD_DEFAULT);
    if (!loginHoldValid(loginHoldSec)) loginHoldSec = LOGIN_HOLD_DEFAULT;
    prefs.end();
    connLimits.adminMax   = constrain(connLimits.adminMax, (uint8_t)1, CONN_ADMIN_MAX);
    // At least one viewer place: with none, a browser could never stay long
    // enough to log in.
    connLimits.viewHost   = constrain(connLimits.viewHost, (uint8_t)1, CONN_VIEW_HOST_MAX);
    connLimits.viewClient = constrain(connLimits.viewClient, (uint8_t)1, CONN_VIEW_CLIENT_MAX);
    connLimits.viewHost   = min(connLimits.viewHost, viewHostLimitFor(connLimits.adminMax));
    raceRules.rankMode     = raceRules.rankMode <= 2 ? raceRules.rankMode : 0;
    raceRules.timeLimitSec = constrain(raceRules.timeLimitSec, (uint16_t)30, (uint16_t)1800);
    // Raw bytes from flash: keep them in the ranges the API enforces.
    settings.lapMode    = settings.lapMode == 1 ? 1 : 0;
    settings.cooldownMs = constrain(settings.cooldownMs, 500u, 30000u);
    settings.sdLogMode  = settings.sdLogMode <= 2 ? settings.sdLogMode : 0;
    settings.totalLaps  = settings.totalLaps <= 99 ? settings.totalLaps : 0;
    settings.gain       = settings.gain <= 89 ? settings.gain : C5_DEFAULT_GAIN;

    prefs.begin("mic", true);
    MicSettings mc;
    if (prefs.getBytes("cfg", &mc, sizeof mc) == sizeof mc) micCfg = mc;
    prefs.end();
    micCfg.start      = micCfg.start ? 1 : 0;
    micCfg.falseStart = micCfg.falseStart ? 1 : 0;
    micCfg.startCount = constrain(micCfg.startCount, (uint8_t)1, (uint8_t)9);
    micCfg.startNow   = micCfg.startNow ? 1 : 0;
    micCfg.clapClear  = micCfg.clapClear ? 1 : 0;
    micCfg.startLvl   = min(micCfg.startLvl, (uint8_t)100);
    micCfg.fsLvl      = min(micCfg.fsLvl, (uint8_t)100);
    if (!micHoldValid(micCfg.startHoldMs)) micCfg.startHoldMs = 100;
    if (!micHoldValid(micCfg.fsHoldMs)) micCfg.fsHoldMs = 100;

    prefs.begin("elrs", true);
    if (prefs.getUInt("ver", 0) == kElrsVer) {
        ElrsSettings e;
        if (prefs.getBytes("cfg", &e, sizeof e) == sizeof e) elrsCfg = e;
    }
    elrsRdSwitch = prefs.getUChar("rdsw", 0) == 1 ? 1 : 0;
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
    StaIpConfig ipc;
    if (prefs.getBytes("ipcfg", &ipc, sizeof ipc) == sizeof ipc) staIp = ipc;
    if (staIp.fixed && (!staIp.ip || !staIp.mask)) staIp.fixed = 0;
    if (!netFallbackValid(netFallbackSec)) netFallbackSec = NET_FALLBACK_DEFAULT_S;
    prefs.end();
}

void nvsUsage(uint32_t& used, uint32_t& total) {
    nvs_stats_t st = {};
    used = total = 0;
    if (nvs_get_stats(NULL, &st) != ESP_OK) return;
    used = st.used_entries;
    total = st.total_entries;
}

static bool nvsReady = true;
bool nvsWasReady() { return nvsReady; }

// Can a namespace be opened for writing? (Read-only fails on a new timer,
// where nothing is stored yet.)
static esp_err_t nvsProbe() {
    nvs_handle_t h;
    esp_err_t e = nvs_open("diag", NVS_READWRITE, &h);
    if (e == ESP_OK) nvs_close(h);
    return e;
}

bool nvsEnsureReady() {
    esp_err_t e = nvsProbe();
    for (int i = 0; e != ESP_OK && i < 5; i++) {
        Serial.printf("[NVS] can't open the settings store (0x%x), trying again\n", e);
        delay(200);
        nvs_flash_init();   // no-op when it is initialised; never erases
        e = nvsProbe();
    }
    nvsReady = e == ESP_OK;
    if (!nvsReady) Serial.printf("[NVS] settings store not usable (0x%x): running on default settings\n", e);
    return nvsReady;
}

void nvsReport(bool nvsOk) {
    int active = 0;
    for (int s = 0; s < MAX_ACTIVE; s++) active += activePilots[s] >= 0;
    uint32_t used, total;
    nvsUsage(used, total);
    char net[64];
    if (netMode == 1) snprintf(net, sizeof net, "client \"%s\" (host after %u s)", staSsid.c_str(), netFallbackSec);
    else snprintf(net, sizeof net, "host");
    evLog("start: %s, fw %s, settings %s: %s, %d pilots (%d on channels), ELRS %s, store %lu/%lu", sysBootReason(),
          sysFirmware(), nvsOk ? "read" : "NOT READABLE (defaults)", net, rosterCount, active,
          elrsCfg.enabled ? "on" : "off", (unsigned long)used, (unsigned long)total);
}
