#pragma once

#include <cstddef>
#include <cstdint>

namespace drivelink::protocol {

/// Frame layout (docs/protocol/framing.md):
///
///     AA AA AA | MSG_TYPE u8 | LEN u16 | HCRC u8 | payload | CRC16
///
/// - LEN and CRC16 are little-endian.
/// - HCRC is crc8() over MSG_TYPE and LEN (3 bytes), so a receiver can reject
///   a false preamble before it waits for the payload.
/// - CRC16 is crc16Ccitt() over MSG_TYPE, LEN, HCRC and the payload.
inline constexpr uint8_t kStartByte = 0xAA;
inline constexpr size_t kStartLen = 3;
inline constexpr size_t kHeaderCrcSpan = 3; // MSG_TYPE + LEN, covered by HCRC
inline constexpr size_t kHeaderLen = 4;     // MSG_TYPE + LEN + HCRC
inline constexpr size_t kCrcLen = 2;
inline constexpr size_t kOverhead = kStartLen + kHeaderLen + kCrcLen; // 9

/// Largest payload of any frame: a protocol constant. A receiver may be
/// built with a smaller limit, never a larger one.
inline constexpr size_t kMaxPayload = 1024;

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

/// The LEN field of a received header.
/// @param header  Pointer to the MSG_TYPE byte; must hold kHeaderLen bytes
constexpr size_t headerPayloadLen(const uint8_t *header)
{
    return static_cast<size_t>(header[1]) | (static_cast<size_t>(header[2]) << 8U);
}

/// Check the HCRC of a received header.
/// @param header  Pointer to the MSG_TYPE byte; must hold kHeaderLen bytes
/// @return        true if HCRC matches MSG_TYPE and LEN
bool headerCrcValid(const uint8_t *header);

/// Check the CRC16 of a received frame body.
/// @param data         Pointer to the MSG_TYPE byte (the first byte after the preamble)
/// @param payload_len  The LEN field value. `data` must hold
///                     kHeaderLen + payload_len + kCrcLen bytes.
/// @return             true if the CRC matches
bool validateCrc(const uint8_t *data, size_t payload_len);

} // namespace drivelink::protocol
