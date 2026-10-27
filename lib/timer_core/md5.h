// Small portable MD5 (RFC 1321), only used to derive ELRS backpack UIDs
// from bind phrases. Not for anything security related.
#pragma once

#include <stddef.h>
#include <stdint.h>

class Md5 {
public:
    Md5();
    void update(const void* data, size_t len);
    void finish(uint8_t digest[16]);

private:
    void block(const uint8_t* p);
    uint32_t h_[4];
    uint64_t bytes_ = 0;
    uint8_t buf_[64];
    size_t bufLen_ = 0;
};
