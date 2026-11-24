#include "core/testgen_service.hpp"
#include "core/capture_service.hpp"
#include "core/loop.hpp"
#include "core_os/clock.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace core {
namespace {
constexpr double kTau = 6.2831853071795864769;
enum class Swap : uint8_t { Idle, Ready, Applying, Applied };
static_assert(core_os::lockFreeAtomic<Swap>() && core_os::lockFreeAtomic<bool>());
Status bad(Detail d = Detail::BadValue) { return Status::fail(Result::InvalidArgument, d); }
Status busy() { return Status::fail(Result::Busy, Detail::StagingBusy); }
bool armed(uint32_t w) { return (w & (ArmState::kArmed | ArmState::kFault | ArmState::kStopping)) == ArmState::kArmed; }
struct Slot {
    std::atomic<Swap> swap{Swap::Idle};
    core_os::Event applied;
    TestgenConfig staged{}, shadow{}, active{};
    BoundSignal staged_binding{}, binding{};
    uint32_t staged_arm = 0, staged_run = 0, next_run = 0;
    bool stopping = false;
    Status result{}; // loop publishes through Applied
    // Loop-owned active state.
    bool generating = false, first = false;
    uint32_t arm_word = 0, run = 0, repetitions = 0, elapsed_ms = 0, value = 0;
    int64_t started_us = 0;
    TestgenState state = TestgenState::Idle;
    TestgenReason reason = TestgenReason::None;
    // Every field is atomic: readers retry a bounded sequence snapshot.
    std::atomic<uint32_t> sequence{0}, pub_state{0}, pub_reason{0}, pub_run{0}, pub_arm{0},
        pub_elapsed{0}, pub_repetitions{0}, pub_value{0};
    std::atomic<bool> pending{false};
    TestgenSnapshot terminal{}; // immutable from pending release until config consumes it
    void publish() {
        sequence.fetch_add(1);
        pub_state.store(static_cast<uint32_t>(state)); pub_reason.store(static_cast<uint32_t>(reason));
        pub_run.store(run); pub_arm.store(arm_word); pub_elapsed.store(elapsed_ms);
        pub_repetitions.store(repetitions); pub_value.store(value);
        sequence.fetch_add(1);
    }
    void finish(TestgenReason why, int64_t now) {
        generating = false; state = TestgenState::Done; reason = why;
        binding.gate->testgen_active = false;
        binding.gate->command_after_us = why == TestgenReason::Disarmed ? 0 : std::max(now, core_os::nowUs());
        terminal = {state, reason, run, elapsed_ms, repetitions, value};
        // Fence restart before publishing Done. Otherwise START could slip
        // between the Done store and the event flag and overwrite its result.
        pending.store(true, std::memory_order_release);
        publish();
    }
};
}

double testgenValue(const TestgenConfig &c, double elapsed_s)
{
    const double duration = static_cast<double>(c.duration_ms) * .001;
    const double t = std::clamp(elapsed_s, 0.0, duration);
    const double f0 = c.frequency_start, f1 = c.frequency_end;
    double shape = 1;
    switch (c.waveform) {
    case TestgenWaveform::Step: break;
    case TestgenWaveform::Ramp: shape = t / duration; break;
    case TestgenWaveform::Square: shape = std::fmod(t * f0, 1.0) < .5 ? 1 : -1; break;
    case TestgenWaveform::Sine: shape = std::sin(kTau * std::fmod(t * f0, 1.0)); break;
    case TestgenWaveform::ChirpLinear:
        shape = std::sin(kTau * std::fmod(f0 * t + .5 * (f1 - f0) * t * t / duration, 1.0)); break;
    case TestgenWaveform::ChirpLog: {
        const double log_ratio = std::log(f1 / f0);
        const double cycles = std::abs(log_ratio) < 1e-12 ? f0 * t :
            f0 * duration * std::expm1(log_ratio * t / duration) / log_ratio;
        shape = std::sin(kTau * std::fmod(cycles, 1.0)); break;
    }
    }
    return static_cast<double>(c.offset) + static_cast<double>(c.amplitude) * shape;
}

struct TestgenService::Impl {
    const detail::InstanceTable &table;
    std::span<Loop *> loops;
    ArmState &arm;
    CaptureService &capture;
    std::unique_ptr<Slot[]> slots;
    Impl(const detail::InstanceTable &t, std::span<Loop *> l, ArmState &a, CaptureService &c)
        : table(t), loops(l), arm(a), capture(c), slots(std::make_unique<Slot[]>(l.size())) {}
    size_t index(uint8_t id) const {
        for (size_t i = 0; i < loops.size(); ++i) { if (loops[i] && loops[i]->id() == id) { return i; } }
        return loops.size();
    }
    Slot *slot(uint8_t id) const { const auto i = index(id); return i == loops.size() ? nullptr : &slots[i]; }
};
TestgenService::TestgenService(const detail::InstanceTable &t, std::span<Loop *> l, ArmState &a, CaptureService &c)
    : m_impl(std::make_unique<Impl>(t, l, a, c)) {}
TestgenService::~TestgenService() = default;
size_t TestgenService::slotBytes() { return sizeof(Slot); }

Status TestgenService::stageStart(const TestgenConfig &c, uint32_t &run)
{
    auto &d = *m_impl;
    const auto index = d.index(c.loop);
    if (index == d.loops.size()) { return bad(Detail::LoopMismatch); }
    auto &s = d.slots[index];
    if (s.swap.load(std::memory_order_acquire) != Swap::Idle) { return busy(); }
    if (s.pub_state.load() == static_cast<uint32_t>(TestgenState::Running) || s.pending.load(std::memory_order_acquire)) {
        return bad(Detail::WrongState);
    }
    BoundSignal binding;
    auto st = bindSignal(d.table, c.target, c.loop, true, binding);
    if (!st.ok()) { return st; }
    if (!defs::hasFlag(*binding.item, defs::ItemFlag::Testgen) || binding.item->type != defs::ValueType::F32) {
        return bad(Detail::Unsupported);
    }
    if (static_cast<uint8_t>(c.waveform) > static_cast<uint8_t>(TestgenWaveform::ChirpLog) || !c.duration_ms ||
        !std::isfinite(c.offset) || !std::isfinite(c.amplitude) || !std::isfinite(c.frequency_start) ||
        !std::isfinite(c.frequency_end)) { return bad(); }
    const bool periodic = c.waveform == TestgenWaveform::Square || c.waveform >= TestgenWaveform::Sine;
    const bool chirp = c.waveform >= TestgenWaveform::ChirpLinear;
    const double nyquist = 500000.0 / d.loops[index]->periodUs();
    if ((periodic ? !(c.frequency_start > 0 && c.frequency_start < nyquist) : c.frequency_start != 0) ||
        (chirp ? !(c.frequency_end > 0 && c.frequency_end < nyquist) : c.frequency_end != 0)) { return bad(); }
    const auto word = d.arm.ticket();
    if (!armed(word)) { return Status::fail(Result::NotArmed, Detail::WrongState); }
    s.staged = c; s.staged_binding = binding; s.staged_arm = word;
    s.staged_run = ++s.next_run; s.stopping = false; s.result = {};
    s.applied.clear(); s.swap.store(Swap::Ready, std::memory_order_release);
    run = s.staged_run;
    return {};
}
Status TestgenService::stageStop(uint8_t loop)
{
    auto *s = m_impl->slot(loop);
    if (!s) { return bad(Detail::LoopMismatch); }
    if (s->swap.load(std::memory_order_acquire) != Swap::Idle) { return busy(); }
    s->stopping = true; s->result = {};
    s->applied.clear(); s->swap.store(Swap::Ready, std::memory_order_release);
    return {};
}
Status TestgenService::complete(uint8_t loop, uint32_t timeout_us)
{
    auto *s = m_impl->slot(loop);
    if (!s) { return bad(Detail::LoopMismatch); }
    if (s->swap.load(std::memory_order_acquire) == Swap::Idle) { return busy(); }
    const auto deadline = core_os::nowUs() + timeout_us;
    while (s->swap.load(std::memory_order_acquire) != Swap::Applied) {
        const auto now = core_os::nowUs();
        if (now >= deadline) {
            auto expected = Swap::Ready;
            if (s->swap.compare_exchange_strong(expected, Swap::Idle, std::memory_order_acq_rel)) {
                return Status::fail(Result::Busy, Detail::Withdrawn);
            }
        }
        (void)s->applied.wait(now < deadline ? static_cast<uint32_t>(deadline - now) : 1000U);
    }
    const auto result = s->result;
    if (result.ok() && !s->stopping) { s->shadow = s->staged; }
    s->swap.store(Swap::Idle, std::memory_order_release);
    return result;
}
Status TestgenService::start(const TestgenConfig &config, uint32_t &run)
{
    auto st = stageStart(config, run);
    if (!st.ok()) { return st; }
    return complete(config.loop, ParamService::stagingTimeoutUs(m_impl->loops[m_impl->index(config.loop)]->periodUs()));
}
Status TestgenService::stop(uint8_t loop)
{
    auto st = stageStop(loop);
    if (!st.ok()) { return st; }
    return complete(loop, ParamService::stagingTimeoutUs(m_impl->loops[m_impl->index(loop)]->periodUs()));
}
const TestgenConfig *TestgenService::config(uint8_t loop) const
{ const auto *s = m_impl->slot(loop); return s ? &s->shadow : nullptr; }
Status TestgenService::snapshot(uint8_t loop, TestgenSnapshot &out) const
{
    const auto *s = m_impl->slot(loop);
    if (!s) { return bad(Detail::LoopMismatch); }
    for (unsigned i = 0; i < 3; ++i) {
        const auto seq = s->sequence.load();
        if (seq & 1U) { continue; }
        TestgenSnapshot snap{static_cast<TestgenState>(s->pub_state.load()), static_cast<TestgenReason>(s->pub_reason.load()),
            s->pub_run.load(), s->pub_elapsed.load(), s->pub_repetitions.load(), s->pub_value.load()};
        const auto epoch = s->pub_arm.load();
        if (seq != s->sequence.load()) { continue; }
        if (snap.state == TestgenState::Running && epoch != m_impl->arm.ticket()) {
            snap.state = TestgenState::Done; snap.reason = TestgenReason::Disarmed;
        }
        out = snap; return {};
    }
    return busy();
}
bool TestgenService::running() const
{
    const auto word = m_impl->arm.ticket();
    if (!armed(word)) { return false; }
    for (size_t i = 0; i < m_impl->loops.size(); ++i) {
        const auto &s = m_impl->slots[i];
        if (s.pub_state.load() == static_cast<uint32_t>(TestgenState::Running) && s.pub_arm.load() == word) { return true; }
    }
    return false;
}
bool TestgenService::takeDone(uint8_t &loop, TestgenSnapshot &out)
{
    for (size_t i = 0; i < m_impl->loops.size(); ++i) {
        auto &s = m_impl->slots[i];
        if (!s.pending.load(std::memory_order_acquire)) { continue; }
        out = s.terminal;
        loop = m_impl->loops[i]->id();
        s.pending.store(false, std::memory_order_release); return true;
    }
    return false;
}
void TestgenService::beginTick(uint8_t loop, int64_t now, uint32_t arm_word)
{
    auto &s = *m_impl->slot(loop);
    if (s.generating && (arm_word != s.arm_word || !armed(m_impl->arm.ticket()))) { s.finish(TestgenReason::Disarmed, now); }
    auto expected = Swap::Ready;
    if (s.swap.compare_exchange_strong(expected, Swap::Applying, std::memory_order_acq_rel)) {
        if (s.stopping) {
            if (s.generating) { s.finish(TestgenReason::Stopped, now); }
        } else if (s.staged_arm != arm_word || s.staged_arm != m_impl->arm.ticket() || !armed(arm_word)) {
            s.result = Status::fail(Result::NotArmed, Detail::DisarmRaced);
        } else {
            s.active = s.staged; s.binding = s.staged_binding; s.run = s.staged_run;
            s.value = loadValue(defs::ValueType::F32, s.binding.storage);
            s.arm_word = arm_word; s.started_us = now; s.repetitions = s.elapsed_ms = 0;
            s.state = TestgenState::Running; s.reason = TestgenReason::None; s.generating = s.first = true;
            s.binding.gate->testgen_active = true;
        }
    }
    if (s.generating) {
        const auto elapsed = static_cast<uint64_t>(std::max<int64_t>(0, now - s.started_us));
        const auto duration = static_cast<uint64_t>(s.active.duration_ms) * 1000;
        const auto count = elapsed / duration;
        s.elapsed_ms = static_cast<uint32_t>(std::min<uint64_t>(elapsed / 1000, UINT32_MAX));
        s.repetitions = static_cast<uint32_t>(std::min<uint64_t>(count, UINT32_MAX));
        if (s.active.repeat && count >= s.active.repeat) { s.finish(TestgenReason::Completed, now); }
    }
}
void TestgenService::sample(uint8_t loop, int64_t now)
{
    auto &s = *m_impl->slot(loop);
    if (s.generating && s.arm_word != m_impl->arm.ticket()) { s.finish(TestgenReason::Disarmed, now); }
    if (s.generating) {
        const auto elapsed = static_cast<uint64_t>(std::max<int64_t>(0, now - s.started_us));
        const auto duration = static_cast<uint64_t>(s.active.duration_ms) * 1000;
        double value = testgenValue(s.active, static_cast<double>(elapsed % duration) * 1e-6);
        const auto &item = *s.binding.item;
        const double lo = item.has_min ? defs::f32FromBits(item.min_bits) : -std::numeric_limits<float>::max();
        const double hi = item.has_max ? defs::f32FromBits(item.max_bits) : std::numeric_limits<float>::max();
        s.value = defs::f32Bits(static_cast<float>(std::clamp(value, lo, hi)));
        storeValue(defs::ValueType::F32, s.binding.storage, s.value);
        if (s.binding.receipt_us) { *s.binding.receipt_us = core_os::nowUs(); }
        if (s.first) { m_impl->capture.testgenStarted(loop); s.first = false; }
        s.publish();
    }
    if (s.swap.load(std::memory_order_relaxed) == Swap::Applying) {
        s.swap.store(Swap::Applied, std::memory_order_release); s.applied.signal();
    }
}
}
