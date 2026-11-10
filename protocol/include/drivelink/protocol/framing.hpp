#pragma once

#include <cstddef>
#include <cstdint>

namespace drivelink::protocol {

/// Frame layout: `AA AA AA | MSG_TYPE | LEN | payload | CRC16`.
/// The CRC (little-endian on the wire) covers MSG_TYPE, LEN and the payload.
inline constexpr uint8_t kStartByte = 0xAA;
inline constexpr size_t kStartLen = 3;
inline constexpr size_t kHeaderLen = 2; // MSG_TYPE + LEN
inline constexpr size_t kCrcLen = 2;
inline constexpr size_t kOverhead = kStartLen + kHeaderLen + kCrcLen;

/// Largest payload the LEN field can carry.
inline constexpr size_t kMaxPayload = 255;

/// Total size of a frame with a payload of `payload_len` bytes.
constexpr size_t frameSize(size_t payload_len)
{
    return kOverhead + payload_len;
}

/// MSG_TYPE ranges.
inline constexpr uint8_t kCmdMin = 0x01;
inline constexpr uint8_t kCmdMax = 0x3F;
inline constexpr uint8_t kRspMin = 0x40;
inline constexpr uint8_t kRspMax = 0x7F;
inline constexpr uint8_t kStreamMin = 0x80;
inline constexpr uint8_t kStreamMax = 0xBF;
inline constexpr uint8_t kEventMin = 0xC0;
inline constexpr uint8_t kEventMax = 0xFE;
inline constexpr size_t kStreamChannelCount = kStreamMax - kStreamMin + 1U; // 64

constexpr bool isCommand(uint8_t type) { return type >= kCmdMin && type <= kCmdMax; }
constexpr bool isResponse(uint8_t type) { return type >= kRspMin && type <= kRspMax; }
constexpr bool isStream(uint8_t type) { return type >= kStreamMin && type <= kStreamMax; }
constexpr bool isEvent(uint8_t type) { return type >= kEventMin && type <= kEventMax; }

/// Encode a complete frame into `buf`.
/// @param buf       Output buffer
/// @param capacity  Size of `buf` in bytes
/// @param msg_type  Message type byte (not range-checked here)
/// @param payload   Payload data (may be nullptr if len == 0)
/// @param len       Payload length
/// @return          Frame size written, or 0 if `buf` is null, `len` is above
///                  kMaxPayload, `payload` is null with len > 0, or the frame
///                  does not fit in `capacity`. Nothing is written on failure.
size_t encodeFrame(uint8_t *buf, size_t capacity, uint8_t msg_type, const uint8_t *payload, size_t len);

/// Check the CRC of a received frame body.
/// @param data         Pointer to the MSG_TYPE byte (the first byte after the preamble)
/// @param payload_len  The LEN field value. `data` must hold payload_len + 4 bytes
///                     (MSG_TYPE, LEN, payload, CRC16).
/// @return             true if the CRC matches
bool validateCrc(const uint8_t *data, size_t payload_len);

} // namespace drivelink::protocol
