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
// The microphone is there (judged from the data, see micTask). Without the
// Sense board the data line is pulled low: the decoded samples sit at one
// end of the range with no noise in them. A microphone gives samples around
// its small DC offset with some noise, even in a quiet room.
volatile bool     present = false;
volatile uint8_t  lastLevel = 0, peakLevel = 0;
volatile uint32_t windows = 0;
portMUX_TYPE      mux = portMUX_INITIALIZER_UNLOCKED;

struct Window {
    uint8_t level;   // 0-100 (100 + dBFS)
    float   mean;    // average sample
    float   rms;     // around the mean
};

Window measure(const int16_t* s, size_t n) {
    // RMS around the mean (a PDM microphone has a DC offset).
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += s[i];
    float mean = (float)sum / n, acc = 0;
    for (size_t i = 0; i < n; i++) {
        float v = s[i] - mean;
        acc += v * v;
    }
    float rms = sqrtf(acc / n);
    Window w = {0, mean, rms};
    if (rms >= 1) {
        int lvl = (int)lroundf(100.0f + 20.0f * log10f(rms / 32768.0f));
        w.level = (uint8_t)constrain(lvl, 0, 100);
    }
    return w;
}

// Presence: the decoder needs a moment to settle after it starts (its first
// windows swing even with nothing on the pin), so the first kSettleWindows
// are skipped. Then, in each run of kJudgeWindows, a window looks like a
// microphone when it has some noise and its average is not pinned near either
// end of the range and is not near full scale; kJudgeNeed of them make the
// microphone present.
constexpr uint32_t kSettleWindows = 25;   // 0.5 s
constexpr uint32_t kJudgeWindows  = 25;   // 0.5 s
constexpr uint32_t kJudgeNeed     = 20;
constexpr float    kMinRms        = 2.0f;
constexpr float    kMaxAbsMean    = 16000.0f;
// Clock crosstalk on a bare pin can decode as loud noise; a microphone's
// background in a room stays well below this.
constexpr uint8_t  kMaxBgLevel    = 90;

void micTask(void*) {
    static int16_t buf[kWindow];
    uint32_t seen = 0, judged = 0, good = 0;
    for (;;) {
        size_t got = i2s.readBytes((char*)buf, sizeof buf);
        if (got < sizeof buf) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        Window w = measure(buf, kWindow);
        portENTER_CRITICAL(&mux);
        lastLevel = w.level;
        if (w.level > peakLevel) peakLevel = w.level;
        windows = windows + 1;
        portEXIT_CRITICAL(&mux);
        if (present || ++seen <= kSettleWindows) continue;
        if (w.rms >= kMinRms && fabsf(w.mean) < kMaxAbsMean && w.level < kMaxBgLevel) good++;
        if (++judged < kJudgeWindows) continue;
        if (good >= kJudgeNeed) {
            present = true;
            Serial.printf("[Mic] microphone found (Sense board): level %u, mean %.0f\n", w.level, w.mean);
        } else if (seen == kSettleWindows + kJudgeWindows) {
            Serial.printf("[Mic] no microphone (mean %.0f, rms %.1f): microphone features off\n", w.mean, w.rms);
        }
        judged = good = 0;
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
