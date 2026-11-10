#pragma once

#include <atomic>

#include <cstddef>
#include <cstdint>
#include <vector>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace uart_comm {

struct RxFrame;

namespace detail {

/// Software UART frame parser.
///
/// Bytes are pulled from the UART driver's ring buffer into a local window
/// and scanned for `AA AA AA | MSG_TYPE | LEN | PAYLOAD | CRC`. A candidate
/// whose LEN is too large or whose CRC does not match is treated as noise:
/// only its first byte is dropped and the scan resumes, so a false
/// `AA AA AA` inside a payload can never hide a real frame that follows it,
/// and valid frames already buffered are never flushed.
///
/// Used only from the RX task.
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
    Parser(uart_port_t port, uint8_t max_payload);

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
    enum class ScanResult { Frame, NeedMore, Dropped };

    /// Copy bytes waiting in the driver into the window (non-blocking).
    /// @return number of bytes read
    size_t fillFromDriver();
    /// Try to decode a frame at the start of the window.
    ScanResult scan(RxFrame& out_frame);
    /// Remove `n` bytes from the start of the window.
    void consume(size_t n);
    /// Drop `n` bytes that were not part of a valid frame.
    void discard(size_t n);

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
    uint8_t m_max_payload;
    std::vector<uint8_t> m_buf;   // receive window, valid bytes are [0, m_len)
    size_t m_len;
    TickType_t m_last_rx_tick;    // when bytes last arrived from the driver
    AtomicCounters m_counters;
};

} // namespace detail
} // namespace uart_comm
