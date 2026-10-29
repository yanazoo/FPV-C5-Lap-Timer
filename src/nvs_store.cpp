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

    prefs.begin("elrs", true);
    if (prefs.getUInt("ver", 0) == kElrsVer) {
        ElrsSettings e;
        if (prefs.getBytes("cfg", &e, sizeof e) == sizeof e) elrsCfg = e;
    }
    prefs.end();

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
