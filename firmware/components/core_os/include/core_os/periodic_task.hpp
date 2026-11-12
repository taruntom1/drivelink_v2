#pragma once

#include <cstdint>

#include "core_os/detail/backend_types.hpp"
#include "core_os/types.hpp"

namespace core_os {

/// A task that calls `fn(ctx, elapsed_periods)` once per period.
///
/// On the device it is an `rtos_util::StoppableTask` woken by an
/// `rtos_util::TaskTimer` (a periodic esp_timer), so the rate does not depend
/// on `CONFIG_FREERTOS_HZ`; natively it is a `std::thread` that sleeps until
/// each deadline. A late wake-up is not made up with extra calls: the next
/// call gets the number of periods that went by (the catch-up count), so
/// `dt = elapsed × period`.
///
/// `start()` allocates the task; nothing is allocated after it. Not
/// thread-safe: the owner serializes `start`, `setPeriod` and `stop`.
class PeriodicTask {
public:
    PeriodicTask();
    ~PeriodicTask(); ///< stop()

    PeriodicTask(const PeriodicTask &) = delete;
    PeriodicTask &operator=(const PeriodicTask &) = delete;

    /// Start the task. `name` must outlive it. `core` is 0, 1 or
    /// `kNoAffinity`, and `priority` a FreeRTOS priority (ignored natively).
    /// @return kOk, or an error (already running, a zero period or a null
    ///         `fn`, or the task or timer could not be created).
    Err start(const char *name, uint32_t stack_bytes, uint8_t priority, int8_t core, uint32_t period_us, TickFn fn,
              void *ctx);

    /// Change the period. While running, the timer restarts at the new
    /// period; otherwise the next `start()` uses it. On an error the old
    /// period stays. @return kOk, or an error (a zero period, or the timer
    /// could not be restarted).
    Err setPeriod(uint32_t period_us);

    /// Stop the timer, then wait for the body to return. Idempotent.
    void stop();

    bool isRunning() const;
    uint32_t periodUs() const;

    /// The least free stack since the task started, in bytes (0 when not
    /// running, and natively).
    uint32_t stackHighWaterMark() const;

private:
    detail::PeriodicTaskState m_state;
};

} // namespace core_os
