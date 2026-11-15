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
             uint8_t fallbackSec, uint16_t raceHoldSec);
