#include "event_log.h"
#include <LittleFS.h>
#include <stdarg.h>

namespace {
const char*  kPath = "/eventlog.txt";
const size_t kMaxBytes = 12000;   // cut back to kKeepBytes past this
const size_t kKeepBytes = 6000;
const size_t kShowBytes = 6000;   // what Settings gets
uint32_t     bootNo = 0;
bool         ready = false;
SemaphoreHandle_t mux;

// Keep the newest kKeepBytes, from the start of a line.
void trim() {
    File f = LittleFS.open(kPath, "r");
    if (!f) return;
    size_t size = f.size();
    if (size <= kMaxBytes) { f.close(); return; }
    f.seek(size - kKeepBytes);
    String tail = f.readString();
    f.close();
    int nl = tail.indexOf('\n');
    if (nl >= 0) tail.remove(0, nl + 1);
    File w = LittleFS.open(kPath, "w");
    if (!w) return;
    w.print(tail);
    w.close();
}
}  // namespace

void evLogBegin(uint32_t n) {
    bootNo = n;
    mux = xSemaphoreCreateMutex();
    ready = true;
    trim();
}

void evLog(const char* fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    Serial.printf("[Log] %s\n", msg);
    if (!ready) return;
    xSemaphoreTake(mux, portMAX_DELAY);
    File f = LittleFS.open(kPath, "a");
    if (f) {
        f.printf("#%lu +%lus  %s\n", (unsigned long)bootNo, (unsigned long)(millis() / 1000), msg);
        f.close();
    }
    xSemaphoreGive(mux);
}

String evLogText() {
    String out;
    if (!ready) return out;
    xSemaphoreTake(mux, portMAX_DELAY);
    File f = LittleFS.open(kPath, "r");
    if (f) {
        size_t size = f.size();
        if (size > kShowBytes) f.seek(size - kShowBytes);
        out = f.readString();
        f.close();
        if (size > kShowBytes) {
            int nl = out.indexOf('\n');
            if (nl >= 0) out.remove(0, nl + 1);
        }
    }
    xSemaphoreGive(mux);
    return out;
}
