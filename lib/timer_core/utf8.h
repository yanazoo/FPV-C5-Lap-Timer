// UTF-8 text that fits a fixed buffer.
//
// strlcpy() cuts at a byte: a Japanese name longer than the buffer was cut
// in the middle of a character. Browsers close a WebSocket that carries
// such a string (a text frame must be valid UTF-8), so one long pilot name
// took the web UI down for everybody. utf8::copy keeps whole characters
// only, and drops bytes that are not UTF-8 at all (a CSV saved in another
// encoding).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace utf8 {

// Length of the valid sequence at s (1..4), 0 if s does not start one.
inline size_t seqLen(const char* s) {
    const uint8_t* p = (const uint8_t*)s;
    uint8_t c = p[0];
    if (c < 0x80) return c ? 1 : 0;
    size_t n = (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
    if (!n) return 0;
    for (size_t i = 1; i < n; ++i)
        if ((p[i] & 0xC0) != 0x80) return 0;   // also stops at the end of the string
    // Overlong forms, surrogates and values past U+10FFFF are not UTF-8.
    if (c == 0xE0 && p[1] < 0xA0) return 0;
    if (c == 0xED && p[1] > 0x9F) return 0;
    if (c == 0xF0 && p[1] < 0x90) return 0;
    if (c == 0xF4 && p[1] > 0x8F) return 0;
    return n;
}

// Copy src into dst[cap]: whole, valid characters only, as many as fit.
// dst may be src (cleaning a string in place). Returns the bytes written.
inline size_t copy(char* dst, const char* src, size_t cap) {
    if (!cap) return 0;
    size_t out = 0;
    while (*src) {
        size_t n = seqLen(src);
        if (!n) { ++src; continue; }   // not UTF-8: leave it out
        if (out + n > cap - 1) break;
        for (size_t i = 0; i < n; ++i) dst[out++] = src[i];
        src += n;
    }
    dst[out] = '\0';
    return out;
}

}  // namespace utf8
