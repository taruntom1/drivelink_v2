#include "task_timer.hpp"

#include <cinttypes>

#include "esp_log.h"
#include "freertos/semphr.h"
#include "tick_util.hpp"

static const char* const TAG = "task_timer";

namespace rtos_util {

namespace {
constexpr TickType_t kFenceWarnTicks = pdMS_TO_TICKS(1000);
// Fallback when the fence timer cannot be used: well over one callback's
// duration (the callback only notifies a task).
constexpr uint32_t kFenceFallbackDelayMs = 10;
} // namespace

TaskTimer::~TaskTimer()
{
    destroy();
}

esp_err_t TaskTimer::create(StoppableTask& task, const char* name)
{
    if (m_timer != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    m_task = &task;
    m_name = (name != nullptr) ? name : "";

    esp_timer_create_args_t args{};
    args.callback = &TaskTimer::callback;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = m_name;
    args.skip_unhandled_events = true;

    const esp_err_t err = esp_timer_create(&args, &m_timer);
    if (err != ESP_OK) {
        m_timer = nullptr;
        ESP_LOGE(TAG, "%s: esp_timer_create failed: %s", m_name, esp_err_to_name(err));
    }
    return err;
}

esp_err_t TaskTimer::start(uint32_t period_us)
{
    if (m_timer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (period_us == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = isRunning() ? esp_timer_restart(m_timer, period_us)
                                      : esp_timer_start_periodic(m_timer, period_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: start at %" PRIu32 " us failed: %s", m_name, period_us, esp_err_to_name(err));
        return err;
    }
    m_period_us.store(period_us, std::memory_order_relaxed);
    return ESP_OK;
}

esp_err_t TaskTimer::setPeriod(uint32_t period_us)
{
    if (period_us == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (m_timer != nullptr && isRunning()) {
        return start(period_us);
    }
    m_period_us.store(period_us, std::memory_order_relaxed);
    return ESP_OK;
}

esp_err_t TaskTimer::stop()
{
    if (m_timer == nullptr) {
        return ESP_OK;
    }
    const esp_err_t err = esp_timer_stop(m_timer);
    // ESP_ERR_INVALID_STATE just means it was not running.
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "%s: esp_timer_stop failed: %s", m_name, esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

void TaskTimer::destroy()
{
    if (m_timer == nullptr) {
        return;
    }

    (void)stop();

    // esp_timer_stop() does not wait for a callback the esp_timer task has
    // already started. Callbacks run one at a time on that task, so once a
    // one-shot "fence" timer has fired, any in-flight callback of ours has
    // returned and none can run again.
    StaticSemaphore_t fence_storage;
    SemaphoreHandle_t fence_done = xSemaphoreCreateBinaryStatic(&fence_storage);
    esp_timer_create_args_t fence_args{};
    fence_args.callback = [](void* arg) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(arg)); };
    fence_args.arg = fence_done;
    fence_args.dispatch_method = ESP_TIMER_TASK;
    fence_args.name = "task_timer_fence";
    esp_timer_handle_t fence = nullptr;
    esp_err_t err = esp_timer_create(&fence_args, &fence);
    if (err == ESP_OK) {
        err = esp_timer_start_once(fence, 1);
        if (err == ESP_OK) {
            while (xSemaphoreTake(fence_done, kFenceWarnTicks) != pdTRUE) {
                ESP_LOGW(TAG, "%s: still waiting for the esp_timer task to drain the timer", m_name);
            }
        }
        esp_timer_delete(fence);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: timer fence failed (%s); delaying instead", m_name, esp_err_to_name(err));
        vTaskDelay(msToTicksCeil(kFenceFallbackDelayMs) + 1U);
    }
    vSemaphoreDelete(fence_done);

    const esp_err_t del_err = esp_timer_delete(m_timer);
    if (del_err != ESP_OK) {
        ESP_LOGW(TAG, "%s: esp_timer_delete failed: %s", m_name, esp_err_to_name(del_err));
    }
    m_timer = nullptr;
    m_task = nullptr;
}

bool TaskTimer::isRunning() const
{
    return (m_timer != nullptr) && esp_timer_is_active(m_timer);
}

void TaskTimer::callback(void* arg)
{
    // Runs on the esp_timer task: only wake the owner's task.
    StoppableTask* task = static_cast<TaskTimer*>(arg)->m_task;
    if (task != nullptr) {
        task->notify();
    }
}

} // namespace rtos_util
