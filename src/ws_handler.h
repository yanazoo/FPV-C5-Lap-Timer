#pragma once
#include <Arduino.h>

void wsText(const String& msg);
void wsText(const char* msg);
// Telemetry: skipped for a browser whose send queue is backing up.
void wsTelemetry(const String& msg);
void initWsHandler();
// active_update to every client (after slot / roster changes).
void wsActiveUpdate();
void wsSettings();      // race settings changed: tell every browser

// Admin logins and browser roles (see ws_handler.cpp).
bool wsLogin(String& token);            // false: as many admins as allowed are logged in
void wsLogout(const String& token);
bool wsTokenValid(const String& token);
void wsCounts(int& admins, int& viewers);   // connected browsers
int  wsAdminLogins();                   // logins held (connected or within the grace)
int  wsViewerLimit();                   // for the current WiFi mode
void wsLoop(uint32_t now);              // call from loop()
