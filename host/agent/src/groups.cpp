#include "groups.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/wire_codec.hpp"
#include <algorithm>

namespace drivelink::agent
{
namespace defs = protocol::defs;
namespace msg = protocol::defs::msg;

Route classify(uint8_t msg_type, uint8_t family, uint8_t op)
{
    using defs::Family;
    if (msg_type == msg::kDisarm)
    {
        return family == static_cast<uint8_t>(Family::System) && op == static_cast<uint8_t>(defs::SystemOp::Disarm)
                   ? Route::Disarm
                   : Route::Malformed;
    }
    if (msg_type >= defs::kCmdGroupChannelFirst && msg_type < defs::kCmdGroupChannelFirst + defs::kCmdGroupChannelCount)
    {
        return Route::CommandFrame;
    }
    if (msg_type == msg::kDiagQuery) { return Route::Diag; }
    if (msg_type == msg::kAgentCommand) { return Route::Agent; }
    if (msg_type != msg::kCoreCommand) { return Route::Drop; } // time pings, anything else
    switch (static_cast<Family>(family))
    {
    case Family::System:
        switch (static_cast<defs::SystemOp>(op))
        {
        case defs::SystemOp::Arm: return Route::Arm;
        case defs::SystemOp::Disarm: return Route::Disarm;
        case defs::SystemOp::Save:
        case defs::SystemOp::Revert:
        case defs::SystemOp::FactoryReset:
        case defs::SystemOp::Reboot:
        case defs::SystemOp::ExitSafeMode:
        case defs::SystemOp::SetName:
        case defs::SystemOp::LinkConfirm: // confirms a running link trial
        case defs::SystemOp::BenchStall: // test builds only; they stall a loop or crash
        case defs::SystemOp::BenchCrash: return Route::Exclusive;
        default: return Route::Forward; // GET_INFO, ACK_FAULT, unknown
        }
    case Family::Group:
        switch (static_cast<defs::GroupOp>(op))
        {
        case defs::GroupOp::CmdConfig:
        case defs::GroupOp::CmdClear: return Route::CmdGroup;
        case defs::GroupOp::SubConfig:
        case defs::GroupOp::SubEnable:
        case defs::GroupOp::SubClear:
        case defs::GroupOp::List: return Route::Group;
        default: return Route::Forward; // the device answers Unsupported
        }
    // In these families the reads are open and every other op needs the lease.
    case Family::Layout:
        return op == static_cast<uint8_t>(defs::LayoutOp::ListInstances) ? Route::Forward : Route::Exclusive;
    case Family::Capture:
        return op == static_cast<uint8_t>(defs::CaptureOp::Status) || op == static_cast<uint8_t>(defs::CaptureOp::Read)
                   ? Route::Forward
                   : Route::Exclusive;
    case Family::Testgen:
        return op == static_cast<uint8_t>(defs::TestgenOp::Status) ? Route::Forward : Route::Exclusive;
    case Family::Action: return Route::Exclusive;
    case Family::Ota:
        return op == static_cast<uint8_t>(defs::OtaOp::Status) ? Route::Forward : Route::Exclusive;
    default: return Route::Forward; // describe, param (GET, SET, GET_PENDING), unknown families
    }
}

std::expected<ListPage, std::string> parseListPage(std::span<const uint8_t> body)
{
    protocol::wire::Reader r(body.data(), body.size());
    ListPage page;
    page.total = r.readU16Le();
    page.next = r.readU16Le();
    const auto count = r.readU8();
    if (!r.ok()) { return std::unexpected("truncated LIST page header"); }
    for (unsigned i = 0; i < count; ++i)
    {
        GroupEntry e;
        e.kind = r.readU8(); e.group = r.readU8(); e.flags = r.readU8(); e.loop = r.readU8();
        e.epoch = r.readU8(); e.divider = r.readU16Le();
        const auto n = r.readU8();
        if (!r.ok() || e.kind > 1 || r.remaining() < 4U * n) { return std::unexpected("truncated or invalid LIST entry"); }
        for (unsigned j = 0; j < n; ++j)
        {
            std::array<uint8_t, 4> ref{};
            for (auto &b : ref) { b = r.readU8(); }
            e.refs.push_back(ref);
        }
        page.entries.push_back(std::move(e));
    }
    if (!r.done()) { return std::unexpected("trailing bytes after LIST entries"); }
    return page;
}

void GroupMap::reset(uint8_t subs, uint8_t cmds)
{
    m_slots[0].assign(subs, Slot{});
    m_slots[1].assign(cmds, Slot{});
    m_count = {subs, cmds};
    m_seen.clear();
}

void GroupMap::resize(uint8_t subs, uint8_t cmds)
{
    m_count = {subs, cmds};
    for (size_t k = 0; k < m_slots.size(); ++k)
    {
        if (m_slots[k].size() < m_count[k]) { m_slots[k].resize(m_count[k]); }
    }
}

std::optional<uint8_t> GroupMap::find(ClientId client, uint8_t vid, Kind kind) const
{
    const auto &slots = pool(kind);
    for (size_t i = 0; i < slots.size(); ++i)
    {
        const auto &s = slots[i];
        if (s.state == State::Owned && s.client == client && s.vid == vid) { return static_cast<uint8_t>(i); }
    }
    return std::nullopt;
}

std::optional<uint8_t> GroupMap::acquire(ClientId client, uint8_t vid, Kind kind)
{
    auto &slots = pool(kind);
    if (auto existing = find(client, vid, kind))
    {
        ++slots[*existing].inflight;
        return existing;
    }
    for (size_t i = 0; i < count(kind); ++i)
    {
        auto &s = slots[i];
        if (s.state != State::Free) { continue; }
        s = Slot{};
        s.state = State::Owned; s.client = client; s.vid = vid; s.inflight = 1; s.serial = ++m_serial;
        s.epoch = lastEpoch(client, vid, kind);
        return static_cast<uint8_t>(i);
    }
    return std::nullopt;
}

void GroupMap::finish(uint8_t device_slot, uint64_t serial, bool accepted, uint8_t epoch, bool cleared, bool uncertain, Kind kind)
{
    auto &s = pool(kind).at(device_slot);
    if (s.serial != serial) { return; } // an earlier allocation's answer
    if (s.inflight) { --s.inflight; }
    if (s.state != State::Owned) { return; } // Dirty: the agent's own CLEAR decides
    if (accepted)
    {
        s.epoch = epoch;
        s.configured = !cleared;
        s.maybe = false;
        setLastEpoch(s.client, s.vid, epoch, kind);
    }
    else if (uncertain)
    {
        s.maybe = true; // it may be configured on the device: keep it until a CLEAR
    }
    if (!s.inflight && !s.configured && !s.maybe)
    {
        s = Slot{};
    }
}

std::optional<std::pair<ClientId, uint8_t>> GroupMap::route(uint8_t device_slot, uint8_t epoch) const
{
    const auto &slots = pool(Kind::Sub);
    if (device_slot >= slots.size()) { return std::nullopt; }
    const auto &s = slots[device_slot];
    if (s.state != State::Owned || !s.configured || s.epoch != epoch) { return std::nullopt; }
    return std::pair{s.client, s.vid};
}

std::vector<uint8_t> GroupMap::release(ClientId client, Kind kind)
{
    std::vector<uint8_t> dirty;
    auto &slots = pool(kind);
    for (size_t i = 0; i < slots.size(); ++i)
    {
        auto &s = slots[i];
        if (s.state != State::Owned || s.client != client) { continue; }
        if (s.configured || s.maybe || s.inflight)
        {
            const auto inflight = s.inflight;
            const auto serial = s.serial;
            s = Slot{};
            s.state = State::Dirty; s.inflight = inflight; s.serial = serial;
            dirty.push_back(static_cast<uint8_t>(i));
        }
        else
        {
            s = Slot{};
        }
    }
    std::erase_if(m_seen, [client, kind](const Seen &seen) { return seen.client == client && seen.kind == kind; });
    return dirty;
}

void GroupMap::markDirty(uint8_t device_slot, Kind kind)
{
    auto &s = pool(kind).at(device_slot);
    if (s.state == State::Free) { s = Slot{}; s.state = State::Dirty; s.serial = ++m_serial; }
}

void GroupMap::cleaned(uint8_t device_slot, Kind kind)
{
    auto &s = pool(kind).at(device_slot);
    if (s.state == State::Dirty) { s = Slot{}; }
}

std::vector<uint8_t> GroupMap::dirty(Kind kind) const
{
    std::vector<uint8_t> out;
    const auto &slots = pool(kind);
    for (size_t i = 0; i < slots.size(); ++i)
    {
        if (slots[i].state == State::Dirty) { out.push_back(static_cast<uint8_t>(i)); }
    }
    return out;
}

uint8_t GroupMap::lastEpoch(ClientId client, uint8_t vid, Kind kind) const
{
    for (const auto &seen : m_seen)
    {
        if (seen.client == client && seen.kind == kind && seen.vid == vid) { return seen.epoch; }
    }
    return 0;
}

void GroupMap::setLastEpoch(ClientId client, uint8_t vid, uint8_t epoch, Kind kind)
{
    for (auto &seen : m_seen)
    {
        if (seen.client == client && seen.kind == kind && seen.vid == vid) { seen.epoch = epoch; return; }
    }
    m_seen.push_back({client, kind, vid, epoch});
}

std::optional<ListAnswer> synthesizeList(const GroupMap &map, ClientId client,
                                         std::span<const GroupEntry> device_entries,
                                         uint16_t start, size_t max_body)
{
    const uint16_t total = static_cast<uint16_t>(map.subs() + map.cmds());
    if (start > total) { return std::nullopt; }
    auto entryFor = [&](uint16_t index) {
        GroupEntry e;
        const bool sub = index < map.subs();
        e.kind = sub ? 0 : 1;
        const auto kind = sub ? Kind::Sub : Kind::Cmd;
        const auto vid = static_cast<uint8_t>(sub ? index : index - map.subs());
        e.group = vid;
        e.epoch = map.lastEpoch(client, vid, kind);
        if (auto slot = map.find(client, vid, kind))
        {
            for (const auto &d : device_entries)
            {
                if (d.kind == e.kind && d.group == *slot)
                {
                    e = d;
                    e.group = vid;
                    break;
                }
            }
        }
        return e;
    };
    ListAnswer answer;
    std::vector<uint8_t> w;
    auto u8 = [&w](uint8_t v) { w.push_back(v); };
    auto u16 = [&w](uint16_t v) { w.push_back(static_cast<uint8_t>(v)); w.push_back(static_cast<uint8_t>(v >> 8)); };
    u16(total); u16(0); u8(0);
    uint16_t next = start;
    uint8_t count = 0;
    size_t used = 5;
    for (; next < total && count < 255; ++next)
    {
        const auto e = entryFor(next);
        if (used + e.size() > max_body) { break; }
        u8(e.kind); u8(e.group); u8(e.flags); u8(e.loop);
        u8(e.epoch); u16(e.divider); u8(static_cast<uint8_t>(e.refs.size()));
        for (const auto &ref : e.refs) { w.insert(w.end(), ref.begin(), ref.end()); }
        used += e.size();
        ++count;
    }
    if (next < total && count == 0)
    {
        answer.too_small = true;
        return answer;
    }
    answer.body = std::move(w);
    const uint16_t cursor = next < total ? next : kListEnd;
    answer.body[2] = static_cast<uint8_t>(cursor);
    answer.body[3] = static_cast<uint8_t>(cursor >> 8);
    answer.body[4] = count;
    return answer;
}
} // namespace drivelink::agent
