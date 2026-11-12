#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace drivelink::protocol {

/// Frame scanner: finds `AA AA AA | MSG_TYPE | LEN u16 | HCRC | payload | CRC16`
/// in a byte stream.
///
/// The caller fills the receive window (`writable()` + `commit()`), then calls
/// `next()` until it returns false. At each preamble the checks run in order:
///   1. HCRC over MSG_TYPE and LEN (as soon as the 4 header bytes are in)
///   2. LEN against the limit
///   3. CRC16, once the whole frame is in
/// A candidate that fails a check is noise: only its first byte is dropped and
/// the scan resumes. Checks 1 and 2 need no payload bytes, so a false
/// `AA AA AA` almost never makes the scanner wait. The preamble is not
/// escaped, so a false preamble inside a payload can never hide a real frame
/// behind it.
///
/// Pure logic: no clock, no logging, no I/O. The window is allocated once, in
/// the constructor. Not thread-safe; use it from one task.
class Scanner {
public:
    struct Counters {
        uint32_t crc_errors = 0;        ///< Candidates rejected for a CRC16 mismatch
        uint32_t header_crc_errors = 0; ///< Candidates rejected for an HCRC mismatch
        uint32_t length_errors = 0;     ///< Candidates with a valid header whose LEN exceeded the limit
        uint32_t timeouts = 0;          ///< Partial frames dropped by dropStale()
        uint32_t discarded_bytes = 0;   ///< Bytes that were not part of any valid frame
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
    /// then call commit() with the number written. Moves the unconsumed bytes
    /// to the start of the window first (the only place bytes are moved).
    uint8_t *writable(size_t &space);
    void commit(size_t n);

    /// Extract the next complete, valid frame.
    /// @return true if `out` was filled; false if more bytes are needed.
    bool next(Frame &out);

    /// Drop the partial frame at the start of the window, because the sender
    /// stopped part-way through it. Drops one byte of a candidate that has a
    /// full preamble (so a real frame starting inside it is still found), or
    /// all of a lone partial preamble. Counts a timeout in the first case.
    /// @return false if the window was empty
    bool dropStale();

    /// Bytes in the window that have not been consumed.
    size_t buffered() const { return m_len - m_head - m_pending; }

    const Counters &counters() const { return m_counters; }

private:
    /// Apply the consume that the last returned frame deferred.
    void settle();
    void consume(size_t n);
    void discard(size_t n);
    size_t available() const { return m_len - m_head; }
    uint8_t at(size_t i) const { return m_buf[m_head + i]; }

    size_t m_max_payload;
    std::vector<uint8_t> m_buf; ///< window; unconsumed bytes are [m_head, m_len)
    size_t m_head = 0;
    size_t m_len = 0;
    size_t m_pending = 0;       ///< bytes of the last returned frame, consumed lazily
    Counters m_counters{};
};

} // namespace drivelink::protocol
