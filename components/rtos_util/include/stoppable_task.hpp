#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace rtos_util {

/// A FreeRTOS task with a cooperative stop and a real join.
///
/// The task body runs until it returns. To stop it, the owner calls
/// `stop()`: this sets a stop flag, wakes the body if it is sleeping in
/// `sleepFor()` / `sleepUntil()` (or blocked in `ulTaskNotifyTake()`),
/// and then waits until the body has returned. The task is never deleted
/// while it is still running its body, so it can never be killed while
/// holding a mutex, and its handle is never used after the task is gone.
///
/// Rules for the body:
///   - Poll `stopRequested()` regularly and return once it is true.
///   - Every blocking wait must have a bounded timeout (or use the sleep
///     helpers below), so a stop request is noticed promptly.
///
/// `stop()` waits as long as it takes and logs a warning every second
/// while the body has not returned yet.
///
/// The object must outlive the task: its destructor calls `stop()`.
class StoppableTask {
public:
    using Body = std::function<void()>;

    StoppableTask();
    ~StoppableTask();

    StoppableTask(const StoppableTask&) = delete;
    StoppableTask& operator=(const StoppableTask&) = delete;
    StoppableTask(StoppableTask&&) = delete;
    StoppableTask& operator=(StoppableTask&&) = delete;

    /// Create the task and run `body` on it.
    /// @return ESP_OK, ESP_ERR_INVALID_STATE if already running,
    ///         ESP_ERR_INVALID_ARG if `body` is empty,
    ///         ESP_ERR_NO_MEM if the task could not be created.
    esp_err_t start(const char* name, uint32_t stack_size, UBaseType_t priority, Body body);

    /// Ask the body to return and wake it if it is sleeping. Does not wait.
    /// Safe to call when not running.
    void requestStop();

    /// requestStop(), then wait until the body has returned and delete the
    /// task. Idempotent. Must not be called from the task itself.
    void stop();

    /// True between a successful start() and the end of stop().
    bool isRunning() const;

    /// True once a stop has been requested. Intended for the body's loop.
    bool stopRequested() const;

    /// Wake the task (xTaskNotifyGive). No-op when not running.
    void notify();

    /// Sleep for `ticks`, returning early if a stop is requested.
    /// Call only from the body. @return false if a stop was requested.
    bool sleepFor(TickType_t ticks);

    /// Fixed-rate sleep with vTaskDelayUntil() semantics, returning early if
    /// a stop is requested. Call only from the body.
    /// @return false if a stop was requested.
    bool sleepUntil(TickType_t& last_wake, TickType_t period);

private:
    static void entry(void* arg);

    Body m_body;
    std::atomic<TaskHandle_t> m_handle;
    std::atomic<bool> m_stop_requested;
    StaticSemaphore_t m_done_storage;
    SemaphoreHandle_t m_done;
    const char* m_name;
};

} // namespace rtos_util
