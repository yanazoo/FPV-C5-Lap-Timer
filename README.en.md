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
                                                        │ MSP v2: A. WiFi TCP 8080 via the
                                                        │ netpack XIAO, or B. UART 115200 direct
                                                        ▼
                    ESP-NOW bridge (ESP32 Wrover-E or XIAO ESP32-C3) ─ESP-NOW ch1─▶ goggles
```

## Wiring

| XIAO ESP32-S3 | | XIAO ESP32-C5 |
|---|---|---|
| D3 / GPIO4 (TX) | → | D4 / GPIO23 (RX) |
| D4 / GPIO5 (RX) | ← | D5 / GPIO24 (TX) |
| 5V | — | 5V (**power the C5 from 5 V**, not the S3's 3V3) |
| GND | — | GND |

- microSD: XIAO ESP32-S3 Sense slot (CS 21, SCK 7, MISO 8, MOSI 9).
- elrs-netpack, option A (default, WiFi): the netpack XIAO joins the same
  WiFi as the timer (its AP in host mode, or the same router in client mode);
  the timer finds it on port 8080 automatically (`auto` scans AP clients, or
  looks up `elrs-netpack.local` in client mode).
- Option B (UART): no netpack XIAO and no WiFi setup; the ESP-NOW bridge
  (`elrs-espnow-bridge`) is wired straight to the S3 — see
  [docs/ELRS_UART_SETUP.md](docs/ELRS_UART_SETUP.md) (Japanese). Connect GND only, not the power pins.

  | XIAO ESP32-S3 | | XIAO ESP32-C3 (`xiao_esp32c3`) | ESP32 Wrover-E (`esp32dev`) |
  |---|---|---|---|
  | D6 / GPIO43 (TX) | → | D7 / GPIO20 (RX) | GPIO26 (RX) |
  | D7 / GPIO44 (RX) | ← | D6 / GPIO21 (TX) | GPIO27 (TX) |
  | GND | — | GND | GND |

## Build and flash

```bash
git clone --recurse-submodules https://github.com/yanazoo/FPV-C5-Lap-Timer.git
cd FPV-C5-Lap-Timer
# C5 receiver (FPVGate C5MK, from the submodule)
pio run -d third_party/FPVGateC5MK/multipilot -e xiaoc5 -t upload
# S3 timer + web UI
pio run -e xiao_esp32s3 -t upload
pio run -e xiao_esp32s3 -t uploadfs
# ESP-NOW bridge (xiao_esp32c3 or esp32dev)
pio run -d third_party/elrs-netpack/elrs-espnow-bridge -e xiao_esp32c3 -t upload
```

Add `--upload-port COMx` if the wrong port is picked; if the port is not
found, hold BOOT while pressing RESET and retry. Prebuilt binaries are in the
CI artifacts `fpv_c5-xiao_esp32s3`, `fpvgate_c5mk-xiaoc5` and
`elrs-espnow-bridge`.

Then join WiFi `FPV-C5-LT` (password `fpv-c5-lt`), open `http://20.0.0.1`,
log in (`admin`), **change the WiFi (host AP) and admin passwords** in
Settings (the defaults are public), add pilots with their VTX channel and ELRS bind phrase, assign them
to Ch1–8, enable ELRS OSD and calibrate Enter/Exit in the Calib tab
(C5 scale 0..1023, 16 counts per dB). The race tab shows only the assigned
channels.

### WiFi modes (Settings → 📶 WiFi)

- **Host** (default): the timer is an access point, `http://20.0.0.1`.
- **Client**: the timer joins an existing 2.4 GHz router and is reachable at
  `http://fpv-c5-lt.local`. If it cannot connect for 60 s (at boot, or after
  losing the link), it falls back to host mode until the next reboot. The
  router password is stored on the device only and never sent back to the
  browser.

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
builds the S3 timer, the C5 firmware and the ESP-NOW bridge (Wrover-E and
XIAO ESP32-C3).

## Licences

FPVGate C5MK (including `c5host`) is CC BY-NC-SA 4.0 (non-commercial);
vrxc_elrs and elrs-netpack are GPL-3.0. The combined firmware is meant for
personal, non-commercial use.
