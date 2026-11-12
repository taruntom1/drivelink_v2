#pragma once

#include <cstdint>

namespace core_os {

/// Monotonic microseconds since boot (`esp_timer_get_time` on the device,
/// `steady_clock` natively). Any context, never blocks.
int64_t nowUs();

} // namespace core_os
