// core_os::PeriodicTask, ESP-IDF backend: a StoppableTask woken by a
// TaskTimer (periodic esp_timer, task dispatch).

#include "core_os/periodic_task.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "tick_util.hpp"
#include "esp_task_wdt.h"
#include "core_os/realtime.hpp"

static const char *const TAG = "core_os_task";

namespace core_os {

namespace {

/// The body's notify wait: bounded, so a missed stop request is noticed
/// (stop() also notifies the task, so this is only a backstop).
constexpr uint32_t kWaitBackstopMs = 100;

} // namespace

PeriodicTask::PeriodicTask() = default;

PeriodicTask::~PeriodicTask()
{
    stop();
}

Err PeriodicTask::start(const char *name, uint32_t stack_bytes, uint8_t priority, int8_t core, uint32_t period_us,
                        TickFn fn, void *ctx, bool realtime)
{
    if (m_state.task.isRunning()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fn == nullptr || period_us == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    m_state.fn = fn;
    m_state.ctx = ctx;
    m_state.period_us.store(period_us, std::memory_order_relaxed);
    m_state.watchdog_ready.store(0);

    esp_err_t err = m_state.timer.create(m_state.task, name);
    if (err != ESP_OK) {
        return err;
    }

    detail::PeriodicTaskState *state = &m_state;
    const BaseType_t core_id = (core == kNoAffinity) ? tskNO_AFFINITY : static_cast<BaseType_t>(core);
    err = m_state.task.start(
        name, stack_bytes, priority,
        [state, realtime]() {
            const auto wd = realtime ? esp_task_wdt_add(nullptr) : ESP_OK;
            state->watchdog_ready.store(wd == ESP_OK ? 1 : wd, std::memory_order_release);
            if (wd != ESP_OK) { return; }
            const TickType_t backstop = rtos_util::msToTicksCeil(kWaitBackstopMs);
            while (!state->task.stopRequested()) {
                // The count is the number of timer periods since the last wake-up.
                const uint32_t elapsed = ulTaskNotifyTake(pdTRUE, backstop);
                if (state->task.stopRequested()) {
                    break;
                }
                if (elapsed > 0U) {
                    state->fn(state->ctx, elapsed);
                    if (realtime) {
                        const auto reset = esp_task_wdt_reset();
                        if (reset != ESP_OK) { ESP_LOGE(TAG, "WDT reset failed: %s", esp_err_to_name(reset)); }
                    }
                }
            }
            if (realtime) {
                const auto removed = esp_task_wdt_delete(nullptr);
                if (removed != ESP_OK) { ESP_LOGE(TAG, "WDT delete failed: %s", esp_err_to_name(removed)); }
            }
        },
        core_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: task start failed: %s", name, esp_err_to_name(err));
        m_state.timer.destroy();
        return err;
    }

    // The owner waits only during setup; the task registers itself before
    // the timer can wake it. A registration failure leaves no task behind.
    const auto deadline = esp_timer_get_time() + 1000000;
    while (m_state.watchdog_ready.load(std::memory_order_acquire) == 0 && esp_timer_get_time() < deadline) { vTaskDelay(1); }
    const auto ready = m_state.watchdog_ready.load(std::memory_order_acquire);
    if (ready != 1) { m_state.timer.destroy(); m_state.task.stop(); return ready == 0 ? ESP_ERR_TIMEOUT : ready; }

    err = m_state.timer.start(period_us);
    if (err != ESP_OK) {
        m_state.timer.destroy();
        m_state.task.stop();
        return err;
    }
    return ESP_OK;
}

Err PeriodicTask::setPeriod(uint32_t period_us)
{
    if (period_us == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (m_state.task.isRunning()) {
        const esp_err_t err = m_state.timer.setPeriod(period_us);
        if (err != ESP_OK) {
            return err;
        }
    }
    m_state.period_us.store(period_us, std::memory_order_relaxed);
    return ESP_OK;
}

void PeriodicTask::stop()
{
    // The timer first, waiting out an in-flight callback, so nothing notifies
    // the task once it is gone.
    m_state.timer.destroy();
    m_state.task.stop();
}

bool PeriodicTask::isRunning() const
{
    return m_state.task.isRunning();
}

uint32_t PeriodicTask::periodUs() const
{
    return m_state.period_us.load(std::memory_order_relaxed);
}

uint32_t PeriodicTask::stackHighWaterMark() const
{
    return m_state.task.stackHighWaterMark();
}

bool enterRealtime(bool) noexcept { return false; }
void leaveRealtime(bool) noexcept {}
bool realtimeActive() noexcept { return false; }
void recordRealtimeViolation() noexcept {}
uint64_t realtimeViolations() noexcept { return 0; }

} // namespace core_os
