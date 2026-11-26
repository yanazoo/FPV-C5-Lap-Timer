// Restart and diagnostics.
//
// The restart button (and the reboot after a WiFi change) used ESP.restart():
// a software reset leaves the outside world as it was (the router still has
// the timer associated, the SD card mid-session, the C5 streaming), and a
// timer restarted that way could come back unreachable until power was cut.
// sysRequestRestart() now tidies up first (tells the router and the phones
// on our access point we are leaving, closes the browsers' connections,
// unmounts the card) and restarts through a 2 s deep sleep, which resets
// the chip much like a power-on.
//
// Diagnostics: the reason for the last start, and a count of starts after a
// crash (panic, watchdog, brown-out) kept in NVS, so it survives the power
// cut that got a crashing timer back. GET /api/sys shows them with the free
// memory; an admin can clear the count (POST /api/sys/clear-crashes).
#pragma once
#include <Arduino.h>

void   sysBootNote();                    // first thing in setup()
void   sysClearCrashes();               // the crash count back to 0 (button on the page)
void   sysRequestRestart(uint32_t inMs);   // done from loop() (sysLoop)
void   sysLoop(uint32_t now);
String sysJson();
// The S3's chip temperature, C (its internal sensor), or NAN. Measured in
// the sensor's 20-100 C range (the chip runs well above the air around
// it), below 20 C in its -10-80 C range: Arduino's temperatureRead() uses
// 10-50 C only, and read 80-odd C with a large error.
float  sysCpuTemp();
