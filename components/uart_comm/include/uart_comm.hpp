/**
 * @file   uart_comm.hpp
 * @brief  Public/internal declarations for uart_comm.
 *
 * Auto-generated documentation header for component API/internal declarations.
 *
 * @author Project Contributors
 * @date   2026-04-07
 */
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
///   comm.registerCallback(0x81, [](const RxFrame& f) { /* motor cmd */ });
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
    /// @param msg_type  Message type to listen for
    /// @param callback  Function to call on frame reception
    /// @return ESP_OK on success
    esp_err_t registerCallback(uint8_t msg_type, RxCallback callback);

    /// Receive the next frame from the RX queue.
    /// Only returns frames for MSG_TYPEs that do NOT have a registered callback.
    /// @param frame    Output frame
    /// @param timeout  FreeRTOS tick timeout (portMAX_DELAY to block indefinitely)
    /// @return ESP_OK on success, ESP_ERR_TIMEOUT if no frame within timeout
    esp_err_t receive(RxFrame& frame, TickType_t timeout);

    /// Get cumulative error counters.
    ErrorCounters getErrorCounters() const;

private:
    static void rxTaskEntry(void* arg);
    static void txTaskEntry(void* arg);

    struct Impl;
    Impl* m_impl;
};

} // namespace uart_comm
