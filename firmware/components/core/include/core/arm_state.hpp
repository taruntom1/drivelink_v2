#pragma once
#include <atomic>
#include <cstdint>

namespace core {
enum class ArmStatus : uint8_t { Disarmed, Armed, Fault };
enum class DisarmReason : uint8_t { Boot, Explicit, Watchdog, Fault, LoopStalled, Shutdown };

// One lock-free word is the ARM commit point. Disarm never waits for a
// config caller; even a disarm while already disarmed invalidates its ticket.
class ArmState {
public:
    static constexpr uint32_t kArmed = 1, kFault = 2, kStopping = 4, kEpoch = 8;
    std::atomic<uint32_t> word{0};
    uint32_t ticket() const { return word.load(std::memory_order_acquire); }
    ArmStatus status() const {
        const auto w = ticket();
        return (w & kFault) ? ArmStatus::Fault : (w & kArmed) ? ArmStatus::Armed : ArmStatus::Disarmed;
    }
    bool commit(uint32_t expected, int64_t now) {
        if (expected & (kFault | kStopping | kArmed)) { return false; }
        m_arm_low.store(static_cast<uint32_t>(now), std::memory_order_relaxed);
        m_arm_high.store(static_cast<uint32_t>(static_cast<uint64_t>(now) >> 32U), std::memory_order_relaxed);
        return word.compare_exchange_strong(expected, expected | kArmed, std::memory_order_acq_rel);
    }
    bool snapshot(uint32_t &w, int64_t &armed_at) const {
        w = ticket();
        const uint64_t low = m_arm_low.load(std::memory_order_relaxed);
        const uint64_t high = m_arm_high.load(std::memory_order_relaxed);
        armed_at = static_cast<int64_t>((high << 32U) | low);
        return w == ticket();
    }
    void disarm(DisarmReason reason = DisarmReason::Explicit, bool latch = false) {
        // Invalidate a concurrent ACK/ARM before publishing the fault bit.
        // An ACK that wins after this increment but before fetch_or is
        // followed by the new fault latch, rather than erasing that fault.
        word.fetch_add(kEpoch, std::memory_order_acq_rel);
        if (latch) { word.fetch_or(kFault, std::memory_order_acq_rel); }
        // The reason is advisory; the safety state and epoch are authoritative.
        m_reason.store(static_cast<uint32_t>(reason), std::memory_order_relaxed);
        word.fetch_and(~kArmed, std::memory_order_acq_rel);
    }
    bool acknowledge(uint32_t expected) {
        return !(expected & (kArmed | kStopping)) &&
            word.compare_exchange_strong(expected, (expected + kEpoch) & ~kFault, std::memory_order_acq_rel);
    }
    DisarmReason reason() const { return static_cast<DisarmReason>(m_reason.load(std::memory_order_relaxed)); }
private:
    std::atomic<uint32_t> m_arm_low{0}, m_arm_high{0}, m_reason{0};
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
} // namespace core
