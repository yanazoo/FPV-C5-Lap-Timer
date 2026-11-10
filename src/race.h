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
// The web Start / Stop buttons. Start runs the same sequence as the race
// director's switch (race_ready, stage, start 3.3 s later), or resumes a
// stopped race; Stop also cancels a start under way. Any task may call
// these; raceLoop() acts on them.
void raceRequestStart();
void raceRequestStop();
// The race director's TX switch (MSP_ELRS_BACKPACK_SET_RECORDING_STATE,
// 1 = on). Any task may call it; raceLoop() acts on it. Depending on the
// switch setting it starts the race (3 s countdown, as from the web UI),
// resumes a stopped one, or stops it.
void raceDirectorSwitch(uint8_t state);
// Delete a wrong pass of a pilot (its time joins the next lap; see
// race::Core::deleteLap). Every browser reloads the laps; the goggles get the
// new lap count, positions and best lap. False if there is no such pass.
bool raceDeleteLap(int slot, int index, uint32_t lapMs);
// The admin's lap button: a pass for this slot now (the cooldown applies).
// False if no race is running or nobody is in the slot.
bool raceManualLap(int slot);
// Clear button: optionally save to SD, then forget the race.
// Returns the JSON response for /api/race/save.
String raceClear();

// C5 receiver + ELRS link status to every WebSocket client (also sent 1 Hz).
void sendC5Status();

const char* activeName(int slot);
int         activeSlotOf(int rosterIdx);
