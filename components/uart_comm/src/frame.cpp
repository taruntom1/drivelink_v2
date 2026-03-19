#include "frame.hpp"
#include <cstring>

namespace uart_comm {
namespace detail {

/// CRC-16/CCITT lookup table generation (polynomial 0x1021, init 0xFFFF)



constexpr uint16_t crc16_ccitt_entry(uint8_t index)
{
    uint16_t crc = static_cast<uint16_t>(index) << 8;

    for (int i = 0; i < 8; ++i)
    {
        if (crc & 0x8000)
            crc = (crc << 1) ^ 0x1021;
        else
            crc <<= 1;
    }

    return crc;
}

constexpr std::array<uint16_t, 256> generate_crc16_table()
{
    std::array<uint16_t, 256> table = {};

    for (uint16_t i = 0; i < 256; ++i)
    {
        table[i] = crc16_ccitt_entry(static_cast<uint8_t>(i));
    }

    return table;
}

//Compile-time generated table
constexpr auto kCrcTable = generate_crc16_table();


uint16_t crc16Ccitt(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t index = static_cast<uint8_t>((crc >> 8) ^ data[i]);
        crc = kCrcTable[index] ^ (crc << 8);
    }
    return crc;
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
    uint16_t received = static_cast<uint16_t>(data[2 + payload_len])
                      | (static_cast<uint16_t>(data[3 + payload_len]) << 8);
    return computed == received;
}

} // namespace detail
} // namespace uart_comm
