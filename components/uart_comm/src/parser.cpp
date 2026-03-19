#include "parser.hpp"
#include "frame.hpp"
#include "uart_comm.hpp"
#include "esp_log.h"
#include "sdkconfig.h"
#include <cstring>

static const char *TAG = "uart_parser";

namespace uart_comm {
namespace detail {

static constexpr TickType_t kReadTimeout = pdMS_TO_TICKS(100);

Parser::Parser(uart_port_t port, uint8_t max_payload)
    : m_port(port)
    , m_max_payload(max_payload)
    , m_buf(new uint8_t[kHeaderLen + max_payload + kCrcLen])
    , m_counters{}
{
}

Parser::~Parser()
{
    delete[] m_buf;
}

bool Parser::processEvent(const uart_event_t& event, RxFrame& out_frame)
{
    bool success = false;

    switch (event.type) {
    case UART_PATTERN_DET:
        handlePatternDetected(out_frame, success);
        break;

    case UART_FIFO_OVF:
        ESP_LOGW(TAG, "UART FIFO overflow on port %d", m_port);
        m_counters.overflows++;
        handleOverflow();
        break;

    case UART_BUFFER_FULL:
        ESP_LOGW(TAG, "UART ring buffer full on port %d", m_port);
        m_counters.overflows++;
        handleOverflow();
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
        // Data without pattern detection — discard stray bytes
        discardBufferedData();
        break;

    default:
        break;
    }

    return success;
}

void Parser::handlePatternDetected(RxFrame& out_frame, bool& success)
{
    int pos = uart_pattern_pop_pos(m_port);
    if (pos == -1) {
        return; // stale event, pattern queue was reset
    }

    // Discard any data before the pattern.
    // pos is the index of the first byte of the pattern in the buffer.
    // Data before index (pos - 1) is junk.
    int junk_len = pos -  1;
    if (junk_len > 0) {
        uint8_t discard[64];
        int remaining = junk_len;
        while (remaining > 0) {
            int chunk = remaining > (int)sizeof(discard) ? (int)sizeof(discard) : remaining;
            uart_read_bytes(m_port, discard, chunk, 0);
            remaining -= chunk;
        }
    }

    // Consume the 3 start bytes (0xAA 0xAA 0xAA)
    uint8_t start[kStartLen];
    int n = uart_read_bytes(m_port, start, kStartLen, kReadTimeout);
    if (n < kStartLen) {
        m_counters.timeouts++;
        return;
    }

    // Read header: MSG_TYPE + LEN = 2 bytes
    n = uart_read_bytes(m_port, m_buf, kHeaderLen, kReadTimeout);
    if (n < kHeaderLen) {
        ESP_LOGW(TAG, "Incomplete header on port %d (got %d bytes)", m_port, n);
        m_counters.timeouts++;
        return;
    }

    uint8_t msg_type = m_buf[0];
    uint8_t payload_len = m_buf[1];

    if (payload_len > m_max_payload) {
        ESP_LOGW(TAG, "Payload length %u exceeds max %u, discarding frame", payload_len, m_max_payload);
        m_counters.crc_errors++;
        discardBufferedData();
        return;
    }

    // Read payload + CRC
    int remaining = payload_len + kCrcLen;
    n = uart_read_bytes(m_port, &m_buf[kHeaderLen], remaining, kReadTimeout);
    if (n < remaining) {
        ESP_LOGW(TAG, "Incomplete frame on port %d (expected %d, got %d bytes)",
                 m_port, remaining, n);
        m_counters.timeouts++;
        return;
    }

    // Validate CRC over MSG_TYPE + LEN + PAYLOAD
    if (!validateCrc(m_buf, payload_len)) {
        ESP_LOGW(TAG, "CRC mismatch on port %d, msg_type=0x%02X len=%u",
                 m_port, msg_type, payload_len);
        m_counters.crc_errors++;
        return;
    }

    // Frame valid — fill output
    out_frame.msg_type = msg_type;
    out_frame.len = payload_len;
    if (payload_len > 0) {
        std::memcpy(out_frame.payload, &m_buf[kHeaderLen], payload_len);
    }

    success = true;
}

void Parser::handleOverflow()
{
    uart_flush_input(m_port);
    uart_pattern_queue_reset(m_port, CONFIG_UART_COMM_PATTERN_QUEUE_SIZE);
}

void Parser::discardBufferedData()
{
    size_t buffered = 0;
    uart_get_buffered_data_len(m_port, &buffered);
    if (buffered > 0) {
        uint8_t discard[128];
        while (buffered > 0) {
            size_t chunk = buffered > sizeof(discard) ? sizeof(discard) : buffered;
            uart_read_bytes(m_port, discard, chunk, 0);
            buffered -= chunk;
        }
    }
}

Parser::Counters Parser::getCounters() const
{
    return m_counters;
}

} // namespace detail
} // namespace uart_comm
