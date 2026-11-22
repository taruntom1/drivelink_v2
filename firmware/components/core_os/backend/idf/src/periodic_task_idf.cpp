// core_os::PeriodicTask, ESP-IDF backend: a StoppableTask woken by a
// TaskTimer (periodic esp_timer, task dispatch).

#include "core_os/periodic_task.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "tick_util.hpp"
#include "esp_task_wdt.h"
#include "core_os/realtime.hpp"
#include "core_os/bench_affinity.hpp"
#include <array>

#if !defined(CONFIG_ESP_TASK_WDT_EN) || !defined(CONFIG_ESP_TASK_WDT_INIT) || !defined(CONFIG_ESP_TASK_WDT_PANIC)
#error "Core real-time loops require the initialized, panic-reset Task WDT"
#endif

static const char *const TAG = "core_os_task";

namespace core_os {

namespace {

/// The body's notify wait: bounded, so a missed stop request is noticed
/// (stop() also notifies the task, so this is only a backstop).
constexpr uint32_t kWaitBackstopMs = 100;

#if CONFIG_CORE_BENCH_CRASH
// The owner alone registers, logs and removes these pointers. The task only
// publishes its snapshot; stop() joins it before the owner frees the state.
std::array<detail::PeriodicTaskState *, 8> s_bench_tasks{};
int32_t affinityOfSelf()
{
    const auto affinity = xTaskGetCoreID(nullptr);
    return affinity == tskNO_AFFINITY ? kNoAffinity : static_cast<int32_t>(affinity);
}
#endif

} // namespace

#if CONFIG_CORE_BENCH_CRASH
void logBenchTaskAffinities()
{
    for (const auto *state : s_bench_tasks) {
        if (!state) { continue; }
        const auto after = state->bench_after.load(std::memory_order_acquire);
        if (after == -2) { continue; }
        ESP_LOGI(TAG, "Bench affinity: %s requested=%ld initial=%ld initial_cpu=%ld after=%ld after_cpu=%ld",
            state->bench_name, static_cast<long>(state->bench_requested), static_cast<long>(state->bench_initial),
            static_cast<long>(state->bench_initial_cpu), static_cast<long>(after), static_cast<long>(state->bench_after_cpu));
    }
}
#endif

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
#if CONFIG_CORE_BENCH_CRASH
    m_state.bench_name = name;
    m_state.bench_requested = core;
    m_state.bench_after.store(-2);
#endif

    esp_err_t err = m_state.timer.create(m_state.task, name);
    if (err != ESP_OK) {
        return err;
    }

    detail::PeriodicTaskState *state = &m_state;
    const BaseType_t core_id = (core == kNoAffinity) ? tskNO_AFFINITY : static_cast<BaseType_t>(core);
    err = m_state.task.start(
        name, stack_bytes, priority,
        [state, realtime]() {
#if CONFIG_CORE_BENCH_CRASH
            state->bench_initial = affinityOfSelf();
            state->bench_initial_cpu = xPortGetCoreID();
#endif
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
#if CONFIG_CORE_BENCH_CRASH
                    if (state->bench_after.load(std::memory_order_relaxed) == -2) {
                        const auto affinity = affinityOfSelf();
                        // A gated callback has done no FP work. For Any core,
                        // wait until a real tick has caused IDF's FPU pinning.
                        if (affinity != kNoAffinity) {
                            state->bench_after_cpu = xPortGetCoreID();
                            state->bench_after.store(affinity, std::memory_order_release);
                        }
                    }
#endif
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
#if CONFIG_CORE_BENCH_CRASH
    for (auto &entry : s_bench_tasks) {
        if (!entry) { entry = &m_state; break; }
    }
#endif
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
#if CONFIG_CORE_BENCH_CRASH
    for (auto &entry : s_bench_tasks) {
        if (entry == &m_state) { entry = nullptr; }
    }
#endif
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
