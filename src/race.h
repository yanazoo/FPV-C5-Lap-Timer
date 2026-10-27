// Glue between the C5 link, race::Core, the web UI (WebSocket) and the
// ELRS OSD. Replaces ESP-NOW-Lap-Timer's gate_comm.cpp: there is no gate
// node any more, the S3 detects passes itself.
#pragma once
#include <Arduino.h>

void raceBegin();
// Called from loop(): passes -> laps, 10 Hz RSSI telemetry, 1 Hz C5 status.
void raceLoop();

// Push frequencies and thresholds of the active slots to the C5 link and
// the pilot list to the OSD. Call after any roster/slot/calibration change.
void applyGateConfig();

void raceStage();          // countdown started (ARM NOW on the goggles)
void raceStart();
bool raceResume();
void raceStop();
// Clear button: optionally save to SD, then forget the race.
// Returns the JSON response for /api/race/save.
String raceClear();

// C5 receiver + ELRS link status to every WebSocket client (also sent 1 Hz).
void sendC5Status();

const char* activeName(int slot);
int         activeSlotOf(int rosterIdx);
