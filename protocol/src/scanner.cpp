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
    space = m_buf.size() - m_len;
    return &m_buf[m_len];
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
        while (start + kStartLen <= m_len &&
               !(m_buf[start] == kStartByte && m_buf[start + 1U] == kStartByte && m_buf[start + 2U] == kStartByte)) {
            ++start;
        }

        if (start + kStartLen > m_len) {
            // No full preamble. Keep a trailing run of up to kStartLen - 1
            // start bytes, which may be the beginning of the next preamble.
            size_t keep = 0;
            while (keep < kStartLen - 1U && keep < m_len && m_buf[m_len - 1U - keep] == kStartByte) {
                ++keep;
            }
            if (m_len > keep) {
                discard(m_len - keep);
            }
            return false;
        }

        if (start > 0U) {
            discard(start);
        }

        if (m_len < kStartLen + kHeaderLen) {
            return false;
        }

        const uint8_t msg_type = m_buf[kStartLen];
        const size_t payload_len = m_buf[kStartLen + 1U];

        // A start byte right after the preamble means the window starts with a
        // run of 4+ start bytes. That is either a frame with MSG_TYPE 0xAA or
        // a stray start byte just before a real preamble. Rejecting it is part
        // of normal resync, so it is not counted as an error.
        const bool ambiguous = (msg_type == kStartByte);

        if (payload_len > m_max_payload) {
            if (!ambiguous) {
                ++m_counters.length_errors;
            }
            discard(1);
            continue;
        }

        const size_t total = frameSize(payload_len);
        if (m_len < total) {
            return false;
        }

        if (!validateCrc(&m_buf[kStartLen], payload_len)) {
            if (!ambiguous) {
                ++m_counters.crc_errors;
            }
            // Only the first byte is dropped: a real frame may start inside this one.
            discard(1);
            continue;
        }

        out.msg_type = msg_type;
        out.len = payload_len;
        out.payload = &m_buf[kStartLen + kHeaderLen];
        m_pending = total; // consumed on the next call, so `out.payload` stays valid
        return true;
    }
}

bool Scanner::dropStale()
{
    settle();
    if (m_len == 0U) {
        return false;
    }
    if (m_len >= kStartLen) {
        ++m_counters.timeouts;
        discard(1);
    } else {
        discard(m_len);
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
    n = std::min(n, m_len);
    if (n < m_len) {
        std::memmove(m_buf.data(), &m_buf[n], m_len - n);
    }
    m_len -= n;
}

void Scanner::discard(size_t n)
{
    n = std::min(n, m_len);
    m_counters.discarded_bytes += static_cast<uint32_t>(n);
    consume(n);
}

} // namespace drivelink::protocol
