#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace drivelink::protocol {
using Sha256Digest = std::array<uint8_t, 32>;
// Incremental SHA-256 (FIPS 180-4). No allocation; final() leaves this reusable.
class Sha256 {
public:
    void update(std::span<const uint8_t> bytes);
    Sha256Digest final() const;
private:
    void block(const uint8_t *bytes);
    std::array<uint32_t, 8> m_h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                              0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<uint8_t, 64> m_buffer{};
    uint64_t m_bytes = 0;
};
Sha256Digest sha256(std::span<const uint8_t> bytes);
} // namespace drivelink::protocol
