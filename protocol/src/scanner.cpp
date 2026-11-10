#include "drivelink/protocol/scanner.hpp"

#include <algorithm>
#include <cstring>

#include "drivelink/protocol/framing.hpp"

namespace drivelink::protocol {

Scanner::Scanner(size_t max_payload)
    : m_max_payload(std::min(max_payload, kMaxPayload)),
      m_buf(2U * frameSize(m_max_payload))
{
}

uint8_t *Scanner::writable(size_t &space)
{
    settle();
    if (m_head > 0U) {
        const size_t n = available();
        if (n > 0U) {
            std::memmove(m_buf.data(), &m_buf[m_head], n);
        }
        m_head = 0;
        m_len = n;
    }
    space = m_buf.size() - m_len;
    return m_buf.data() + m_len;
}

void Scanner::commit(size_t n)
{
    settle();
    m_len += std::min(n, m_buf.size() - m_len);
}

bool Scanner::next(Frame &out)
{
    settle();

    while (true) {
        // Find the first preamble in the window.
        size_t start = 0;
        while (start + kStartLen <= available() &&
               !(at(start) == kStartByte && at(start + 1U) == kStartByte && at(start + 2U) == kStartByte)) {
            ++start;
        }

        if (start + kStartLen > available()) {
            // No full preamble. Keep a trailing run of up to kStartLen - 1
            // start bytes, which may be the beginning of the next preamble.
            const size_t n = available();
            size_t keep = 0;
            while (keep < kStartLen - 1U && keep < n && at(n - 1U - keep) == kStartByte) {
                ++keep;
            }
            if (n > keep) {
                discard(n - keep);
            }
            return false;
        }

        if (start > 0U) {
            discard(start);
        }

        if (available() < kStartLen + kHeaderLen) {
            return false;
        }

        const uint8_t *header = &m_buf[m_head + kStartLen];
        const uint8_t msg_type = header[0];

        // A start byte right after the preamble means the window starts with a
        // run of 4+ start bytes. That is either a frame with MSG_TYPE 0xAA or
        // a stray start byte just before a real preamble. Rejecting it is part
        // of normal resync, so it is not counted as an error.
        const bool ambiguous = (msg_type == kStartByte);

        // Header checks first: they need no payload bytes, so a false
        // preamble is rejected without waiting for LEN bytes to arrive.
        if (!headerCrcValid(header)) {
            if (!ambiguous) {
                ++m_counters.header_crc_errors;
            }
            discard(1);
            continue;
        }

        const size_t payload_len = headerPayloadLen(header);
        if (payload_len > m_max_payload) {
            if (!ambiguous) {
                ++m_counters.length_errors;
            }
            discard(1);
            continue;
        }

        const size_t total = frameSize(payload_len);
        if (available() < total) {
            return false;
        }

        if (!validateCrc(header, payload_len)) {
            if (!ambiguous) {
                ++m_counters.crc_errors;
            }
            // Only the first byte is dropped: a real frame may start inside this one.
            discard(1);
            continue;
        }

        out.msg_type = msg_type;
        out.len = payload_len;
        out.payload = header + kHeaderLen;
        m_pending = total; // consumed on the next call, so `out.payload` stays valid
        return true;
    }
}

bool Scanner::dropStale()
{
    settle();
    const size_t n = available();
    if (n == 0U) {
        return false;
    }
    if (n >= kStartLen) {
        ++m_counters.timeouts;
        discard(1);
    } else {
        discard(n);
    }
    return true;
}

void Scanner::settle()
{
    if (m_pending != 0U) {
        const size_t n = m_pending;
        m_pending = 0;
        consume(n);
    }
}

void Scanner::consume(size_t n)
{
    m_head += std::min(n, available());
    if (m_head == m_len) {
        // Empty: restart at the front, so the next refill needs no move.
        m_head = 0;
        m_len = 0;
    }
}

void Scanner::discard(size_t n)
{
    n = std::min(n, available());
    m_counters.discarded_bytes += static_cast<uint32_t>(n);
    consume(n);
}

} // namespace drivelink::protocol
