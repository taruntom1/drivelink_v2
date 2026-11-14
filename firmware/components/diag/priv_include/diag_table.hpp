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

    bool latch(size_t slot) {
        if (slot >= N) { return false; }
        m_slots[slot].count.fetch_add(1U, std::memory_order_relaxed);
        return !(m_slots[slot].state.fetch_or(protocol::kStateOccurred | protocol::kStateActive,
            std::memory_order_acq_rel) & protocol::kStateActive);
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

inline constexpr size_t kRuntimeSlots = 254;
inline constexpr size_t kSafetyCodeCount = [] {
    size_t n = 0;
    for (const auto &code : kCodeTable) { if (code.clears && (code.blocks_arm || code.disarms)) { ++n; } }
    return n;
}();
// Sparse instance slots. The base slots retain their generated indexes for
// non-instance reports. Additional identities are claimed atomically once
// and remain stable until reboot, including after Clear.
class SlotTable : public BasicSlotTable<kRuntimeSlots> {
public:
    SlotTable() { for(auto &key:m_keys) { key.store(0); } }
    size_t resolve(uint8_t code,uint8_t instance) {
        if(instance==255) { return code; }
        const uint32_t key=1U+(static_cast<uint32_t>(code)<<8U)+instance;
        for(size_t i=kSlotCount;i<kRuntimeSlots;++i) {
            uint32_t current=m_keys[i].load(std::memory_order_acquire);
            if(current==key) { return i; }
            if(current==0 && m_keys[i].compare_exchange_strong(current,key,std::memory_order_acq_rel)) { return i; }
            if(current==key) { return i; }
        }
        return kRuntimeSlots;
    }
    uint8_t codeSlot(size_t slot) const {
        return slot<kSlotCount?static_cast<uint8_t>(slot):static_cast<uint8_t>((m_keys[slot].load(std::memory_order_acquire)-1U)>>8U);
    }
    uint8_t instance(size_t slot) const {
        return slot<kSlotCount?255:static_cast<uint8_t>(m_keys[slot].load(std::memory_order_acquire)-1U);
    }
    bool allocated(size_t slot) const { return slot<kSlotCount || m_keys[slot].load(std::memory_order_acquire)!=0; }
    void markDirty(size_t slot, uint32_t bits = 1) { if (slot < kRuntimeSlots) { m_dirty[slot].fetch_or(bits, std::memory_order_release); } }
    uint32_t takeDirty(size_t slot) { return m_dirty[slot].exchange(0, std::memory_order_acq_rel); }
    bool activeSafety(bool disarms_only) const {
        size_t index = 0;
        for (const auto &code : kCodeTable) {
            if (!code.clears || !(code.blocks_arm || code.disarms)) { continue; }
            if (code.disarms || (!disarms_only && code.blocks_arm)) {
                for (size_t word = 0; word < 8; ++word) {
                    if (m_overflow[index * 8 + word].load(std::memory_order_acquire)) { return true; }
                }
            }
            ++index;
        }
        for (size_t i = 0; i < kRuntimeSlots; ++i) {
            if (!allocated(i) || !(read(i).state & protocol::kStateActive)) { continue; }
            const auto &code = kCodeTable[codeSlot(i)];
            if (code.disarms || (!disarms_only && code.blocks_arm)) { return true; }
        }
        return false;
    }
    // Report slots are finite, but active safety identities must remain
    // clearable. Each generated safety code gets one bit per instance at
    // setup (32 bytes/code), used only after sparse report slots run out.
    void safetyOverflow(Code code, uint8_t instance, bool active = true) {
        size_t index = 0;
        for (const auto &meta : kCodeTable) {
            if (!meta.clears || !(meta.blocks_arm || meta.disarms)) { continue; }
            if (meta.slot == code.slot) {
                auto &word = m_overflow[index * 8 + instance / 32];
                const uint32_t bit = uint32_t{1} << (instance % 32);
                if (active) { word.fetch_or(bit, std::memory_order_acq_rel); }
                else { word.fetch_and(~bit, std::memory_order_acq_rel); }
                return;
            }
            ++index;
        }
    }
    void resetSafetyOverflow() { for (auto &word : m_overflow) { word.store(0); } }
private:
    std::array<std::atomic<uint32_t>,kRuntimeSlots> m_keys{};
    std::array<std::atomic<uint32_t>,kRuntimeSlots> m_dirty{};
    std::array<std::atomic<uint32_t>, kSafetyCodeCount * 8> m_overflow{};
};

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
                      protocol::EntryState *out, size_t max, uint8_t &total, uint8_t &next, const SlotTable *table = nullptr);

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
                            size_t &cursor, protocol::CounterEntry *out, uint8_t *out_slots, size_t max, const SlotTable *table = nullptr);

} // namespace diag::detail
