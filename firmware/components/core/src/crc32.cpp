// CRC-32 (IEEE 802.3 / zlib) for the NVS blobs. Table-driven; the table is
// built at compile time.

#include <array>

#include "core/config_blob.hpp"

namespace core {

namespace {

constexpr uint32_t kPoly = 0xEDB88320U;

constexpr std::array<uint32_t, 256> makeTable()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256U; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = ((c & 1U) != 0U) ? (kPoly ^ (c >> 1U)) : (c >> 1U);
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<uint32_t, 256> kTable = makeTable();

} // namespace

uint32_t crc32(const uint8_t *data, size_t len)
{
    uint32_t c = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; ++i) {
        c = kTable[(c ^ data[i]) & 0xFFU] ^ (c >> 8U);
    }
    return c ^ 0xFFFFFFFFU;
}

} // namespace core
