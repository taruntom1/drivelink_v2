#include "frame.hpp"
#include <cstring>
#include "esp_rom_crc.h"

namespace uart_comm::detail {

uint16_t crc16Ccitt(const uint8_t* data, size_t len)
{
    // esp_rom_crc16_be() inverts the seed on entry and the result on exit
    // (the zlib convention). To get CRC-16/CCITT-FALSE (init 0xFFFF, no final
    // XOR) we pass the pre-inverted init (~0xFFFF = 0) and undo its final inversion.
    const uint16_t crc = esp_rom_crc16_be(static_cast<uint16_t>(~uint16_t{0xFFFF}), data, len);
    return static_cast<uint16_t>(~crc);
}

size_t encodeFrame(uint8_t* buf, uint8_t msg_type, const uint8_t* payload, uint8_t len)
{
    // Start bytes
    buf[0] = kStartByte;
    buf[1] = kStartByte;
    buf[2] = kStartByte;

    // Header
    buf[3] = msg_type;
    buf[4] = len;

    // Payload
    if (len > 0 && payload != nullptr) {
        std::memcpy(&buf[5], payload, len);
    }

    // CRC over MSG_TYPE + LEN + PAYLOAD
    uint16_t crc = crc16Ccitt(&buf[3], 2 + len);
    buf[5 + len] = static_cast<uint8_t>(crc & 0xFF);
    buf[6 + len] = static_cast<uint8_t>(crc >> 8);

    return kStartLen + kHeaderLen + len + kCrcLen;
}

bool validateCrc(const uint8_t* data, uint8_t payload_len)
{
    // data points to MSG_TYPE byte
    // Layout: [MSG_TYPE][LEN][PAYLOAD...][CRC_LO][CRC_HI]
    uint16_t computed = crc16Ccitt(data, 2 + payload_len);
    uint16_t received = static_cast<uint16_t>( // NOSONAR: explicit 16-bit reconstruction is intentional for on-wire CRC fields.
        static_cast<uint16_t>(data[2 + payload_len]) |
        (static_cast<uint16_t>(data[3 + payload_len]) << 8));
    return computed == received;
}

} // namespace uart_comm::detail
