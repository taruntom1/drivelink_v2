#include "drivelink/protocol/crc.hpp"

#include <array>

namespace drivelink::protocol {

namespace {

constexpr uint16_t kCrc16Polynomial = 0x1021U;
constexpr uint16_t kCrc16InitialValue = 0xFFFFU;
constexpr uint8_t kCrc8Polynomial = 0x07U;
constexpr uint8_t kCrc8InitialValue = 0x00U;

constexpr std::array<uint16_t, 256> makeCrc16Table()
{
    std::array<uint16_t, 256> table{};
    for (size_t i = 0; i < table.size(); ++i) {
        auto crc = static_cast<uint16_t>(i << 8U);
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<uint16_t>(((crc & 0x8000U) != 0U) ? ((crc << 1U) ^ kCrc16Polynomial) : (crc << 1U));
        }
        table[i] = crc;
    }
    return table;
}

constexpr std::array<uint8_t, 256> makeCrc8Table()
{
    std::array<uint8_t, 256> table{};
    for (size_t i = 0; i < table.size(); ++i) {
        auto crc = static_cast<uint8_t>(i);
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<uint8_t>(((crc & 0x80U) != 0U) ? ((crc << 1U) ^ kCrc8Polynomial) : (crc << 1U));
        }
        table[i] = crc;
    }
    return table;
}

constexpr std::array<uint16_t, 256> kCrc16Table = makeCrc16Table();
constexpr std::array<uint8_t, 256> kCrc8Table = makeCrc8Table();

} // namespace

uint16_t crc16Ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = kCrc16InitialValue;
    for (size_t i = 0; (data != nullptr) && (i < len); ++i) {
        crc = static_cast<uint16_t>((crc << 8U) ^ kCrc16Table[static_cast<uint8_t>((crc >> 8U) ^ data[i])]);
    }
    return crc;
}

uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = kCrc8InitialValue;
    for (size_t i = 0; (data != nullptr) && (i < len); ++i) {
        crc = kCrc8Table[static_cast<uint8_t>(crc ^ data[i])];
    }
    return crc;
}

} // namespace drivelink::protocol
