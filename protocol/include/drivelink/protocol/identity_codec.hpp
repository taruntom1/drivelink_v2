#pragma once

#include <array>
#include <string_view>
#include "drivelink/protocol/wire_codec.hpp"

namespace drivelink::protocol {
using DeviceId = std::array<uint8_t, 6>;
inline constexpr size_t kDeviceNameBytes = 16;

// UTF-8 scalar values, excluding C0/C1 controls. No normalization or truncation.
inline bool validDeviceName(std::string_view name)
{
    if (name.size() > kDeviceNameBytes) { return false; }
    for (size_t i = 0; i < name.size();) {
        const auto first = static_cast<uint8_t>(name[i++]);
        uint32_t cp = first;
        size_t extra = 0;
        uint32_t minimum = 0;
        if (first >= 0xC2 && first <= 0xDF) { cp = first & 0x1F; extra = 1; minimum = 0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { cp = first & 0x0F; extra = 2; minimum = 0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { cp = first & 7; extra = 3; minimum = 0x10000; }
        else if (first >= 0x80) { return false; }
        if (extra > name.size() - i) { return false; }
        for (size_t j = 0; j < extra; ++j) {
            const auto b = static_cast<uint8_t>(name[i++]);
            if ((b & 0xC0) != 0x80) { return false; }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ||
            cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) { return false; }
    }
    return true;
}

inline bool writeDeviceName(wire::Writer &w, std::string_view name)
{
    if (!validDeviceName(name)) { return false; }
    w.appendU8(static_cast<uint8_t>(name.size()));
    w.appendBytes(reinterpret_cast<const uint8_t *>(name.data()), name.size());
    return w.ok();
}

// Fixed output; publish only after the entire field validates. Caller checks done().
inline bool readDeviceName(wire::Reader &r, std::array<char, kDeviceNameBytes + 1> &out)
{
    const size_t n = r.readU8();
    if (!r.ok() || n > kDeviceNameBytes || n > r.remaining()) { return false; }
    std::array<char, kDeviceNameBytes + 1> next{};
    for (size_t i = 0; i < n; ++i) { next[i] = static_cast<char>(r.readU8()); }
    if (!validDeviceName({next.data(), n})) { return false; }
    out = next;
    return true;
}
} // namespace drivelink::protocol
