#include "sys_diag.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include "data_model.h"
#include "sd_store.h"
#include "nvs_store.h"
#include "wifi_mgr.h"

namespace {

esp_reset_reason_t bootReason = ESP_RST_UNKNOWN;
uint32_t crashes = 0, boots = 0, lastCrashBoot = 0;
uint8_t  lastCrash = 0;
volatile uint32_t restartAt = 0;   // millis(), 0 = none

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

const char* sysFirmware() {
#ifdef FW_VERSION
    return FW_VERSION;
#else
    return "dev";
#endif
}

// This chip's own (factory) MAC, last three bytes: tells two timers apart
// (both may broadcast the same SSID with the same web UI).
const char* sysDeviceId() {
    static char id[8] = "";
    if (!id[0]) {
        uint8_t mac[6] = {};
        esp_efuse_mac_get_default(mac);
        snprintf(id, sizeof id, "%02X%02X%02X", mac[3], mac[4], mac[5]);
    }
    return id;
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
    esp_sleep_enable_timer_wakeup(2000 * 1000);   // 2 s: off long enough for the router to see us gone
    esp_deep_sleep_start();
}

String sysJson() {
    JsonDocument d;
    d["uptime"]   = (uint32_t)(millis() / 1000);
    d["fw"]       = sysFirmware();
    uint32_t nu, nt;
    nvsUsage(nu, nt);
    d["nvsUsed"]  = nu;    // settings storage: entries in use / in total
    d["nvsTotal"] = nt;
    d["id"]       = sysDeviceId();
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
    String s;
    serializeJson(d, s);
    return s;
}
