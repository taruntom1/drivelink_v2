#pragma once

#include <atomic>

#include <cstddef>
#include <cstdint>
#include "drivelink/protocol/scanner.hpp"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"

namespace uart_comm {

struct RxFrame;

namespace detail {

/// The UART-driver side of frame reception.
///
/// Bytes are pulled from the UART driver's ring buffer straight into the
/// receive window of a `drivelink::protocol::Scanner`, which finds the frames
/// (see scanner.hpp for the resync rules: a rejected candidate drops one byte,
/// and valid frames already buffered are never flushed). This class adds what
/// the pure scanner leaves out: the driver reads, the stale-frame timer,
/// driver error events, the counters other tasks read, and logging.
///
/// Used only from the RX task, except getCounters().
class Parser {
public:
    struct Counters {
        uint32_t crc_errors;
        uint32_t timeouts;
        uint32_t uart_errors;
        uint32_t overflows;
        uint32_t length_errors;
        uint32_t discarded_bytes;
    };

    /// @param port        UART port number
    /// @param max_payload Maximum payload size to accept
    Parser(uart_port_t port, size_t max_payload);

    ~Parser() = default;

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    /// Handle one driver event (errors and overflows). Received bytes are not
    /// read here: nextFrame() pulls them from the driver as needed.
    void processEvent(const uart_event_t& event);

    /// Extract the next complete, CRC-valid frame.
    /// Reads any bytes waiting in the driver first. Call repeatedly until it
    /// returns false, after every event and on idle timeouts, so a partial
    /// frame that never completes is eventually dropped.
    /// @return true if `out_frame` was filled
    bool nextFrame(RxFrame& out_frame);

    /// Get cumulative error counters.
    Counters getCounters() const;

private:
    /// Copy bytes waiting in the driver into the scanner window (non-blocking).
    /// @return number of bytes read
    size_t fillFromDriver();
    /// Copy the scanner's counters into the atomics, and log new rejects.
    void publishCounters();

    // Written only by the RX task, read from any task via getCounters().
    // Each counter is atomic; a snapshot of all of them is not taken atomically.
    struct AtomicCounters {
        std::atomic<uint32_t> crc_errors;
        std::atomic<uint32_t> timeouts;
        std::atomic<uint32_t> uart_errors;
        std::atomic<uint32_t> overflows;
        std::atomic<uint32_t> length_errors;
        std::atomic<uint32_t> discarded_bytes;
    };

    uart_port_t m_port;
    drivelink::protocol::Scanner m_scanner;
    TickType_t m_last_rx_tick;    // when bytes last arrived from the driver
    AtomicCounters m_counters;
};

} // namespace detail
} // namespace uart_comm
