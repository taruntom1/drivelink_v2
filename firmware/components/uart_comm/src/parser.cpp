#include "parser.hpp"
#include "frame.hpp"
#include "uart_comm.hpp"
#include "esp_log.h"
#include <algorithm>
#include <cstring>

static const char *const TAG = "uart_parser";

namespace uart_comm::detail {

// A partial frame that gets no new bytes for this long is dropped. It must be
// longer than the gaps USB-UART adapters leave inside a frame (a few ms).
static constexpr TickType_t kStaleFrameTimeout = pdMS_TO_TICKS(100);

Parser::Parser(uart_port_t port, uint8_t max_payload)
    : m_port(port)
    , m_max_payload(max_payload)
    // Two maximum-size frames, so a full window always holds one whole
    // candidate and the scan can always make progress.
    , m_buf(2U * (kStartLen + kHeaderLen + max_payload + kCrcLen))
    , m_len(0)
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
        switch (scan(out_frame)) {
        case ScanResult::Frame:
            return true;

        case ScanResult::Dropped:
            continue;

        case ScanResult::NeedMore:
            if (fillFromDriver() > 0) {
                continue;
            }
            if (m_len == 0 || (xTaskGetTickCount() - m_last_rx_tick) < kStaleFrameTimeout) {
                return false;
            }
            // The sender stopped part-way through a frame. Drop its first
            // byte and rescan, in case a real frame starts inside it.
            if (m_len >= kStartLen) {
                ESP_LOGW(TAG, "Incomplete frame on port %d (have %u bytes), dropping",
                         m_port, static_cast<unsigned>(m_len));
                m_counters.timeouts++;
                discard(1);
            } else {
                discard(m_len); // a lone partial preamble
            }
            continue;
        }
    }
}

size_t Parser::fillFromDriver()
{
    const size_t space = m_buf.size() - m_len;
    if (space == 0) {
        return 0;
    }

    size_t buffered = 0;
    if (uart_get_buffered_data_len(m_port, &buffered) != ESP_OK || buffered == 0) {
        return 0;
    }

    const size_t want = std::min(buffered, space);
    const int n = uart_read_bytes(m_port, &m_buf[m_len], static_cast<uint32_t>(want), 0);
    if (n <= 0) {
        return 0;
    }

    m_len += static_cast<size_t>(n);
    m_last_rx_tick = xTaskGetTickCount();
    return static_cast<size_t>(n);
}

Parser::ScanResult Parser::scan(RxFrame& out_frame)
{
    // Find the first preamble in the window.
    size_t start = 0;
    while (start + kStartLen <= m_len &&
           !(m_buf[start] == kStartByte && m_buf[start + 1] == kStartByte &&
             m_buf[start + 2] == kStartByte)) {
        ++start;
    }

    if (start + kStartLen > m_len) {
        // No full preamble. Keep a trailing run of up to kStartLen - 1 start
        // bytes, which may be the beginning of the next preamble.
        size_t keep = 0;
        while (keep < kStartLen - 1U && keep < m_len &&
               m_buf[m_len - 1U - keep] == kStartByte) {
            ++keep;
        }
        if (m_len > keep) {
            discard(m_len - keep);
        }
        return ScanResult::NeedMore;
    }

    if (start > 0) {
        discard(start);
    }

    if (m_len < kStartLen + kHeaderLen) {
        return ScanResult::NeedMore;
    }

    const uint8_t msg_type = m_buf[kStartLen];
    const uint8_t payload_len = m_buf[kStartLen + 1];

    // A start byte right after the preamble means this window starts with a
    // run of 4+ start bytes. That is either a frame with MSG_TYPE 0xAA or a
    // stray start byte just before a real preamble. Rejecting it is part of
    // normal resync, so it is not counted as an error.
    const bool ambiguous = (msg_type == kStartByte);

    if (payload_len > m_max_payload) {
        if (!ambiguous) {
            ESP_LOGW(TAG, "Payload length %u exceeds max %u on port %d, resyncing",
                     payload_len, m_max_payload, m_port);
            m_counters.length_errors++;
        }
        discard(1);
        return ScanResult::Dropped;
    }

    const size_t frame_len = kStartLen + kHeaderLen + payload_len + kCrcLen;
    if (m_len < frame_len) {
        return ScanResult::NeedMore;
    }

    if (!validateCrc(&m_buf[kStartLen], payload_len)) {
        if (!ambiguous) {
            ESP_LOGW(TAG, "CRC mismatch on port %d, msg_type=0x%02X len=%u, resyncing",
                     m_port, msg_type, payload_len);
            m_counters.crc_errors++;
        }
        // Only the first byte is dropped: a real frame may start inside this one.
        discard(1);
        return ScanResult::Dropped;
    }

    out_frame.msg_type = msg_type;
    out_frame.len = payload_len;
    if (payload_len > 0) {
        std::memcpy(out_frame.payload, &m_buf[kStartLen + kHeaderLen], payload_len);
    }
    consume(frame_len);
    return ScanResult::Frame;
}

void Parser::consume(size_t n)
{
    n = std::min(n, m_len);
    if (n < m_len) {
        std::memmove(m_buf.data(), &m_buf[n], m_len - n);
    }
    m_len -= n;
}

void Parser::discard(size_t n)
{
    n = std::min(n, m_len);
    m_counters.discarded_bytes += static_cast<uint32_t>(n);
    consume(n);
}

Parser::Counters Parser::getCounters() const
{
    Counters out = {};
    out.crc_errors = m_counters.crc_errors.load(std::memory_order_relaxed);
    out.timeouts = m_counters.timeouts.load(std::memory_order_relaxed);
    out.uart_errors = m_counters.uart_errors.load(std::memory_order_relaxed);
    out.overflows = m_counters.overflows.load(std::memory_order_relaxed);
    out.length_errors = m_counters.length_errors.load(std::memory_order_relaxed);
    out.discarded_bytes = m_counters.discarded_bytes.load(std::memory_order_relaxed);
    return out;
}

} // namespace uart_comm::detail
