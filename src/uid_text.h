// A pilot's UID (6 bytes) as text: the web UI's field and the SD roster
// backup.
#pragma once
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// A pilot's UID as the ELRS configurator shows it: six numbers, "210,141,...".
inline void uidText(const uint8_t uid[6], char out[24], char sep = ',') {
    snprintf(out, 24, "%u%c%u%c%u%c%u%c%u%c%u", uid[0], sep, uid[1], sep, uid[2], sep, uid[3], sep, uid[4], sep, uid[5]);
}

// A UID typed in: six numbers 0..255 (commas, spaces or dots between), or
// twelve hex digits (colons or dashes allowed).
inline bool parseUid(const char* s, uint8_t uid[6]) {
    int n = 0;
    const char* p = s;
    while (*p && n < 7) {
        while (*p == ' ' || *p == ',' || *p == '.') p++;
        if (!*p) break;
        if (!isdigit((unsigned char)*p)) { n = -1; break; }
        unsigned v = 0;
        int digits = 0;
        while (isdigit((unsigned char)*p)) v = v * 10 + (*p++ - '0'), digits++;
        if (v > 255 || digits > 3 || (*p && *p != ' ' && *p != ',' && *p != '.')) { n = -1; break; }
        if (n < 6) uid[n] = (uint8_t)v;
        n++;
    }
    if (n == 6) return true;
    char hex[13];
    int h = 0;
    for (p = s; *p; p++) {
        if (*p == ':' || *p == '-' || *p == ' ') continue;
        if (!isxdigit((unsigned char)*p) || h >= 12) return false;
        hex[h++] = *p;
    }
    if (h != 12) return false;
    for (int k = 0; k < 6; k++) {
        char b[3] = {hex[2 * k], hex[2 * k + 1], 0};
        uid[k] = (uint8_t)strtoul(b, nullptr, 16);
    }
    return true;
}
