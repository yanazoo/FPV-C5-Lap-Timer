// ELRS backpack OSD over elrs-netpack: the firmware side of vrxc_elrs.
//
// An osd::Engine (lib/timer_core) runs in its own task and sends MSP to
//   * TCP: the elrs-netpack XIAO (elrs-xiao-webui, port 8080), which joins
//     this timer's WiFi AP, exactly as RotorHazard + vrxc_elrs would, or
//   * UART: the elrs-netpack ESP-NOW bridge (Wrover-E / XIAO ESP32-C3)
//     wired to the S3.
// Race code posts events with the elrs*() calls below; they never block.
#pragma once
#include <Arduino.h>
#include "osd_engine.h"

struct ElrsStatus {
    bool connected;
    char peer[40];       // "20.0.0.2:8080" / "UART"
    char version[16];    // backpack version reported over TCP, if any
    uint32_t bytesSent;
};

void elrsBegin();
// Roster, active slots or ELRS settings changed: recompute recipients.
void elrsConfigChanged();
// Transport settings changed: drop the connection and reconnect.
void elrsReconnect();

void elrsRaceStage();
void elrsRaceStart(uint32_t elapsedMs);
void elrsLap(int slot, const osd::LapInfo& lap);
void elrsPosition(int slot, int laps, int position);
void elrsPilotDone(int slot, int position, uint32_t totalMs);
void elrsRaceStop();
void elrsLapsClear();
void elrsAnnounce(const char* text);
void elrsTest(int slot);   // -1 = every OSD recipient

ElrsStatus elrsStatus();
