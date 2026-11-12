#include "drivelink/client.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/wire_codec.hpp"
#include <algorithm>

namespace drivelink
{
using protocol::wire::Reader;
using protocol::wire::Writer;
namespace
{
[[noreturn]] void malformed()
{
    throw Error(ErrorKind::Protocol, "malformed device response");
}
void exact(const Reader &r)
{
    if (!r.done())
    {
        malformed();
    }
}
std::string string(Reader &r)
{
    const auto n = r.readU16Le();
    if (!r.ok() || n > r.remaining())
    {
        malformed();
    }
    std::string out;
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i)
    {
        out += static_cast<char>(r.readU8());
    }
    return out;
}
Bytes refBody(std::span<const Ref> refs, std::span<const uint32_t> values = {})
{
    if (refs.size() > 64 || (!values.empty() && values.size() != refs.size()))
    {
        throw Error(ErrorKind::Usage, "batch must have at most 64 matching refs and values");
    }
    Bytes out(1 + refs.size() * (values.empty() ? 4 : 8));
    Writer w(out.data(), out.size());
    w.appendU8(static_cast<uint8_t>(refs.size()));
    for (size_t i = 0; i < refs.size(); ++i)
    {
        w.appendU8(refs[i].type);
        w.appendU8(refs[i].instance);
        w.appendU16Le(refs[i].item);
        if (!values.empty())
        {
            w.appendU32Le(values[i]);
        }
    }
    return out;
}
Json values(const Reply &reply, size_t expected)
{
    Reader r(reply.body.data(), reply.body.size());
    const auto n = r.readU8();
    if (n != expected)
    {
        malformed();
    }
    Json result = Json::array();
    for (unsigned i = 0; i < n; ++i)
    {
        result.push_back(r.readU32Le());
    }
    exact(r);
    return {{"result", reply.result}, {"pending_reboot", reply.result == 8}, {"values", result}};
}
} // namespace
Reply Client::accepted(uint8_t family, uint8_t op, std::span<const uint8_t> body)
{
    auto reply = m_session.request(family, op, body);
    if (reply.result != 0 && reply.result != 8)
    {
        throw Error(ErrorKind::Rejected,
                    "device rejected request (result=" + std::to_string(reply.result) +
                        ", detail=" + std::to_string(reply.detail) + ")",
                    reply.result, reply.detail);
    }
    return reply;
}
Json Client::info()
{
    auto reply = accepted(1, 1);
    Reader r(reply.body.data(), reply.body.size());
    Json j;
    j["protocol_version"] = r.readU8();
    if (j["protocol_version"] != 1)
    {
        throw Error(ErrorKind::Protocol, "incompatible core protocol; expected v1");
    }
    j["firmware_version"] = string(r);
    j["build_id"] = string(r);
    j["descriptor_hash"] = r.readU32Le();
    j["capture_capacity"] = r.readU32Le();
    const auto cap = r.readU16Le();
    j["max_payload"] = cap;
    j["running_instances"] = r.readU8();
    j["flags"] = r.readU8();
    const auto n = r.readU8();
    j["loops"] = Json::array();
    for (unsigned i = 0; i < n; ++i)
    {
        Json l;
        l["id"] = r.readU8();
        l["name"] = string(r);
        l["rate_hz"] = r.readU32Le();
        j["loops"].push_back(l);
    }
    exact(r);
    m_session.setMaxPayload(cap);
    return j;
}
Json Client::pages(uint8_t family, uint8_t op, const Bytes &prefix)
{
    Json all = Json::array();
    uint16_t cursor = 0;
    unsigned known_total = 65536;
    do
    {
        Bytes body = prefix;
        body.push_back(static_cast<uint8_t>(cursor));
        body.push_back(static_cast<uint8_t>(cursor >> 8));
        auto reply = accepted(family, op, body);
        Reader r(reply.body.data(), reply.body.size());
        const auto total = r.readU16Le(), next = r.readU16Le();
        const auto count = r.readU8();
        if (!r.ok() || (known_total != 65536 && total != known_total) || total < cursor ||
            count > total - cursor ||
            (next != 65535 && (next != cursor + count || next <= cursor || next >= total)) ||
            (next == 65535 && cursor + count != total))
        {
            malformed();
        }
        known_total = total;
        for (unsigned i = 0; i < count; ++i)
        {
            Json j;
            if (family == 2)
            {
                j["type"] = r.readU8();
                j["instance"] = r.readU8();
                j["loop"] = r.readU8();
                j["state"] = r.readU8();
                j["label"] = string(r);
            }
            else if (op == 1)
            {
                j["type"] = r.readU8();
                j["loop_mask"] = r.readU8();
                j["name"] = string(r);
            }
            else if (op == 2)
            {
                j["id"] = r.readU16Le();
                j["kind"] = r.readU8();
                j["value_type"] = r.readU8();
                j["flags"] = r.readU8();
                const auto limits = r.readU8();
                j["min_bits"] = r.readU32Le();
                j["max_bits"] = r.readU32Le();
                j["default_bits"] = r.readU32Le();
                j["has_min"] = (limits & 1) != 0;
                j["has_max"] = (limits & 2) != 0;
                j["name"] = string(r);
                j["unit"] = string(r);
                j["description"] = string(r);
                j["ref"] = string(r);
                j["runs_in"] = r.readU8();
                j["blob_direction"] = r.readU8();
                const auto nargs = r.readU8(), nresults = r.readU8();
                if (nargs > 4 || nresults > 4)
                {
                    malformed();
                }
                for (const auto &[name, n] : {std::pair{"args", nargs}, std::pair{"results", nresults}})
                {
                    j[name] = Json::array();
                    for (unsigned a = 0; a < n; ++a)
                    {
                        Json arg;
                        arg["value_type"] = r.readU8();
                        arg["name"] = string(r);
                        arg["unit"] = string(r);
                        j[name].push_back(arg);
                    }
                }
            }
            else if (op == 3)
            {
                j["value"] = r.readU8();
                j["name"] = string(r);
            }
            else
            {
                j["source"] = r.readU8();
                j["code"] = r.readU16Le();
                j["severity"] = r.readU8();
                j["kind"] = r.readU8();
                j["flags"] = r.readU8();
                j["name"] = string(r);
            }
            all.push_back(std::move(j));
        }
        exact(r);
        cursor = next;
    } while (cursor != 65535);
    return all;
}
Json Client::types()
{
    return pages(3, 1);
}
Json Client::items(uint8_t type)
{
    return pages(3, 2, {type});
}
Json Client::enums(uint8_t type, uint16_t item)
{
    return pages(3, 3, {type, static_cast<uint8_t>(item), static_cast<uint8_t>(item >> 8)});
}
Json Client::codes()
{
    return pages(3, 4);
}
Json Client::instances(bool pending)
{
    return pages(2, 1, {static_cast<uint8_t>(pending)});
}
Json Client::add(uint8_t type, const std::string &name, uint8_t loop)
{
    if (name.empty() || name.size() > 16 || name.find('\0') != std::string::npos)
    {
        throw Error(ErrorKind::Usage, "label must have 1..16 non-NUL bytes");
    }
    Bytes body{type, loop, static_cast<uint8_t>(name.size())};
    body.insert(body.end(), name.begin(), name.end());
    auto reply = accepted(2, 2, body);
    if (reply.body.size() != 1)
    {
        malformed();
    }
    return {{"instance", reply.body[0]}, {"pending_reboot", true}};
}
Json Client::remove(uint8_t type, uint8_t instance)
{
    auto r = accepted(2, 3, Bytes{type, instance});
    if (!r.body.empty())
    {
        malformed();
    }
    return {{"pending_reboot", true}};
}
Json Client::label(uint8_t type, uint8_t instance, const std::string &name)
{
    if (name.empty() || name.size() > 16 || name.find('\0') != std::string::npos)
    {
        throw Error(ErrorKind::Usage, "label must have 1..16 non-NUL bytes");
    }
    Bytes body{type, instance, static_cast<uint8_t>(name.size())};
    body.insert(body.end(), name.begin(), name.end());
    auto r = accepted(2, 4, body);
    if (!r.body.empty())
    {
        malformed();
    }
    return {{"pending_reboot", true}};
}
Json Client::discard()
{
    auto r = accepted(2, 5);
    if (!r.body.empty())
    {
        malformed();
    }
    return {{"ok", true}};
}
Json Client::system(uint8_t op)
{
    auto r = accepted(1, op);
    if (!r.body.empty())
    {
        malformed();
    }
    return {{"ok", true}, {"result", r.result}};
}
Json Client::get(std::span<const Ref> refs, bool pending)
{
    return values(accepted(4, pending ? 3 : 1, refBody(refs)), refs.size());
}
Json Client::set(std::span<const Ref> refs, std::span<const uint32_t> data)
{
    if (refs.size() != data.size())
    {
        throw Error(ErrorKind::Usage, "ref/value count mismatch");
    }
    return values(accepted(4, 2, refBody(refs, data)), refs.size());
}
Json Client::action(Ref target, std::array<uint32_t, 4> args, std::span<const uint8_t> blob)
{
    if (blob.size() > 512)
    {
        throw Error(ErrorKind::Usage, "action blob exceeds 512 bytes");
    }
    Bytes body(22 + blob.size());
    Writer w(body.data(), body.size());
    w.appendU8(target.type);
    w.appendU8(target.instance);
    w.appendU16Le(target.item);
    for (auto a : args)
    {
        w.appendU32Le(a);
    }
    w.appendU16Le(static_cast<uint16_t>(blob.size()));
    w.appendBytes(blob.data(), blob.size());
    auto reply = accepted(8, 1, body);
    Reader r(reply.body.data(), reply.body.size());
    Json out = Json::array();
    for (unsigned i = 0; i < 4; ++i)
    {
        out.push_back(r.readU32Le());
    }
    const auto n = r.readU16Le();
    if (n > 512 || n != r.remaining())
    {
        malformed();
    }
    Bytes bytes;
    for (unsigned i = 0; i < n; ++i)
    {
        bytes.push_back(r.readU8());
    }
    exact(r);
    return {{"values", out}, {"blob_hex", hex(bytes)}};
}
Json Client::item(Ref ref)
{
    for (const auto &d : items(ref.type))
    {
        if (d.at("id") == ref.item)
        {
            return d;
        }
    }
    throw Error(ErrorKind::Usage, "item not found in device descriptors");
}
Json Client::diagInfo()
{
    auto data = m_session.diagRequest(1);
    Reader r(data.data(), data.size());
    Json j;
    j["flags"] = r.readU8();
    j["uptime_us"] = r.readU64Le();
    j["reset_reason"] = r.readU8();
    j["instances_ok"] = r.readU8();
    j["instances_failed"] = r.readU8();
    j["safe_mode"] = r.readU8();
    j["records_dropped"] = r.readU32Le();
    j["events_tx_dropped"] = r.readU32Le();
    const auto n = r.readU8();
    if (n > 32 || n != r.remaining())
    {
        malformed();
    }
    std::string version;
    for (unsigned i = 0; i < n; ++i)
    {
        version += static_cast<char>(r.readU8());
    }
    j["firmware_version"] = version;
    exact(r);
    return j;
}
Json Client::diagEntries()
{
    Json all = Json::array();
    uint8_t cursor = 0;
    do
    {
        auto data = m_session.diagRequest(2, Bytes{cursor});
        Reader r(data.data(), data.size());
        const auto total = r.readU8(), next = r.readU8(), n = r.readU8();
        if (!r.ok() || (next != 255 && (next <= cursor || !n)) || all.size() + n > total || total > 254)
        {
            malformed();
        }
        for (unsigned i = 0; i < n; ++i)
        {
            Json j;
            j["source"] = r.readU8();
            j["instance"] = r.readU8();
            j["code"] = r.readU16Le();
            j["state"] = r.readU8();
            j["count"] = r.readU32Le();
            all.push_back(j);
        }
        exact(r);
        cursor = next;
    } while (cursor != 255);
    return all;
}
Json Client::diagClear(uint8_t what)
{
    auto data = m_session.diagRequest(3, Bytes{what});
    if (!data.empty())
    {
        malformed();
    }
    return {{"ok", true}};
}
Json Client::event(const Frame &frame) const
{
    Reader r(frame.payload.data(), frame.payload.size());
    Json j;
    j["msg_type"] = frame.type;
    const auto version = r.readU8();
    if (frame.type == protocol::defs::msg::kStatusStream)
    {
        if (version != 1)
        {
            malformed();
        }
        j["epoch"] = r.readU8();
        j["counter"] = r.readU16Le();
        j["timestamp_us"] = r.readU64Le();
        j["arm_state"] = r.readU8();
        j["flags"] = r.readU8();
        j["active_errors"] = r.readU8();
        j["cpu_load"] = Json::array({r.readU8(), r.readU8()});
        j["overruns"] = Json::array({r.readU32Le(), r.readU32Le()});
        j["last_disarm_reason"] = r.readU8();
    }
    else if (frame.type == protocol::defs::msg::kDiagEvent)
    {
        if (version != 3)
        {
            malformed();
        }
        j["sequence"] = r.readU16Le();
        const auto kind = r.readU8();
        j["kind"] = kind;
        j["flags"] = r.readU8();
        j["timestamp_us"] = r.readU64Le();
        if (kind == 4)
        {
            j["reset_reason"] = r.readU8();
            j["instances_ok"] = r.readU8();
            j["instances_failed"] = r.readU8();
            j["safe_mode"] = r.readU8();
        }
        else if (kind == 1 || kind == 3)
        {
            if (kind == 1)
            {
                j["severity"] = r.readU8();
            }
            j["source"] = r.readU8();
            j["instance"] = r.readU8();
            j["code"] = r.readU16Le();
            if (kind == 1)
            {
                j["error"] = r.readI32Le();
                j["arg0"] = r.readU32Le();
                j["arg1"] = r.readU32Le();
            }
        }
        else if (kind == 2)
        {
            const auto n = r.readU8();
            j["entries"] = Json::array();
            for (unsigned i = 0; i < n; ++i)
            {
                Json e;
                e["source"] = r.readU8();
                e["instance"] = r.readU8();
                e["code"] = r.readU16Le();
                e["delta"] = r.readU32Le();
                e["total"] = r.readU32Le();
                j["entries"].push_back(e);
            }
        }
        else
        {
            malformed();
        }
    }
    else
    {
        return {{"msg_type", frame.type}, {"payload_hex", hex(frame.payload)}};
    }
    exact(r);
    return j;
}
} // namespace drivelink
