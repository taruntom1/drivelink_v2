#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include "sdkconfig.h"

namespace uart_comm {
namespace detail {

/// Protocol constants
static constexpr uint8_t kStartByte = 0xAA;
static constexpr uint8_t kStartLen = 3;
static constexpr uint8_t kHeaderLen = 2;   // MSG_TYPE + LEN
static constexpr uint8_t kCrcLen = 2;
static constexpr uint8_t kMaxPayload = CONFIG_UART_COMM_MAX_PAYLOAD;
static constexpr size_t kMaxFrameSize = kStartLen + kHeaderLen + kMaxPayload + kCrcLen;

/// MSG_TYPE range boundaries
static constexpr uint8_t kCmdMin = 0x01;
static constexpr uint8_t kCmdMax = 0x3F;
static constexpr uint8_t kRspMin = 0x40;
static constexpr uint8_t kRspMax = 0x7F;
static constexpr uint8_t kStreamMin = 0x80;
static constexpr uint8_t kStreamMax = 0xBF;
static constexpr uint8_t kEventMin = 0xC0;
static constexpr uint8_t kEventMax = 0xFE;
static constexpr uint8_t kStreamChannelCount = kStreamMax - kStreamMin + 1; // 64

inline bool isCommand(uint8_t type)  { return type >= kCmdMin && type <= kCmdMax; }
inline bool isResponse(uint8_t type) { return type >= kRspMin && type <= kRspMax; }
inline bool isStream(uint8_t type)   { return type >= kStreamMin && type <= kStreamMax; }
inline bool isEvent(uint8_t type)    { return type >= kEventMin && type <= kEventMax; }

/// Compute CRC-16/CCITT over a byte buffer.
/// Uses polynomial 0x1021 with initial value 0xFFFF.
/// @param data  Pointer to data buffer
/// @param len   Number of bytes
/// @return      16-bit CRC value
uint16_t crc16Ccitt(const uint8_t* data, size_t len);

/// Encode a complete frame into a pre-allocated buffer.
/// Buffer must be at least kStartLen + kHeaderLen + len + kCrcLen bytes.
/// @param buf       Output buffer
/// @param msg_type  Message type byte
/// @param payload   Payload data (may be nullptr if len == 0)
/// @param len       Payload length
/// @return          Total frame size written to buf
size_t encodeFrame(uint8_t* buf, uint8_t msg_type, const uint8_t* payload, uint8_t len);

/// Validate CRC on a received frame body (MSG_TYPE + LEN + PAYLOAD + CRC).
/// @param data         Pointer to MSG_TYPE byte (first byte after the 3xAA start)
/// @param payload_len  Payload length (the LEN field value)
/// @return             true if CRC matches
bool validateCrc(const uint8_t* data, uint8_t payload_len);

} // namespace detail
} // namespace uart_comm
