#include "drivelink/protocol/crc.hpp"

#include <array>

namespace drivelink::protocol {

namespace {

constexpr uint16_t kPolynomial = 0x1021U;
constexpr uint16_t kInitialValue = 0xFFFFU;

constexpr std::array<uint16_t, 256> makeTable()
{
    std::array<uint16_t, 256> table{};
    for (size_t i = 0; i < table.size(); ++i) {
        auto crc = static_cast<uint16_t>(i << 8U);
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<uint16_t>(((crc & 0x8000U) != 0U) ? ((crc << 1U) ^ kPolynomial) : (crc << 1U));
        }
        table[i] = crc;
    }
    return table;
}

constexpr std::array<uint16_t, 256> kTable = makeTable();

} // namespace

uint16_t crc16Ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = kInitialValue;
    for (size_t i = 0; (data != nullptr) && (i < len); ++i) {
        crc = static_cast<uint16_t>((crc << 8U) ^ kTable[static_cast<uint8_t>((crc >> 8U) ^ data[i])]);
    }
    return crc;
}

} // namespace drivelink::protocol
