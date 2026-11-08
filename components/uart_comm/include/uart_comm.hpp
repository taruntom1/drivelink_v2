#pragma once

#include <cstdint>
#include <functional>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

namespace uart_comm {

/// Received frame delivered to the application.
struct RxFrame {
    uint8_t msg_type;
    uint8_t len;
    uint8_t payload[CONFIG_UART_COMM_MAX_PAYLOAD];
};

/// Callback type for frame reception.
/// Called from the internal RX task context.
using RxCallback = std::function<void(const RxFrame&)>;

class CallbackRegistration;

/// Cumulative error counters for diagnostics.
struct ErrorCounters {
    uint32_t crc_errors;
    uint32_t timeouts;
    uint32_t uart_errors;
    uint32_t overflows;
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
/// integrity checking and hardware-accelerated frame detection.
/// Carries two kinds of traffic on the same wire:
///   - Control packets (commands, responses, events) — queued FIFO, high priority
///   - Data streams (sensor/control) — latest-wins per channel, low priority
///
/// Usage:
///   uart_comm::UartComm comm({});
///   if (!comm.isValid()) { /* handle error */ }
///   uart_comm::CallbackRegistration motor_cb;  // unregisters when destroyed
///   comm.registerCallback(0x81, [](const RxFrame& f) { /* motor cmd */ }, motor_cb);
///   comm.sendData(0x80, sensor_buf, sizeof(sensor_buf));
///
class UartComm {
public:
    /// Construct and initialize the UART communication component.
    /// Configures UART hardware, enables pattern detection, and
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
    /// @param len       Payload length (0–255)
    /// @return ESP_OK on success, ESP_ERR_NO_MEM if queue full,
    ///         ESP_ERR_INVALID_ARG if msg_type is a stream type
    esp_err_t sendCommand(uint8_t msg_type, const uint8_t* payload, uint8_t len);

    /// Update a data stream channel with the latest value.
    /// The payload overwrites any pending data for this channel.
    /// Fire-and-forget: always succeeds.
    /// @param channel  Stream channel (0x80–0xBF)
    /// @param payload  Payload data
    /// @param len      Payload length
    void sendData(uint8_t channel, const uint8_t* payload, uint8_t len);

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

private:
    friend class CallbackRegistration;

    struct Impl;

    static void rxTaskBody(Impl* impl);
    static void txTaskBody(Impl* impl);
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
