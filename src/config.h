#pragma once
#include <IPAddress.h>

// ── FPVGate C5MK receiver (XIAO ESP32-C5) on UART1 ─────────────────────────
// XIAO S3 D3 / GPIO4 (TX) -> XIAO C5 D4 / GPIO23 (RX)
// XIAO S3 D4 / GPIO5 (RX) <- XIAO C5 D5 / GPIO24 (TX)
// 921600 baud 8N1 (c5host::kBaud). Power the C5 from 5V, not the S3's 3V3.
#define C5_TX_PIN        4
#define C5_RX_PIN        5
#define C5_RX_BUFFER  4096    // ~95 ms of data at 8 pilots
#define C5_DEFAULT_GAIN 15

// ── elrs-netpack, direct UART mode (optional) ──────────────────────────────
// Replaces the netpack's XIAO: the S3 talks to the ESP-NOW bridge itself
// (elrs-netpack/elrs-espnow-bridge on an ESP32 Wrover-E or a XIAO ESP32-C3).
// XIAO S3 D6 / GPIO43 (TX) -> Wrover GPIO26 / XIAO C3 D7 GPIO20 (RX)
//         D7 / GPIO44 (RX) <- Wrover GPIO27 / XIAO C3 D6 GPIO21 (TX)
// See docs/ELRS_UART_SETUP.md.
#define ELRS_UART_TX_PIN 43
#define ELRS_UART_RX_PIN 44
#define ELRS_UART_BAUD   115200
#define ELRS_TCP_PORT    8080   // elrs-netpack XIAO's TCP port

// ── microSD (XIAO ESP32-S3 Sense expansion board pins) ─────────────────────
#define SD_CS_PIN    21
#define SD_SCK_PIN    7
#define SD_MISO_PIN   8
#define SD_MOSI_PIN   9
#define SD_MAX_RACE_FILES 20   // rotate mode keeps at most this many race_*.csv

// ── Pilots and timing ──────────────────────────────────────────────────────
#define MAX_REGISTERED  20
#define MAX_ACTIVE       8
// Thresholds on the C5's 0..1023 scale (16 counts per dB), for the default
// gain 15. Calibrate per venue in the Calib tab.
#define DEFAULT_ENTER  300
#define DEFAULT_EXIT   280
#define RSSI_INTERVAL_MS  100UL   // UI telemetry rate; detection runs on every reading
// RSSI recorded during a race, for adding a missed lap afterwards: the
// highest reading of every 40 ms (25 a second), up to 30 min of race time,
// per channel, in PSRAM (8 x 45000 x 2 bytes = 720 KB).
#define C5_REC_MS        40
#define C5_REC_SAMPLES   45000
#define SIGNAL_LOST_MS    300UL

// ── WiFi access point ──────────────────────────────────────────────────────
// Public defaults (also in the README). Change them from the Config tab:
// they are stored in NVS, never in this repository.
#define AP_DEFAULT_SSID "FPV-C5-LT"
#define AP_DEFAULT_PASS "fpv-c5-lt"
#define AP_CHANNEL       6     // default; the Config tab picks 6..11
#define AP_CHANNEL_MIN   6     // ESP-NOW (the goggles' backpacks) is on 1: stay clear of it
// The WiFi driver runs in world safe mode (no country set: the timer is used
// in more than one country), which allows channels 1-11 only. An access
// point on 12 or 13 doesn't start (seen on hardware: the timer restarted
// over and over).
#define AP_CHANNEL_MAX   11
#define STA_NETS         5     // client mode: networks to choose from / join automatically
#define MDNS_NAME       "fpv-c5-lt"
static const IPAddress AP_IP      (20, 0, 0, 1);
static const IPAddress AP_GATEWAY (20, 0, 0, 1);
static const IPAddress AP_SUBNET  (255, 255, 255, 0);
