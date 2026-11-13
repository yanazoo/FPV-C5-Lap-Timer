#pragma once
#include <Arduino.h>

void loadAll();                 // roster, active slots, settings, ELRS, auth, AP
void saveRosterPilot(int i);
void saveRosterAll();           // after a delete or restore (rewrites every pilot)
void saveActive();
void saveSettings();
void saveElrs();
void saveConnLimits();
void saveMic();
void saveStaIp();             // client mode address (applied after a reboot)
void saveAdminPassword(const String& pw);
// WiFi mode and both credential sets (applied after a reboot).
void saveNet(uint8_t mode, const String& apS, const String& apP, const String& staS, const String& staP,
             uint8_t fallbackSec);

// Log what loadAll() read (WiFi mode and network, pilots, ELRS) and how full
// the NVS partition is. The Arduino core erases the whole partition when it
// can't open it at boot (no free page): every setting then starts over
// (host mode, no pilots), and this line shows it.
void nvsReport(bool nvsOk);
// The NVS partition can be opened (initialising it again a few times if
// not; never erasing it). False: this start runs on default settings, and
// says so, instead of looking like a new timer.
bool nvsEnsureReady();
bool nvsWasReady();
// NVS partition entries in use / in total (0 / 0 if unknown).
void nvsUsage(uint32_t& used, uint32_t& total);
