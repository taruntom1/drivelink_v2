#include "core/capture_service.hpp"

#include <algorithm>
#include <cstring>

#include "core/loop.hpp"
#include "core_os/capture_memory.hpp"

namespace core {
namespace {
constexpr int kClaimSpins = 256;    // the loop holds the claim for one sample (µs)
constexpr int kClaimSleeps = 10;    // then sleep: a lower-priority loop on this core
constexpr uint32_t kClaimWaitUs = 1000;
Status bad(Detail detail) { return Status::fail(Result::InvalidArgument, detail); }
Status wrongState() { return bad(Detail::WrongState); }
bool active(CaptureState s) { return s == CaptureState::Armed || s == CaptureState::Triggered; }
bool sameRef(const Ref &a, const Ref &b) { return a.type == b.type && a.instance == b.instance && a.id == b.id; }
bool isZero(const Ref &r) { return r.type == 0 && r.instance == 0 && r.id == 0; }
template <typename T> T as(uint32_t bits)
{
    T v{};
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}
template <typename T> bool crosses(T previous, T current, T level, CaptureEdge edge)
{
    return edge == CaptureEdge::Rising ? (previous < level && current >= level)
                                       : (previous > level && current <= level);
}
} // namespace

CaptureService::CaptureService(const detail::InstanceTable &table, std::span<Loop *> loops, uint8_t max_refs,
                               size_t capacity_bytes)
    : m_table(table), m_loops(loops),
      m_max_refs(std::clamp<uint8_t>(max_refs, 1, static_cast<uint8_t>(kMaxCaptureRefs))),
      m_capacity(capacity_bytes)
{
}

CaptureService::~CaptureService() { freeRing(); }

void CaptureService::freeRing()
{
    core_os::captureFree(m_ring);
    m_ring = nullptr;
}

bool CaptureService::claimWait()
{
    // The loop holds the claim only while it records one sample. Spin first;
    // sleep only if it was preempted while holding it (a lower-priority loop
    // on this core), which costs whole FreeRTOS ticks.
    for (int i = 0; i < kClaimSpins; ++i) {
        if (claim()) { return true; }
    }
    for (int i = 0; i < kClaimSleeps; ++i) {
        (void)m_wait.wait(kClaimWaitUs);
        if (claim()) { return true; }
    }
    return false;
}

Status CaptureService::configure(uint8_t loop, uint16_t divider, uint32_t samples, uint32_t pre_trigger,
                                 const CaptureTrigger &trigger, std::span<const Ref> refs)
{
    // Refuse an active capture without touching the claim, so a refused
    // request never takes a tick from a recording loop. Only this caller
    // enters Armed, so a recording capture cannot start in between.
    if (active(static_cast<CaptureState>(m_state.load(std::memory_order_acquire)))) { return wrongState(); }
    if (!claimWait()) { return Status::fail(Result::Busy, Detail::StagingBusy); }
    struct Release { CaptureService *s; ~Release() { s->release(); } } held{this};
    if (active(static_cast<CaptureState>(m_state.load(std::memory_order_acquire)))) { return wrongState(); }
    const Loop *target = nullptr;
    for (auto *l : m_loops) { if (l && l->id() == loop) { target = l; } }
    if (!target) { return bad(Detail::LoopMismatch); }
    if (!divider || !samples || pre_trigger >= samples || refs.empty()) { return bad(Detail::BadValue); }
    if (refs.size() > m_max_refs) { return bad(Detail::BatchTooLarge); }

    // Trigger. testgen_start arrives with the test signal generator (F7b).
    BoundSignal trigger_binding{};
    switch (trigger.kind) {
    case CaptureTriggerKind::Manual:
        if (!isZero(trigger.ref) || trigger.edge != CaptureEdge::Rising || trigger.value) { return bad(Detail::BadValue); }
        break;
    case CaptureTriggerKind::TestgenStart: return bad(Detail::Unsupported);
    case CaptureTriggerKind::ValueChange:
    case CaptureTriggerKind::Threshold: {
        const bool threshold = trigger.kind == CaptureTriggerKind::Threshold;
        if (!threshold && (trigger.edge != CaptureEdge::Rising || trigger.value)) { return bad(Detail::BadValue); }
        if (trigger.edge != CaptureEdge::Rising && trigger.edge != CaptureEdge::Falling) { return bad(Detail::BadValue); }
        const auto st = bindSignal(m_table, trigger.ref, loop, false, trigger_binding);
        if (!st.ok()) { return st; }
        if (threshold) {
            // Validate the encoding only; a level beyond the item's limits is kept.
            const auto check = checkValue(*trigger_binding.item, trigger.value);
            if (!check.ok) { return bad(check.detail); }
        }
        break;
    }
    default: return bad(Detail::BadValue);
    }

    // Validate every reference before anything changes.
    for (size_t i = 0; i < refs.size(); ++i) {
        BoundSignal probe{};
        auto st = bindSignal(m_table, refs[i], loop, false, probe);
        if (!st.ok()) { st.index = static_cast<uint16_t>(i); return st; }
        for (size_t j = 0; j < i; ++j) {
            if (sameRef(refs[i], refs[j])) { return Status::fail(Result::InvalidArgument, Detail::DuplicateRef, static_cast<uint16_t>(i)); }
        }
    }
    const uint64_t bytes = static_cast<uint64_t>(samples) * refs.size() * sizeof(uint32_t);
    if (bytes > m_capacity) { return bad(Detail::Capacity); }

    // The loop leaves everything below alone in Idle and Done.
    m_info = {};
    m_count = 0;
    m_state.store(static_cast<uint8_t>(CaptureState::Idle), std::memory_order_release);
    m_done_pending.store(false, std::memory_order_relaxed);
    freeRing();
    m_ring = static_cast<uint32_t *>(core_os::captureAlloc(static_cast<size_t>(bytes)));
    if (!m_ring) { return Status::fail(Result::NoMemory, Detail::None); }
    for (size_t i = 0; i < refs.size(); ++i) {
        (void)bindSignal(m_table, refs[i], loop, false, m_bindings[i]);
        m_info.refs[i] = refs[i];
    }
    m_loop.store(loop, std::memory_order_relaxed);
    m_trigger = trigger;
    m_trigger_binding = trigger_binding;
    m_samples = samples;
    m_pre = pre_trigger;
    m_post = samples - pre_trigger;
    m_divider = divider;
    m_count = static_cast<uint8_t>(refs.size());
    m_total.store(0, std::memory_order_relaxed);
    m_info.configured = true;
    m_info.loop = loop;
    m_info.divider = divider;
    m_info.samples = samples;
    m_info.pre_trigger = pre_trigger;
    m_info.trigger = trigger;
    m_info.count = m_count;
    return {};
}

Status CaptureService::arm()
{
    if (!m_info.configured || active(static_cast<CaptureState>(m_state.load(std::memory_order_acquire)))) {
        return wrongState();
    }
    const Loop *target = nullptr;
    for (auto *l : m_loops) { if (l && l->id() == m_info.loop) { target = l; } }
    if (!target) { return bad(Detail::LoopMismatch); }
    // The claim orders this reset after a final record() that stored Done and
    // its pending event; the loop never claims in Idle or Done.
    if (!claimWait()) { return Status::fail(Result::Busy, Detail::StagingBusy); }
    m_armed_period_us = target->periodUs();
    m_head = 0;
    m_phase = static_cast<uint16_t>(m_divider - 1U); // the first tick records
    m_remaining = 0;
    m_have_prev_tick = m_have_prev_value = false;
    m_prev_value = 0;
    m_prev_tick_us = m_trigger_us = 0;
    m_total.store(0, std::memory_order_relaxed);
    m_trigger_total.store(0, std::memory_order_relaxed);
    m_irregular.store(false, std::memory_order_relaxed);
    m_force.store(false, std::memory_order_relaxed);
    m_done_pending.store(false, std::memory_order_relaxed);
    release();
    // Published after the claim is free, so the first tick can record.
    m_state.store(static_cast<uint8_t>(CaptureState::Armed), std::memory_order_release);
    return {};
}

Status CaptureService::trigger()
{
    if (static_cast<CaptureState>(m_state.load(std::memory_order_acquire)) != CaptureState::Armed) { return wrongState(); }
    m_force.store(true, std::memory_order_release);
    return {};
}

Status CaptureService::abort()
{
    if (!claimWait()) { return Status::fail(Result::Busy, Detail::StagingBusy); }
    m_state.store(static_cast<uint8_t>(CaptureState::Idle), std::memory_order_release);
    m_done_pending.store(false, std::memory_order_relaxed);
    m_force.store(false, std::memory_order_relaxed);
    release();
    return {};
}

CaptureSnapshot CaptureService::snapshot() const
{
    CaptureSnapshot s;
    s.state = static_cast<CaptureState>(m_state.load(std::memory_order_acquire));
    if (s.state == CaptureState::Idle) { return s; }
    s.irregular = m_irregular.load(std::memory_order_relaxed);
    s.period_us = m_armed_period_us;
    const uint32_t total = m_total.load(std::memory_order_relaxed);
    s.recorded = std::min(total, m_samples);
    if (s.state == CaptureState::Triggered || s.state == CaptureState::Done) {
        // Written before the Triggered store, never again until the next ARM.
        const uint32_t trigger_total = m_trigger_total.load(std::memory_order_relaxed);
        if (s.state == CaptureState::Done || total == UINT32_MAX) {
            s.trigger_index = std::min(trigger_total - 1U, m_pre); // exact once Done
        } else {
            // While Triggered: the trigger's current index among the newest samples.
            const uint32_t since = total - trigger_total;
            s.trigger_index = s.recorded > since ? s.recorded - 1U - since : 0U;
        }
        s.trigger_us = m_trigger_us;
    }
    return s;
}

Status CaptureService::read(uint32_t first, uint32_t count, std::span<uint32_t> out) const
{
    if (static_cast<CaptureState>(m_state.load(std::memory_order_acquire)) != CaptureState::Done) { return wrongState(); }
    const uint32_t total = m_total.load(std::memory_order_relaxed);
    const uint32_t recorded = std::min(total, m_samples);
    if (first > recorded || count > recorded - first) { return bad(Detail::BadValue); }
    if (static_cast<size_t>(count) * m_count > out.size()) { return bad(Detail::BufferTooSmall); }
    const uint32_t oldest = total >= m_samples ? m_head : 0U;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = static_cast<uint32_t>((static_cast<uint64_t>(oldest) + first + i) % m_samples);
        std::copy_n(m_ring + static_cast<size_t>(slot) * m_count, m_count, out.data() + static_cast<size_t>(i) * m_count);
    }
    return {};
}

bool CaptureService::takeDone(CaptureSnapshot &out)
{
    if (!m_done_pending.exchange(false, std::memory_order_acq_rel)) { return false; }
    out = snapshot();
    return out.state == CaptureState::Done;
}

void CaptureService::sample(uint8_t loop, int64_t now_us, uint32_t period_us)
{
    const auto state = static_cast<CaptureState>(m_state.load(std::memory_order_acquire));
    if (!active(state) || m_loop.load(std::memory_order_relaxed) != loop) { return; }
    if (!claim()) {
        // Only ABORT contends with a recording loop; the tick is lost.
        m_irregular.store(true, std::memory_order_relaxed);
        return;
    }
    const auto held = static_cast<CaptureState>(m_state.load(std::memory_order_acquire));
    if (active(held) && m_loop.load(std::memory_order_relaxed) == loop) { record(held, now_us, period_us); }
    release();
}

bool CaptureService::fires(uint32_t previous, uint32_t current) const
{
    if (m_trigger.kind == CaptureTriggerKind::ValueChange) { return previous != current; }
    const auto edge = m_trigger.edge;
    switch (m_trigger_binding.item->type) {
    case defs::ValueType::F32: return crosses(as<float>(previous), as<float>(current), as<float>(m_trigger.value), edge);
    case defs::ValueType::I32: return crosses(as<int32_t>(previous), as<int32_t>(current), as<int32_t>(m_trigger.value), edge);
    default: return crosses(previous, current, m_trigger.value, edge);
    }
}

void CaptureService::record(CaptureState state, int64_t now_us, uint32_t period_us)
{
    // A tick more than 1.5 periods after the previous one (an overrun, a
    // paused loop, a late wake) or a rate change breaks the even spacing.
    if ((m_have_prev_tick && (now_us - m_prev_tick_us) * 2 > static_cast<int64_t>(period_us) * 3) ||
        period_us != m_armed_period_us) {
        m_irregular.store(true, std::memory_order_relaxed);
    }
    m_prev_tick_us = now_us;
    m_have_prev_tick = true;
    if (++m_phase < m_divider) { return; }
    m_phase = 0;

    latchSignals({m_bindings.data(), m_count}, {m_ring + static_cast<size_t>(m_head) * m_count, m_count});
    m_head = (m_head + 1U == m_samples) ? 0U : m_head + 1U;
    uint32_t total = m_total.load(std::memory_order_relaxed);
    if (total != UINT32_MAX) { ++total; }
    m_total.store(total, std::memory_order_relaxed);

    if (state == CaptureState::Armed) {
        bool fire = m_force.exchange(false, std::memory_order_acq_rel);
        if (m_trigger.kind == CaptureTriggerKind::ValueChange || m_trigger.kind == CaptureTriggerKind::Threshold) {
            const uint32_t value = loadValue(m_trigger_binding.item->type, m_trigger_binding.storage);
            fire = fire || (m_have_prev_value && fires(m_prev_value, value));
            m_prev_value = value;
            m_have_prev_value = true;
        }
        if (fire) {
            m_trigger_us = now_us;
            m_trigger_total.store(total, std::memory_order_relaxed);
            m_remaining = m_post;
            state = CaptureState::Triggered;
            m_state.store(static_cast<uint8_t>(state), std::memory_order_release);
        }
    }
    if (state == CaptureState::Triggered && --m_remaining == 0U) {
        m_state.store(static_cast<uint8_t>(CaptureState::Done), std::memory_order_release);
        m_done_pending.store(true, std::memory_order_release); // after Done: takeDone sees it
    }
}

} // namespace core
