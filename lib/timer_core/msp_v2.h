// MSP v2 framing and ELRS backpack helpers.
//
// Byte-for-byte the same packets vrxc_elrs (custom_plugins/vrxc_elrs/msp.py)
// sends to an ELRS backpack, so elrs-netpack's XIAO (TCP) and Wrover
// (UART -> ESP-NOW) bridges forward them unchanged.
//
// Frame: '$' 'X' type flags func(u16 LE) size(u16 LE) payload crc8_dvb_s2
// where the CRC covers flags..payload.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace msp {

constexpr uint16_t kElrsGetBackpackVersion = 0x0010;
constexpr uint16_t kElrsSetSendUid         = 0x00B5;
constexpr uint16_t kElrsSetOsd             = 0x00B6;
constexpr uint16_t kElrsBackpackSetMode    = 0x0380;

constexpr uint8_t kCommand  = '<';
constexpr uint8_t kResponse = '>';

// elrs-netpack's MSP parser (lib/msp/msp.h) holds at most 64 payload bytes.
constexpr size_t kMaxPayload = 64;
constexpr size_t kOverhead   = 9;   // $ X type flags func(2) size(2) crc
constexpr size_t kMaxFrame   = kMaxPayload + kOverhead;

uint8_t crc8DvbS2(uint8_t crc, uint8_t a);

// Writes one frame to out. Returns its length, or 0 if it doesn't fit
// (payload over kMaxPayload or outCap too small).
size_t encode(uint16_t function, const uint8_t* payload, size_t len,
              uint8_t* out, size_t outCap, uint8_t type = kCommand);

// The 6-byte backpack UID for a bind phrase, as ExpressLRS derives it:
// MD5 of `-DMY_BINDING_PHRASE="<phrase>"`, first 6 bytes, multicast bit
// cleared. Same as vrxc_elrs ELRSBackpack.hash_phrase().
void bindPhraseUid(const char* phrase, uint8_t uid[6]);

// Incremental MSP v2 parser, for the replies a backpack bridge sends back.
class Parser {
public:
    // Feed one byte. Returns true when a complete, CRC-valid frame ends.
    bool feed(uint8_t b);
    uint8_t type() const { return type_; }
    uint16_t function() const { return function_; }
    const uint8_t* payload() const { return payload_; }
    uint16_t size() const { return size_; }

private:
    enum State : uint8_t { Idle, Dollar, X, Header, Payload, Crc } st_ = Idle;
    uint8_t hdr_[5] = {};
    uint8_t hdrPos_ = 0;
    uint8_t type_ = 0;
    uint8_t crc_ = 0;
    uint16_t function_ = 0;
    uint16_t size_ = 0;
    uint16_t pos_ = 0;
    uint8_t payload_[kMaxPayload] = {};
};

}  // namespace msp
