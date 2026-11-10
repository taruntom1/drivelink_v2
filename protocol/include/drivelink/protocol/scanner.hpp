#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace drivelink::protocol {

/// Frame scanner: finds `AA AA AA | MSG_TYPE | LEN | payload | CRC16` in a
/// byte stream.
///
/// The caller fills the receive window (`writable()` + `commit()`), then calls
/// `next()` until it returns false. A candidate whose LEN is above the limit,
/// or whose CRC does not match, is noise: only its first byte is dropped and
/// the scan resumes. The preamble is not escaped, so a false `AA AA AA` inside
/// a payload can never hide a real frame behind it.
///
/// Pure logic: no clock, no logging, no I/O. The window is allocated once, in
/// the constructor. Not thread-safe; use it from one task.
class Scanner {
public:
    struct Counters {
        uint32_t crc_errors = 0;       ///< Candidates rejected for a CRC mismatch
        uint32_t length_errors = 0;    ///< Candidates rejected because LEN exceeded the limit
        uint32_t timeouts = 0;         ///< Partial frames dropped by dropStale()
        uint32_t discarded_bytes = 0;  ///< Bytes that were not part of any valid frame
    };

    /// A decoded frame. `payload` points into the receive window and stays
    /// valid until the next call to next(), writable(), commit() or dropStale().
    struct Frame {
        uint8_t msg_type = 0;
        size_t len = 0;
        const uint8_t *payload = nullptr;
    };

    /// @param max_payload  Largest payload to accept, clamped to kMaxPayload.
    ///                     The window holds two maximum-size frames, so a full
    ///                     window always holds one whole candidate and the
    ///                     scan can always make progress.
    explicit Scanner(size_t max_payload);

    size_t maxPayload() const { return m_max_payload; }

    /// Free space at the end of the window. Write up to `space` bytes there,
    /// then call commit() with the number written.
    uint8_t *writable(size_t &space);
    void commit(size_t n);

    /// Extract the next complete, CRC-valid frame.
    /// @return true if `out` was filled; false if more bytes are needed.
    bool next(Frame &out);

    /// Drop the partial frame at the start of the window, because the sender
    /// stopped part-way through it. Drops one byte of a candidate that has a
    /// full preamble (so a real frame starting inside it is still found), or
    /// all of a lone partial preamble. Counts a timeout in the first case.
    /// @return false if the window was empty
    bool dropStale();

    /// Bytes in the window that have not been consumed.
    size_t buffered() const { return m_len - m_pending; }

    const Counters &counters() const { return m_counters; }

private:
    /// Apply the consume that the last returned frame deferred.
    void settle();
    void consume(size_t n);
    void discard(size_t n);

    size_t m_max_payload;
    std::vector<uint8_t> m_buf; ///< window; valid bytes are [0, m_len)
    size_t m_len = 0;
    size_t m_pending = 0;       ///< bytes of the last returned frame, consumed lazily
    Counters m_counters{};
};

} // namespace drivelink::protocol
