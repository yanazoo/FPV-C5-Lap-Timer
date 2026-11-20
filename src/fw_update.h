// Updating the timer's own firmware and web UI from the browser:
//   * online: in client mode with internet, the timer reads the latest
//     release's version from GitHub, downloads its firmware.bin and
//     littlefs.bin (HTTPS, checked against src/github_ca.h) and restarts;
//   * from files: the browser sends firmware.bin / littlefs.bin (from the
//     release zip) and the timer writes them.
// Never during a race (or with laps not cleared). Settings and pilots stay
// (NVS is not touched). A rescue page in the firmware stands in for the web
// UI if it is missing (a web UI update cut off half way).
#pragma once
#include <Arduino.h>

// The release this firmware is ("v2.2.0", "v2.2.0-3-gabc1234", "dev").
const char* fwVersion();
// HTTP routes: /api/update (status), /api/update/check, /api/update/online,
// /api/update/upload, and the rescue page.
void fwUpdateRoutes();
// The rescue page, when the web UI's files are missing.
bool fwWebUiMissing();
void fwSendRescuePage(class AsyncWebServerRequest* req);
