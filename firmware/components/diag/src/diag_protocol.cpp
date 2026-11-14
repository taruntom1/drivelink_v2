#include "diag_protocol.hpp"

#include "drivelink/protocol/wire_codec.hpp"

namespace diag::protocol {

using drivelink::protocol::wire::Reader;
using drivelink::protocol::wire::Writer;

namespace {

void appendEventHeader(Writer &w, uint16_t seq, EventKind kind, uint8_t flags)
{
    w.appendU8(kProtocolVersion);
    w.appendU16Le(seq);
    w.appendU8(static_cast<uint8_t>(kind));
    w.appendU8(flags);
}

size_t finish(const Writer &w)
{
    return w.ok() ? w.size() : 0U;
}

} // namespace

size_t encodeRecordEvent(uint16_t seq, uint8_t flags, const RecordEvent &ev, uint8_t *out, size_t cap)
{
    Writer w(out, cap);
    appendEventHeader(w, seq, EventKind::Record, flags);
    w.appendU64Le(ev.timestamp_us);
    w.appendU8(ev.severity);
    w.appendU8(ev.source);
    w.appendU8(ev.instance);
    w.appendU16Le(ev.code);
    w.appendI32Le(ev.esp_err);
    w.appendU32Le(ev.arg0);
    w.appendU32Le(ev.arg1);
    return finish(w);
}

size_t encodeClearedEvent(uint16_t seq, uint8_t flags, const ClearedEvent &ev, uint8_t *out, size_t cap)
{
    Writer w(out, cap);
    appendEventHeader(w, seq, EventKind::Cleared, flags);
    w.appendU64Le(ev.timestamp_us);
    w.appendU8(ev.source);
    w.appendU8(ev.instance);
    w.appendU16Le(ev.code);
    return finish(w);
}

size_t encodeBootEvent(uint16_t seq, uint8_t flags, const BootEvent &ev, uint8_t *out, size_t cap)
{
    Writer w(out, cap);
    appendEventHeader(w, seq, EventKind::Boot, flags);
    w.appendU64Le(ev.timestamp_us);
    w.appendU8(ev.reset_reason);
    w.appendU8(ev.instances_ok);
    w.appendU8(ev.instances_failed);
    w.appendU8(ev.safe_mode);
    return finish(w);
}

size_t encodeCounterDeltaEvent(uint16_t seq, uint8_t flags, uint64_t timestamp_us,
                               const CounterEntry *entries, size_t n, uint8_t *out, size_t cap)
{
    if (n > 255U || (n > 0U && entries == nullptr)) {
        return 0U;
    }
    Writer w(out, cap);
    appendEventHeader(w, seq, EventKind::CounterDelta, flags);
    w.appendU64Le(timestamp_us);
    w.appendU8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        w.appendU8(entries[i].source);
        w.appendU8(entries[i].instance);
        w.appendU16Le(entries[i].code);
        w.appendU32Le(entries[i].delta);
        w.appendU32Le(entries[i].total);
    }
    return finish(w);
}

bool decodeQueryHeader(const uint8_t *payload, size_t len, QueryHeader &header,
                       const uint8_t *&body, size_t &body_len)
{
    header = QueryHeader{};
    body = nullptr;
    body_len = 0;

    Reader r(payload, len);
    const uint8_t version = r.readU8();
    const bool version_ok = r.ok() && version == kProtocolVersion;
    if (!version_ok || len < kQueryRequestHeaderSize) {
        if (version_ok && len >= 2U) {
            header.seq = payload[1];
        }
        return false;
    }

    header.seq = r.readU8();
    header.subcommand = r.readU8();
    body = &payload[kQueryRequestHeaderSize];
    body_len = len - kQueryRequestHeaderSize;
    return true;
}

bool decodeGetInfo(const uint8_t *body, size_t len)
{
    (void)body;
    return len == 0U;
}

bool decodeGetEntries(const uint8_t *body, size_t len, uint8_t &start)
{
    if (len != kGetEntriesRequestSize) {
        return false;
    }
    Reader r(body, len);
    const uint8_t value = r.readU8();
    if (!r.done()) {
        return false;
    }
    start = value;
    return true;
}

bool decodeClear(const uint8_t *body, size_t len, uint8_t &what)
{
    if (len != kClearRequestSize) {
        return false;
    }
    Reader r(body, len);
    const uint8_t value = r.readU8();
    if (!r.done() || (value & static_cast<uint8_t>(~kClearAllBits)) != 0U) {
        return false;
    }
    what = value;
    return true;
}

size_t encodeInfoBody(const InfoBody &info, uint8_t *out, size_t cap)
{
    size_t version_len = (info.version != nullptr) ? info.version_len : 0U;
    if (version_len > kMaxVersionLen) {
        version_len = kMaxVersionLen;
    }
    Writer w(out, cap);
    w.appendU8(info.flags);
    w.appendU64Le(info.uptime_us);
    w.appendU8(info.reset_reason);
    w.appendU8(info.instances_ok);
    w.appendU8(info.instances_failed);
    w.appendU8(info.safe_mode);
    w.appendU32Le(info.records_dropped);
    w.appendU32Le(info.events_tx_dropped);
    w.appendU8(static_cast<uint8_t>(version_len));
    w.appendBytes(reinterpret_cast<const uint8_t *>(info.version), version_len);
    return finish(w);
}

size_t encodeEntriesBody(uint8_t total, uint8_t next, const EntryState *entries, size_t n,
                         uint8_t *out, size_t cap)
{
    if (n > 255U || (n > 0U && entries == nullptr)) {
        return 0U;
    }
    Writer w(out, cap);
    w.appendU8(total);
    w.appendU8(next);
    w.appendU8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        w.appendU8(entries[i].source);
        w.appendU8(entries[i].instance);
        w.appendU16Le(entries[i].code);
        w.appendU8(entries[i].state);
        w.appendU32Le(entries[i].count);
    }
    return finish(w);
}

size_t encodeQueryResponse(const QueryHeader &header, ResultCode result, const uint8_t *body,
                           size_t body_len, uint8_t *out, size_t cap)
{
    Writer w(out, cap);
    w.appendU8(kProtocolVersion);
    w.appendU8(header.seq);
    w.appendU8(header.subcommand);
    w.appendU8(static_cast<uint8_t>(result));
    w.appendBytes(body, body_len);
    return finish(w);
}

} // namespace diag::protocol
