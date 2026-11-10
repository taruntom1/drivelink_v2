#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

namespace uart_comm {

/// Received frame delivered to the application.
struct RxFrame {
    uint8_t msg_type;
    uint16_t len;
    uint8_t payload[CONFIG_UART_COMM_MAX_PAYLOAD];
};

/// Callback type for frame reception.
/// Called from the internal RX task context.
using RxCallback = std::function<void(const RxFrame&)>;

class CallbackRegistration;

/// Cumulative error counters for diagnostics.
struct ErrorCounters {
    uint32_t crc_errors;      ///< Frame candidates rejected for a CRC mismatch
    uint32_t timeouts;        ///< Partial frames dropped because the sender stopped mid-frame
    uint32_t uart_errors;     ///< UART framing or parity errors
    uint32_t overflows;       ///< RX hardware FIFO overflows or ring buffer full events
    uint32_t length_errors;   ///< Frame candidates rejected because LEN exceeded the max payload
    uint32_t discarded_bytes; ///< Received bytes that were not part of any valid frame
    uint32_t stream_skipped;  ///< sendData() calls skipped because the stream slot was busy
};

/// Smallest amount of stack, in bytes, that has been free in each internal task
/// since it started.
struct StackHighWater {
    uint32_t rx_free;
    uint32_t tx_free;
};

/// Configuration for UartComm. All fields default to Kconfig values.
struct UartCommConfig {
    int port_num    = CONFIG_UART_COMM_PORT_NUM;
    int tx_pin      = CONFIG_UART_COMM_TX_PIN;
    int rx_pin      = CONFIG_UART_COMM_RX_PIN;
    int baud_rate   = CONFIG_UART_COMM_BAUD_RATE;
};

/// Structured UART communication component.
///
/// Wraps a UART link in a lightweight binary protocol with CRC-16
/// integrity checking and software frame detection (see drivelink::protocol::Scanner).
/// Carries two kinds of traffic on the same wire:
///   - Control packets (commands, responses, events) — queued FIFO, high priority
///   - Data streams (sensor/control) — latest-wins per channel, low priority
///
/// Usage:
///   uart_comm::UartComm comm({});
///   if (!comm.isValid()) { /* handle error */ }
///   uart_comm::CallbackRegistration motor_cb;  // unregisters when destroyed
///   comm.registerCallback(0x81, [](const RxFrame& f) { /* motor cmd */ }, motor_cb);
///   comm.openStream(0x80, sizeof(sensor_buf));  // once, at setup
///   comm.sendData(0x80, sensor_buf, sizeof(sensor_buf));
///
class UartComm {
public:
    /// Construct and initialize the UART communication component.
    /// Configures UART hardware and
    /// starts internal RX/TX tasks.
    /// @param config  UART configuration (defaults from Kconfig)
    explicit UartComm(const UartCommConfig& config);

    /// Destructor. Stops tasks, releases UART driver.
    ~UartComm();

    UartComm(const UartComm&) = delete;
    UartComm& operator=(const UartComm&) = delete;

    UartComm(UartComm&& other) noexcept;
    UartComm& operator=(UartComm&& other) noexcept;

    /// Returns true if UART initialization succeeded and tasks are running.
    bool isValid() const;

    /// Queue a command, response, or event frame for transmission.
    /// MSG_TYPE must be in ranges 0x01–0x3F, 0x40–0x7F, or 0xC0–0xFE.
    /// @param msg_type  Message type byte
    /// @param payload   Payload data (may be nullptr if len == 0)
    /// @param len       Payload length (0 to CONFIG_UART_COMM_MAX_PAYLOAD)
    ///
    /// Never blocks: the frame is encoded into a preallocated buffer.
    /// @return ESP_OK on success, ESP_ERR_NO_MEM if queue full,
    ///         ESP_ERR_INVALID_STATE if not initialized,
    ///         ESP_ERR_INVALID_ARG if msg_type is a stream type or outside every
    ///         range (0x00, 0xFF), len is too large, or payload is null with len > 0
    esp_err_t sendCommand(uint8_t msg_type, const uint8_t* payload, size_t len);

    /// Allocate the slot of a data stream channel. Call it once per channel at
    /// setup, before the first sendData() on that channel (it allocates).
    /// @param channel  Stream channel (0x80–0xBF)
    /// @param max_len  Largest payload that will be sent on this channel
    ///                 (1 to CONFIG_UART_COMM_MAX_PAYLOAD)
    /// @return ESP_OK, ESP_ERR_INVALID_STATE if not initialized or the channel
    ///         is already open, ESP_ERR_INVALID_ARG if channel is not a stream
    ///         channel or max_len is out of range, ESP_ERR_NO_MEM
    esp_err_t openStream(uint8_t channel, size_t max_len);

    /// Update a data stream channel with the latest value.
    /// The payload overwrites any pending data for this channel.
    /// Fire-and-forget: there is no delivery confirmation.
    ///
    /// Real-time safe: it never blocks and never allocates. The slot has two
    /// buffers, so the TX task can send one while this writes the other. If
    /// the slot's lock is busy (another writer, or the TX task swapping
    /// buffers) the write is skipped and counted in
    /// ErrorCounters::stream_skipped; the next call carries newer data anyway.
    /// @param channel  Stream channel (0x80–0xBF), opened with openStream()
    /// @param payload  Payload data (may be nullptr if len == 0)
    /// @param len      Payload length (0 to the max_len given to openStream())
    /// @return ESP_OK when the slot was updated,
    ///         ESP_ERR_INVALID_STATE if not initialized or the channel was not opened,
    ///         ESP_ERR_TIMEOUT if the write was skipped because the slot was busy,
    ///         ESP_ERR_INVALID_ARG if channel is not a stream channel, len is
    ///         too large, or payload is null with len > 0
    esp_err_t sendData(uint8_t channel, const uint8_t* payload, size_t len);

    /// Register a callback for a specific MSG_TYPE.
    /// The callback fires from the RX task context. If a callback is
    /// registered, frames of this type are NOT queued (delivered via
    /// callback only).
    ///
    /// The callback stays registered while `registration` is alive.
    /// Destroying or resetting `registration` unregisters it and waits until
    /// any invocation that is already running has returned, so it is safe to
    /// destroy objects the callback captures right after that.
    /// If `registration` is already active, it is reset first.
    ///
    /// Callbacks run with the internal callback lock held. They may register
    /// or unregister callbacks, but must not block on another task that is
    /// itself waiting to register or unregister a callback.
    ///
    /// Every registration must be released before this UartComm is destroyed.
    ///
    /// @param msg_type      Message type to listen for
    /// @param callback      Function to call on frame reception (must not be empty)
    /// @param registration  Receives ownership of the registration
    /// @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized or
    ///         another callback is already registered for `msg_type`,
    ///         ESP_ERR_INVALID_ARG if `callback` is empty
    esp_err_t registerCallback(uint8_t msg_type, RxCallback callback, CallbackRegistration& registration);

    /// Receive the next frame from the RX queue.
    /// Only returns frames for MSG_TYPEs that do NOT have a registered callback.
    /// @param frame    Output frame
    /// @param timeout  FreeRTOS tick timeout (portMAX_DELAY to block indefinitely)
    /// @return ESP_OK on success, ESP_ERR_TIMEOUT if no frame within timeout
    esp_err_t receive(RxFrame& frame, TickType_t timeout);

    /// Get cumulative error counters.
    ErrorCounters getErrorCounters() const;

    /// Stack headroom of the internal RX and TX tasks, for logging.
    StackHighWater stackHighWater() const;

private:
    friend class CallbackRegistration;

    struct Impl;

    static void rxTaskBody(Impl* impl);
    static void txTaskBody(Impl* impl);
    static void releaseResources(Impl* impl);
    static void unregisterCallback(Impl* impl, uint8_t msg_type);

    Impl* m_impl;
};

/// Owns one callback registration made with UartComm::registerCallback().
/// Unregisters the callback when destroyed or reset. Move-only.
/// Must not outlive the UartComm it was registered with.
class CallbackRegistration {
public:
    CallbackRegistration() = default;
    ~CallbackRegistration();

    CallbackRegistration(const CallbackRegistration&) = delete;
    CallbackRegistration& operator=(const CallbackRegistration&) = delete;

    CallbackRegistration(CallbackRegistration&& other) noexcept;
    CallbackRegistration& operator=(CallbackRegistration&& other) noexcept;

    /// Unregister now. Blocks until any running invocation of the callback
    /// has returned (unless called from inside that callback). Idempotent.
    void reset();

    /// True while a callback is registered through this object.
    bool isActive() const { return m_impl != nullptr; }

private:
    friend class UartComm;

    UartComm::Impl* m_impl = nullptr;
    uint8_t m_msg_type = 0;
};

} // namespace uart_comm
