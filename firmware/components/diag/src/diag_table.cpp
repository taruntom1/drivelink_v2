#include "diag_table.hpp"

namespace diag::detail {

size_t collectEntries(const Code *codes, const SlotSnapshot *snaps, size_t n, uint8_t start,
                      protocol::EntryState *out, size_t max, uint8_t &total, uint8_t &next)
{
    size_t listed = 0;
    size_t written = 0;
    next = protocol::kNoNextPage;

    for (size_t slot = 0; slot < n; ++slot) {
        if (!isListed(snaps[slot])) {
            continue;
        }
        ++listed;
        if (slot < start) {
            continue;
        }
        if (written < max) {
            protocol::EntryState &e = out[written++];
            e.source = static_cast<uint8_t>(codes[slot].source);
            e.code = codes[slot].code;
            e.state = snaps[slot].state;
            e.count = snaps[slot].count;
        } else if (next == protocol::kNoNextPage) {
            next = static_cast<uint8_t>(slot);
        }
    }

    total = static_cast<uint8_t>((listed > 255U) ? 255U : listed);
    return written;
}

size_t collectCounterDeltas(const Code *codes, const uint32_t *now, const uint32_t *last, size_t n,
                            size_t &cursor, protocol::CounterEntry *out, uint8_t *out_slots, size_t max)
{
    size_t written = 0;
    while (cursor < n && written < max) {
        const size_t slot = cursor++;
        if (codes[slot].kind != CodeKind::Counter || now[slot] == last[slot]) {
            continue;
        }
        protocol::CounterEntry &e = out[written];
        e.source = static_cast<uint8_t>(codes[slot].source);
        e.code = codes[slot].code;
        e.delta = now[slot] - last[slot];
        e.total = now[slot];
        out_slots[written] = static_cast<uint8_t>(slot);
        ++written;
    }
    return written;
}

} // namespace diag::detail
