#pragma once
#include <Arduino.h>

void loadAll();                 // roster, active slots, settings, ELRS, auth, AP
void saveRosterPilot(int i);
void saveRosterAll();           // after a delete or restore (rewrites every pilot)
void saveActive();
void saveSettings();
void saveElrs();
void saveAdminPassword(const String& pw);
void saveAp(const String& ssid, const String& pass);
