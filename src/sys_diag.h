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
// memory.
#pragma once
#include <Arduino.h>

void   sysBootNote();                    // first thing in setup()
void   sysRequestRestart(uint32_t inMs);   // done from loop() (sysLoop)
void   sysLoop(uint32_t now);
String sysJson();

// Firmware version (git describe at build time) and this chip's id (MAC tail).
const char* sysFirmware();
const char* sysDeviceId();
uint32_t    sysBootCount();   // this start's number (from sysBootNote)
const char* sysBootReason();  // why this start: poweron, restart, panic, ...
