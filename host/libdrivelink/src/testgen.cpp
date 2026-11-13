#include "drivelink/client.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include <cmath>

namespace drivelink {
namespace {
using protocol::wire::Reader;
using protocol::wire::Writer;
namespace defs = protocol::defs;
constexpr uint8_t kFamily = static_cast<uint8_t>(defs::Family::Testgen);
constexpr const char *kWaveforms[]{"step", "square", "ramp", "sine", "chirp_linear", "chirp_log"};
constexpr const char *kStates[]{"idle", "running", "done"};
constexpr const char *kReasons[]{"none", "completed", "stopped", "disarmed"};
[[noreturn]] void malformed() { throw Error(ErrorKind::Protocol, "malformed testgen response"); }
void exact(const Reader &r) { if (!r.done()) { malformed(); } }
uint8_t op(defs::TestgenOp o) { return static_cast<uint8_t>(o); }
}
uint8_t parseTestgenWaveform(const std::string &text)
{
    for (uint8_t i=0; i<6; ++i) { if (text == kWaveforms[i]) { return i; } }
    throw Error(ErrorKind::Usage, "unknown testgen waveform: " + text);
}
Json Client::testgenStart(const TestgenConfig &c)
{
    if (c.loop == 255 || c.waveform > 5 || !c.duration_ms || !std::isfinite(c.offset) || !std::isfinite(c.amplitude) ||
        !std::isfinite(c.frequency_start) || !std::isfinite(c.frequency_end)) {
        throw Error(ErrorKind::Usage, "invalid testgen configuration");
    }
    Bytes body(30); Writer w(body.data(), body.size());
    w.appendU8(c.loop); w.appendU8(c.target.type); w.appendU8(c.target.instance); w.appendU16Le(c.target.item);
    w.appendU8(c.waveform); w.appendU32Le(defs::f32Bits(c.offset)); w.appendU32Le(defs::f32Bits(c.amplitude));
    w.appendU32Le(defs::f32Bits(c.frequency_start)); w.appendU32Le(defs::f32Bits(c.frequency_end));
    w.appendU32Le(c.duration_ms); w.appendU32Le(c.repeat);
    const auto reply = accepted(kFamily, op(defs::TestgenOp::Start), body);
    Reader r(reply.body.data(), reply.body.size()); const auto run = r.readU32Le(); exact(r);
    return {{"loop", c.loop}, {"run", run}};
}
Json Client::testgenStop(uint8_t loop)
{
    if (loop == 255) { throw Error(ErrorKind::Usage, "invalid loop"); }
    if (!accepted(kFamily, op(defs::TestgenOp::Stop), Bytes{loop}).body.empty()) { malformed(); }
    return {{"ok", true}};
}
Json Client::testgenStatus(uint8_t loop)
{
    if (loop == 255) { throw Error(ErrorKind::Usage, "invalid loop"); }
    const auto reply = accepted(kFamily, op(defs::TestgenOp::Status), Bytes{loop});
    Reader r(reply.body.data(), reply.body.size());
    const auto received_loop = r.readU8(), state = r.readU8(), reason = r.readU8();
    if (received_loop != loop || state > 2 || reason > 3 ||
        ((state == 2) != (reason != 0))) { malformed(); }
    Json j{{"loop", loop}, {"state", kStates[state]}, {"reason", kReasons[reason]}};
    j["run"] = r.readU32Le(); j["elapsed_ms"] = r.readU32Le(); j["repetitions"] = r.readU32Le();
    const float value = defs::f32FromBits(r.readU32Le());
    j["value"] = value;
    j["target"] = {{"type", r.readU8()}, {"instance", r.readU8()}, {"item", r.readU16Le()}};
    const auto waveform = r.readU8(); if (waveform > 5) { malformed(); }
    j["waveform"] = kWaveforms[waveform];
    for (const auto *key : {"offset", "amplitude", "frequency_start", "frequency_end"}) {
        const float x = defs::f32FromBits(r.readU32Le()); if (!std::isfinite(x)) { malformed(); }
        j[key] = x;
    }
    j["duration_ms"] = r.readU32Le(); j["repeat"] = r.readU32Le();
    exact(r); if (!std::isfinite(value)) { malformed(); }
    return j;
}
}
