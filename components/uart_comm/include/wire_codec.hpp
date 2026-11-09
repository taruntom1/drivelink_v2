#pragma once

/// Bounds-checked little-endian field codec for protocol payloads.
///
/// Every protocol-facing component encodes and decodes its payloads field by
/// field with these helpers (see "Protocol conventions" in CLAUDE.md). Floats
/// are IEEE-754 binary32. The header is pure C++ with no ESP-IDF dependency,
/// so the host unit tests use it directly.
///
/// Both classes have a sticky error flag. The first read or append that
/// would run past the end of the buffer sets it; that call and every later
/// one then does nothing (reads return 0). Check ok() once after a sequence
/// of calls instead of after each one.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace uart_comm::wire {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "wire floats are IEEE-754 binary32");

class Reader {
public:
    Reader(const uint8_t *data, size_t len) : m_data(data), m_len((data != nullptr) ? len : 0U) {}

    uint8_t readU8() { return static_cast<uint8_t>(readLe(1U)); }
    uint16_t readU16Le() { return static_cast<uint16_t>(readLe(2U)); }
    uint32_t readU32Le() { return static_cast<uint32_t>(readLe(4U)); }
    uint64_t readU64Le() { return readLe(8U); }
    int16_t readI16Le() { return static_cast<int16_t>(readU16Le()); }
    int32_t readI32Le() { return static_cast<int32_t>(readU32Le()); }
    int64_t readI64Le() { return static_cast<int64_t>(readU64Le()); }

    float readF32Le()
    {
        const uint32_t bits = readU32Le();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    /// Skip n bytes (reserved fields).
    void skip(size_t n)
    {
        if (take(n)) {
            m_offset += n;
        }
    }

    /// True if no read has run past the end.
    bool ok() const { return m_ok; }
    /// True if every read succeeded and the whole buffer was consumed.
    bool done() const { return m_ok && m_offset == m_len; }
    size_t offset() const { return m_offset; }
    size_t remaining() const { return m_len - m_offset; }

private:
    bool take(size_t n)
    {
        if (!m_ok || n > (m_len - m_offset)) {
            m_ok = false;
            return false;
        }
        return true;
    }

    uint64_t readLe(size_t n)
    {
        if (!take(n)) {
            return 0U;
        }
        uint64_t value = 0U;
        for (size_t i = 0; i < n; ++i) {
            value |= static_cast<uint64_t>(m_data[m_offset + i]) << (8U * i);
        }
        m_offset += n;
        return value;
    }

    const uint8_t *m_data;
    size_t m_len;
    size_t m_offset = 0U;
    bool m_ok = true;
};

class Writer {
public:
    Writer(uint8_t *data, size_t capacity) : m_data(data), m_cap((data != nullptr) ? capacity : 0U) {}

    void appendU8(uint8_t value) { appendLe(value, 1U); }
    void appendU16Le(uint16_t value) { appendLe(value, 2U); }
    void appendU32Le(uint32_t value) { appendLe(value, 4U); }
    void appendU64Le(uint64_t value) { appendLe(value, 8U); }
    void appendI16Le(int16_t value) { appendU16Le(static_cast<uint16_t>(value)); }
    void appendI32Le(int32_t value) { appendU32Le(static_cast<uint32_t>(value)); }
    void appendI64Le(int64_t value) { appendU64Le(static_cast<uint64_t>(value)); }

    void appendF32Le(float value)
    {
        uint32_t bits = 0U;
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32Le(bits);
    }

    /// Append raw bytes (an already-encoded body).
    void appendBytes(const uint8_t *src, size_t n)
    {
        if (n == 0U) {
            return;
        }
        if (src == nullptr || !fits(n)) {
            m_ok = false;
            return;
        }
        std::memcpy(&m_data[m_size], src, n);
        m_size += n;
    }

    /// Overwrite one byte that was already written, such as an entry count
    /// that is only known after the entries. Fails (sticky) past size().
    void patchU8(size_t offset, uint8_t value)
    {
        if (!m_ok || offset >= m_size) {
            m_ok = false;
            return;
        }
        m_data[offset] = value;
    }

    /// True if no append has run past the capacity.
    bool ok() const { return m_ok; }
    size_t size() const { return m_size; }
    size_t remaining() const { return m_cap - m_size; }

private:
    bool fits(size_t n) const { return m_ok && n <= (m_cap - m_size); }

    void appendLe(uint64_t value, size_t n)
    {
        if (!fits(n)) {
            m_ok = false;
            return;
        }
        for (size_t i = 0; i < n; ++i) {
            m_data[m_size + i] = static_cast<uint8_t>((value >> (8U * i)) & 0xFFU);
        }
        m_size += n;
    }

    uint8_t *m_data;
    size_t m_cap;
    size_t m_size = 0U;
    bool m_ok = true;
};

} // namespace uart_comm::wire
