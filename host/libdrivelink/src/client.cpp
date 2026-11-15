#include "drivelink/client.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/core_codec.hpp"
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
ParamResult values(const Reply &reply, size_t expected)
{
    Reader r(reply.body.data(), reply.body.size());
    const auto n = r.readU8();
    if (n != expected)
    {
        malformed();
    }
    std::vector<uint32_t> result;
    for (unsigned i = 0; i < n; ++i)
    {
        result.push_back(r.readU32Le());
    }
    exact(r);
    return {reply.result, reply.result == 8, std::move(result)};
}
} // namespace
void Client::contextualize(Error &error,uint8_t family,uint8_t op,bool mutation) {
    if(error.has_request_context) { return; }
    error.family=family;error.op=op;error.has_request_context=true;
    // Local Usage failures precede submission. A malformed success body is
    // not a valid answer about the effect of the submitted mutation.
    if(error.kind==ErrorKind::Protocol && mutation) { error.outcome_unknown=true; }
}
Reply Client::accepted(uint8_t family, uint8_t op, std::span<const uint8_t> body)
{
    auto reply = m_session.request(family, op, body);
    if (reply.result != 0 && reply.result != 8)
    {
        throw DeviceError(
                    "device rejected request (result=" + std::to_string(reply.result) +
                        ", detail=" + std::to_string(reply.detail) + ")",
                    reply.result, reply.detail, family, op);
    }
    return reply;
}
SystemResult Client::setName(std::string_view name)
try
{
    if (!protocol::validDeviceName(name)) {
        throw Error(ErrorKind::Usage, "name must be at most 16 UTF-8 bytes without control characters");
    }
    std::array<uint8_t, protocol::kDeviceNameBytes + 1> body{};
    Writer w(body.data(), body.size());
    (void)protocol::writeDeviceName(w, name);
    const auto reply = accepted(1, static_cast<uint8_t>(protocol::defs::SystemOp::SetName), {body.data(), w.size()});
    Reader r(reply.body.data(), reply.body.size()); exact(r);
    return {true, reply.result};
} catch(Error &e) { contextualize(e,1,static_cast<uint8_t>(protocol::defs::SystemOp::SetName),true);throw; }

DeviceInfo Client::info()
try
{
    auto reply = accepted(1, 1);
    Reader r(reply.body.data(), reply.body.size());
    DeviceInfo j;
    j.protocol_version = r.readU8();
    if (j.protocol_version != protocol::core_wire::kVersion)
    {
        throw Error(ErrorKind::Protocol,
                    "incompatible core protocol; expected v" + std::to_string(protocol::core_wire::kVersion));
    }
    j.firmware_version = string(r);
    j.build_id = string(r);
    j.descriptor_hash = r.readU32Le();
    j.capture_capacity = r.readU32Le();
    const auto cap = r.readU16Le();
    j.max_payload = cap;
    j.running_instances = r.readU8();
    j.flags = r.readU8();
    const auto n = r.readU8();
    if (n > 8) { malformed(); }
    std::array<bool, 256> seen{};

    for (unsigned i = 0; i < n; ++i)
    {
        LoopInfo l;
        const auto id = r.readU8();
        if (id == 255 || seen[id]) { malformed(); }
        seen[id] = true;
        l.id = id;
        l.name = string(r);
        l.rate_hz = r.readU32Le();
        const auto flags = r.readU8();
        if (flags & ~1U) { malformed(); }
        l.flags = flags;

        j.loops.push_back(l);
    }
    for (auto &byte : j.device_id) { byte = r.readU8(); }
    std::array<char, protocol::kDeviceNameBytes + 1> name{};
    if (!r.ok() || !protocol::readDeviceName(r, name)) { malformed(); }
    j.name = name.data();
    exact(r);
    m_session.setMaxPayload(cap);
    return j;
} catch(Error &e) { contextualize(e,1,1,false);throw; }
template<class T> std::vector<T> Client::pages(uint8_t family, uint8_t op, const Bytes &prefix)
try
{
    std::vector<T> all;
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
            T j;
            if constexpr (std::is_same_v<T,Instance>)
            {
                j.type = r.readU8();
                j.instance = r.readU8();
                j.loop = r.readU8();
                j.state = r.readU8();
                j.label = string(r);
            }
            else if constexpr (std::is_same_v<T,TypeDescriptor>)
            {
                j.type = r.readU8();
                j.loop_mask = r.readU8();
                j.name = string(r);
            }
            else if constexpr (std::is_same_v<T,ItemDescriptor>)
            {
                j.id = r.readU16Le();
                j.kind = static_cast<ItemKind>(r.readU8());
                j.value_type = static_cast<ValueType>(r.readU8());
                j.flags = r.readU8();
                const auto limits = r.readU8();
                if(static_cast<uint8_t>(j.kind)>3 ||
                   (static_cast<uint8_t>(j.value_type)>6 && j.value_type!=ValueType::None) || limits>3) { malformed(); }

                j.min_bits = r.readU32Le();
                j.max_bits = r.readU32Le();
                j.default_bits = r.readU32Le();
                j.has_min = (limits & 1) != 0;
                j.has_max = (limits & 2) != 0;
                j.name = string(r);
                j.unit = string(r);
                j.description = string(r);
                j.ref = string(r);
                j.runs_in = static_cast<ActionPlacement>(r.readU8());
                j.blob_direction = static_cast<BlobDir>(r.readU8());
                if(static_cast<uint8_t>(j.runs_in)>2 || static_cast<uint8_t>(j.blob_direction)>3) { malformed(); }
                const auto nargs = r.readU8(), nresults = r.readU8();
                if (nargs > 4 || nresults > 4)
                {
                    malformed();
                }
                for (auto pair : {std::pair{&j.args,nargs},std::pair{&j.results,nresults}}) {
                    for(unsigned a=0;a<pair.second;++a) {
                        ScalarDescriptor arg;
                        arg.value_type=static_cast<ValueType>(r.readU8());
                        arg.name=string(r);arg.unit=string(r);
                        if (static_cast<uint8_t>(arg.value_type)>6) { malformed(); }
                        pair.first->push_back(std::move(arg));
                    }
                }
            }
            else if constexpr (std::is_same_v<T,EnumEntry>)
            {
                j.value = r.readU8();
                j.name = string(r);
            }
            else
            {
                j.source = r.readU8();
                j.code = r.readU16Le();
                j.severity = r.readU8();
                j.kind = r.readU8();
                j.flags = r.readU8();
                j.name = string(r);
            }
            all.push_back(std::move(j));
        }
        exact(r);
        cursor = next;
    } while (cursor != 65535);
    return all;
} catch(Error &e) { contextualize(e,family,op,false);throw; }
std::vector<TypeDescriptor> Client::types()
{
    return pages<TypeDescriptor>(3, 1);
}
std::vector<ItemDescriptor> Client::items(uint8_t type)
{
    return pages<ItemDescriptor>(3, 2, {type});
}
std::vector<EnumEntry> Client::enums(uint8_t type, uint16_t item)
{
    return pages<EnumEntry>(3, 3, {type, static_cast<uint8_t>(item), static_cast<uint8_t>(item >> 8)});
}
std::vector<CodeDescriptor> Client::codes()
{
    return pages<CodeDescriptor>(3, 4);
}
std::vector<Instance> Client::instances(bool pending)
{
    return pages<Instance>(2, 1, {static_cast<uint8_t>(pending)});
}
AddedInstance Client::add(uint8_t type, const std::string &name, uint8_t loop)
try
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
    return {reply.body[0], true};
} catch(Error &e) { contextualize(e,2,2,true);throw; }
LayoutResult Client::remove(uint8_t type, uint8_t instance)
try
{
    auto r = accepted(2, 3, Bytes{type, instance});
    if (!r.body.empty())
    {
        malformed();
    }
    return {true};
} catch(Error &e) { contextualize(e,2,3,true);throw; }
LayoutResult Client::label(uint8_t type, uint8_t instance, const std::string &name)
try
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
    return {true};
} catch(Error &e) { contextualize(e,2,4,true);throw; }
OperationResult Client::discard()
try
{
    auto r = accepted(2, 5);
    if (!r.body.empty())
    {
        malformed();
    }
    return {};
} catch(Error &e) { contextualize(e,2,5,true);throw; }
SystemResult Client::system(uint8_t op)
try
{
    auto r = accepted(1, op);
    if (!r.body.empty())
    {
        malformed();
    }
    return {true, r.result};
} catch(Error &e) { contextualize(e,1,op,op != 1);throw; }
RequestHandle Client::disarmAsync(Completion<SystemResult> completion) {
    return m_session.asyncRequest(1,3,{},[completion=std::move(completion)](std::expected<Reply,Error> reply) mutable {
        if(!reply) {completion(std::unexpected(reply.error()));return;}
        if(reply->result!=0 && reply->result!=8) {completion(std::unexpected(DeviceError("DISARM refused",reply->result,reply->detail,1,3)));return;}
        if(!reply->body.empty()) {Error e(ErrorKind::Protocol,"malformed DISARM response");contextualize(e,1,3,true);completion(std::unexpected(e));return;}
        completion(SystemResult{true,reply->result});
    });
}
ParamResult Client::get(std::span<const Ref> refs, bool pending)
try
{
    return values(accepted(4, pending ? 3 : 1, refBody(refs)), refs.size());
} catch(Error &e) { contextualize(e,4,pending ? 3 : 1,false);throw; }
OperationResult Client::benchStall(uint8_t loop, uint16_t duration_ms) try
{
    const auto reply = accepted(1,253,Bytes{loop,static_cast<uint8_t>(duration_ms),static_cast<uint8_t>(duration_ms>>8)});
    if (!reply.body.empty()) { malformed(); }
    return {};
} catch(Error &e) { contextualize(e,1,253,true);throw; }
ParamResult Client::set(std::span<const Ref> refs, std::span<const uint32_t> data)
try
{
    if (refs.size() != data.size())
    {
        throw Error(ErrorKind::Usage, "ref/value count mismatch");
    }
    return values(accepted(4, 2, refBody(refs, data)), refs.size());
} catch(Error &e) { contextualize(e,4,2,true);throw; }
ActionResult Client::action(Ref target, std::array<uint32_t, 4> args, std::span<const uint8_t> blob)
try
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
    std::array<uint32_t,4> out{};
    for (unsigned i = 0; i < 4; ++i)
    {
        out[i]=r.readU32Le();
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
    return {out, std::move(bytes)};
} catch(Error &e) { contextualize(e,8,1,true);throw; }
ItemDescriptor Client::item(Ref ref)
{
    for (const auto &d : items(ref.type))
    {
        if (d.id == ref.item)
        {
            return d;
        }
    }
    throw Error(ErrorKind::Usage, "item not found in device descriptors");
}
DiagInfo Client::diagInfo()
try
{
    auto data = m_session.diagRequest(1);
    Reader r(data.data(), data.size());
    DiagInfo j;
    j.flags = r.readU8();
    j.uptime_us = r.readU64Le();
    j.reset_reason = r.readU8();
    j.instances_ok = r.readU8();
    j.instances_failed = r.readU8();
    j.safe_mode = r.readU8();
    j.records_dropped = r.readU32Le();
    j.events_tx_dropped = r.readU32Le();
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
    j.firmware_version = version;
    exact(r);
    return j;
} catch(Error &e) { contextualize(e,0,1,false);throw; }
std::vector<DiagEntry> Client::diagEntries()
try
{
    std::vector<DiagEntry> all;
    uint8_t cursor = 0;
    unsigned known_total=256;
    do
    {
        auto data = m_session.diagRequest(2, Bytes{cursor});
        Reader r(data.data(), data.size());
        const auto total = r.readU8(), next = r.readU8(), n = r.readU8();
        if (!r.ok() || (known_total!=256 && known_total!=total) || (next != 255 && next!=cursor+n) || (next != 255 && (next <= cursor || next >= 254 || !n)) || all.size() + n > total ||
            total > 254 || (next == 255 && all.size() + n != total))
        {
            malformed();
        }
        known_total=total;
        for (unsigned i = 0; i < n; ++i)
        {
            DiagEntry j;
            j.source = r.readU8();
            j.instance = r.readU8();
            j.code = r.readU16Le();
            j.state = r.readU8();
            j.count = r.readU32Le();
            all.push_back(j);
        }
        exact(r);
        cursor = next;
    } while (cursor != 255);
    return all;
} catch(Error &e) { contextualize(e,0,2,false);throw; }
OperationResult Client::diagClear(uint8_t what)
try
{
    auto data = m_session.diagRequest(3, Bytes{what});
    if (!data.empty())
    {
        malformed();
    }
    return {};
} catch(Error &e) { contextualize(e,0,3,true);throw; }
Event Client::event(const Frame &frame) const {return decodeEvent(frame);}
Event Client::decodeEvent(const Frame &frame) {
    auto decode=[&]() -> Event {
    Reader r(frame.payload.data(),frame.payload.size());
    const auto version=r.readU8();
    if (frame.type==protocol::defs::msg::kStatusStream) {
        if(version!=protocol::core_wire::kVersion) { malformed(); }
        StatusFrame j;
        j.epoch=r.readU8();j.counter=r.readU16Le();j.timestamp_us=r.readU64Le();
        j.arm_state=r.readU8();j.flags=r.readU8();j.active_errors=r.readU8();
        j.cpu_load={r.readU8(),r.readU8()};const auto n=r.readU8();
        if(n>8) { malformed(); }std::array<bool,256> seen{};
        for(unsigned i=0;i<n;++i) {
            const auto id=r.readU8();if(id==255 || seen[id]) { malformed(); }seen[id]=true;
            j.overruns.push_back({id,r.readU32Le()});
        }
        j.last_disarm_reason=r.readU8();exact(r);return j;
    }
    if(frame.type==protocol::defs::msg::kCoreEvent) {
        if(version!=protocol::core_wire::kVersion) { malformed(); }
        const auto event=r.readU8();
        if(event==static_cast<uint8_t>(protocol::defs::CoreEvent::TestgenDone)) {
            TestgenDone j;j.loop=r.readU8();j.run=r.readU32Le();const auto reason=r.readU8();
            if(reason<1 || reason>3 || j.loop==255) { malformed(); }
            j.reason=static_cast<TestgenReason>(reason);exact(r);return j;
        }
        if(event!=static_cast<uint8_t>(protocol::defs::CoreEvent::CaptureDone)) { malformed(); }
        CaptureDone j;j.recorded=r.readU32Le();j.trigger_index=r.readU32Le();const auto flags=r.readU8();
        if(flags>3) { malformed(); }j.irregular=(flags & 2)!=0;exact(r);return j;
    }
    if(frame.type==protocol::defs::msg::kDiagEvent) {
        if(version!=3) { malformed(); }
        const auto seq=r.readU16Le();const auto kind=r.readU8(),flags=r.readU8();const auto ts=r.readU64Le();
        if(kind==4) {
            BootEvent j{seq,flags,ts,r.readU8(),r.readU8(),r.readU8(),r.readU8()};exact(r);return j;
        }
        if(kind==1) {
            DiagRecord j{seq,flags,ts,r.readU8(),r.readU8(),r.readU8(),r.readU16Le(),r.readI32Le(),r.readU32Le(),r.readU32Le()};
            exact(r);return j;
        }
        if(kind==3) {
            DiagCleared j{seq,flags,ts,r.readU8(),r.readU8(),r.readU16Le()};exact(r);return j;
        }
        if(kind==2) {
            DiagCounters j{seq,flags,ts,{}};const auto n=r.readU8();
            for(unsigned i=0;i<n;++i) { j.entries.push_back({r.readU8(),r.readU8(),r.readU16Le(),r.readU32Le(),r.readU32Le()}); }
            exact(r);return j;
        }
        malformed();
    }
    return frame;
    };
    auto result=decode();
    std::visit([&](auto &item) {
        if constexpr(requires {item.timestamp_us;item.time;}) {
            item.time=mapTime(frame.clock,item.timestamp_us,frame.generation);
        }
    },result);
    return result;
}
} // namespace drivelink
