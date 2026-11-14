#include "drivelink/client.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "drivelink/protocol/gen/type_system.hpp"

namespace drivelink {
namespace {
using protocol::wire::Reader;
using protocol::wire::Writer;
namespace defs = protocol::defs;
[[noreturn]] void malformed() { throw Error(ErrorKind::Protocol, "malformed group response"); }
void exact(const Reader &r) { if (!r.done()) { malformed(); } }
Bytes configBody(uint8_t group, uint8_t loop, uint16_t divider, std::span<const Ref> refs, bool sub)
{
    if (refs.size() > 64 || loop == 255 || (sub && !divider)) {
        throw Error(ErrorKind::Usage, "invalid group configuration");
    }
    Bytes bytes((sub ? 5 : 3) + 4 * refs.size());
    Writer w(bytes.data(), bytes.size());
    w.appendU8(group); w.appendU8(loop);
    if (sub) { w.appendU16Le(divider); }
    w.appendU8(static_cast<uint8_t>(refs.size()));
    for (const auto &ref : refs) { w.appendU8(ref.type); w.appendU8(ref.instance); w.appendU16Le(ref.item); }
    return bytes;
}
}
GroupList Client::groups()
{
    GroupList result;
    namespace sys = protocol::defs::system;
    const std::array<Ref, 3> limits{{{0,0,sys::kLimitsSubGroups}, {0,0,sys::kLimitsCmdGroups}, {0,0,sys::kLimitsGroupEntries}}};
    const auto caps = get(limits).values;
    const auto subs = caps[0], cmds = caps[1], entries = caps[2];
    if (subs > defs::kSubGroupChannelCount || cmds > defs::kCmdGroupChannelCount || !entries || entries > 64) { malformed(); }
    result.subscription_capacity = subs; result.command_capacity = cmds; result.entry_capacity = entries;
    uint16_t cursor = 0;
    unsigned known_total = 65536;
    do {
        Bytes body{static_cast<uint8_t>(cursor), static_cast<uint8_t>(cursor >> 8)};
        const auto reply = accepted(static_cast<uint8_t>(defs::Family::Group), static_cast<uint8_t>(defs::GroupOp::List), body);
        Reader r(reply.body.data(), reply.body.size());
        const auto total = r.readU16Le(), next = r.readU16Le(); const auto count = r.readU8();
        if (!r.ok() || total != subs + cmds || (known_total != 65536 && total != known_total) || cursor > total || count > total - cursor ||
            (next == 65535 ? cursor + count != total : next != cursor + count || next <= cursor || next >= total)) { malformed(); }
        known_total = total;
        result.subscription_capacity = subs; result.command_capacity = cmds; result.entry_capacity = entries;
        for (unsigned i = 0; i < count; ++i) {
            const auto kind = r.readU8(), id = r.readU8(), flags = r.readU8(), loop = r.readU8(), epoch = r.readU8();
            const auto divider = r.readU16Le(); const auto n = r.readU8();
            const unsigned index = cursor + i;
            if (kind != (index < subs ? 0 : 1) || id != (index < subs ? index : index - subs) || flags > 3 ||
                n > entries || !divider || (flags & 1 ? loop == 255 : flags != 0 || loop != 255 || n != 0) ||
                (kind == 1 && divider != 1)) { malformed(); }
            std::vector<Ref> refs;
            for (unsigned j = 0; j < n; ++j) {
                Ref ref{r.readU8(),r.readU8(),r.readU16Le()};
                for (const auto &old : refs) { if (old == ref) { malformed(); } }
                refs.push_back(ref);
            }
            result.groups.push_back({static_cast<GroupKind>(kind),id,
                static_cast<uint8_t>((kind==0?defs::kSubGroupChannelFirst:defs::kCmdGroupChannelFirst)+id),
                (flags & 1)!=0,(flags & 2)!=0,loop,epoch,divider,std::move(refs)});
        }
        exact(r); cursor = next;
    } while (cursor != 65535);
    return result;
}
GroupMutation Client::groupMutation(uint8_t op, const Bytes &body)
{
    const auto reply = accepted(static_cast<uint8_t>(defs::Family::Group), op, body);
    Reader r(reply.body.data(), reply.body.size());
    const auto epoch = r.readU8(), channel = r.readU8();
    const bool command = op == static_cast<uint8_t>(defs::GroupOp::CmdConfig) || op == static_cast<uint8_t>(defs::GroupOp::CmdClear);
    const auto first = command ? defs::kCmdGroupChannelFirst : defs::kSubGroupChannelFirst;
    const auto capacity = command ? defs::kCmdGroupChannelCount : defs::kSubGroupChannelCount;
    if (body.empty() || body[0] >= capacity || channel != first + body[0]) { malformed(); }
    exact(r);
    return {epoch,channel};
}
GroupMutation Client::subConfigure(uint8_t group, uint8_t loop, uint16_t divider, std::span<const Ref> refs)
{ return groupMutation(static_cast<uint8_t>(defs::GroupOp::SubConfig), configBody(group, loop, divider, refs, true)); }
GroupMutation Client::cmdConfigure(uint8_t group, uint8_t loop, std::span<const Ref> refs)
{ return groupMutation(static_cast<uint8_t>(defs::GroupOp::CmdConfig), configBody(group, loop, 1, refs, false)); }
GroupMutation Client::subEnable(uint8_t group, bool enabled)
{ return groupMutation(static_cast<uint8_t>(defs::GroupOp::SubEnable), {group, static_cast<uint8_t>(enabled)}); }
GroupMutation Client::groupClear(uint8_t group, bool command)
{ return groupMutation(static_cast<uint8_t>(command ? defs::GroupOp::CmdClear : defs::GroupOp::SubClear), {group}); }
Bytes commandPayload(uint8_t epoch, uint16_t counter, std::span<const uint32_t> values)
{
    if (values.size() > 64) { throw Error(ErrorKind::Usage, "too many command values"); }
    Bytes bytes(4 + 4 * values.size()); Writer w(bytes.data(), bytes.size());
    w.appendU8(protocol::core_wire::kVersion); w.appendU8(epoch); w.appendU16Le(counter);
    for (auto value : values) { w.appendU32Le(value); }
    return bytes;
}
bool Client::command(uint8_t group, uint8_t epoch, uint16_t counter, std::span<const uint32_t> values)
{ return m_session.sendCommandGroup(group, commandPayload(epoch, counter, values)); }
std::optional<SubscriptionSample> decodeSubscription(const Frame &frame,const Group &group,std::span<const ItemDescriptor> descriptors) {
    Reader r(frame.payload.data(),frame.payload.size());const auto version=r.readU8(),epoch=r.readU8();
    if(!r.ok() || version!=protocol::core_wire::kVersion) { malformed(); }
    if(frame.type!=group.channel || epoch!=group.epoch) { return std::nullopt; }
    if(!group.configured || group.kind!=GroupKind::Subscription || descriptors.size()!=group.refs.size() ||
       descriptors.size()>64 || frame.payload.size()!=12+4*descriptors.size()) { malformed(); }
    SubscriptionSample j{group.id,epoch,r.readU16Le(),r.readU64Le(),{}, {}};
    for(const auto &d:descriptors) {
        const auto bits=r.readU32Le();j.values.push_back(bits);j.typed_values.push_back(decodeValue(d.value_type,bits));
    }
    exact(r);return j;
}
} // namespace drivelink
