#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"

namespace rtos_util {

/// Milliseconds to FreeRTOS ticks, rounded up.
///
/// `pdMS_TO_TICKS` rounds down, so at a 100 Hz tick any value below 10 ms
/// becomes 0 ticks (no wait at all). Use this instead wherever the value is
/// small or comes from configuration. Any `ms > 0` gives at least one tick.
constexpr TickType_t msToTicksCeil(uint32_t ms)
{
    return static_cast<TickType_t>(
        ((static_cast<uint64_t>(ms) * configTICK_RATE_HZ) + 999U) / 1000U);
}

} // namespace rtos_util
