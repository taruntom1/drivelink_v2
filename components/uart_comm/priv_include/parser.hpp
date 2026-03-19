#pragma once

#include <cstdint>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace uart_comm {

struct RxFrame;

namespace detail {

/// Event-driven UART frame parser using hardware pattern detection.
///
/// Processes UART events from the driver event queue. When a
/// UART_PATTERN_DET event arrives (3×0xAA detected by hardware),
/// reads the subsequent header, payload, and CRC from the ring buffer,
/// validates CRC, and returns a complete frame.
class Parser {
public:
    struct Counters {
        uint32_t crc_errors;
        uint32_t timeouts;
        uint32_t uart_errors;
        uint32_t overflows;
    };

    /// @param port        UART port number
    /// @param max_payload Maximum payload size to accept
    Parser(uart_port_t port, uint8_t max_payload);

    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    /// Process a single UART event.
    /// @param event      The UART event from the driver event queue
    /// @param out_frame  Filled with the parsed frame on success
    /// @return           true if a complete, CRC-valid frame was parsed
    bool processEvent(const uart_event_t& event, RxFrame& out_frame);

    /// Get cumulative error counters.
    Counters getCounters() const;

private:
    void handlePatternDetected(RxFrame& out_frame, bool& success);
    void handleOverflow();
    void discardBufferedData();

    uart_port_t m_port;
    uint8_t m_max_payload;
    uint8_t* m_buf;
    Counters m_counters;
};

} // namespace detail
} // namespace uart_comm
