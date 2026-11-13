/*
 * FPV C5 Lap Timer — XIAO ESP32-S3 firmware
 *
 *   XIAO ESP32-C5 running FPVGate C5MK  --UART 921600-->  this S3
 *   (5.8 GHz VTX RSSI, ~1 kHz per pilot)                  gate detection, laps,
 *                                                         Web UI (WiFi AP),
 *                                                         ELRS OSD (vrxc_elrs port)
 *   this S3  --TCP 8080 / UART-->  elrs-netpack  --ESP-NOW-->  goggles' ELRS backpack
 */
#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include "c5_link.h"
#include "data_model.h"
#include "elrs.h"
#include "http_routes.h"
#include "mic.h"
#include "nvs_store.h"
#include "race.h"
#include "sd_store.h"
#include "wifi_mgr.h"
#include "sys_diag.h"
#include "ws_handler.h"

SemaphoreHandle_t gLock;

void setup() {
    Serial.begin(115200);
    Serial.println("\n[Web] FPV C5 Lap Timer");
    sysBootNote();
    gLock = xSemaphoreCreateRecursiveMutex();

    loadAll();
    nvsReport();   // what this start read: mode, pilots, ELRS, how full NVS is

    netBegin();   // access point, or join a network (AP fallback after 15 / 30 / 60 s)
    // mDNS: http://fpv-c5-lt.local in client mode, and to find elrs-netpack.
    if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);

    if (!LittleFS.begin(true)) Serial.println("[Web] LittleFS failed");
    sdInit();

    c5Begin();
    elrsBegin();
    raceBegin();
    micBegin();

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");
    initWsHandler();
    registerHttpRoutes();
    server.begin();
    Serial.printf("[Web] HTTP started  roster=%d/%d\n", rosterCount, MAX_REGISTERED);
}

void loop() {
    netLoop();
    // Frees closed clients only: the limits are wsLoop's (the library's own
    // default closes the oldest past 8, admins included).
    ws.cleanupClients(24);
    wsLoop(millis());
    sysLoop(millis());
    raceLoop();
    delay(2);
}
