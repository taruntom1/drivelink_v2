#pragma once

#include <cstring>
#include "drivelink/protocol/defs.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/wire_codec.hpp"

namespace drivelink::protocol::core_wire {
inline constexpr uint8_t kVersion = 4;
inline constexpr uint16_t kEnd = 0xFFFF;
inline constexpr size_t kRequestHeader = 4;
inline constexpr size_t kResponseHeader = 6;
// LIST must fit one whole group: response header, pool/page header, record
// header, then four bytes per Ref. This is tighter than the stream prefix.
inline constexpr size_t kGroupListOverhead = kResponseHeader + 5 + 8;
constexpr uint8_t groupEntryCapacity(size_t payload, uint8_t requested)
{
    const size_t available = payload > kGroupListOverhead ? (payload - kGroupListOverhead) / 4 : 0;
    const size_t bounded = requested < 64 ? requested : 64;
    return static_cast<uint8_t>(bounded < available ? bounded : available);
}
struct Header { uint8_t seq = 0, family = 0xFF, op = 0xFF; };
inline bool decodeHeader(wire::Reader &r, Header &h)
{
    const auto version = r.readU8();
    if (!r.ok() || version != kVersion) { return false; }
    h.seq = r.readU8();
    const auto family = r.readU8();
    const auto op = r.readU8();
    if (!r.ok()) { return false; }
    h.family = family; h.op = op;
    return true;
}
inline void response(wire::Writer &w, Header h, defs::Result result, defs::Detail detail)
{
    w.appendU8(kVersion); w.appendU8(h.seq); w.appendU8(h.family); w.appendU8(h.op);
    w.appendU8(static_cast<uint8_t>(result)); w.appendU8(static_cast<uint8_t>(detail));
}
// Strings are UTF-8 bytes with a u16 byte length, without a terminator.
inline void string(wire::Writer &w, const char *s)
{
    const size_t n = s ? std::strlen(s) : 0;
    w.appendU16Le(static_cast<uint16_t>(n));
    w.appendBytes(reinterpret_cast<const uint8_t *>(s), n);
}
inline void item(wire::Writer &w, const defs::ItemDesc &d)
{
    w.appendU16Le(d.id); w.appendU8(static_cast<uint8_t>(d.kind));
    w.appendU8(static_cast<uint8_t>(d.type)); w.appendU8(d.flags);
    w.appendU8((d.has_min ? 1 : 0) | (d.has_max ? 2 : 0));
    w.appendU32Le(d.min_bits); w.appendU32Le(d.max_bits); w.appendU32Le(d.default_bits);
    string(w,d.name); string(w,d.unit); string(w,d.description); string(w,d.ref);
    w.appendU8(static_cast<uint8_t>(d.runs_in)); w.appendU8(static_cast<uint8_t>(d.blob));
    w.appendU8(static_cast<uint8_t>(d.args.size())); w.appendU8(static_cast<uint8_t>(d.results.size()));
    for (auto args : {d.args, d.results}) {
        for (const auto &a : args) { w.appendU8(static_cast<uint8_t>(a.type)); string(w,a.name); string(w,a.unit); }
    }
}
inline size_t itemSize(const defs::ItemDesc &d)
{
    size_t n = 30;
    for (const char *s : {d.name,d.unit,d.description,d.ref}) { if (s) { n += std::strlen(s); } }
    for (auto args : {d.args,d.results}) { for (const auto &a : args) { n += 5 + std::strlen(a.name) + std::strlen(a.unit); } }
    return n;
}
} // namespace drivelink::protocol::core_wire
