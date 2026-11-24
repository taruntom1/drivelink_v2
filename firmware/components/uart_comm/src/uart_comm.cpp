#include "uart_comm.hpp"
#include "drivelink/protocol/time_codec.hpp"
#include "esp_timer.h"
#include "parser.hpp"
#include "drivelink/protocol/framing.hpp"
#include "stoppable_task.hpp"
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

static const char *const TAG = "uart_comm";

namespace uart_comm {

namespace protocol = drivelink::protocol;

namespace {

constexpr size_t kMaxPayload = CONFIG_UART_COMM_MAX_PAYLOAD;
constexpr size_t kMaxFrameSize = protocol::frameSize(kMaxPayload);
constexpr size_t kCmdPoolSize = CONFIG_UART_COMM_CMD_QUEUE_DEPTH;

static_assert(kMaxPayload <= protocol::kMaxPayload, "CONFIG_UART_COMM_MAX_PAYLOAD is above the protocol limit");
static_assert(kCmdPoolSize <= 255U, "command buffers are identified by a uint8_t index");

// The TX task waits at most this long for a stream slot's lock. Writers hold
// it only for a memcpy, so this is never reached in practice.
constexpr TickType_t kStreamLockTimeout = pdMS_TO_TICKS(100);

} // namespace

// ── Stream slot: holds the latest payload for one data stream channel ──
//
// Two buffers: the TX task sends from one while sendData() fills the other.
//   ready   index of the buffer holding unsent data, or -1. Guarded by `mutex`.
//   sending index of the buffer the TX task is encoding from, or -1. Set by
//           the TX task under `mutex`, cleared by it without. sendData() reads
//           it under `mutex` and never writes the buffer it names.

struct StreamSlot {
    StreamSlot(size_t capacity_bytes, SemaphoreHandle_t lock)
        : capacity(capacity_bytes), mutex(lock)
    {
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = std::make_unique<uint8_t[]>(capacity_bytes);
        }
    }

    ~StreamSlot()
    {
        vSemaphoreDelete(mutex);
    }

    StreamSlot(const StreamSlot&) = delete;
    StreamSlot& operator=(const StreamSlot&) = delete;

    const size_t capacity;
    SemaphoreHandle_t const mutex;
    std::array<std::unique_ptr<uint8_t[]>, 2> buf;
    std::array<size_t, 2> len{};
    int ready = -1;
    std::atomic<int> sending{-1};
};

// ── Impl ────────────────────────────────────────────────────────────────

struct UartComm::Impl {
    // Configuration
    uart_port_t port;
    bool driver_installed;

    // UART event queue (from driver)
    QueueHandle_t uart_event_queue;

    // Tasks
    rtos_util::StoppableTask rx_task;
    rtos_util::StoppableTask tx_task;

    // TX: command frames. A fixed pool of frame buffers moves between two
    // queues of buffer indices: `cmd_free` holds the idle ones, `cmd_ready`
    // the encoded ones in send order. A queue never holds more than the pool
    // size, so posting an index cannot fail.
    std::unique_ptr<uint8_t[]> cmd_pool;           // kCmdPoolSize * kMaxFrameSize bytes
    std::array<size_t, kCmdPoolSize> cmd_size;    // encoded size per buffer
    std::array<bool, kCmdPoolSize> cmd_stamp{};   // owned with its pool entry
    QueueHandle_t cmd_free;
    QueueHandle_t cmd_ready;

    // TX: low-priority stream slots (latest-wins per channel). A slot is created
    // by openStream() and read by the TX task, hence the atomic pointers.
    std::array<std::atomic<StreamSlot*>, protocol::kStreamChannelCount> stream_slots;
    std::atomic<uint32_t> stream_skipped;
    std::unique_ptr<uint8_t[]> tx_frame;           // TX task: encode buffer for stream frames

    // RX: delivery queue for frames without a registered callback
    QueueHandle_t rx_queue;
    RxFrame rx_frame;                              // RX task: the frame being delivered

    // RX: per-MSG_TYPE callback table.
    // callback_mutex is recursive. The RX task holds it while a callback runs,
    // so unregistering waits for an in-flight invocation, and a callback may
    // still register/unregister callbacks itself.
    std::array<RxCallback, 256> callbacks{};
    SemaphoreHandle_t callback_mutex;
    uint32_t active_registrations; // guarded by callback_mutex

    // Parser
    detail::Parser* parser;

    std::atomic<bool> valid;
};

// ── Task entry points ────────────────────────────────────────────────────

void UartComm::rxTaskBody(Impl* impl)
{
    uart_event_t event = {};
    RxFrame& frame = impl->rx_frame;

    while (!impl->rx_task.stopRequested()) {
        // Driver events only wake us up (and report errors); the parser pulls
        // the bytes itself. On a timeout we still scan, so a partial frame
        // that never completes is eventually dropped.
        if (xQueueReceive(impl->uart_event_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            impl->parser->processEvent(event);
        }

        while (!impl->rx_task.stopRequested() && impl->parser->nextFrame(frame)) {
            // Invoke the callback with callback_mutex held so that an unregister
            // (CallbackRegistration::reset) waits until it has returned. Calling a
            // local copy keeps it valid even if the callback unregisters itself.
            xSemaphoreTakeRecursive(impl->callback_mutex, portMAX_DELAY);
            RxCallback cb = impl->callbacks[frame.msg_type];
            const bool handled = static_cast<bool>(cb);
            if (handled) {
                cb(frame);
            }
            xSemaphoreGiveRecursive(impl->callback_mutex);

            if (!handled) {
                // No callback: push to RX delivery queue (non-blocking)
                if (xQueueSend(impl->rx_queue, &frame, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "RX queue full, dropping frame msg_type=0x%02X", frame.msg_type);
                }
            }
        }
    }
}

void UartComm::txTaskBody(Impl* impl)
{
    while (!impl->tx_task.stopRequested()) {
        // Block until notified that there's work to do (or a stop request)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        // Phase 1: Drain ALL pending commands (high priority)
        uint8_t index = 0;
        while (xQueueReceive(impl->cmd_ready, &index, 0) == pdTRUE) {
            auto *bytes=&impl->cmd_pool[static_cast<size_t>(index) * kMaxFrameSize];
            if (!impl->cmd_stamp[index] || protocol::time_wire::stampFrame(bytes, impl->cmd_size[index],
                    static_cast<uint64_t>(esp_timer_get_time()))) {
                const auto written=uart_write_bytes(impl->port, bytes, impl->cmd_size[index]);
                if (written != static_cast<int>(impl->cmd_size[index])) { ESP_LOGE(TAG,"command write failed"); }
            }
            impl->cmd_stamp[index]=false;
            // Hand the buffer back. This cannot fail: the free queue has room for the whole pool.
            (void)xQueueSend(impl->cmd_free, &index, 0);
        }

        // Phase 2: Send the latest value of each stream channel that has new data
        for (size_t i = 0; i < protocol::kStreamChannelCount; i++) {
            StreamSlot* slot = impl->stream_slots[i].load(std::memory_order_acquire);
            if (slot == nullptr) {
                continue;
            }

            int taken = -1;
            if (xSemaphoreTake(slot->mutex, kStreamLockTimeout) == pdTRUE) {
                taken = slot->ready;
                if (taken >= 0) {
                    slot->ready = -1;
                    slot->sending.store(taken);
                }
                xSemaphoreGive(slot->mutex);
            }
            if (taken < 0) {
                continue;
            }

            // The lock is not held here: sendData() fills the other buffer meanwhile.
            const auto channel = static_cast<uint8_t>(protocol::kStreamMin + i);
            const size_t frame_size = protocol::encodeFrame(impl->tx_frame.get(), kMaxFrameSize, channel,
                                                            slot->buf[taken].get(), slot->len[taken]);
            if (frame_size > 0U) {
                uart_write_bytes(impl->port, impl->tx_frame.get(), frame_size);
            }
            slot->sending.store(-1);
        }
    }
}

// ── Constructor ─────────────────────────────────────────────────────────

UartComm::UartComm(const UartCommConfig& config)
    : m_impl(new Impl{}) // NOSONAR: explicit ownership is paired with staged teardown ordering for RTOS/driver resources.
{
    m_impl->port = static_cast<uart_port_t>(config.port_num);
    m_impl->driver_installed = false;
    m_impl->uart_event_queue = nullptr;
    m_impl->cmd_size.fill(0);
    m_impl->cmd_free = nullptr;
    m_impl->cmd_ready = nullptr;
    for (auto& slot : m_impl->stream_slots) {
        slot.store(nullptr);
    }
    m_impl->stream_skipped = 0;
    m_impl->rx_queue = nullptr;
    m_impl->rx_frame = {};
    m_impl->callback_mutex = nullptr;
    m_impl->active_registrations = 0;
    m_impl->parser = nullptr;
    m_impl->valid = false;

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

    // Install UART driver with event queue. Frames are found in software
    // (see Parser); hardware pattern detection is not used because the start
    // pattern is not escaped and can also appear inside payloads.
    ret = uart_driver_install(m_impl->port,
                              CONFIG_UART_COMM_RX_BUF_SIZE,
                              CONFIG_UART_COMM_TX_BUF_SIZE,
                              CONFIG_UART_COMM_EVENT_QUEUE_SIZE,
                              &m_impl->uart_event_queue,
                              0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver on port %d: %s",
                 m_impl->port, esp_err_to_name(ret));
        return;
    }
    m_impl->driver_installed = true;

    // RX timeout: the driver posts UART_DATA after this many idle symbols,
    // which wakes the RX task promptly at the end of each burst.
    ret = uart_set_rx_timeout(m_impl->port, CONFIG_UART_COMM_RX_TIMEOUT_SYMBOLS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set RX timeout on port %d: %s",
                 m_impl->port, esp_err_to_name(ret));
    }

    // Preallocate every buffer and queue now, so nothing allocates while running.
    // A failed `new` aborts, which is acceptable for setup-time allocation.
    m_impl->cmd_pool = std::make_unique<uint8_t[]>(kCmdPoolSize * kMaxFrameSize);
    m_impl->tx_frame = std::make_unique<uint8_t[]>(kMaxFrameSize);

    m_impl->cmd_free = xQueueCreate(kCmdPoolSize, sizeof(uint8_t));
    m_impl->cmd_ready = xQueueCreate(kCmdPoolSize, sizeof(uint8_t));
    if (m_impl->cmd_free == nullptr || m_impl->cmd_ready == nullptr) {
        ESP_LOGE(TAG, "Failed to create TX command queues");
        releaseResources(m_impl);
        return;
    }
    for (size_t i = 0; i < kCmdPoolSize; ++i) {
        const auto index = static_cast<uint8_t>(i);
        (void)xQueueSend(m_impl->cmd_free, &index, 0);
    }

    m_impl->rx_queue = xQueueCreate(CONFIG_UART_COMM_RX_QUEUE_DEPTH, sizeof(RxFrame));
    if (m_impl->rx_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create RX delivery queue");
        releaseResources(m_impl);
        return;
    }

    m_impl->callback_mutex = xSemaphoreCreateRecursiveMutex();
    if (m_impl->callback_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create callback mutex");
        releaseResources(m_impl);
        return;
    }

    // Create parser
    m_impl->parser = new detail::Parser(m_impl->port, kMaxPayload); // NOSONAR: parser lifetime is explicitly coordinated with task shutdown.

    // Create RX task
    Impl* impl = m_impl;
    esp_err_t task_ret = m_impl->rx_task.start("uart_rx",
                                               CONFIG_UART_COMM_RX_TASK_STACK,
                                               CONFIG_UART_COMM_RX_TASK_PRIO,
                                               [impl] { rxTaskBody(impl); });
    if (task_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RX task");
        releaseResources(m_impl);
        return;
    }

    // Create TX task
    task_ret = m_impl->tx_task.start("uart_tx",
                                     CONFIG_UART_COMM_TX_TASK_STACK,
                                     CONFIG_UART_COMM_TX_TASK_PRIO,
                                     [impl] { txTaskBody(impl); });
    if (task_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create TX task");
        m_impl->rx_task.stop();
        releaseResources(m_impl);
        return;
    }

    m_impl->valid = true;
    ESP_LOGI(TAG, "Initialized on UART%d tx=%d rx=%d baud=%d",
             m_impl->port, config.tx_pin, config.rx_pin, config.baud_rate);
}

// ── Teardown ────────────────────────────────────────────────────────────

// Release every resource that exists, in reverse order of creation. Safe to
// call on a partly built Impl, and more than once. The tasks must be stopped.
void UartComm::releaseResources(Impl* impl)
{
    if (impl->driver_installed) {
        uart_driver_delete(impl->port);
        impl->driver_installed = false;
        impl->uart_event_queue = nullptr;
    }

    delete impl->parser; // NOSONAR: parser must outlive RX task loop and be released after shutdown.
    impl->parser = nullptr;

    if (impl->callback_mutex != nullptr) {
        vSemaphoreDelete(impl->callback_mutex);
        impl->callback_mutex = nullptr;
    }
    if (impl->rx_queue != nullptr) {
        vQueueDelete(impl->rx_queue);
        impl->rx_queue = nullptr;
    }
    for (auto& slot : impl->stream_slots) {
        delete slot.exchange(nullptr); // NOSONAR: slots are created by openStream() and owned by the Impl.
    }
    if (impl->cmd_ready != nullptr) {
        vQueueDelete(impl->cmd_ready);
        impl->cmd_ready = nullptr;
    }
    if (impl->cmd_free != nullptr) {
        vQueueDelete(impl->cmd_free);
        impl->cmd_free = nullptr;
    }
    impl->tx_frame.reset();
    impl->cmd_pool.reset();
}

// ── Destructor ──────────────────────────────────────────────────────────

UartComm::~UartComm()
{
    if (m_impl) {
        if (m_impl->valid.exchange(false)) {
            xSemaphoreTakeRecursive(m_impl->callback_mutex, portMAX_DELAY);
            const uint32_t active = m_impl->active_registrations;
            xSemaphoreGiveRecursive(m_impl->callback_mutex);
            if (active != 0U) {
                // A CallbackRegistration still points at this instance and
                // would touch freed memory when it is reset. Fail loudly.
                ESP_LOGE(TAG, "Destroyed with %lu active callback registration(s); "
                              "reset them before destroying UartComm",
                         static_cast<unsigned long>(active));
                abort();
            }

            // Join both tasks: after this nothing touches the driver,
            // queues, mutexes or parser.
            m_impl->rx_task.stop();
            m_impl->tx_task.stop();

            releaseResources(m_impl);
        }
        delete m_impl; // NOSONAR: impl owns non-RAII RTOS handles that are released in ordered teardown above.
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

esp_err_t UartComm::sendCommand(uint8_t msg_type, const uint8_t* payload, size_t len, bool stamp_time)
{
    if (!m_impl || !m_impl->valid) {
        return ESP_ERR_INVALID_STATE;
    }

    if (protocol::isStream(msg_type)) {
        ESP_LOGE(TAG, "sendCommand called with stream type 0x%02X, use sendData()", msg_type);
        return ESP_ERR_INVALID_ARG;
    }

    if (!protocol::isCommand(msg_type) && !protocol::isResponse(msg_type) && !protocol::isEvent(msg_type)) {
        ESP_LOGE(TAG, "sendCommand called with reserved msg_type 0x%02X", msg_type);
        return ESP_ERR_INVALID_ARG;
    }

    if (len > kMaxPayload) {
        ESP_LOGE(TAG, "sendCommand payload too long: msg_type=0x%02X len=%u max=%u",
                 msg_type, static_cast<unsigned>(len), static_cast<unsigned>(kMaxPayload));
        return ESP_ERR_INVALID_ARG;
    }

    if (payload == nullptr && len > 0) {
        ESP_LOGE(TAG, "sendCommand called with null payload, msg_type=0x%02X len=%u",
                 msg_type, static_cast<unsigned>(len));
        return ESP_ERR_INVALID_ARG;
    }

    if (stamp_time) {
        protocol::time_wire::Response value;
        if (msg_type!=protocol::defs::msg::kTimeResponse || !protocol::time_wire::decodeResponse(payload,len,value)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    // Take an idle buffer without waiting and encode straight into it.
    uint8_t index = 0;
    if (xQueueReceive(m_impl->cmd_free, &index, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TX command queue full, msg_type=0x%02X", msg_type);
        return ESP_ERR_NO_MEM;
    }

    const size_t size = protocol::encodeFrame(&m_impl->cmd_pool[static_cast<size_t>(index) * kMaxFrameSize],
                                              kMaxFrameSize, msg_type, payload, len);
    if (size == 0U) {
        // Unreachable after the checks above; give the buffer back anyway.
        (void)xQueueSend(m_impl->cmd_free, &index, 0);
        return ESP_ERR_INVALID_ARG;
    }
    m_impl->cmd_size[index] = size;
    m_impl->cmd_stamp[index] = stamp_time;

    // Cannot fail: the ready queue has room for the whole pool.
    (void)xQueueSend(m_impl->cmd_ready, &index, 0);
    m_impl->tx_task.notify();
    return ESP_OK;
}

// ── openStream ──────────────────────────────────────────────────────────

size_t UartComm::streamSlotBytes(size_t max_len)
{
    return sizeof(StreamSlot) + sizeof(StaticSemaphore_t) + (2U * max_len);
}

esp_err_t UartComm::openStream(uint8_t channel, size_t max_len)
{
    if (!m_impl || !m_impl->valid) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!protocol::isStream(channel) || max_len == 0U || max_len > kMaxPayload) {
        ESP_LOGE(TAG, "openStream invalid arguments: channel=0x%02X max_len=%u max=%u",
                 channel, static_cast<unsigned>(max_len), static_cast<unsigned>(kMaxPayload));
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (lock == nullptr) {
        ESP_LOGE(TAG, "openStream: failed to create the mutex for channel 0x%02X", channel);
        return ESP_ERR_NO_MEM;
    }
    auto* slot = new StreamSlot(max_len, lock); // NOSONAR: owned by the Impl, deleted in releaseResources().

    StreamSlot* expected = nullptr;
    if (!m_impl->stream_slots[channel - protocol::kStreamMin].compare_exchange_strong(
            expected, slot, std::memory_order_acq_rel)) {
        delete slot; // NOSONAR: lost the race, or the channel was already open.
        ESP_LOGE(TAG, "openStream: channel 0x%02X is already open", channel);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

// ── sendData ────────────────────────────────────────────────────────────

esp_err_t UartComm::sendData(uint8_t channel, const uint8_t* payload, size_t len)
{
    if (!m_impl || !m_impl->valid) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!protocol::isStream(channel)) {
        ESP_LOGE(TAG, "sendData called with non-stream channel 0x%02X", channel);
        return ESP_ERR_INVALID_ARG;
    }

    StreamSlot* slot = m_impl->stream_slots[channel - protocol::kStreamMin].load(std::memory_order_acquire);
    if (slot == nullptr) {
        ESP_LOGE(TAG, "sendData on channel 0x%02X that was not opened with openStream()", channel);
        return ESP_ERR_INVALID_STATE;
    }

    if (len > slot->capacity) {
        ESP_LOGE(TAG, "sendData payload too long: channel=0x%02X len=%u max=%u",
                 channel, static_cast<unsigned>(len), static_cast<unsigned>(slot->capacity));
        return ESP_ERR_INVALID_ARG;
    }

    if (payload == nullptr && len > 0) {
        ESP_LOGE(TAG, "sendData called with null payload, channel=0x%02X len=%u",
                 channel, static_cast<unsigned>(len));
        return ESP_ERR_INVALID_ARG;
    }

    // Never wait: this runs on the control tick. A busy lock skips this write.
    if (xSemaphoreTake(slot->mutex, 0) != pdTRUE) {
        m_impl->stream_skipped.fetch_add(1, std::memory_order_relaxed);
        return ESP_ERR_TIMEOUT;
    }
    // Fill the buffer the TX task is not sending. If unsent data is waiting in
    // it, this overwrites it: only the latest value is sent.
    const int target = (slot->sending.load() == 0) ? 1 : 0;
    if (len > 0) {
        std::memcpy(slot->buf[target].get(), payload, len);
    }
    slot->len[target] = len;
    slot->ready = target;
    xSemaphoreGive(slot->mutex);

    m_impl->tx_task.notify();
    return ESP_OK;
}

// ── registerCallback ────────────────────────────────────────────────────

esp_err_t UartComm::registerCallback(uint8_t msg_type, RxCallback callback, CallbackRegistration& registration)
{
    if (!m_impl || !m_impl->valid) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }

    registration.reset();

    xSemaphoreTakeRecursive(m_impl->callback_mutex, portMAX_DELAY);
    if (m_impl->callbacks[msg_type]) {
        xSemaphoreGiveRecursive(m_impl->callback_mutex);
        ESP_LOGE(TAG, "Callback already registered for msg_type=0x%02X", msg_type);
        return ESP_ERR_INVALID_STATE;
    }
    m_impl->callbacks[msg_type] = std::move(callback);
    ++m_impl->active_registrations;
    xSemaphoreGiveRecursive(m_impl->callback_mutex);

    registration.m_impl = m_impl;
    registration.m_msg_type = msg_type;
    return ESP_OK;
}

void UartComm::unregisterCallback(Impl* impl, uint8_t msg_type)
{
    // The RX task holds callback_mutex while a callback runs, so taking it
    // here waits until any in-flight invocation has returned.
    xSemaphoreTakeRecursive(impl->callback_mutex, portMAX_DELAY);
    impl->callbacks[msg_type] = nullptr;
    --impl->active_registrations;
    xSemaphoreGiveRecursive(impl->callback_mutex);
}

// ── CallbackRegistration ────────────────────────────────────────────────

CallbackRegistration::~CallbackRegistration()
{
    reset();
}

CallbackRegistration::CallbackRegistration(CallbackRegistration&& other) noexcept
    : m_impl(std::exchange(other.m_impl, nullptr)),
      m_msg_type(other.m_msg_type)
{
}

CallbackRegistration& CallbackRegistration::operator=(CallbackRegistration&& other) noexcept
{
    if (this != &other) {
        reset();
        m_impl = std::exchange(other.m_impl, nullptr);
        m_msg_type = other.m_msg_type;
    }
    return *this;
}

void CallbackRegistration::reset()
{
    if (m_impl != nullptr) {
        UartComm::unregisterCallback(m_impl, m_msg_type);
        m_impl = nullptr;
    }
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
    out.header_crc_errors = pc.header_crc_errors;
    out.timeouts = pc.timeouts;
    out.uart_errors = pc.uart_errors;
    out.overflows = pc.overflows;
    out.length_errors = pc.length_errors;
    out.discarded_bytes = pc.discarded_bytes;
    out.stream_skipped = m_impl->stream_skipped.load(std::memory_order_relaxed);
    return out;
}

// ── stackHighWater ──────────────────────────────────────────────────────

esp_err_t UartComm::waitTxIdle(uint32_t timeout_ms)
{
    if(!m_impl || !m_impl->valid) { return ESP_ERR_INVALID_STATE; }
    const TickType_t start=xTaskGetTickCount();
    const TickType_t timeout=pdMS_TO_TICKS(timeout_ms);
    while(uxQueueMessagesWaiting(m_impl->cmd_free)<kCmdPoolSize) {
        if(xTaskGetTickCount()-start>=timeout) { return ESP_ERR_TIMEOUT; }
        vTaskDelay(1);
    }
    const TickType_t elapsed=xTaskGetTickCount()-start;
    return uart_wait_tx_done(m_impl->port,elapsed<timeout?timeout-elapsed:0);
}

StackHighWater UartComm::stackHighWater() const
{
    if (!m_impl || !m_impl->valid) {
        return {};
    }
    return {m_impl->rx_task.stackHighWaterMark(), m_impl->tx_task.stackHighWaterMark()};
}

} // namespace uart_comm
