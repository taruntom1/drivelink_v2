#include "parser.hpp"
#include "uart_comm.hpp"
#include "drivelink/protocol/framing.hpp"
#include "esp_log.h"
#include <algorithm>
#include <cstring>

static const char *const TAG = "uart_parser";

namespace uart_comm::detail {

// A partial frame that gets no new bytes for this long is dropped. It must be
// longer than the gaps USB-UART adapters leave inside a frame (a few ms).
static constexpr TickType_t kStaleFrameTimeout = pdMS_TO_TICKS(100);

Parser::Parser(uart_port_t port, size_t max_payload)
    : m_port(port)
    , m_scanner(max_payload)
    , m_last_rx_tick(xTaskGetTickCount())
    , m_counters{}
{
}

void Parser::processEvent(const uart_event_t& event)
{
    switch (event.type) {
    case UART_FIFO_OVF:
        // The driver has already reset the hardware FIFO, so bytes were lost.
        // The frame they belonged to fails its CRC and the scan resyncs.
        ESP_LOGW(TAG, "UART FIFO overflow on port %d", m_port);
        m_counters.overflows++;
        break;

    case UART_BUFFER_FULL:
        // The driver stashes the FIFO and stops RX interrupts until we read,
        // so no bytes are lost here. nextFrame() drains the ring buffer.
        ESP_LOGW(TAG, "UART ring buffer full on port %d", m_port);
        m_counters.overflows++;
        break;

    case UART_FRAME_ERR:
        ESP_LOGW(TAG, "UART frame error on port %d", m_port);
        m_counters.uart_errors++;
        break;

    case UART_PARITY_ERR:
        ESP_LOGW(TAG, "UART parity error on port %d", m_port);
        m_counters.uart_errors++;
        break;

    case UART_DATA:
        // Bytes are pulled from the ring buffer by nextFrame(). Never flush
        // here: the ring buffer may already hold the next valid frames.
    default:
        break;
    }
}

bool Parser::nextFrame(RxFrame& out_frame)
{
    (void)fillFromDriver();

    while (true) {
        drivelink::protocol::Scanner::Frame frame;
        if (m_scanner.next(frame)) {
            out_frame.msg_type = frame.msg_type;
            out_frame.len = static_cast<uint16_t>(frame.len);
            if (frame.len > 0U) {
                std::memcpy(out_frame.payload, frame.payload, frame.len);
            }
            publishCounters();
            return true;
        }

        // The scanner needs more bytes.
        if (fillFromDriver() > 0) {
            continue;
        }
        if (m_scanner.buffered() == 0U || (xTaskGetTickCount() - m_last_rx_tick) < kStaleFrameTimeout) {
            publishCounters();
            return false;
        }
        // The sender stopped part-way through a frame. The scanner drops its
        // first byte and rescans, in case a real frame starts inside it. A
        // lone partial preamble is noise and is dropped without a message.
        if (m_scanner.buffered() >= drivelink::protocol::kStartLen) {
            ESP_LOGW(TAG, "Incomplete frame on port %d (have %u bytes), dropping",
                     m_port, static_cast<unsigned>(m_scanner.buffered()));
        }
        (void)m_scanner.dropStale();
    }
}

size_t Parser::fillFromDriver()
{
    size_t space = 0;
    uint8_t* dst = m_scanner.writable(space);
    if (space == 0U) {
        return 0;
    }

    size_t buffered = 0;
    if (uart_get_buffered_data_len(m_port, &buffered) != ESP_OK || buffered == 0) {
        return 0;
    }

    const size_t want = std::min(buffered, space);
    const int n = uart_read_bytes(m_port, dst, static_cast<uint32_t>(want), 0);
    if (n <= 0) {
        return 0;
    }

    m_scanner.commit(static_cast<size_t>(n));
    m_last_rx_tick = xTaskGetTickCount();
    return static_cast<size_t>(n);
}

void Parser::publishCounters()
{
    const drivelink::protocol::Scanner::Counters& c = m_scanner.counters();

    if (c.crc_errors != m_counters.crc_errors.load(std::memory_order_relaxed)) {
        ESP_LOGW(TAG, "CRC mismatch on port %d (%lu so far), resyncing", m_port,
                 static_cast<unsigned long>(c.crc_errors));
    }
    if (c.header_crc_errors != m_counters.header_crc_errors.load(std::memory_order_relaxed)) {
        ESP_LOGW(TAG, "Header CRC mismatch on port %d (%lu so far), resyncing", m_port,
                 static_cast<unsigned long>(c.header_crc_errors));
    }
    if (c.length_errors != m_counters.length_errors.load(std::memory_order_relaxed)) {
        ESP_LOGW(TAG, "Payload length above max %u on port %d (%lu so far), resyncing",
                 static_cast<unsigned>(m_scanner.maxPayload()), m_port,
                 static_cast<unsigned long>(c.length_errors));
    }

    m_counters.crc_errors.store(c.crc_errors, std::memory_order_relaxed);
    m_counters.header_crc_errors.store(c.header_crc_errors, std::memory_order_relaxed);
    m_counters.length_errors.store(c.length_errors, std::memory_order_relaxed);
    m_counters.timeouts.store(c.timeouts, std::memory_order_relaxed);
    m_counters.discarded_bytes.store(c.discarded_bytes, std::memory_order_relaxed);
}

Parser::Counters Parser::getCounters() const
{
    Counters out = {};
    out.crc_errors = m_counters.crc_errors.load(std::memory_order_relaxed);
    out.header_crc_errors = m_counters.header_crc_errors.load(std::memory_order_relaxed);
    out.timeouts = m_counters.timeouts.load(std::memory_order_relaxed);
    out.uart_errors = m_counters.uart_errors.load(std::memory_order_relaxed);
    out.overflows = m_counters.overflows.load(std::memory_order_relaxed);
    out.length_errors = m_counters.length_errors.load(std::memory_order_relaxed);
    out.discarded_bytes = m_counters.discarded_bytes.load(std::memory_order_relaxed);
    return out;
}

} // namespace uart_comm::detail
