# FPV C5 Lap Timer

🌐 [日本語](README.md) | **English**

An FPV drone lap timer built on [ESP-NOW Lap Timer](https://github.com/yanazoo/ESP-NOW-Lap-Timer). No transponder on the
quad: a **XIAO ESP32-C5 running [FPVGate C5MK](https://github.com/yanazoo/FPVGateC5MK)**
measures each pilot's own 5.8 GHz VTX at ~1 kHz, a **XIAO ESP32-S3** detects
gate passes at the RSSI peak, and lap times go to the pilots' goggles through
**[elrs-netpack](https://github.com/yanazoo/elrs-netpack)** using a firmware
port of **[vrxc_elrs](https://github.com/yanazoo/vrxc_elrs)**'s OSD logic.

```
VTX 5.8 GHz ─▶ XIAO ESP32-C5 (C5MK) ─UART 921600─▶ XIAO ESP32-S3 (this firmware)
                                                    gate detection, laps, Web UI, SD,
                                                    ELRS OSD (vrxc_elrs port)
                                                        │ TCP 8080 (WiFi) or UART, MSP v2
                                                        ▼
                                  elrs-netpack (XIAO S3 + Wrover) ─ESP-NOW─▶ goggles
```

## Wiring

| XIAO ESP32-S3 | | XIAO ESP32-C5 |
|---|---|---|
| D3 / GPIO4 (TX) | → | D4 / GPIO23 (RX) |
| D4 / GPIO5 (RX) | ← | D5 / GPIO24 (TX) |
| 5V | — | 5V (**power the C5 from 5 V**, not the S3's 3V3) |
| GND | — | GND |

- microSD: XIAO ESP32-S3 Sense slot (CS 21, SCK 7, MISO 8, MOSI 9).
- elrs-netpack, option A (default): its XIAO joins this timer's WiFi AP; the
  timer finds it on port 8080 automatically.
- Option B: the netpack's ESP-NOW bridge (ESP32 Wrover-E or XIAO ESP32-C3) wired straight to
  the S3 — see [docs/ELRS_UART_SETUP.md](docs/ELRS_UART_SETUP.md) (Japanese). Wrover-E wiring:
  D6/GPIO43 (TX) → Wrover GPIO26, D7/GPIO44 (RX) ← Wrover GPIO27, GND.

## Build and flash

```bash
git clone --recurse-submodules https://github.com/yanazoo/FPV-C5-Lap-Timer.git
cd FPV-C5-Lap-Timer
# C5 receiver (FPVGate C5MK, from the submodule)
pio run -d third_party/FPVGateC5MK/multipilot -e xiaoc5 -t upload
# S3 timer + web UI
pio run -e xiao_esp32s3 -t upload
pio run -e xiao_esp32s3 -t uploadfs
```

Then join WiFi `FPV-C5-LT` (password `fpv-c5-lt`), open `http://20.0.0.1`,
log in (`admin`), **change the AP and admin passwords** (the defaults are
public), add pilots with their VTX channel and ELRS bind phrase, assign them
to Ch1–8, enable ELRS OSD and calibrate Enter/Exit in the Calib tab
(C5 scale 0..1023, 16 counts per dB).

## Layout

- `lib/timer_core/` — platform-independent core: `race_core` (laps,
  holeshot, cooldown, pause, positions, finish), `gate_tracker` (FPVGate's
  pass detection), `osd_engine` (vrxc_elrs port), `msp_v2` (MSP v2 + bind
  phrase UID).
- `src/` — Arduino glue: C5 UART task, web server, SD, NVS, OSD transport.
- `data/` — web UI.
- `third_party/` — FPVGateC5MK, elrs-netpack and vrxc_elrs as pinned submodules.

## Tests

`bash test/run.sh` runs the core unit tests, a simulated C5MK UART →
c5host → gate detection → laps pipeline, and a cross-check that loads the real
vrxc_elrs Python source and compares bind-phrase UIDs and OSD bytes with the
firmware's, byte for byte. CI (`.github/workflows/ci.yml`) runs them and
builds the S3 and C5 firmware.

## Licences

FPVGate C5MK (including `c5host`) is CC BY-NC-SA 4.0 (non-commercial);
vrxc_elrs and elrs-netpack are GPL-3.0. The combined firmware is meant for
personal, non-commercial use.
