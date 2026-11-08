#include "time_sync.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "stoppable_task.hpp"

static const char* TAG = "time_sync";

namespace time_sync {

namespace {

int64_t nowUnixUs()
{
    timeval tv = {};
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000000LL + static_cast<int64_t>(tv.tv_usec);
}

esp_err_t setUnixUs(int64_t unix_us)
{
    if (unix_us < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    timeval tv = {};
    tv.tv_sec = static_cast<time_t>(unix_us / 1000000LL);
    tv.tv_usec = static_cast<suseconds_t>(unix_us % 1000000LL);
    if (settimeofday(&tv, nullptr) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

} // namespace

struct SyncResponseEvent {
    protocol::TimeSyncResponse response;
    int64_t client_unix_recv_us;
};

struct TimeSync::Impl {
    uart_comm::UartComm* uart;
    TimeSyncConfig config;

    SemaphoreHandle_t sync_mutex;
    SemaphoreHandle_t state_mutex;
    QueueHandle_t response_queue;

    rtos_util::StoppableTask periodic_task;
    uart_comm::CallbackRegistration response_cb;
    std::atomic<bool> valid;

    std::atomic<uint16_t> next_request_id;

    int64_t estimated_offset_us;
    uint32_t last_round_trip_us;
    int64_t last_server_unix_us;
    uint64_t last_sync_monotonic_us;
};

TimeSync::TimeSync(uart_comm::UartComm& uart, const TimeSyncConfig& config)
    : m_impl(new Impl{})
{
    m_impl->uart = &uart;
    m_impl->config = config;
    m_impl->sync_mutex = nullptr;
    m_impl->state_mutex = nullptr;
    m_impl->response_queue = nullptr;
    m_impl->valid.store(false, std::memory_order_relaxed);
    m_impl->next_request_id.store(1, std::memory_order_relaxed);
    m_impl->estimated_offset_us = 0;
    m_impl->last_round_trip_us = 0;
    m_impl->last_server_unix_us = 0;
    m_impl->last_sync_monotonic_us = 0;

    if (!m_impl->uart->isValid()) {
        ESP_LOGE(TAG, "uart_comm is not valid; cannot initialize time_sync");
        return;
    }

    m_impl->sync_mutex = xSemaphoreCreateMutex();
    if (m_impl->sync_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create sync mutex");
        return;
    }

    m_impl->state_mutex = xSemaphoreCreateMutex();
    if (m_impl->state_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        vSemaphoreDelete(m_impl->sync_mutex);
        m_impl->sync_mutex = nullptr;
        return;
    }

    m_impl->response_queue = xQueueCreate(1, sizeof(SyncResponseEvent));
    if (m_impl->response_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create response queue");
        vSemaphoreDelete(m_impl->state_mutex);
        vSemaphoreDelete(m_impl->sync_mutex);
        m_impl->state_mutex = nullptr;
        m_impl->sync_mutex = nullptr;
        return;
    }

    esp_err_t ret = m_impl->uart->registerCallback(
        m_impl->config.response_msg_type,
        [this](const uart_comm::RxFrame& frame) {
            onResponseFrame(frame);
        },
        m_impl->response_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register callback for msg_type=0x%02X: %s",
                 m_impl->config.response_msg_type, esp_err_to_name(ret));
        vQueueDelete(m_impl->response_queue);
        vSemaphoreDelete(m_impl->state_mutex);
        vSemaphoreDelete(m_impl->sync_mutex);
        m_impl->response_queue = nullptr;
        m_impl->state_mutex = nullptr;
        m_impl->sync_mutex = nullptr;
        return;
    }

    m_impl->valid.store(true, std::memory_order_relaxed);

    if (m_impl->config.sync_interval_ms > 0) {
        ret = startPeriodicSync();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Periodic sync not started: %s", esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "Initialized request_type=0x%02X response_type=0x%02X interval_ms=%lu rtt_comp=%s",
             m_impl->config.request_msg_type,
             m_impl->config.response_msg_type,
             static_cast<unsigned long>(m_impl->config.sync_interval_ms),
             m_impl->config.enable_round_trip_compensation ? "on" : "off");
}

TimeSync::~TimeSync()
{
    if (!m_impl) {
        return;
    }

    m_impl->valid.store(false, std::memory_order_relaxed);

    // Join the periodic task first (an in-progress sync finishes or times
    // out), then unregister; reset() waits for a callback that is already
    // running on the UART RX task. Only then free what they use.
    stopPeriodicSync();
    m_impl->response_cb.reset();

    if (m_impl->response_queue) {
        vQueueDelete(m_impl->response_queue);
    }
    if (m_impl->state_mutex) {
        vSemaphoreDelete(m_impl->state_mutex);
    }
    if (m_impl->sync_mutex) {
        vSemaphoreDelete(m_impl->sync_mutex);
    }

    delete m_impl;
    m_impl = nullptr;
}

bool TimeSync::isValid() const
{
    return m_impl && m_impl->valid.load(std::memory_order_relaxed);
}

esp_err_t TimeSync::triggerManualSync(bool wait_for_response, TickType_t timeout)
{
    if (!isValid()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!wait_for_response) {
        protocol::TimeSyncRequest req = {};
        req.request_id = m_impl->next_request_id.fetch_add(1, std::memory_order_relaxed);
        req.client_unix_send_us = nowUnixUs();

        uint8_t payload[protocol::kRequestEncodedSize] = {};
        size_t len = protocol::encodeRequest(req, payload, sizeof(payload));
        if (len == 0) {
            return ESP_ERR_INVALID_ARG;
        }

        return m_impl->uart->sendCommand(m_impl->config.request_msg_type, payload, static_cast<uint8_t>(len));
    }

    return performSync(timeout);
}

esp_err_t TimeSync::startPeriodicSync()
{
    if (!isValid()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (m_impl->config.sync_interval_ms == 0) {
        ESP_LOGE(TAG, "sync_interval_ms is 0; periodic sync is disabled by config");
        return ESP_ERR_INVALID_ARG;
    }

    if (m_impl->periodic_task.isRunning()) {
        return ESP_OK;
    }

    esp_err_t ret = m_impl->periodic_task.start("time_sync_task", 4096, 6, [this] { periodicTask(); });
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create periodic sync task: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    return ESP_OK;
}

void TimeSync::stopPeriodicSync()
{
    if (!m_impl) {
        return;
    }

    // Wakes the task if it is sleeping between syncs and waits until it has
    // returned. Never force-deletes it, so sync_mutex can't be left held.
    m_impl->periodic_task.stop();
}

bool TimeSync::isPeriodicSyncRunning() const
{
    return m_impl && m_impl->periodic_task.isRunning();
}

int64_t TimeSync::getEstimatedOffsetUs() const
{
    if (!isValid()) {
        return 0;
    }

    int64_t value = 0;
    xSemaphoreTake(m_impl->state_mutex, portMAX_DELAY);
    value = m_impl->estimated_offset_us;
    xSemaphoreGive(m_impl->state_mutex);
    return value;
}

uint32_t TimeSync::getLastRoundTripUs() const
{
    if (!isValid()) {
        return 0;
    }

    uint32_t value = 0;
    xSemaphoreTake(m_impl->state_mutex, portMAX_DELAY);
    value = m_impl->last_round_trip_us;
    xSemaphoreGive(m_impl->state_mutex);
    return value;
}

int64_t TimeSync::getLastServerUnixUs() const
{
    if (!isValid()) {
        return 0;
    }

    int64_t value = 0;
    xSemaphoreTake(m_impl->state_mutex, portMAX_DELAY);
    value = m_impl->last_server_unix_us;
    xSemaphoreGive(m_impl->state_mutex);
    return value;
}

uint64_t TimeSync::getLastSyncMonotonicUs() const
{
    if (!isValid()) {
        return 0;
    }

    uint64_t value = 0;
    xSemaphoreTake(m_impl->state_mutex, portMAX_DELAY);
    value = m_impl->last_sync_monotonic_us;
    xSemaphoreGive(m_impl->state_mutex);
    return value;
}

void TimeSync::periodicTask()
{
    TickType_t last_wake = xTaskGetTickCount();

    while (!m_impl->periodic_task.stopRequested()) {
        TickType_t timeout = pdMS_TO_TICKS(m_impl->config.response_timeout_ms);
        esp_err_t ret = performSync(timeout);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Periodic sync failed: %s", esp_err_to_name(ret));
        }

        m_impl->periodic_task.sleepUntil(last_wake, pdMS_TO_TICKS(m_impl->config.sync_interval_ms));
    }
}

void TimeSync::onResponseFrame(const uart_comm::RxFrame& frame)
{
    protocol::TimeSyncResponse response = {};
    if (!protocol::decodeResponse(frame.payload, frame.len, response)) {
        ESP_LOGW(TAG, "Invalid time sync response len=%u msg_type=0x%02X", frame.len, frame.msg_type);
        return;
    }

    SyncResponseEvent event = {};
    event.response = response;
    event.client_unix_recv_us = nowUnixUs();

    xQueueOverwrite(m_impl->response_queue, &event);
}

esp_err_t TimeSync::performSync(TickType_t timeout)
{
    if (xSemaphoreTake(m_impl->sync_mutex, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    protocol::TimeSyncRequest req = {};
    req.request_id = m_impl->next_request_id.fetch_add(1, std::memory_order_relaxed);
    req.client_unix_send_us = nowUnixUs();

    uint8_t payload[protocol::kRequestEncodedSize] = {};
    size_t len = protocol::encodeRequest(req, payload, sizeof(payload));
    if (len == 0) {
        xSemaphoreGive(m_impl->sync_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    SyncResponseEvent stale = {};
    while (xQueueReceive(m_impl->response_queue, &stale, 0) == pdTRUE) {
    }

    esp_err_t send_ret = m_impl->uart->sendCommand(m_impl->config.request_msg_type, payload, static_cast<uint8_t>(len));
    if (send_ret != ESP_OK) {
        xSemaphoreGive(m_impl->sync_mutex);
        return send_ret;
    }

    TickType_t start = xTaskGetTickCount();
    while (true) {
        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed = now - start;
        if (elapsed >= timeout) {
            xSemaphoreGive(m_impl->sync_mutex);
            return ESP_ERR_TIMEOUT;
        }

        TickType_t remaining = timeout - elapsed;
        SyncResponseEvent event = {};
        if (xQueueReceive(m_impl->response_queue, &event, remaining) != pdTRUE) {
            xSemaphoreGive(m_impl->sync_mutex);
            return ESP_ERR_TIMEOUT;
        }

        if (event.response.request_id != req.request_id) {
            continue;
        }

        if (event.response.status != 0) {
            ESP_LOGW(TAG, "Server returned error status=%u request_id=%u",
                     event.response.status,
                     event.response.request_id);
            xSemaphoreGive(m_impl->sync_mutex);
            return ESP_ERR_INVALID_RESPONSE;
        }

        int64_t t0 = req.client_unix_send_us;
        int64_t t3 = event.client_unix_recv_us;
        int64_t t1 = event.response.server_unix_rx_us;
        int64_t t2 = event.response.server_unix_tx_us;

        int64_t offset_us = t2 - t3;
        int64_t rtt_us = std::max<int64_t>(0, t3 - t0);

        if (m_impl->config.enable_round_trip_compensation) {
            // NTP-style estimate: theta = ((t1 - t0) + (t2 - t3)) / 2
            offset_us = ((t1 - t0) + (t2 - t3)) / 2;
            // delta = (t3 - t0) - (t2 - t1)
            rtt_us = (t3 - t0) - (t2 - t1);
            if (rtt_us < 0) {
                rtt_us = 0;
            }
        }

        int64_t corrected_now_us = t3 + offset_us;
        esp_err_t set_ret = setUnixUs(corrected_now_us);
        if (set_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to apply synchronized time request_id=%u",
                     event.response.request_id);
            xSemaphoreGive(m_impl->sync_mutex);
            return set_ret;
        }

        xSemaphoreTake(m_impl->state_mutex, portMAX_DELAY);
        m_impl->estimated_offset_us = offset_us;
        m_impl->last_round_trip_us = static_cast<uint32_t>(std::min<int64_t>(rtt_us, UINT32_MAX));
        m_impl->last_server_unix_us = t2;
        m_impl->last_sync_monotonic_us = static_cast<uint64_t>(esp_timer_get_time());
        xSemaphoreGive(m_impl->state_mutex);

        ESP_LOGI(TAG,
                 "Synced request_id=%u offset_us=%lld rtt_us=%lu",
                 event.response.request_id,
                 static_cast<long long>(offset_us),
                 static_cast<unsigned long>(m_impl->last_round_trip_us));

        xSemaphoreGive(m_impl->sync_mutex);
        return ESP_OK;
    }
}

} // namespace time_sync
