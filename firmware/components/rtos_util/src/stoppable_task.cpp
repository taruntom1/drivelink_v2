#include "stoppable_task.hpp"

#include <utility>

#include "esp_log.h"

static const char* const TAG = "stoppable_task";

namespace rtos_util {

namespace {
constexpr TickType_t kJoinWarnInterval = pdMS_TO_TICKS(1000);
} // namespace

StoppableTask::StoppableTask()
    : m_body(),
      m_handle(nullptr),
      m_stop_requested(false),
      m_done_storage(),
      m_done(xSemaphoreCreateBinaryStatic(&m_done_storage)),
      m_name("")
{
}

StoppableTask::~StoppableTask()
{
    stop();
    vSemaphoreDelete(m_done);
}

esp_err_t StoppableTask::start(const char* name, uint32_t stack_size, UBaseType_t priority, Body body,
                                BaseType_t core_id)
{
    if (isRunning()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!body) {
        return ESP_ERR_INVALID_ARG;
    }
    if (core_id != tskNO_AFFINITY && (core_id < 0 || core_id >= portNUM_PROCESSORS)) {
        ESP_LOGE(TAG, "%s: invalid core_id %d", (name != nullptr) ? name : "", static_cast<int>(core_id));
        return ESP_ERR_INVALID_ARG;
    }

    m_body = std::move(body);
    m_name = (name != nullptr) ? name : "";
    m_stop_requested.store(false, std::memory_order_release);

    TaskHandle_t handle = nullptr;
    if (xTaskCreatePinnedToCore(&StoppableTask::entry, m_name, stack_size, this, priority, &handle,
                                core_id) != pdPASS) {
        m_body = nullptr;
        return ESP_ERR_NO_MEM;
    }
    m_handle.store(handle, std::memory_order_release);
    return ESP_OK;
}

void StoppableTask::requestStop()
{
    m_stop_requested.store(true, std::memory_order_release);
    // The handle stays valid until stop() deletes the task: after the body
    // returns, entry() parks the task instead of deleting itself.
    notify();
}

void StoppableTask::stop()
{
    TaskHandle_t handle = m_handle.load(std::memory_order_acquire);
    if (handle == nullptr) {
        return;
    }
    if (handle == xTaskGetCurrentTaskHandle()) {
        ESP_LOGE(TAG, "%s: stop() called from the task itself; ignoring", m_name);
        return;
    }

    requestStop();

    while (xSemaphoreTake(m_done, kJoinWarnInterval) != pdTRUE) {
        ESP_LOGW(TAG, "%s: still waiting for task to stop", m_name);
        notify();
    }

    // The body has returned and holds nothing; the task is parked in entry().
    vTaskDelete(handle);
    m_handle.store(nullptr, std::memory_order_release);
    m_body = nullptr;
}

bool StoppableTask::isRunning() const
{
    return m_handle.load(std::memory_order_acquire) != nullptr;
}

bool StoppableTask::stopRequested() const
{
    return m_stop_requested.load(std::memory_order_acquire);
}

void StoppableTask::notify()
{
    TaskHandle_t handle = m_handle.load(std::memory_order_acquire);
    if (handle != nullptr) {
        xTaskNotifyGive(handle);
    }
}

uint32_t StoppableTask::stackHighWaterMark() const
{
    TaskHandle_t handle = m_handle.load(std::memory_order_acquire);
    return (handle != nullptr) ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(handle)) : 0U;
}

bool StoppableTask::sleepFor(TickType_t ticks)
{
    TickType_t last_wake = xTaskGetTickCount();
    return sleepUntil(last_wake, ticks);
}

bool StoppableTask::sleepUntil(TickType_t& last_wake, TickType_t period)
{
    // A period that rounded down to 0 ticks (pdMS_TO_TICKS of a value below
    // one tick) would return immediately every time and spin the caller.
    if (period == 0) {
        period = 1;
    }
    while (!stopRequested()) {
        // Unsigned arithmetic handles tick-count wrap-around.
        const TickType_t elapsed = xTaskGetTickCount() - last_wake;
        if (elapsed >= period) {
            last_wake += period;
            return true;
        }
        (void)ulTaskNotifyTake(pdTRUE, period - elapsed);
    }
    return false;
}

void StoppableTask::entry(void* arg)
{
    auto* self = static_cast<StoppableTask*>(arg);
    self->m_body();
    xSemaphoreGive(self->m_done);
    // Do not touch `self` past this point: the owner may destroy it as soon
    // as it sees the semaphore. Park until the owner deletes this task.
    for (;;) {
        vTaskSuspend(nullptr);
    }
}

} // namespace rtos_util
