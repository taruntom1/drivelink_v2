#pragma once

#include <cstdint>

namespace core_os {

/// An error code. 0 is success. The IDF backend returns `esp_err_t` values, so
/// a failure can be passed straight to diag; the native backend returns -1.
using Err = int32_t;

inline constexpr Err kOk = 0;

/// The body of a `PeriodicTask`: called once per wake-up with the number of
/// periods that elapsed since the previous call (1 when on time; more after
/// a late wake-up). A plain function pointer, so nothing is allocated.
using TickFn = void (*)(void *ctx, uint32_t elapsed_periods);

/// "No core affinity" for `PeriodicTask::start`.
inline constexpr int8_t kNoAffinity = -1;

} // namespace core_os
