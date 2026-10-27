#include "msp_v2.h"

#include <string.h>

#include "md5.h"

namespace msp {

uint8_t crc8DvbS2(uint8_t crc, uint8_t a) {
    crc ^= a;
    for (int i = 0; i < 8; ++i) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0xD5) : (uint8_t)(crc << 1);
    return crc;
}

size_t encode(uint16_t function, const uint8_t* payload, size_t len,
              uint8_t* out, size_t outCap, uint8_t type) {
    if (len > kMaxPayload || outCap < len + kOverhead) return 0;
    size_t n = 0;
    out[n++] = '$';
    out[n++] = 'X';
    out[n++] = type;
    const size_t body = n;
    out[n++] = 0;   // flags
    out[n++] = (uint8_t)(function & 0xFF);
    out[n++] = (uint8_t)(function >> 8);
    out[n++] = (uint8_t)(len & 0xFF);
    out[n++] = (uint8_t)(len >> 8);
    if (len) memcpy(out + n, payload, len);
    n += len;
    uint8_t crc = 0;
    for (size_t i = body; i < n; ++i) crc = crc8DvbS2(crc, out[i]);
    out[n++] = crc;
    return n;
}

void bindPhraseUid(const char* phrase, uint8_t uid[6]) {
    static const char kPrefix[] = "-DMY_BINDING_PHRASE=\"";
    Md5 md5;
    md5.update(kPrefix, sizeof kPrefix - 1);
    md5.update(phrase, strlen(phrase));
    md5.update("\"", 1);
    uint8_t digest[16];
    md5.finish(digest);
    memcpy(uid, digest, 6);
    if (uid[0] & 1) uid[0] -= 1;
}

bool Parser::feed(uint8_t b) {
    switch (st_) {
    case Idle:
        if (b == '$') st_ = Dollar;
        return false;
    case Dollar:
        st_ = (b == 'X') ? X : Idle;
        return false;
    case X:
        if (b == kCommand || b == kResponse) {
            type_ = b;
            hdrPos_ = 0;
            crc_ = 0;
            st_ = Header;
        } else {
            st_ = Idle;
        }
        return false;
    case Header:
        hdr_[hdrPos_++] = b;
        crc_ = crc8DvbS2(crc_, b);
        if (hdrPos_ < sizeof hdr_) return false;
        function_ = (uint16_t)(hdr_[1] | hdr_[2] << 8);
        size_ = (uint16_t)(hdr_[3] | hdr_[4] << 8);
        pos_ = 0;
        if (size_ > kMaxPayload) st_ = Idle;
        else st_ = size_ ? Payload : Crc;
        return false;
    case Payload:
        payload_[pos_++] = b;
        crc_ = crc8DvbS2(crc_, b);
        if (pos_ == size_) st_ = Crc;
        return false;
    case Crc:
        st_ = Idle;
        return b == crc_;
    }
    st_ = Idle;
    return false;
}

}  // namespace msp
