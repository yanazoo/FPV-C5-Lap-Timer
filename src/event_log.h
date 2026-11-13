// A short record of each start and of what WiFi did, kept on the timer
// (LittleFS, /eventlog.txt) and shown in Settings: what happened can be read
// on the phone afterwards, without a serial cable at the field. Every line is
// also printed on the serial port.
//
//   #41 +0s   start: power on, fw v2.0.1-16-g..., settings OK: client "net", 5 pilots, ELRS on
//   #41 +16s  WiFi: no link for 15 s (network not found): access point started
//
// Only rare events are written (a start, a WiFi change): a few lines per
// start, the file is cut back to its newest part when it grows.
#pragma once
#include <Arduino.h>

void evLogBegin(uint32_t bootNo);                 // after LittleFS is mounted
void evLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
String evLogText();                               // the newest part of the file
