#pragma once

#include <cstddef>
#include <cstdint>

namespace drivelink::protocol {

/// CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no input or
/// output reflection, no final XOR. The check value of "123456789" is 0x29B1.
/// Pure table lookup, so the firmware and the host compute it the same way.
/// @param data  Bytes to checksum (may be nullptr if len == 0)
/// @param len   Number of bytes
uint16_t crc16Ccitt(const uint8_t *data, size_t len);

} // namespace drivelink::protocol
