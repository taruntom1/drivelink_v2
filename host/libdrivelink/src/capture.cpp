#include "drivelink/client.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include <algorithm>

namespace drivelink {
namespace {
using protocol::wire::Reader;
using protocol::wire::Writer;
namespace defs = protocol::defs;
constexpr uint8_t kFamily = static_cast<uint8_t>(defs::Family::Capture);
constexpr size_t kReadBytes = 512;
[[noreturn]] void malformed() { throw Error(ErrorKind::Protocol, "malformed capture response"); }
void exact(const Reader &r) { if (!r.done()) { malformed(); } }
uint8_t op(defs::CaptureOp o) { return static_cast<uint8_t>(o); }
const char *stateName(uint8_t s)
{
    static constexpr const char *kNames[] = {"idle", "armed", "triggered", "done"};
    return s < 4 ? kNames[s] : "unknown";
}
const char *triggerName(uint8_t k)
{
    static constexpr const char *kNames[] = {"manual", "testgen_start", "value_change", "threshold"};
    return k < 4 ? kNames[k] : "unknown";
}
Json refJson(Reader &r) { return {{"type", r.readU8()}, {"instance", r.readU8()}, {"item", r.readU16Le()}}; }
}

uint8_t parseCaptureTrigger(const std::string &text)
{
    for (uint8_t k = 0; k < 4; ++k) { if (text == triggerName(k)) { return k; } }
    throw Error(ErrorKind::Usage, "unknown capture trigger: " + text);
}

Json Client::captureConfig(uint8_t loop, uint16_t divider, uint32_t samples, uint32_t pre_trigger,
                           const CaptureTrigger &trigger, std::span<const Ref> refs)
{
    if (refs.empty() || refs.size() > 64 || loop == 255 || !divider || !samples || pre_trigger >= samples ||
        trigger.kind > 3 || trigger.edge > 1) {
        throw Error(ErrorKind::Usage, "invalid capture configuration");
    }
    Bytes body(22 + 4 * refs.size());
    Writer w(body.data(), body.size());
    w.appendU8(loop); w.appendU16Le(divider); w.appendU32Le(samples); w.appendU32Le(pre_trigger);
    w.appendU8(trigger.kind); w.appendU8(trigger.ref.type); w.appendU8(trigger.ref.instance);
    w.appendU16Le(trigger.ref.item); w.appendU8(trigger.edge); w.appendU32Le(trigger.value);
    w.appendU8(static_cast<uint8_t>(refs.size()));
    for (const auto &ref : refs) { w.appendU8(ref.type); w.appendU8(ref.instance); w.appendU16Le(ref.item); }
    if (!accepted(kFamily, op(defs::CaptureOp::Config), body).body.empty()) { malformed(); }
    return {{"ok", true}};
}
Json Client::captureOp(uint8_t o)
{
    if (!accepted(kFamily, o).body.empty()) { malformed(); }
    return {{"ok", true}};
}
Json Client::captureArm() { return captureOp(op(defs::CaptureOp::Arm)); }
Json Client::captureTrigger() { return captureOp(op(defs::CaptureOp::Trigger)); }
Json Client::captureAbort() { return captureOp(op(defs::CaptureOp::Abort)); }

Json Client::captureStatus()
{
    const auto reply = accepted(kFamily, op(defs::CaptureOp::Status));
    Reader r(reply.body.data(), reply.body.size());
    Json j;
    const auto state = r.readU8(), flags = r.readU8();
    if (!r.ok() || state > 3 || flags > 3) { malformed(); }
    j["state"] = stateName(state);
    j["state_code"] = state;
    j["configured"] = (flags & 1) != 0;
    j["irregular"] = (flags & 2) != 0;
    j["loop"] = r.readU8();
    const auto divider = r.readU16Le();
    j["divider"] = divider;
    j["samples"] = r.readU32Le();
    j["pre_trigger"] = r.readU32Le();
    const auto kind = r.readU8();
    if (kind > 3) { malformed(); }
    Json trigger = {{"kind", triggerName(kind)}, {"kind_code", kind}};
    trigger["ref"] = refJson(r);
    const auto edge = r.readU8();
    if (edge > 1) { malformed(); }
    trigger["edge"] = edge ? "falling" : "rising";
    trigger["value"] = r.readU32Le();
    j["trigger"] = trigger;
    j["recorded"] = r.readU32Le();
    const auto index = r.readU32Le();
    j["trigger_index"] = index == 0xFFFFFFFFU ? Json(nullptr) : Json(index);
    const auto period = r.readU32Le();
    j["period_us"] = period;
    j["sample_period_us"] = static_cast<uint64_t>(period) * divider;
    j["trigger_timestamp_us"] = r.readU64Le();
    const auto n = r.readU8();
    if (!r.ok() || n > 64 || r.remaining() != 4U * n || ((flags & 1) == 0) != (n == 0) ||
        j["recorded"].get<uint32_t>() > j["samples"].get<uint32_t>()) { malformed(); }
    j["refs"] = Json::array();
    for (unsigned i = 0; i < n; ++i) { j["refs"].push_back(refJson(r)); }
    exact(r);
    return j;
}

Json Client::captureRead(uint32_t first, uint16_t count, size_t width)
{
    if (!width || !count) { throw Error(ErrorKind::Usage, "invalid capture read"); }
    Bytes body(6);
    Writer w(body.data(), body.size());
    w.appendU32Le(first); w.appendU16Le(count);
    const auto reply = accepted(kFamily, op(defs::CaptureOp::Read), body);
    Reader r(reply.body.data(), reply.body.size());
    if (r.readU32Le() != first || r.readU16Le() != count || r.remaining() != 4U * count * width) { malformed(); }
    Json samples = Json::array();
    for (unsigned i = 0; i < count; ++i) {
        Json row = Json::array();
        for (size_t k = 0; k < width; ++k) { row.push_back(r.readU32Le()); }
        samples.push_back(row);
    }
    exact(r);
    return {{"first", first}, {"count", count}, {"samples", samples}};
}

Json Client::captureData()
{
    auto status = captureStatus();
    if (status["state"] != "done") { throw Error(ErrorKind::Usage, "capture is not done"); }
    const size_t width = status["refs"].size();
    const uint32_t recorded = status["recorded"];
    // A reply holds the response and READ headers, then whole samples.
    const size_t room = std::min<size_t>(kReadBytes, m_max_payload > 12 ? m_max_payload - 12U : 0U);
    const uint32_t page = static_cast<uint32_t>(room / (4U * width));
    if (!page) { throw Error(ErrorKind::Protocol, "capture sample does not fit the device payload"); }
    Json samples = Json::array();
    for (uint32_t first = 0; first < recorded;) {
        const auto count = static_cast<uint16_t>(std::min(page, recorded - first));
        auto page_json = captureRead(first, count, width); // owns the rows while they move
        for (auto &row : page_json["samples"]) { samples.push_back(std::move(row)); }
        first += count;
    }
    status["data"] = samples;
    return status;
}
} // namespace drivelink
