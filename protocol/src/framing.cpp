#include "drivelink/protocol/framing.hpp"

#include <cstring>

#include "drivelink/protocol/crc.hpp"

namespace drivelink::protocol {

size_t encodeFrame(uint8_t *buf, size_t capacity, uint8_t msg_type, const uint8_t *payload, size_t len)
{
    if (buf == nullptr || len > kMaxPayload || (payload == nullptr && len > 0U) || capacity < frameSize(len)) {
        return 0;
    }

    buf[0] = kStartByte;
    buf[1] = kStartByte;
    buf[2] = kStartByte;
    uint8_t *header = &buf[kStartLen];
    header[0] = msg_type;
    header[1] = static_cast<uint8_t>(len & 0xFFU);
    header[2] = static_cast<uint8_t>(len >> 8U);
    header[3] = crc8(header, kHeaderCrcSpan);
    if (len > 0U) {
        std::memcpy(&header[kHeaderLen], payload, len);
    }

    const uint16_t crc = crc16Ccitt(header, kHeaderLen + len);
    header[kHeaderLen + len] = static_cast<uint8_t>(crc & 0xFFU);
    header[kHeaderLen + len + 1U] = static_cast<uint8_t>(crc >> 8U);
    return frameSize(len);
}

bool headerCrcValid(const uint8_t *header)
{
    return crc8(header, kHeaderCrcSpan) == header[kHeaderCrcSpan];
}

bool validateCrc(const uint8_t *data, size_t payload_len)
{
    // Layout: [MSG_TYPE][LEN_LO][LEN_HI][HCRC][PAYLOAD...][CRC_LO][CRC_HI]
    const uint16_t computed = crc16Ccitt(data, kHeaderLen + payload_len);
    const auto received = static_cast<uint16_t>(static_cast<uint16_t>(data[kHeaderLen + payload_len]) |
                                                (static_cast<uint16_t>(data[kHeaderLen + payload_len + 1U]) << 8U));
    return computed == received;
}

} // namespace drivelink::protocol
