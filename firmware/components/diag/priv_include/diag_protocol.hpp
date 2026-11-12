#pragma once

/// Diagnostic wire v3: pure byte-in/byte-out codec with no
/// ESP-IDF or FreeRTOS dependency, so the host unit tests cover it.
/// docs/protocol/diag.md is the spec. MSG_TYPEs come from the generated registry (diag_reporter.hpp).

#include <cstddef>
#include <cstdint>

namespace diag::protocol {

constexpr uint8_t kProtocolVersion = 0x03;
/// Echoed as the subcommand when a query header could not be parsed.
constexpr uint8_t kUnparsedHeaderByte = 0xFF;
/// GetEntries `next` value when no listed slot remains.
constexpr uint8_t kNoNextPage = 0xFF;

/// Event header `flags` bits.
constexpr uint8_t kReservedFlags = 0;

/// Entry `state` bits (GetEntries).
constexpr uint8_t kStateOccurred = 0x01;
constexpr uint8_t kStateActive = 0x02;

/// Clear request `what` bits.
constexpr uint8_t kClearOccurred = 0x01;
constexpr uint8_t kClearCounts = 0x02;
constexpr uint8_t kClearAllBits = kClearOccurred | kClearCounts;

enum class EventKind : uint8_t {
    Record = 0x01,
    CounterDelta = 0x02,
    Cleared = 0x03,
    Boot = 0x04,
};

enum class QuerySubcommand : uint8_t {
    GetInfo = 0x01,
    GetEntries = 0x02,
    Clear = 0x03,
};

enum class ResultCode : uint8_t {
    Ok = 0x00,
    InvalidArgument = 0x01,
    NotSupported = 0x02,
};

// ── Sizes ───────────────────────────────────────────────────────────────
constexpr size_t kEventHeaderSize = 5;          // version, seq u16, kind, flags
constexpr size_t kRecordBodySize = 25;
constexpr size_t kClearedBodySize = 12;
constexpr size_t kBootBodySize = 12;
constexpr size_t kCounterDeltaFixedSize = 9;    // timestamp u64, n u8
constexpr size_t kCounterEntrySize = 12;        // source, code u16, delta u32, total u32

constexpr size_t kQueryRequestHeaderSize = 3;   // version, seq, subcommand
constexpr size_t kQueryResponseHeaderSize = 4;  // version, seq, subcommand, result
constexpr size_t kGetEntriesRequestSize = 1;
constexpr size_t kClearRequestSize = 1;
constexpr size_t kEntriesFixedSize = 3;         // total, next, n
constexpr size_t kEntrySize = 9;                // source, code u16, state, count u32
constexpr size_t kInfoFixedSize = 22;           // everything in GetInfo but the version bytes
constexpr size_t kMaxVersionLen = 32;

/// Largest fixed-size event payload (Record).
constexpr size_t kMaxFixedEventSize = kEventHeaderSize + kRecordBodySize;
/// Largest GetInfo response payload.
constexpr size_t kMaxInfoResponseSize = kQueryResponseHeaderSize + kInfoFixedSize + kMaxVersionLen;

/// Counter entries that fit one CounterDelta event of at most `payload_cap` bytes.
constexpr size_t maxCounterEntries(size_t payload_cap)
{
    const size_t fixed = kEventHeaderSize + kCounterDeltaFixedSize;
    const size_t n = (payload_cap > fixed) ? (payload_cap - fixed) / kCounterEntrySize : 0U;
    return (n > 255U) ? 255U : n;
}

/// Entries that fit one GetEntries response of at most `payload_cap` bytes.
constexpr size_t maxEntriesPerPage(size_t payload_cap)
{
    const size_t fixed = kQueryResponseHeaderSize + kEntriesFixedSize;
    const size_t n = (payload_cap > fixed) ? (payload_cap - fixed) / kEntrySize : 0U;
    return (n > 255U) ? 255U : n;
}

// ── Events (ESP → host) ─────────────────────────────────────────────────

struct EventHeader {
    uint16_t seq = 0;
    EventKind kind = EventKind::Record;
    uint8_t flags = 0;
};

struct RecordEvent {
    uint64_t timestamp_us = 0;
    uint8_t severity = 0;
    uint8_t source = 0;
    uint8_t instance = 255;
    uint16_t code = 0;
    int32_t esp_err = 0;
    uint32_t arg0 = 0;
    uint32_t arg1 = 0;
};

struct CounterEntry {
    uint8_t source = 0;
    uint16_t code = 0;
    uint32_t delta = 0;
    uint32_t total = 0;
    uint8_t instance = 255;
};

struct ClearedEvent {
    uint64_t timestamp_us = 0;
    uint8_t source = 0;
    uint8_t instance = 255;
    uint16_t code = 0;
};

struct BootEvent {
    uint64_t timestamp_us = 0;
    uint8_t reset_reason = 0;
    uint8_t instances_ok = 0;
    uint8_t instances_failed = 0;
    uint8_t safe_mode = 0;
};

// Each encoder sets the header kind itself and returns the payload length,
// or 0 if it does not fit in `cap`.
size_t encodeRecordEvent(uint16_t seq, uint8_t flags, const RecordEvent &ev, uint8_t *out, size_t cap);
size_t encodeClearedEvent(uint16_t seq, uint8_t flags, const ClearedEvent &ev, uint8_t *out, size_t cap);
size_t encodeBootEvent(uint16_t seq, uint8_t flags, const BootEvent &ev, uint8_t *out, size_t cap);
/// `n` must be at most 255.
size_t encodeCounterDeltaEvent(uint16_t seq, uint8_t flags, uint64_t timestamp_us,
                               const CounterEntry *entries, size_t n, uint8_t *out, size_t cap);

// ── Query (host → ESP) and its response ─────────────────────────────────

/// Echoed in the response. When the header could not be parsed, subcommand
/// is kUnparsedHeaderByte, and seq is echoed only if the version matched and
/// a seq byte was present (0 otherwise).
struct QueryHeader {
    uint8_t seq = 0;
    uint8_t subcommand = kUnparsedHeaderByte;
};

/// Decode the query header (version, seq, subcommand).
/// @param[out] header  Always filled in, so a response can be sent.
/// @param[out] body, body_len  The bytes after the header, on success.
/// @return false if the payload is too short or has the wrong version.
bool decodeQueryHeader(const uint8_t *payload, size_t len, QueryHeader &header,
                       const uint8_t *&body, size_t &body_len);

// Body decoders: exact length required; `out` is unchanged on failure.
bool decodeGetInfo(const uint8_t *body, size_t len);
bool decodeGetEntries(const uint8_t *body, size_t len, uint8_t &start);
/// Rejects a `what` with bits outside kClearAllBits.
bool decodeClear(const uint8_t *body, size_t len, uint8_t &what);

struct InfoBody {
    uint8_t flags = 0;
    uint64_t uptime_us = 0;
    uint8_t reset_reason = 0;
    uint8_t instances_ok = 0;
    uint8_t instances_failed = 0;
    uint8_t safe_mode = 0;
    uint32_t records_dropped = 0;
    uint32_t events_tx_dropped = 0;
    const char *version = nullptr; ///< Need not be NUL-terminated; see version_len
    size_t version_len = 0;        ///< Truncated to kMaxVersionLen
};

struct EntryState {
    uint8_t source = 0;
    uint16_t code = 0;
    uint8_t state = 0;
    uint32_t count = 0;
    uint8_t instance = 255;
};

/// @return The body length, or 0 if it does not fit in `cap`.
size_t encodeInfoBody(const InfoBody &info, uint8_t *out, size_t cap);
/// `n` must be at most 255. @return The body length, or 0 if it does not fit.
size_t encodeEntriesBody(uint8_t total, uint8_t next, const EntryState *entries, size_t n,
                         uint8_t *out, size_t cap);

/// Encode a query response: the 4-byte header, then `body` (may be empty).
/// @return The payload length, or 0 if it does not fit in `cap`.
size_t encodeQueryResponse(const QueryHeader &header, ResultCode result, const uint8_t *body,
                           size_t body_len, uint8_t *out, size_t cap);

} // namespace diag::protocol
