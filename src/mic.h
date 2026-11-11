// The XIAO ESP32-S3 Sense board's PDM microphone (CLK GPIO42, DATA GPIO41).
//
// A task reads it at 16 kHz and turns every 20 ms into a loudness level,
// 0-100 (100 + dBFS: 100 is the loudest the microphone can give, a quiet
// room is around 30-45, a clap close by 80-95). It is relative, not a
// calibrated sound level, so the thresholds are set by looking at the graph
// (Calibration tab).
//
// Uses (race.cpp): a loud sound while nothing is running starts the race
// (start), and a loud sound between ARM NOW and GO calls a false start.
#pragma once
#include <Arduino.h>

void    micBegin();
// The microphone is there (the Sense board) and running: it has picked up
// sound at least once since boot. Until then the features stay off and the
// web UI hides them.
bool    micOk();
// The newest 20 ms window: its level and a counter that goes up by one per
// window (to see how many arrived since the last look). False: none yet.
bool    micLatest(uint8_t& level, uint32_t& seq);
// The loudest window since the previous call (for the 10 Hz telemetry).
uint8_t micPeak();
