#pragma once

#include <cstdint>

#include "core_os/detail/backend_types.hpp"
#include "core_os/types.hpp"

namespace core_os {

/// Call a function once after a delay: an esp_timer one-shot on the device, a
/// thread natively.
///
/// `start()` allocates (the timer on the device, the thread natively), so it
/// belongs to setup. Not thread-safe: the owner serializes `start` and
/// `cancel`. The destructor cancels.
class OneShotTimer {
public:
    OneShotTimer();
    ~OneShotTimer();

    OneShotTimer(const OneShotTimer &) = delete;
    OneShotTimer &operator=(const OneShotTimer &) = delete;

    /// Call `fn(ctx)` once, `delay_us` from now. A timer that is still pending
    /// is cancelled first. `name` must outlive the timer. @return kOk, or an
    /// error (a null `fn`, or the timer could not be created or started).
    Err start(const char *name, uint32_t delay_us, OneShotFn fn, void *ctx);

    /// Cancel a pending call. Idempotent. Natively it also waits for a call
    /// that is running; on the device it does not, so the callback must stay
    /// valid to run (the core's never touches anything that is freed).
    void cancel();

private:
    detail::OneShotState m_state;
};

} // namespace core_os
