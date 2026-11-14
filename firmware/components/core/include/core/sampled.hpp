#pragma once

#include <cstdint>

namespace core {

inline constexpr uint32_t kInvalidAgeUs = 0xFFFFFFFFU;
inline constexpr uint32_t kMaxAgeUs = 0xFFFFFFFEU;
constexpr uint32_t sampleAgeUs(int64_t now_us, int64_t timestamp_us)
{
    if (now_us <= timestamp_us) { return 0; }
    const uint64_t age = static_cast<uint64_t>(now_us) - static_cast<uint64_t>(timestamp_us);
    return age > kMaxAgeUs ? kMaxAgeUs : static_cast<uint32_t>(age);
}

/// Storage owned by the consumer loop, filled by the core before tick().
template <typename T> struct Sampled {
    T value{};
    uint32_t age_us = kInvalidAgeUs;
    bool valid = false;
    bool fresh = false;
};

} // namespace core
