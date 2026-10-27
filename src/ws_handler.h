#pragma once
#include <Arduino.h>

void wsText(const String& msg);
void wsText(const char* msg);
// Lossy send for high-rate telemetry (RSSI). Drops the message when any
// client's queue is full, so telemetry can never starve critical messages
// (lap / race_start / race_stop) of WebSocket queue slots.
bool wsTextLossy(const char* msg);
void initWsHandler();
// active_update to every client (after slot / roster changes).
void wsActiveUpdate();
