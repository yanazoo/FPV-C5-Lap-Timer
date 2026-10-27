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
#include "nvs_store.h"
#include "race.h"
#include "sd_store.h"
#include "wifi_mgr.h"
#include "ws_handler.h"

SemaphoreHandle_t gLock;

void setup() {
    Serial.begin(115200);
    Serial.println("\n[Web] FPV C5 Lap Timer");
    gLock = xSemaphoreCreateRecursiveMutex();

    loadAll();

    netBegin();   // access point, or join a network (AP fallback after 60 s)
    // mDNS: http://fpv-c5-lt.local in client mode, and to find elrs-netpack.
    if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);

    if (!LittleFS.begin(true)) Serial.println("[Web] LittleFS failed");
    sdInit();

    c5Begin();
    elrsBegin();
    raceBegin();

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
    ws.cleanupClients();
    raceLoop();
    if (sdPollEnabled && !raceCore.running()) sdCheckHotplug(millis());
    delay(2);
}
