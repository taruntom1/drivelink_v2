#include "drivelink/client.hpp"
#include "drivelink/protocol/core_codec.hpp"

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
Json Client::groups()
{
    Json result = {{"groups", Json::array()}};
    uint16_t cursor = 0;
    unsigned known_total = 65536;
    do {
        Bytes body{static_cast<uint8_t>(cursor), static_cast<uint8_t>(cursor >> 8)};
        const auto reply = accepted(static_cast<uint8_t>(defs::Family::Group), static_cast<uint8_t>(defs::GroupOp::List), body);
        Reader r(reply.body.data(), reply.body.size());
        const auto subs = r.readU8(), cmds = r.readU8(), entries = r.readU8();
        const auto total = r.readU16Le(), next = r.readU16Le(); const auto count = r.readU8();
        if (!r.ok() || subs > defs::kSubGroupChannelCount || cmds > defs::kCmdGroupChannelCount || !entries || entries > 64 ||
            total != subs + cmds || (known_total != 65536 && (total != known_total || result["subscription_capacity"] != subs ||
                result["command_capacity"] != cmds || result["entry_capacity"] != entries)) || cursor > total || count > total - cursor ||
            (next == 65535 ? cursor + count != total : next != cursor + count || next <= cursor || next >= total)) { malformed(); }
        known_total = total;
        result["subscription_capacity"] = subs; result["command_capacity"] = cmds; result["entry_capacity"] = entries;
        for (unsigned i = 0; i < count; ++i) {
            const auto kind = r.readU8(), id = r.readU8(), flags = r.readU8(), loop = r.readU8(), epoch = r.readU8();
            const auto divider = r.readU16Le(); const auto n = r.readU8();
            const unsigned index = cursor + i;
            if (kind != (index < subs ? 0 : 1) || id != (index < subs ? index : index - subs) || flags > 3 ||
                n > entries || !divider || (flags & 1 ? loop == 255 : flags != 0 || loop != 255 || n != 0) ||
                (kind == 1 && divider != 1)) { malformed(); }
            Json refs = Json::array();
            for (unsigned j = 0; j < n; ++j) {
                Json ref = {{"type", r.readU8()}, {"instance", r.readU8()}, {"item", r.readU16Le()}};
                for (const auto &old : refs) { if (old == ref) { malformed(); } }
                refs.push_back(ref);
            }
            result["groups"].push_back({{"kind", kind == 0 ? "subscription" : "command"}, {"id", id},
                {"channel", (kind == 0 ? defs::kSubGroupChannelFirst : defs::kCmdGroupChannelFirst) + id},
                {"configured", (flags & 1) != 0}, {"enabled", (flags & 2) != 0}, {"loop", loop},
                {"epoch", epoch}, {"divider", divider}, {"refs", refs}});
        }
        exact(r); cursor = next;
    } while (cursor != 65535);
    return result;
}
Json Client::groupMutation(uint8_t op, const Bytes &body)
{
    const auto reply = accepted(static_cast<uint8_t>(defs::Family::Group), op, body);
    Reader r(reply.body.data(), reply.body.size());
    const auto epoch = r.readU8(), channel = r.readU8();
    const bool command = op == static_cast<uint8_t>(defs::GroupOp::CmdConfig) || op == static_cast<uint8_t>(defs::GroupOp::CmdClear);
    const auto first = command ? defs::kCmdGroupChannelFirst : defs::kSubGroupChannelFirst;
    const auto capacity = command ? defs::kCmdGroupChannelCount : defs::kSubGroupChannelCount;
    if (body.empty() || body[0] >= capacity || channel != first + body[0]) { malformed(); }
    exact(r);
    return {{"epoch", epoch}, {"channel", channel}};
}
Json Client::subConfigure(uint8_t group, uint8_t loop, uint16_t divider, std::span<const Ref> refs)
{ return groupMutation(static_cast<uint8_t>(defs::GroupOp::SubConfig), configBody(group, loop, divider, refs, true)); }
Json Client::cmdConfigure(uint8_t group, uint8_t loop, std::span<const Ref> refs)
{ return groupMutation(static_cast<uint8_t>(defs::GroupOp::CmdConfig), configBody(group, loop, 1, refs, false)); }
Json Client::subEnable(uint8_t group, bool enabled)
{ return groupMutation(static_cast<uint8_t>(defs::GroupOp::SubEnable), {group, static_cast<uint8_t>(enabled)}); }
Json Client::groupClear(uint8_t group, bool command)
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
Json decodeSubscription(const Frame &frame, const Json &group, const Json &descriptors)
{
    Reader r(frame.payload.data(), frame.payload.size());
    const auto version = r.readU8(), epoch = r.readU8();
    if (!r.ok() || version != protocol::core_wire::kVersion) { malformed(); }
    if (frame.type != group.at("channel").get<uint8_t>() || epoch != group.at("epoch").get<uint8_t>()) { return nullptr; }
    if (!group.at("configured").get<bool>() || group.at("kind") != "subscription" ||
        descriptors.size() != group.at("refs").size() || descriptors.size() > 64 ||
        frame.payload.size() != 12 + 4 * descriptors.size()) { malformed(); }
    Json j = {{"group", group.at("id")}, {"epoch", epoch}, {"counter", r.readU16Le()}, {"timestamp_us", r.readU64Le()},
              {"values", Json::array()}, {"typed_values", Json::array()}};
    for (const auto &descriptor : descriptors) {
        const auto bits = r.readU32Le(); j["values"].push_back(bits);
        j["typed_values"].push_back(valueJson(descriptor.at("value_type").get<uint8_t>(), bits));
    }
    exact(r); return j;
}
} // namespace drivelink
