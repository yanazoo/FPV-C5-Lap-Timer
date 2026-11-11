#include "mic.h"
#include <ESP_I2S.h>
#include <math.h>
#include <driver/gpio.h>

namespace {

constexpr int8_t   kClkPin  = 42;
constexpr int8_t   kDataPin = 41;
constexpr uint32_t kRate    = 16000;
constexpr size_t   kWindow  = kRate / 50;   // 20 ms

I2SClass i2s;
volatile bool     started = false;
// Seen at least one window with sound in it. Without the Sense board the
// data line is held low (pull-down): every sample is the same, level 0. A
// microphone always gives some noise, even in a quiet room.
volatile bool     present = false;
volatile uint8_t  lastLevel = 0, peakLevel = 0;
volatile uint32_t windows = 0;
portMUX_TYPE      mux = portMUX_INITIALIZER_UNLOCKED;

uint8_t levelOf(const int16_t* s, size_t n) {
    // RMS around the mean (a PDM microphone has a DC offset).
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += s[i];
    float mean = (float)sum / n, acc = 0;
    for (size_t i = 0; i < n; i++) {
        float v = s[i] - mean;
        acc += v * v;
    }
    float rms = sqrtf(acc / n);
    if (rms < 1) return 0;
    float dbfs = 20.0f * log10f(rms / 32768.0f);
    int lvl = (int)lroundf(100.0f + dbfs);
    return (uint8_t)constrain(lvl, 0, 100);
}

void micTask(void*) {
    static int16_t buf[kWindow];
    for (;;) {
        size_t got = i2s.readBytes((char*)buf, sizeof buf);
        if (got < sizeof buf) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        uint8_t lvl = levelOf(buf, kWindow);
        portENTER_CRITICAL(&mux);
        lastLevel = lvl;
        if (lvl > peakLevel) peakLevel = lvl;
        windows = windows + 1;
        portEXIT_CRITICAL(&mux);
        if (lvl > 0 && !present) {
            present = true;
            Serial.println("[Mic] microphone found (Sense board)");
        }
    }
}

}  // namespace

void micBegin() {
    i2s.setPinsPdmRx(kClkPin, kDataPin);
    if (!i2s.begin(I2S_MODE_PDM_RX, kRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
        Serial.println("[Mic] PDM microphone not started");
        return;
    }
    // No board: the data pin would float and read as noise; hold it low.
    gpio_pulldown_en((gpio_num_t)kDataPin);
    started = true;
    xTaskCreatePinnedToCore(micTask, "mic", 4096, nullptr, 1, nullptr, 0);
    Serial.println("[Mic] PDM microphone on GPIO42/41, 16 kHz");
}

bool micOk() { return started && present; }

bool micLatest(uint8_t& level, uint32_t& seq) {
    portENTER_CRITICAL(&mux);
    level = lastLevel;
    seq = windows;
    portEXIT_CRITICAL(&mux);
    return seq > 0;
}

uint8_t micPeak() {
    portENTER_CRITICAL(&mux);
    uint8_t p = peakLevel;
    peakLevel = 0;
    portEXIT_CRITICAL(&mux);
    return p;
}
