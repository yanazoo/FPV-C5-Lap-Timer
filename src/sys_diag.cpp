#include "sys_diag.h"
#include "fw_update.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <driver/temperature_sensor.h>
#include "data_model.h"
#include "sd_store.h"
#include "wifi_mgr.h"

namespace {

esp_reset_reason_t bootReason = ESP_RST_UNKNOWN;
uint32_t crashes = 0, boots = 0, lastCrashBoot = 0;
uint8_t  lastCrash = 0;
volatile uint32_t restartAt = 0;   // millis(), 0 = none

// The chip temperature sensor, in one of two ranges (see sysCpuTemp()).
temperature_sensor_handle_t tsens = nullptr;
bool tsensHigh = true;   // 20-100 C, else -10-80 C

bool tsensOpen(bool high) {
    if (tsens) {
        temperature_sensor_disable(tsens);
        temperature_sensor_uninstall(tsens);
        tsens = nullptr;
    }
    temperature_sensor_config_t cfg = high ? (temperature_sensor_config_t)TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100)
                                           : (temperature_sensor_config_t)TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &tsens) != ESP_OK) { tsens = nullptr; return false; }
    if (temperature_sensor_enable(tsens) != ESP_OK) {
        temperature_sensor_uninstall(tsens);
        tsens = nullptr;
        return false;
    }
    tsensHigh = high;
    return true;
}

bool isCrash(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
           r == ESP_RST_BROWNOUT;
}

const char* reasonName(int r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_DEEPSLEEP: return "restart";   // our restart (deep sleep 2 s)
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_EXT:       return "reset";
        case ESP_RST_USB:       return "usb";
        default:                return "other";
    }
}

}  // namespace

void sysBootNote() {
    bootReason = esp_reset_reason();
    Preferences p;
    p.begin("diag", false);
    boots = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", boots);
    crashes = p.getUInt("crashes", 0);
    lastCrash = p.getUChar("last", 0);
    lastCrashBoot = p.getUInt("lastAt", 0);
    if (isCrash(bootReason)) {
        crashes++;
        lastCrash = (uint8_t)bootReason;
        lastCrashBoot = boots;
        p.putUInt("crashes", crashes);
        p.putUChar("last", lastCrash);
        p.putUInt("lastAt", lastCrashBoot);
    }
    p.end();
    Serial.printf("[Sys] start #%lu, reason: %s%s\n", (unsigned long)boots, reasonName(bootReason),
                  isCrash(bootReason) ? "  (after a crash)" : "");
}

void sysRequestRestart(uint32_t inMs) { restartAt = millis() + (inMs ? inMs : 1); }

void sysLoop(uint32_t now) {
    uint32_t at = restartAt;
    if (!at || (int32_t)(now - at) < 0) return;
    restartAt = 0;
    Serial.println("[Sys] restarting");
    ws.closeAll(1001, "restart");
    if (netIsAp()) esp_wifi_deauth_sta(0);   // every phone on our access point
    else {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false);   // the router forgets us now, not after a timeout
    }
    delay(300);   // the frames go out
    sdShutdown();
    Serial.flush();
    // A new firmware written to the other app slot (update from the page):
    // reset instead. Waking from deep sleep, the bootloader starts the app it
    // started last time (it skips reading the OTA data), so the new firmware
    // would only run after a power cycle.
    if (esp_ota_get_boot_partition() != esp_ota_get_running_partition()) {
        delay(2000);   // as long as the sleep below: the router sees us gone
        esp_restart();
    }
    esp_sleep_enable_timer_wakeup(2000 * 1000);   // 2 s: off long enough for the router to see us gone
    esp_deep_sleep_start();
}

String sysJson() {
    JsonDocument d;
    d["uptime"]   = (uint32_t)(millis() / 1000);
    d["reason"]   = reasonName(bootReason);
    d["boots"]    = boots;
    d["crashes"]  = crashes;
    if (crashes) {
        d["lastCrash"]    = reasonName(lastCrash);
        d["lastCrashAgo"] = boots - lastCrashBoot;   // starts since then (0: this one)
    }
    d["heap"]     = ESP.getFreeHeap();
    d["heapMin"]  = ESP.getMinFreeHeap();
    d["heapMax"]  = ESP.getMaxAllocHeap();
    d["psram"]    = ESP.getFreePsram();
#ifndef FW_BUILD
#define FW_BUILD "?"
#endif
    d["build"]    = FW_BUILD;   // git commit (build_info.py)
    d["version"]  = fwVersion();
    String s;
    serializeJson(d, s);
    return s;
}

float sysCpuTemp() {
    if (!tsens && !tsensOpen(true)) return NAN;
    float t;
    if (temperature_sensor_get_celsius(tsens, &t) == ESP_OK) {
        // Near the range's edge: the other range next time (each is most
        // accurate inside its own span).
        if (tsensHigh && t < 22) tsensOpen(false);
        else if (!tsensHigh && t > 75) tsensOpen(true);
        return t;
    }
    // Out of this range: read again in the other one.
    if (!tsensOpen(!tsensHigh) || temperature_sensor_get_celsius(tsens, &t) != ESP_OK) return NAN;
    return t;
}
