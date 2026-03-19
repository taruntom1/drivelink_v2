#include "uart_comm.hpp"
#include "frame.hpp"
#include "parser.hpp"
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <atomic>

static const char *TAG = "uart_comm";

namespace uart_comm {

// ── Stream slot: holds the latest payload for one data stream channel ──

struct StreamSlot {
    uint8_t payload[CONFIG_UART_COMM_MAX_PAYLOAD];
    uint8_t len;
    bool dirty;
};

// ── Internal TX queue item: a pre-encoded frame buffer ──

struct TxItem {
    uint8_t data[detail::kMaxFrameSize];
    size_t size;
};

// ── Impl ────────────────────────────────────────────────────────────────

struct UartComm::Impl {
    // Configuration
    uart_port_t port;

    // UART event queue (from driver)
    QueueHandle_t uart_event_queue;

    // Tasks
    TaskHandle_t rx_task;
    TaskHandle_t tx_task;
    std::atomic<bool> stop_flag;

    // TX: high-priority command queue
    QueueHandle_t cmd_queue;

    // TX: low-priority stream slots (latest-wins per channel)
    StreamSlot stream_slots[detail::kStreamChannelCount];
    SemaphoreHandle_t stream_mutex;

    // RX: delivery queue for frames without a registered callback
    QueueHandle_t rx_queue;

    // RX: per-MSG_TYPE callback table
    RxCallback callbacks[256];
    SemaphoreHandle_t callback_mutex;

    // Parser
    detail::Parser* parser;

    bool valid;
};

// ── Task entry points ────────────────────────────────────────────────────

void UartComm::rxTaskEntry(void* arg)
{
    auto* impl = static_cast<UartComm::Impl*>(arg);
    uart_event_t event = {};
    RxFrame frame = {};

    while (!impl->stop_flag.load(std::memory_order_relaxed)) {
        if (xQueueReceive(impl->uart_event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        if (!impl->parser->processEvent(event, frame)) {
            continue;
        }

        // Frame valid — check for registered callback
        xSemaphoreTake(impl->callback_mutex, portMAX_DELAY);
        RxCallback cb = impl->callbacks[frame.msg_type];
        xSemaphoreGive(impl->callback_mutex);

        if (cb) {
            cb(frame);
        } else {
            // No callback — push to RX delivery queue (non-blocking)
            if (xQueueSend(impl->rx_queue, &frame, 0) != pdTRUE) {
                ESP_LOGW(TAG, "RX queue full, dropping frame msg_type=0x%02X", frame.msg_type);
            }
        }
    }

    vTaskDelete(nullptr);
}

void UartComm::txTaskEntry(void* arg)
{
    auto* impl = static_cast<UartComm::Impl*>(arg);
    TxItem item = {};

    // Local buffer for encoding stream frames
    uint8_t stream_buf[detail::kMaxFrameSize];
    uint8_t payload_copy[CONFIG_UART_COMM_MAX_PAYLOAD];

    while (!impl->stop_flag.load(std::memory_order_relaxed)) {
        // Block until notified that there's work to do
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        // Phase 1: Drain ALL pending commands (high priority)
        while (xQueueReceive(impl->cmd_queue, &item, 0) == pdTRUE) {
            uart_write_bytes(impl->port, item.data, item.size);
        }

        // Phase 2: Send latest value for each dirty stream slot
        for (int i = 0; i < detail::kStreamChannelCount; i++) {
            uint8_t len = 0;
            bool was_dirty = false;

            xSemaphoreTake(impl->stream_mutex, portMAX_DELAY);
            if (impl->stream_slots[i].dirty) {
                len = impl->stream_slots[i].len;
                std::memcpy(payload_copy, impl->stream_slots[i].payload, len);
                impl->stream_slots[i].dirty = false;
                was_dirty = true;
            }
            xSemaphoreGive(impl->stream_mutex);

            if (was_dirty) {
                uint8_t channel = detail::kStreamMin + static_cast<uint8_t>(i);
                size_t frame_size = detail::encodeFrame(stream_buf, channel, payload_copy, len);
                uart_write_bytes(impl->port, stream_buf, frame_size);
            }
        }
    }

    vTaskDelete(nullptr);
}

// ── Constructor ─────────────────────────────────────────────────────────

UartComm::UartComm(const UartCommConfig& config)
    : m_impl(new Impl{})
{
    m_impl->port = static_cast<uart_port_t>(config.port_num);
    m_impl->rx_task = nullptr;
    m_impl->tx_task = nullptr;
    m_impl->stop_flag.store(false, std::memory_order_relaxed);
    m_impl->parser = nullptr;
    m_impl->valid = false;

    std::memset(m_impl->stream_slots, 0, sizeof(m_impl->stream_slots));
    std::memset(m_impl->callbacks, 0, sizeof(m_impl->callbacks));

    // Configure UART parameters
    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = config.baud_rate;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t ret = uart_param_config(m_impl->port, &uart_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART port %d: %s",
                 m_impl->port, esp_err_to_name(ret));
        return;
    }

    ret = uart_set_pin(m_impl->port, config.tx_pin, config.rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins tx=%d rx=%d: %s",
                 config.tx_pin, config.rx_pin, esp_err_to_name(ret));
        return;
    }

    // Install UART driver with event queue
    ret = uart_driver_install(m_impl->port,
                              CONFIG_UART_COMM_RX_BUF_SIZE,
                              CONFIG_UART_COMM_TX_BUF_SIZE,
                              CONFIG_UART_COMM_PATTERN_QUEUE_SIZE,
                              &m_impl->uart_event_queue,
                              0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver on port %d: %s",
                 m_impl->port, esp_err_to_name(ret));
        return;
    }

    // Enable hardware pattern detection: 3 consecutive 0xAA bytes
    // chr_tout=9 means the gap between pattern bytes must be ≤9 baud-rate cycles
    ret = uart_enable_pattern_det_baud_intr(m_impl->port, detail::kStartByte,
                                            detail::kStartLen, 9, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable pattern detection on port %d: %s",
                 m_impl->port, esp_err_to_name(ret));
        uart_driver_delete(m_impl->port);
        return;
    }

    // Allocate pattern detection queue
    ret = uart_pattern_queue_reset(m_impl->port, CONFIG_UART_COMM_PATTERN_QUEUE_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset pattern queue on port %d: %s",
                 m_impl->port, esp_err_to_name(ret));
        uart_driver_delete(m_impl->port);
        return;
    }

    // Set RX timeout for packet boundary detection
    ret = uart_set_rx_timeout(m_impl->port, CONFIG_UART_COMM_RX_TIMEOUT_SYMBOLS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set RX timeout on port %d: %s",
                 m_impl->port, esp_err_to_name(ret));
    }

    // uart_set_always_rx_timeout(m_impl->port, true);

    // Create FreeRTOS primitives
    m_impl->cmd_queue = xQueueCreate(CONFIG_UART_COMM_CMD_QUEUE_DEPTH, sizeof(TxItem));
    if (m_impl->cmd_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create TX command queue");
        uart_driver_delete(m_impl->port);
        return;
    }

    m_impl->rx_queue = xQueueCreate(CONFIG_UART_COMM_RX_QUEUE_DEPTH, sizeof(RxFrame));
    if (m_impl->rx_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create RX delivery queue");
        vQueueDelete(m_impl->cmd_queue);
        uart_driver_delete(m_impl->port);
        return;
    }

    m_impl->stream_mutex = xSemaphoreCreateMutex();
    if (m_impl->stream_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create stream mutex");
        vQueueDelete(m_impl->rx_queue);
        vQueueDelete(m_impl->cmd_queue);
        uart_driver_delete(m_impl->port);
        return;
    }

    m_impl->callback_mutex = xSemaphoreCreateMutex();
    if (m_impl->callback_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create callback mutex");
        vSemaphoreDelete(m_impl->stream_mutex);
        vQueueDelete(m_impl->rx_queue);
        vQueueDelete(m_impl->cmd_queue);
        uart_driver_delete(m_impl->port);
        return;
    }

    // Create parser
    m_impl->parser = new detail::Parser(m_impl->port, detail::kMaxPayload);

    // Create RX task
    BaseType_t ok = xTaskCreate(rxTaskEntry, "uart_rx",
                                CONFIG_UART_COMM_RX_TASK_STACK,
                                m_impl,
                                CONFIG_UART_COMM_RX_TASK_PRIO,
                                &m_impl->rx_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RX task");
        delete m_impl->parser;
        m_impl->parser = nullptr;
        vSemaphoreDelete(m_impl->callback_mutex);
        vSemaphoreDelete(m_impl->stream_mutex);
        vQueueDelete(m_impl->rx_queue);
        vQueueDelete(m_impl->cmd_queue);
        uart_driver_delete(m_impl->port);
        return;
    }

    // Create TX task
    ok = xTaskCreate(txTaskEntry, "uart_tx",
                     CONFIG_UART_COMM_TX_TASK_STACK,
                     m_impl,
                     CONFIG_UART_COMM_TX_TASK_PRIO,
                     &m_impl->tx_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TX task");
        m_impl->stop_flag.store(true, std::memory_order_relaxed);
        // RX task will self-delete on next loop iteration
        vTaskDelay(pdMS_TO_TICKS(150));
        delete m_impl->parser;
        m_impl->parser = nullptr;
        vSemaphoreDelete(m_impl->callback_mutex);
        vSemaphoreDelete(m_impl->stream_mutex);
        vQueueDelete(m_impl->rx_queue);
        vQueueDelete(m_impl->cmd_queue);
        uart_driver_delete(m_impl->port);
        return;
    }

    m_impl->valid = true;
    ESP_LOGI(TAG, "Initialized on UART%d tx=%d rx=%d baud=%d",
             m_impl->port, config.tx_pin, config.rx_pin, config.baud_rate);
}

// ── Destructor ──────────────────────────────────────────────────────────

UartComm::~UartComm()
{
    if (m_impl) {
        if (m_impl->valid) {
            m_impl->stop_flag.store(true, std::memory_order_relaxed);

            // Wake TX task so it can exit
            if (m_impl->tx_task) {
                xTaskNotifyGive(m_impl->tx_task);
            }

            // Wait for tasks to finish (they call vTaskDelete(nullptr))
            vTaskDelay(pdMS_TO_TICKS(200));

            uart_driver_delete(m_impl->port);

            vSemaphoreDelete(m_impl->callback_mutex);
            vSemaphoreDelete(m_impl->stream_mutex);
            vQueueDelete(m_impl->rx_queue);
            vQueueDelete(m_impl->cmd_queue);

            delete m_impl->parser;
        }
        delete m_impl;
        m_impl = nullptr;
    }
}

// ── Move ────────────────────────────────────────────────────────────────

UartComm::UartComm(UartComm&& other) noexcept
    : m_impl(std::exchange(other.m_impl, nullptr))
{
}

UartComm& UartComm::operator=(UartComm&& other) noexcept
{
    if (this != &other) {
        // Tear down current instance
        UartComm tmp(std::move(*this));
        m_impl = std::exchange(other.m_impl, nullptr);
    }
    return *this;
}

// ── isValid ─────────────────────────────────────────────────────────────

bool UartComm::isValid() const
{
    return m_impl && m_impl->valid;
}

// ── sendCommand ─────────────────────────────────────────────────────────

esp_err_t UartComm::sendCommand(uint8_t msg_type, const uint8_t* payload, uint8_t len)
{
    if (!m_impl || !m_impl->valid) {
        return ESP_ERR_INVALID_STATE;
    }

    if (detail::isStream(msg_type)) {
        ESP_LOGE(TAG, "sendCommand called with stream type 0x%02X, use sendData()", msg_type);
        return ESP_ERR_INVALID_ARG;
    }

    if (len > detail::kMaxPayload) {
        return ESP_ERR_INVALID_ARG;
    }

    TxItem item = {};
    item.size = detail::encodeFrame(item.data, msg_type, payload, len);

    if (xQueueSend(m_impl->cmd_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TX command queue full, msg_type=0x%02X", msg_type);
        return ESP_ERR_NO_MEM;
    }

    xTaskNotifyGive(m_impl->tx_task);
    return ESP_OK;
}

// ── sendData ────────────────────────────────────────────────────────────

void UartComm::sendData(uint8_t channel, const uint8_t* payload, uint8_t len)
{
    if (!m_impl || !m_impl->valid) {
        return;
    }

    if (!detail::isStream(channel)) {
        ESP_LOGE(TAG, "sendData called with non-stream channel 0x%02X", channel);
        return;
    }

    len = std::min(len, detail::kMaxPayload);
    int index = channel - detail::kStreamMin;

    xSemaphoreTake(m_impl->stream_mutex, portMAX_DELAY);
    std::memcpy(m_impl->stream_slots[index].payload, payload, len);
    m_impl->stream_slots[index].len = len;
    m_impl->stream_slots[index].dirty = true;
    xSemaphoreGive(m_impl->stream_mutex);

    xTaskNotifyGive(m_impl->tx_task);
}

// ── registerCallback ────────────────────────────────────────────────────

esp_err_t UartComm::registerCallback(uint8_t msg_type, RxCallback callback)
{
    if (!m_impl || !m_impl->valid) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(m_impl->callback_mutex, portMAX_DELAY);
    m_impl->callbacks[msg_type] = std::move(callback);
    xSemaphoreGive(m_impl->callback_mutex);

    return ESP_OK;
}

// ── receive ─────────────────────────────────────────────────────────────

esp_err_t UartComm::receive(RxFrame& frame, TickType_t timeout)
{
    if (!m_impl || !m_impl->valid) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueReceive(m_impl->rx_queue, &frame, timeout) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

// ── getErrorCounters ────────────────────────────────────────────────────

ErrorCounters UartComm::getErrorCounters() const
{
    if (!m_impl || !m_impl->parser) {
        return {};
    }

    detail::Parser::Counters pc = m_impl->parser->getCounters();
    ErrorCounters out = {};
    out.crc_errors = pc.crc_errors;
    out.timeouts = pc.timeouts;
    out.uart_errors = pc.uart_errors;
    out.overflows = pc.overflows;
    return out;
}

} // namespace uart_comm
