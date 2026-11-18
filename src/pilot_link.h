// Pilots' radios (ELRS backpacks, heard by the ESP-NOW bridge): their link
// to the aircraft and their VTX setting; automatic channel assignment from
// the links (lib/timer_core/auto_assign.h); radios not on the roster, as
// candidates for new pilots.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Channel assignment: manual (the pilot list's channel selects) or from the
// pilots' radio links. NVS "settings"/"amode"; settings JSON "assignMode".
constexpr uint8_t ASSIGN_MANUAL = 0, ASSIGN_AUTO = 1;
extern uint8_t assignMode;
// A registered pilot's radio sends its VTX settings (Lua VTX Administrator,
// "Send VTx", or on linking): take them as the pilot's VTX frequency.
// NVS "settings"/"avtx"; settings JSON "vtxAuto". Default on.
extern bool vtxAuto;

// The ELRS task: the bridge heard a radio (UID) with this uplink LQ / VTX.
void pilotLinkReport(const uint8_t uid[6], uint8_t lq);
void pilotVtxReport(const uint8_t uid[6], uint16_t mhz);
// The ELRS task: a linked radio whose VTX is not known yet, to ask for it
// (a few times, some seconds apart). False if none.
bool pilotVtxWanted(uint8_t uid[6], uint32_t now);
// raceLoop(): apply automatic assignment (between races).
void pilotLinkLoop(uint32_t now);
// For the c5 status: in el "linkAge" (s since the last report, -1 never);
// links: per pilot heard (id, lq, on, wait, vtx); cand: radios heard in the
// last minute that are not on the roster (tag: the UID's first 3 bytes in
// hex, on, lq, vtx, age).
void pilotLinkToJson(JsonObject el, JsonArray links, JsonArray cand);
// A bind phrase's UID tag (as in cand), for the add form's check.
String bindUidTag(const char* phrase);
