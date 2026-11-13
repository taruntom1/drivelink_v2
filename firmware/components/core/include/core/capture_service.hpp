#pragma once

#include <array>
#include <atomic>
#include <span>

#include "core/group_service.hpp"
#include "core_os/event.hpp"

namespace core {

inline constexpr size_t kMaxCaptureRefs = 64;
inline constexpr size_t kCaptureReadBytes = 512; ///< values per READ reply, at most
inline constexpr uint32_t kNoTriggerIndex = 0xFFFFFFFFU;

using CaptureState = defs::CaptureState;
using CaptureTriggerKind = defs::CaptureTrigger;
using CaptureEdge = defs::CaptureEdge;

struct CaptureTrigger {
    CaptureTriggerKind kind = CaptureTriggerKind::Manual;
    Ref ref{};
    CaptureEdge edge = CaptureEdge::Rising;
    uint32_t value = 0; ///< threshold bits, in the trigger item's type
};

/// The configuration as the config caller last applied it.
struct CaptureInfo {
    bool configured = false;
    uint8_t loop = kNoLoop;
    uint16_t divider = 1;
    uint32_t samples = 0;
    uint32_t pre_trigger = 0;
    CaptureTrigger trigger{};
    uint8_t count = 0;
    std::array<Ref, kMaxCaptureRefs> refs{};
};

struct CaptureSnapshot {
    CaptureState state = CaptureState::Idle;
    bool irregular = false;
    uint32_t recorded = 0;                    ///< valid samples in the ring
    uint32_t trigger_index = kNoTriggerIndex; ///< of the trigger sample, once triggered
    uint32_t period_us = 0;                   ///< the loop period at ARM (sample spacing = period × divider)
    int64_t trigger_us = 0;                   ///< tick start of the trigger sample, once triggered
};

/// One capture: up to `refCapacity()` signals or inputs of one loop, recorded
/// into a ring at a divider of its rate. The ring is allocated at CONFIG
/// (PSRAM through core_os) and kept until the next CONFIG.
///
/// Threads: `sample()` runs on the loops (it acts only on the capture's loop).
/// Everything else is the config caller's. The loop records only while
/// Armed or Triggered, under a single-attempt claim, and never waits. The
/// config caller writes the configuration only while the loop leaves it alone
/// (Idle or Done) and publishes it with the Armed store; ABORT takes the claim
/// with a bounded wait. READ is allowed only in Done, when the loop is finished.
class CaptureService {
public:
    CaptureService(const detail::InstanceTable &table, std::span<Loop *> loops, uint8_t max_refs,
                   size_t capacity_bytes);
    ~CaptureService();
    CaptureService(const CaptureService &) = delete;
    CaptureService &operator=(const CaptureService &) = delete;

    uint8_t refCapacity() const { return m_max_refs; }
    size_t capacityBytes() const { return m_capacity; }
    const CaptureInfo &info() const { return m_info; }

    /// Refused while Armed or Triggered. Frees the old ring, then allocates
    /// the new one; on NoMemory the capture is left unconfigured.
    Status configure(uint8_t loop, uint16_t divider, uint32_t samples, uint32_t pre_trigger,
                     const CaptureTrigger &trigger, std::span<const Ref> refs);
    Status arm();     ///< configured, Idle or Done
    Status trigger(); ///< Armed only: the next sample is the trigger
    Status abort();   ///< any state to Idle; keeps the configuration
    CaptureSnapshot snapshot() const;
    /// Done only: samples [first, first + count) (oldest first), n values each.
    Status read(uint32_t first, uint32_t count, std::span<uint32_t> out) const;
    /// True once per completed capture (the CaptureDone event).
    bool takeDone(CaptureSnapshot &out);

    /// Loop step 8: record one tick of `loop`. `period_us` is its current period.
    void sample(uint8_t loop, int64_t now_us, uint32_t period_us);

private:
    bool claim() { return !m_owned.test_and_set(std::memory_order_acquire); }
    void release() { m_owned.clear(std::memory_order_release); }
    bool claimWait();
    void freeRing();
    void record(CaptureState state, int64_t now_us, uint32_t period_us);
    bool fires(uint32_t previous, uint32_t current) const;

    const detail::InstanceTable &m_table;
    std::span<Loop *> m_loops;
    const uint8_t m_max_refs;
    const size_t m_capacity;
    CaptureInfo m_info;            // config caller only
    core_os::Event m_wait;         // a bounded sleep for claimWait, never signalled

    // Written by the config caller while the loop leaves them alone, then
    // published by the Armed store.
    std::atomic<uint8_t> m_loop{kNoLoop};
    std::array<BoundSignal, kMaxCaptureRefs> m_bindings{};
    BoundSignal m_trigger_binding{};
    CaptureTrigger m_trigger{};
    uint32_t *m_ring = nullptr;
    uint32_t m_samples = 0, m_pre = 0, m_post = 0;
    uint16_t m_divider = 1;
    uint8_t m_count = 0;
    uint32_t m_armed_period_us = 0;

    std::atomic_flag m_owned = ATOMIC_FLAG_INIT;
    std::atomic<uint8_t> m_state{static_cast<uint8_t>(CaptureState::Idle)};
    std::atomic<bool> m_force{false}, m_irregular{false}, m_done_pending{false};
    std::atomic<uint32_t> m_total{0};       ///< samples written since ARM
    std::atomic<uint32_t> m_trigger_total{0}; ///< m_total including the trigger sample
    // Loop-owned while recording.
    int64_t m_trigger_us = 0; ///< written before the Triggered store
    int64_t m_prev_tick_us = 0;
    bool m_have_prev_tick = false, m_have_prev_value = false;
    uint32_t m_prev_value = 0, m_head = 0, m_remaining = 0;
    uint16_t m_phase = 0;
};

} // namespace core
