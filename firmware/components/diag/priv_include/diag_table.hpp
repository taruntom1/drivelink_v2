#pragma once

/// Per-code diag state and the selection logic for queries and counter
/// deltas. Pure C++ (std::atomic only), so the host unit tests cover it.

#include "diag_codes.hpp"
#include "diag_protocol.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace diag::detail {

/// One slot as read at one instant.
struct SlotSnapshot {
    uint32_t count = 0;
    uint8_t state = 0; ///< protocol::kStateOccurred | protocol::kStateActive
};

/// Lock-free state for N codes: a count and the occurred / active bits.
///
/// Every method only does relaxed atomic operations on 32-bit words, so it
/// is safe from any context, including ISRs and critical sections, and
/// concurrently with every other method. An out-of-range slot is ignored.
/// The count wraps at 2^32; deltas use unsigned subtraction.
template <size_t N>
class BasicSlotTable {
public:
    /// count += n and set occurred (no-op for n == 0). For counter codes.
    void add(size_t slot, uint32_t n)
    {
        if (slot >= N || n == 0U) {
            return;
        }
        m_slots[slot].count.fetch_add(n, std::memory_order_relaxed);
        m_slots[slot].state.fetch_or(protocol::kStateOccurred, std::memory_order_relaxed);
    }

    /// count += 1 and set occurred, plus active when the code `clears`.
    void raise(size_t slot, bool clears)
    {
        if (slot >= N) {
            return;
        }
        const uint32_t bits = protocol::kStateOccurred | (clears ? protocol::kStateActive : 0U);
        m_slots[slot].count.fetch_add(1U, std::memory_order_relaxed);
        m_slots[slot].state.fetch_or(bits, std::memory_order_relaxed);
    }

    /// Clear the active bit. @return true if it was set.
    bool clearActive(size_t slot)
    {
        if (slot >= N) {
            return false;
        }
        const uint32_t prev = m_slots[slot].state.fetch_and(~static_cast<uint32_t>(protocol::kStateActive),
                                                            std::memory_order_relaxed);
        return (prev & protocol::kStateActive) != 0U;
    }

    SlotSnapshot read(size_t slot) const
    {
        if (slot >= N) {
            return {};
        }
        SlotSnapshot s;
        s.count = m_slots[slot].count.load(std::memory_order_relaxed);
        s.state = static_cast<uint8_t>(m_slots[slot].state.load(std::memory_order_relaxed));
        return s;
    }

    /// Clear every occurred bit. Active bits stay: the condition is still there.
    void clearOccurred()
    {
        for (Slot &s : m_slots) {
            s.state.fetch_and(~static_cast<uint32_t>(protocol::kStateOccurred), std::memory_order_relaxed);
        }
    }

    /// Zero every count.
    void resetCounts()
    {
        for (Slot &s : m_slots) {
            s.count.store(0U, std::memory_order_relaxed);
        }
    }

    static constexpr size_t size() { return N; }

private:
    struct Slot {
        std::atomic<uint32_t> count{0};
        std::atomic<uint32_t> state{0};
    };

    std::array<Slot, N> m_slots{};
};

using SlotTable = BasicSlotTable<kSlotCount>;

/// True if a slot is listed by GetEntries: it has a count or a state bit.
constexpr bool isListed(const SlotSnapshot &s)
{
    return s.count != 0U || s.state != 0U;
}

/// Fill one GetEntries page.
///
/// @param codes, snaps  `n` elements each, indexed by slot.
/// @param start         First slot to consider.
/// @param out, max      Room for the page entries.
/// @param[out] total    Listed slots in the whole table (saturates at 255).
/// @param[out] next     Slot to pass as `start` for the next page, or
///                      protocol::kNoNextPage when no listed slot remains.
/// @return Entries written.
size_t collectEntries(const Code *codes, const SlotSnapshot *snaps, size_t n, uint8_t start,
                      protocol::EntryState *out, size_t max, uint8_t &total, uint8_t &next);

/// Pick counter slots whose count changed since the last send.
///
/// Scans counter-kind slots from `cursor` and writes one entry per slot with
/// now != last (delta = now - last, total = now), up to `max` entries.
/// `out_slots[i]` is the slot of `out[i]`, so the caller can commit
/// last = now for the entries it actually sent.
///
/// @param[in,out] cursor  Slot to resume from; set to n when the scan is done.
/// @return Entries written (0 when nothing changed from `cursor` on).
size_t collectCounterDeltas(const Code *codes, const uint32_t *now, const uint32_t *last, size_t n,
                            size_t &cursor, protocol::CounterEntry *out, uint8_t *out_slots, size_t max);

} // namespace diag::detail
