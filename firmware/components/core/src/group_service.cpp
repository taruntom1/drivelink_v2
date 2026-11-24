#include "core/group_service.hpp"

#include <algorithm>
#include <vector>
#include "core_os/clock.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"

namespace core {
namespace {
enum class Swap : uint8_t { Idle, Ready, Applying, Applied };
struct Config {
    GroupInfo info;
    std::array<BoundSignal, kMaxGroupEntries> bindings{};
};
struct Slot {
    // Each accessor makes one claim. No accessor waits while owning a slot.
    std::atomic_flag owned = ATOMIC_FLAG_INIT;
    std::atomic<uint8_t> route{kNoLoop};
    std::atomic<Swap> swap{Swap::Idle};
    core_os::Event applied;
    Config active, staged;
    GroupInfo shadow; // config caller only, never read by loops/RX
    std::array<uint32_t, kMaxGroupEntries> values{};
    bool command_ready = false, stream_ready = false;
    int64_t command_us = 0;
    // ESP32 has no lock-free 64-bit atomics. Timestamp readers claim the slot.
    int64_t last_command_us = 0;
    uint16_t counter = 0, phase = 0;
    std::array<uint8_t, kGroupStreamBytes> stream{};
    size_t stream_size = 0;
    bool claim() { return !owned.test_and_set(std::memory_order_acquire); }
    void release() { owned.clear(std::memory_order_release); }
};
bool sameRef(const Ref &a, const Ref &b)
{ return a.type == b.type && a.instance == b.instance && a.id == b.id; }
Status bad(Detail detail) { return Status::fail(Result::InvalidArgument, detail); }
Status busy() { return Status::fail(Result::Busy, Detail::StagingBusy); }
}

void latchSignals(std::span<const BoundSignal> signals, std::span<uint32_t> values)
{
    const auto n = std::min(signals.size(), values.size());
    for (size_t i = 0; i < n; ++i) {
        values[i] = loadValue(signals[i].item->type, signals[i].storage);
    }
}

Status bindSignal(const detail::InstanceTable &table, const Ref &ref, uint8_t loop, bool inputs_only,
                  BoundSignal &out)
{
    const auto *rec = table.find(ref.type, ref.instance);
    const auto index = rec ? rec->itemIndex(ref.id) : detail::kNoItem;
    if (!rec || index == detail::kNoItem) { return bad(Detail::UnknownRef); }
    const auto &item = rec->desc->items[index];
    if (item.kind != defs::ItemKind::Input && (inputs_only || item.kind != defs::ItemKind::Signal)) {
        return bad(Detail::WrongKind);
    }
    const uint8_t owner = rec->type_id == defs::kLoopTypeId ? rec->instance : rec->loop;
    if (owner != loop) { return bad(Detail::LoopMismatch); }
    if (rec->state != InstanceState::Running || !rec->bindings[index].bound) { return bad(Detail::DependencyMissing); }
    out = {ref, &item, rec->bindings[index].ptr, rec->bindings[index].receipt_us, &rec->bindings[index]};
    return {};
}

struct GroupService::Impl {
    const detail::InstanceTable &table;
    std::span<Loop *> loops;
    uint8_t sub_count, cmd_count, entries;
    // One allocation per slot (about 4 KB): all of them in one array would need
    // one contiguous block of up to 120 KB.
    std::vector<std::unique_ptr<Slot>> subs, cmds;
    Config candidate; // fixed config-caller scratch; never on a task stack
    Impl(const detail::InstanceTable &t, std::span<Loop *> l, uint8_t s, uint8_t c, uint8_t e)
        : table(t), loops(l),
          sub_count(std::min<uint8_t>(s, defs::kSubGroupChannelCount)),
          cmd_count(std::min<uint8_t>(c, defs::kCmdGroupChannelCount)),
          entries(std::clamp<uint8_t>(e, 1, kMaxGroupEntries)),
          subs(makeSlots(sub_count)), cmds(makeSlots(cmd_count)) {}
    static std::vector<std::unique_ptr<Slot>> makeSlots(size_t count) {
        std::vector<std::unique_ptr<Slot>> slots(count);
        for (auto &slot : slots) { slot = std::make_unique<Slot>(); }
        return slots;
    }
    Slot *slot(GroupKind kind, uint8_t id) const {
        if (kind == GroupKind::Subscription) { return id < sub_count ? subs[id].get() : nullptr; }
        if (kind == GroupKind::Command) { return id < cmd_count ? cmds[id].get() : nullptr; }
        return nullptr;
    }
    Loop *loop(uint8_t id) const {
        for (auto *l : loops) { if (l && l->id() == id) { return l; } }
        return nullptr;
    }
    uint32_t timeout(const Slot &s) const {
        const auto *l = loop(s.route.load(std::memory_order_acquire));
        return l ? ParamService::stagingTimeoutUs(l->periodUs()) : 0;
    }
    Status publish(Slot &s) {
        if (!s.claim()) { return busy(); }
        if (s.swap.load(std::memory_order_acquire) != Swap::Idle) { s.release(); return busy(); }
        candidate.info.epoch = static_cast<uint8_t>(s.shadow.epoch + 1U);
        s.staged = candidate;
        s.applied.clear();
        if (!s.active.info.configured && !candidate.info.configured) {
            s.active = s.staged;
            s.swap.store(Swap::Applied, std::memory_order_release);
        } else {
            if (!s.active.info.configured) { s.route.store(candidate.info.loop, std::memory_order_release); }
            s.swap.store(Swap::Ready, std::memory_order_release);
        }
        s.release();
        return {};
    }
};

GroupService::GroupService(const detail::InstanceTable &table, std::span<Loop *> loops,
                         uint8_t subscriptions, uint8_t commands, uint8_t entries)
    : m_impl(std::make_unique<Impl>(table, loops, subscriptions, commands, entries)) {}
GroupService::~GroupService() = default;
size_t GroupService::slotBytes() { return sizeof(Slot); }
size_t GroupService::baseBytes() { return sizeof(Impl); }
size_t GroupService::capacity(GroupKind kind) const
{ return kind == GroupKind::Subscription ? m_impl->sub_count : kind == GroupKind::Command ? m_impl->cmd_count : 0; }
uint8_t GroupService::entryCapacity() const { return m_impl->entries; }
const GroupInfo *GroupService::info(GroupKind kind, uint8_t id) const
{ const auto *s = m_impl->slot(kind, id); return s ? &s->shadow : nullptr; }

Status GroupService::stageConfig(GroupKind kind, uint8_t id, uint8_t loop, uint16_t divider,
                                std::span<const Ref> refs, bool armed)
{
    auto &d = *m_impl;
    auto *s = d.slot(kind, id);
    if (!s) { return bad(Detail::Capacity); }
    if (kind == GroupKind::Command && armed) { return Status::fail(Result::Armed, Detail::LockedWhileArmed); }
    if (!d.loop(loop)) { return bad(Detail::LoopMismatch); }
    if (!divider || (kind == GroupKind::Command && divider != 1)) { return bad(Detail::BadValue); }
    if (refs.size() > d.entries) { return bad(Detail::BatchTooLarge); }
    d.candidate = {};
    auto &cfg = d.candidate;
    cfg.info.configured = true;
    cfg.info.enabled = true;
    cfg.info.loop = loop;
    cfg.info.divider = divider;
    cfg.info.count = static_cast<uint8_t>(refs.size());
    for (size_t i = 0; i < refs.size(); ++i) {
        const auto &ref = refs[i];
        const auto bound = bindSignal(d.table, ref, loop, kind == GroupKind::Command, cfg.bindings[i]);
        if (!bound.ok()) { return bound; }
        for (size_t j = 0; j < i; ++j) { if (sameRef(ref, refs[j])) { return bad(Detail::DuplicateRef); } }
        if (kind == GroupKind::Command) {
            for (size_t g = 0; g < d.cmd_count; ++g) {
                if (g == id) { continue; }
                const auto &other = d.cmds[g]->shadow;
                for (size_t j = 0; other.configured && j < other.count; ++j) {
                    if (sameRef(ref, other.refs[j])) { return bad(Detail::AlreadyBound); }
                }
            }
        }
        cfg.info.refs[i] = ref;
    }
    return d.publish(*s);
}

Status GroupService::complete(GroupKind kind, uint8_t id, uint32_t timeout_us)
{
    auto *s = m_impl->slot(kind, id);
    if (!s) { return bad(Detail::Capacity); }
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
    // Applied releases the staged configuration; only this caller can write it
    // again, after resetting Idle. Loop/RX never access shadow.
    s->shadow = s->staged.info;
    s->swap.store(Swap::Idle, std::memory_order_release);
    return {};
}

Status GroupService::configure(GroupKind kind, uint8_t id, uint8_t loop, uint16_t divider,
                              std::span<const Ref> refs, bool armed)
{
    auto st = stageConfig(kind, id, loop, divider, refs, armed);
    if (!st.ok()) { return st; }
    return complete(kind, id, m_impl->timeout(*m_impl->slot(kind, id)));
}
Status GroupService::enable(uint8_t id, bool enabled)
{
    auto &d = *m_impl;
    auto *s = d.slot(GroupKind::Subscription, id);
    if (!s || !s->shadow.configured) { return bad(Detail::UnknownRef); }
    if (!s->claim()) { return busy(); }
    d.candidate = s->active;
    s->release();
    d.candidate.info.enabled = enabled;
    auto st = d.publish(*s);
    return st.ok() ? complete(GroupKind::Subscription, id, d.timeout(*s)) : st;
}
Status GroupService::clear(GroupKind kind, uint8_t id, bool armed)
{
    auto &d = *m_impl;
    auto *s = d.slot(kind, id);
    if (!s) { return bad(Detail::Capacity); }
    if (kind == GroupKind::Command && armed) { return Status::fail(Result::Armed, Detail::LockedWhileArmed); }
    d.candidate = {};
    auto st = d.publish(*s);
    return st.ok() ? complete(kind, id, d.timeout(*s)) : st;
}

bool GroupService::receiveCommand(uint8_t id, std::span<const uint8_t> payload, int64_t received_us)
{
    auto *s = m_impl->slot(GroupKind::Command, id);
    auto reject = [] {
        core_os::countDiag(defs::diag::codes::core::kCommandFrameRejected, 1);
        return false;
    };
    if (!s || !s->claim()) { return reject(); }
    const auto &cfg = s->active;
    drivelink::protocol::wire::Reader r(payload.data(), payload.size());
    const auto version = r.readU8(), epoch = r.readU8();
    (void)r.readU16Le(); // loss counter is not a replay/ordering guarantee
    if (!cfg.info.configured || version != drivelink::protocol::core_wire::kVersion ||
        epoch != cfg.info.epoch || !r.ok() || (r.remaining() && r.remaining() != 4U * cfg.info.count)) {
        s->release(); return reject();
    }
    // Validate all entries before publishing any. Scratch is RX-only stack
    // storage (256 bytes); active mailbox remains unchanged on rejection.
    std::array<uint32_t, kMaxGroupEntries> checked{};
    const bool values = r.remaining() != 0;
    if (values) {
        for (size_t i = 0; i < cfg.info.count; ++i) {
            const auto value = checkValue(*cfg.bindings[i].item, r.readU32Le());
            if (!value.ok) { s->release(); return reject(); }
            checked[i] = value.applied_bits;
        }
    }
    s->last_command_us = received_us;
    if (values) {
        s->values = checked;
        s->command_us = received_us;
        s->command_ready = true;
    }
    s->release();
    return true;
}

void GroupService::beginTick(uint8_t loop, bool armed, int64_t armed_at_us)
{
    auto &d = *m_impl;
    for (auto kind : {GroupKind::Subscription, GroupKind::Command}) {
        for (size_t i = 0; i < capacity(kind); ++i) {
            auto &s = *d.slot(kind, static_cast<uint8_t>(i));
            if (s.route.load(std::memory_order_acquire) != loop || !s.claim()) { continue; }
            if (s.route.load(std::memory_order_acquire) != loop) { s.release(); continue; }
            auto expected = Swap::Ready;
            if (s.swap.compare_exchange_strong(expected, Swap::Applying, std::memory_order_acq_rel)) {
                s.active = s.staged;
                s.command_ready = false;
                s.stream_ready = false;
                s.last_command_us = 0;
                s.phase = 0;
                // Keep the stream loss counter continuous across reconfiguration.
                s.route.store(s.active.info.configured ? s.active.info.loop : kNoLoop, std::memory_order_release);
                s.swap.store(Swap::Applied, std::memory_order_release);
                s.applied.signal();
            }
            const auto &cfg = s.active;
            if (kind == GroupKind::Command && cfg.info.configured && cfg.info.loop == loop && s.command_ready) {
                if (armed && s.command_us > armed_at_us) {
                    for (size_t j = 0; j < cfg.info.count; ++j) {
                        const auto *gate = cfg.bindings[j].gate;
                        if (gate && (gate->testgen_active || s.command_us <= gate->command_after_us)) { continue; }
                        storeValue(cfg.bindings[j].item->type, cfg.bindings[j].storage, s.values[j]);
                        if (cfg.bindings[j].receipt_us) { *cfg.bindings[j].receipt_us = s.command_us; }
                    }
                }
                s.command_ready = false;
            }
            s.release();
        }
    }
}

void GroupService::endTick(uint8_t loop, int64_t timestamp_us)
{
    auto &d = *m_impl;
    for (size_t i = 0; i < d.sub_count; ++i) {
        auto &s = *d.subs[i];
        if (s.route.load(std::memory_order_acquire) != loop || !s.claim()) { continue; }
        const auto &cfg = s.active;
        if (cfg.info.loop != loop) { s.release(); continue; }
        if (cfg.info.configured && cfg.info.enabled && ++s.phase >= cfg.info.divider) {
            s.phase = 0;
            latchSignals({cfg.bindings.data(), cfg.info.count}, s.values);
            drivelink::protocol::wire::Writer w(s.stream.data(), s.stream.size());
            w.appendU8(drivelink::protocol::core_wire::kVersion); w.appendU8(cfg.info.epoch);
            w.appendU16Le(s.counter++); w.appendU64Le(static_cast<uint64_t>(timestamp_us));
            for (size_t j = 0; j < cfg.info.count; ++j) { w.appendU32Le(s.values[j]); }
            s.stream_size = w.size();
            s.stream_ready = true;
        }
        s.release();
    }
}
size_t GroupService::takeStream(uint8_t id, std::span<uint8_t> payload)
{
    auto *s = m_impl->slot(GroupKind::Subscription, id);
    if (!s || !s->claim()) { return 0; }
    size_t n = 0;
    if (s->stream_ready && payload.size() >= s->stream_size) {
        n = s->stream_size;
        std::copy_n(s->stream.data(), n, payload.data());
        s->stream_ready = false;
    }
    s->release();
    return n;
}
bool GroupService::lastCommandUs(uint8_t id, int64_t &received_us) const
{
    auto *s = m_impl->slot(GroupKind::Command, id);
    if (!s || !s->claim()) { return false; }
    received_us = s->last_command_us;
    s->release();
    return true;
}
} // namespace core
