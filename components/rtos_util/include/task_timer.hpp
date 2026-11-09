#pragma once

#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "esp_timer.h"
#include "stoppable_task.hpp"

namespace rtos_util {

/// A periodic esp_timer whose only job is to wake a StoppableTask.
///
/// The timer uses task dispatch (`ESP_TIMER_TASK`) with
/// `skip_unhandled_events`, and its callback does nothing but
/// `task.notify()`. The woken task takes the notification with
/// `ulTaskNotifyTake(pdTRUE, timeout)`; the returned count is the number of
/// periods that elapsed since it last woke (plus one for a stop request).
///
/// Lifecycle:
///   create() -> start() / setPeriod() / stop() (any order, repeatedly)
///   -> destroy() (or the destructor).
///
/// `destroy()` waits out a callback that the esp_timer task has already
/// started, so nothing touches the task after it returns. Call it before
/// stopping the task the timer wakes.
///
/// Not thread-safe: the owner serializes calls (the callback itself is safe
/// to run concurrently with any of them).
class TaskTimer {
public:
    TaskTimer() = default;
    ~TaskTimer();

    TaskTimer(const TaskTimer&) = delete;
    TaskTimer& operator=(const TaskTimer&) = delete;
    TaskTimer(TaskTimer&&) = delete;
    TaskTimer& operator=(TaskTimer&&) = delete;

    /// Create the (stopped) timer. `task` and `name` must outlive it.
    /// @return ESP_OK, ESP_ERR_INVALID_STATE if already created, or the
    ///         esp_timer_create error.
    esp_err_t create(StoppableTask& task, const char* name);

    /// Start firing every `period_us`. If already running, restarts it with
    /// the new period. @return ESP_ERR_INVALID_STATE if not created,
    /// ESP_ERR_INVALID_ARG if `period_us` is 0, or the esp_timer error.
    esp_err_t start(uint32_t period_us);

    /// Change the period. Restarts the timer if it is running; otherwise the
    /// new period is used by the next start(). On error the old period stays.
    esp_err_t setPeriod(uint32_t period_us);

    /// Stop firing. Idempotent. Does not wait for an in-flight callback,
    /// which is harmless because it only notifies the task.
    esp_err_t stop();

    /// stop(), wait out an in-flight callback, then delete the timer.
    /// Idempotent; safe to call when create() never ran or failed.
    void destroy();

    bool isCreated() const { return m_timer != nullptr; }
    bool isRunning() const;
    uint32_t periodUs() const { return m_period_us.load(std::memory_order_relaxed); }

private:
    static void callback(void* arg);

    esp_timer_handle_t m_timer = nullptr;
    StoppableTask* m_task = nullptr;
    const char* m_name = "";
    std::atomic<uint32_t> m_period_us{0};
};

} // namespace rtos_util
