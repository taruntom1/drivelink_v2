#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace drivelink::protocol {

/// The serial link's rates. The index is the value of the system enum
/// `link.baud` (protocol/defs/system.yaml); never renumber.
inline constexpr std::array<uint32_t, 8> kLinkBaudRates{115200,  230400,  460800,  921600,
                                                        1000000, 1500000, 2000000, 3000000};

/// The rate of a `link.baud` value, 0 for a value that names none.
constexpr uint32_t linkBaudRate(uint32_t value)
{
    return (value < kLinkBaudRates.size()) ? kLinkBaudRates[value] : 0U;
}

/// The `link.baud` value of a rate, or nothing for a rate it does not offer.
constexpr std::optional<uint8_t> linkBaudValue(uint32_t rate)
{
    for (uint8_t i = 0; i < kLinkBaudRates.size(); ++i) {
        if (kLinkBaudRates[i] == rate) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace drivelink::protocol
