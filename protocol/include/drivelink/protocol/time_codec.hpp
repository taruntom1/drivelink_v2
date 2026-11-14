#pragma once

#include "crc.hpp"
#include "framing.hpp"
#include "gen/registry.hpp"
#include "wire_codec.hpp"

namespace drivelink::protocol::time_wire {
inline constexpr uint8_t kVersion = 1;
inline constexpr size_t kRequestBytes = 3, kResponseBytes = 19;
struct Response { uint16_t id = 0; uint64_t rx_us = 0, tx_us = 0; };

inline size_t request(uint8_t *out, size_t cap, uint16_t id) {
    if (!out || cap < kRequestBytes) { return 0; }
    wire::Writer w(out, cap); w.appendU8(kVersion); w.appendU16Le(id); return w.size();
}
inline bool decodeRequest(const uint8_t *data, size_t len, uint16_t &id) {
    if (!data || len != kRequestBytes) { return false; }
    wire::Reader r(data,len); const auto version=r.readU8(); const auto value=r.readU16Le();
    if (!r.done() || version != kVersion) { return false; } id=value; return true;
}
inline size_t response(uint8_t *out, size_t cap, const Response &value) {
    if (!out || cap < kResponseBytes) { return 0; }
    wire::Writer w(out,cap); w.appendU8(kVersion); w.appendU16Le(value.id);
    w.appendU64Le(value.rx_us); w.appendU64Le(value.tx_us); return w.size();
}
inline bool decodeResponse(const uint8_t *data, size_t len, Response &value) {
    if (!data || len != kResponseBytes) { return false; }
    wire::Reader r(data,len); const auto version=r.readU8();
    Response v; v.id=r.readU16Le(); v.rx_us=r.readU64Le(); v.tx_us=r.readU64Le();
    if (!r.done() || version != kVersion) { return false; } value=v; return true;
}
// Only a complete, valid time reply may be patched. No partial writes on failure.
inline bool stampFrame(uint8_t *frame, size_t size, uint64_t tx_us) {
    if (!frame || size != frameSize(kResponseBytes) || frame[0]!=kStartByte ||
        frame[1]!=kStartByte || frame[2]!=kStartByte || frame[3]!=defs::msg::kTimeResponse ||
        headerPayloadLen(frame+kStartLen)!=kResponseBytes || !headerCrcValid(frame+kStartLen) ||
        !validateCrc(frame+kStartLen,kResponseBytes) || frame[kStartLen+kHeaderLen]!=kVersion) { return false; }
    wire::Writer stamp(frame+kStartLen+kHeaderLen+11,8); stamp.appendU64Le(tx_us);
    const auto crc=crc16Ccitt(frame+kStartLen,kHeaderLen+kResponseBytes);
    wire::Writer trailer(frame+size-kCrcLen,kCrcLen); trailer.appendU16Le(crc); return true;
}
}
