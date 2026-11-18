// Pilots' radio links (ELRS backpack link statistics, heard by the ESP-NOW
// bridge) and automatic channel assignment from them (lib/timer_core/
// auto_assign.h).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Channel assignment: manual (the pilot list's channel selects) or from the
// pilots' radio links. NVS "settings"/"amode"; settings JSON "assignMode".
constexpr uint8_t ASSIGN_MANUAL = 0, ASSIGN_AUTO = 1;
extern uint8_t assignMode;

// The ELRS task: the bridge heard a radio (UID) with this uplink LQ.
void pilotLinkReport(const uint8_t uid[6], uint8_t lq);
// raceLoop(): apply automatic assignment (between races).
void pilotLinkLoop(uint32_t now);
// For the c5 status: "links" (per pilot heard: id, lq, on, wait) and
// whether reports are coming in.
void pilotLinkToJson(JsonObject el, JsonArray links);
