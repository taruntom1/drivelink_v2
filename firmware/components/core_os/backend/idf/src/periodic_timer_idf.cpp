#include "core_os/periodic_timer.hpp"
#include "esp_log.h"
#include "tick_util.hpp"

static const char *const TAG = "core_os_timer";
namespace core_os {
PeriodicTimer::~PeriodicTimer() { stop(); }
Err PeriodicTimer::start(const char *name, uint32_t period_us, OneShotFn fn, void *ctx) {
    if (m_state.handle || !fn || !period_us) { return ESP_ERR_INVALID_ARG; }
    m_state.fn = fn; m_state.ctx = ctx;
    m_state.fence_done = xSemaphoreCreateBinaryStatic(&m_state.fence_storage);
    esp_timer_create_args_t fence{};
    fence.callback = [](void *arg) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(arg)); };
    fence.arg = m_state.fence_done; fence.dispatch_method = ESP_TIMER_TASK; fence.name = "core_mon_fence";
    auto err = esp_timer_create(&fence, &m_state.fence);
    if (err != ESP_OK) { vSemaphoreDelete(m_state.fence_done); m_state.fence_done = nullptr; return err; }
    esp_timer_create_args_t args{};
    args.callback = [](void *arg) { auto *s = static_cast<detail::PeriodicTimerState *>(arg); s->fn(s->ctx); };
    args.arg = &m_state; args.dispatch_method = ESP_TIMER_TASK; args.name = name; args.skip_unhandled_events = true;
    err = esp_timer_create(&args, &m_state.handle);
    if (err == ESP_OK) { err = esp_timer_start_periodic(m_state.handle, period_us); }
    if (err != ESP_OK) { stop(); }
    return err;
}
void PeriodicTimer::stop() {
    if (m_state.handle) {
        esp_err_t stopped;
        while ((stopped = esp_timer_stop(m_state.handle)) != ESP_OK && stopped != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "stop: %s", esp_err_to_name(stopped)); vTaskDelay(1);
        }
        // Both timers dispatch on the same task. The fence is allocated at
        // setup, and proves no callback still uses the owner's context.
        esp_err_t err;
        while ((err = esp_timer_start_once(m_state.fence, 1)) != ESP_OK) {
            ESP_LOGE(TAG, "fence start: %s", esp_err_to_name(err)); vTaskDelay(1);
        }
        while (xSemaphoreTake(m_state.fence_done, rtos_util::msToTicksCeil(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "waiting for timer callback fence");
        }
        const auto deleted = esp_timer_delete(m_state.handle);
        if (deleted != ESP_OK) { ESP_LOGE(TAG, "delete: %s", esp_err_to_name(deleted)); }
        m_state.handle = nullptr;
    }
    if (m_state.fence) {
        const auto deleted = esp_timer_delete(m_state.fence);
        if (deleted != ESP_OK) { ESP_LOGE(TAG, "fence delete: %s", esp_err_to_name(deleted)); }
        m_state.fence = nullptr;
    }
    if (m_state.fence_done) { vSemaphoreDelete(m_state.fence_done); m_state.fence_done = nullptr; }
}
}
