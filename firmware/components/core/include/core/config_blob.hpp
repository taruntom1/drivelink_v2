#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/instance.hpp"
#include "drivelink/protocol/identity_codec.hpp"

namespace core {

/// CRC-32 (IEEE 802.3, the zlib one: reflected, polynomial 0xEDB88320, init
/// and final XOR 0xFFFFFFFF). Check value: "123456789" gives 0xCBF43926.
uint32_t crc32(const uint8_t *data, size_t len);

/// Instance blob format; layouts have their own version from F3d.
inline constexpr uint8_t kBlobFormat = 1;
inline constexpr uint8_t kLayoutBlobFormat = 3;

/// One stored param: its id and its 4-byte slot value.
struct ParamPair {
    uint16_t id;
    uint32_t value;
};

/// One stored layout entry.
struct LayoutRecord {
    uint8_t type_id;
    uint8_t instance;
    uint8_t loop;
    char label[kLabelBytes + 1]; ///< NUL-terminated
};

// The NVS blobs, all little-endian, each closed by the CRC-32 of every byte
// before it:
//   instance blob: fmt u8 | gen u32 | count u16 | count × (id u16, value u32) | crc32
//   layout blob: fmt u8 | gen u32 | count u8 | count × (type u8, inst u8, loop u8, label[16]) | name_len u8 | name bytes | crc32
// A label shorter than 16 bytes is padded with zeros.

inline constexpr size_t kMaxInstancePairs = 0xFFFF;
inline constexpr size_t kMaxLayoutRecords = 0xFF;

constexpr size_t instanceBlobSize(size_t pairs)
{
    return 1U + 4U + 2U + (pairs * 6U) + 4U;
}

constexpr size_t layoutBlobSize(size_t records, size_t name_bytes = 0)
{
    return 1U + 4U + 1U + (records * (3U + kLabelBytes)) + 1U + name_bytes + 4U;
}

/// Encode an instance blob. @return the bytes, or an empty vector when there
/// are more than `kMaxInstancePairs` pairs. Configure time: it allocates.
std::vector<uint8_t> encodeInstanceBlob(uint32_t gen, std::span<const ParamPair> pairs);

/// Decode an instance blob: the format, the exact length for its count and
/// the CRC must all check out. @return false (and `pairs` empty) otherwise.
bool decodeInstanceBlob(std::span<const uint8_t> blob, uint32_t &gen, std::vector<ParamPair> &pairs);

/// Encode a layout blob. @return the bytes, or an empty vector when there are
/// more than `kMaxLayoutRecords` records.
std::vector<uint8_t> encodeLayoutBlob(uint32_t gen, std::span<const LayoutRecord> records, std::string_view name = {});

/// Decode format 3 including the name, or LP format 2 with an empty name.
/// Exact length and CRC required; name and generation publish only on success.
bool decodeLayoutBlob(std::span<const uint8_t> blob, uint32_t &gen, std::vector<LayoutRecord> &records,
                      std::array<char, drivelink::protocol::kDeviceNameBytes + 1> *name = nullptr);

} // namespace core
