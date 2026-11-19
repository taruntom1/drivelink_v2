#include "format.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include <algorithm>
#include <string>
namespace cli
{
namespace
{
const char *captureState(CaptureState s)
{
    static constexpr const char *n[]{"idle", "armed", "triggered", "done"};
    return n[static_cast<uint8_t>(s)];
}
const char *testgenState(TestgenState s)
{
    static constexpr const char *n[]{"idle", "running", "done"};
    return n[static_cast<uint8_t>(s)];
}
const char *reason(TestgenReason s)
{
    static constexpr const char *n[]{"none", "completed", "stopped", "disarmed"};
    return n[static_cast<uint8_t>(s)];
}
const char *trigger(uint8_t s)
{
    static constexpr const char *n[]{"manual", "testgen_start", "value_change", "threshold"};
    return n[s];
}
const char *wave(uint8_t s)
{
    static constexpr const char *n[]{"step", "square", "ramp", "sine", "chirp_linear", "chirp_log"};
    return n[s];
}
} // namespace
Json toJson(const Value &v)
{
    return std::visit(
        [](const auto &x) -> Json {
            if constexpr (std::is_same_v<std::decay_t<decltype(x)>, EnumValue>)
            {
                return x.value;
            }
            else
            {
                return x;
            }
        },
        v);
}
Json valueJson(ValueType type, uint32_t bits)
{
    return toJson(decodeValue(type, bits));
}
Json toJson(const Ref &v)
{
    return {{"type", v.type}, {"instance", v.instance}, {"item", v.item}};
}
Json toJson(const EnumEntry &v)
{
    return {{"value", v.value}, {"name", v.name}};
}
Json toJson(const ScalarDescriptor &v)
{
    return {{"value_type", v.value_type}, {"name", v.name}, {"unit", v.unit}};
}
Json toJson(const TypeDescriptor &v)
{
    return {{"type", v.type}, {"loop_mask", v.loop_mask}, {"name", v.name}};
}
Json toJson(const ItemDescriptor &v)
{
    return {{"id", v.id},
            {"kind", v.kind},
            {"value_type", v.value_type},
            {"flags", v.flags},
            {"min_bits", v.min_bits},
            {"max_bits", v.max_bits},
            {"default_bits", v.default_bits},
            {"has_min", v.has_min},
            {"has_max", v.has_max},
            {"name", v.name},
            {"unit", v.unit},
            {"description", v.description},
            {"ref", v.ref},
            {"runs_in", v.runs_in},
            {"blob_direction", v.blob_direction},
            {"args", toJson(v.args)},
            {"results", toJson(v.results)},
            {"is_source", v.isSource()}};
}
Json toJson(const CodeDescriptor &v)
{
    return {{"source", v.source}, {"code", v.code},   {"severity", v.severity},
            {"kind", v.kind},     {"flags", v.flags}, {"name", v.name}};
}
Json toJson(const Instance &v)
{
    return {{"type", v.type}, {"instance", v.instance}, {"loop", v.loop}, {"state", v.state}, {"label", v.label}};
}
Json toJson(const LoopInfo &v)
{
    return {{"id", v.id}, {"name", v.name}, {"rate_hz", v.rate_hz}, {"flags", v.flags}, {"realtime", v.realtime()}};
}
Json toJson(const DeviceInfo &v)
{
    return {{"device_id", hex(v.device_id)},
            {"name", v.name},
            {"protocol_version", v.protocol_version},
            {"firmware_version", v.firmware_version},
            {"build_id", v.build_id},
            {"descriptor_hash", v.descriptor_hash},
            {"capture_capacity", v.capture_capacity},
            {"max_payload", v.max_payload},
            {"running_instances", v.running_instances},
            {"flags", v.flags},
            {"loops", toJson(v.loops)}};
}
Json toJson(const OperationResult &v)
{
    return {{"ok", v.ok}};
}
Json toJson(const SystemResult &v)
{
    return {{"ok", v.ok}, {"result", v.result}};
}
Json toJson(const LayoutResult &v)
{
    return {{"pending_reboot", v.pending_reboot}};
}
Json toJson(const AddedInstance &v)
{
    return {{"instance", v.instance}, {"pending_reboot", v.pending_reboot}};
}
Json toJson(const ParamResult &v)
{
    return {{"result", v.result}, {"pending_reboot", v.pending_reboot}, {"values", v.values}};
}
Json toJson(const ActionResult &v)
{
    return {{"values", v.values}, {"blob_hex", hex(v.blob)}};
}
Json toJson(const DiagInfo &v)
{
    return {{"flags", v.flags},
            {"uptime_us", v.uptime_us},
            {"reset_reason", v.reset_reason},
            {"instances_ok", v.instances_ok},
            {"instances_failed", v.instances_failed},
            {"safe_mode", v.safe_mode},
            {"records_dropped", v.records_dropped},
            {"events_tx_dropped", v.events_tx_dropped},
            {"firmware_version", v.firmware_version}};
}
Json toJson(const DiagEntry &v)
{
    return {{"source", v.source}, {"instance", v.instance}, {"code", v.code}, {"state", v.state}, {"count", v.count}};
}
Json toJson(const Group &v)
{
    return {{"kind", v.kind == GroupKind::Subscription ? "subscription" : "command"},
            {"id", v.id},
            {"channel", v.channel},
            {"configured", v.configured},
            {"enabled", v.enabled},
            {"loop", v.loop},
            {"epoch", v.epoch},
            {"divider", v.divider},
            {"refs", toJson(v.refs)}};
}
Json toJson(const GroupList &v)
{
    return {{"groups", toJson(v.groups)},
            {"subscription_capacity", v.subscription_capacity},
            {"command_capacity", v.command_capacity},
            {"entry_capacity", v.entry_capacity}};
}
Json toJson(const GroupMutation &v)
{
    return {{"epoch", v.epoch}, {"channel", v.channel}};
}
Json toJson(const CaptureTrigger &v)
{
    return {{"kind", trigger(v.kind)},
            {"ref", toJson(v.ref)},
            {"edge", v.edge ? "falling" : "rising"},
            {"value", v.value},
            {"kind_code", v.kind}};
}
Json toJson(const CaptureStatus &v)
{
    return {{"state", captureState(v.state)},
            {"configured", v.configured},
            {"irregular", v.irregular},
            {"loop", v.loop},
            {"divider", v.divider},
            {"samples", v.samples},
            {"pre_trigger", v.pre_trigger},
            {"trigger", toJson(v.trigger)},
            {"recorded", v.recorded},
            {"trigger_index", v.trigger_index ? Json(*v.trigger_index) : Json(nullptr)},
            {"period_us", v.period_us},
            {"sample_period_us", v.sample_period_us},
            {"trigger_timestamp_us", v.trigger_timestamp_us},
            {"refs", toJson(v.refs)},
            {"state_code", v.state}};
}
Json toJson(const CapturePage &v)
{
    return {{"first", v.first}, {"count", v.count}, {"samples", v.samples}};
}
Json toJson(const TestgenConfig &v)
{
    return {{"loop", v.loop},
            {"target", toJson(v.target)},
            {"waveform", wave(v.waveform)},
            {"offset", v.offset},
            {"amplitude", v.amplitude},
            {"frequency_start", v.frequency_start},
            {"frequency_end", v.frequency_end},
            {"duration_ms", v.duration_ms},
            {"repeat", v.repeat}};
}
Json toJson(const TestgenStarted &v)
{
    return {{"loop", v.loop}, {"run", v.run}};
}
Json toJson(const TestgenStatus &v)
{
    return {{"loop", v.loop},
            {"state", testgenState(v.state)},
            {"reason", reason(v.reason)},
            {"run", v.run},
            {"elapsed_ms", v.elapsed_ms},
            {"repetitions", v.repetitions},
            {"value", v.value},
            {"target", toJson(v.target)},
            {"waveform", wave(v.waveform)},
            {"offset", v.offset},
            {"amplitude", v.amplitude},
            {"frequency_start", v.frequency_start},
            {"frequency_end", v.frequency_end},
            {"duration_ms", v.duration_ms},
            {"repeat", v.repeat}};
}
Json toJson(const OtaStatus &v)
{
    return {{"state", v.state},
            {"running_slot", v.running_slot},
            {"boot_slot", v.boot_slot},
            {"pending_confirm", v.pending_confirm},
            {"size", v.size},
            {"offset", v.offset},
            {"capacity", v.capacity},
            {"chunk_bytes", v.chunk_bytes},
            {"window", v.window},
            {"backend_error", v.backend_error},
            {"stage", v.stage},
            {"sha256", hex(v.sha256)},
            {"elf_sha256", hex(v.elf_sha256)}};
}
Json toJson(const LoopOverruns &v)
{
    return {{"loop_id", v.loop_id}, {"overruns", v.overruns}};
}
Json toJson(const StatusFrame &v)
{
    return {{"epoch", v.epoch},
            {"counter", v.counter},
            {"timestamp_us", v.timestamp_us},
            {"arm_state", v.arm_state},
            {"flags", v.flags},
            {"active_errors", v.active_errors},
            {"cpu_load", v.cpu_load},
            {"overruns", toJson(v.overruns)},
            {"last_disarm_reason", v.last_disarm_reason},
            {"msg_type", 128}};
}
Json toJson(const CaptureDone &v)
{
    return {{"recorded", v.recorded},
            {"trigger_index", v.trigger_index},
            {"irregular", v.irregular},
            {"msg_type", 193},
            {"event", 1},
            {"name", "capture_done"}};
}
Json toJson(const TestgenDone &v)
{
    return {{"loop", v.loop},  {"run", v.run}, {"reason", reason(v.reason)},
            {"msg_type", 193}, {"event", 2},   {"name", "testgen_done"}};
}
Json toJson(const DiagRecord &v)
{
    return {{"sequence", v.sequence}, {"flags", v.flags},   {"timestamp_us", v.timestamp_us},
            {"severity", v.severity}, {"source", v.source}, {"instance", v.instance},
            {"code", v.code},         {"error", v.error},   {"arg0", v.arg0},
            {"arg1", v.arg1},         {"msg_type", 192},    {"kind", 1}};
}
Json toJson(const CounterDelta &v)
{
    return {{"source", v.source}, {"instance", v.instance}, {"code", v.code}, {"delta", v.delta}, {"total", v.total}};
}
Json toJson(const DiagCounters &v)
{
    return {{"sequence", v.sequence},       {"flags", v.flags}, {"timestamp_us", v.timestamp_us},
            {"entries", toJson(v.entries)}, {"msg_type", 192},  {"kind", 2}};
}
Json toJson(const DiagCleared &v)
{
    return {{"sequence", v.sequence},
            {"flags", v.flags},
            {"timestamp_us", v.timestamp_us},
            {"source", v.source},
            {"instance", v.instance},
            {"code", v.code},
            {"msg_type", 192},
            {"kind", 3}};
}
Json toJson(const BootEvent &v)
{
    return {{"sequence", v.sequence},
            {"flags", v.flags},
            {"timestamp_us", v.timestamp_us},
            {"reset_reason", v.reset_reason},
            {"instances_ok", v.instances_ok},
            {"instances_failed", v.instances_failed},
            {"safe_mode", v.safe_mode},
            {"msg_type", 192},
            {"kind", 4}};
}
Json toJson(const CaptureData &v)
{
    auto j = toJson(v.status);
    j["data"] = v.data;
    return j;
}
Json toJson(const SubscriptionSample &v)
{
    Json typed = Json::array();
    for (const auto &x : v.typed_values)
    {
        typed.push_back(toJson(x));
    }
    return {{"group", v.group},   {"epoch", v.epoch},     {"counter", v.counter}, {"timestamp_us", v.timestamp_us},
            {"values", v.values}, {"typed_values", typed}};
}
Json toJson(const Frame &v)
{
    return {{"msg_type", v.type}, {"payload_hex", hex(v.payload)}};
}
Json toJson(const Event &v)
{
    return std::visit([](const auto &x) { return toJson(x); }, v);
}
Json toJson(const agent::DeviceRecord &v)
{
    const bool has_id = std::any_of(v.device_id.begin(), v.device_id.end(), [](uint8_t b) { return b != 0; });
    return {{"alias", v.alias},
            {"online", v.online},
            {"device_id", has_id ? Json(agent::formatDeviceId(v.device_id)) : Json(nullptr)},
            {"name", v.name},
            {"endpoint", v.endpoint},
            {"clients", v.clients},
            {"holder_id", v.holder_id},
            {"holder_name", v.holder_name},
            {"holder_kind", v.holder_id ? Json(clientKindName(v.holder_kind)) : Json(nullptr)}};
}
std::string clientKindName(uint8_t kind)
{
    switch (static_cast<protocol::defs::AgentClientKind>(kind))
    {
    case protocol::defs::AgentClientKind::Gui: return "gui";
    case protocol::defs::AgentClientKind::Cli: return "cli";
    case protocol::defs::AgentClientKind::Ros: return "ros";
    default: return "other";
    }
}
Json toJson(const agent::ClientRecord &v)
{
    return {{"client_id", v.client_id}, {"name", v.name}, {"kind", clientKindName(v.kind)}, {"holder", v.holder}};
}
std::string clientLine(const agent::ClientRecord &v)
{
    return std::to_string(v.client_id) + ' ' + (v.name.empty() ? "-" : v.name) + " (" + clientKindName(v.kind) + ')' +
           (v.holder ? " [holder]" : "");
}
std::string deviceLine(const agent::DeviceRecord &v)
{
    const bool has_id = std::any_of(v.device_id.begin(), v.device_id.end(), [](uint8_t b) { return b != 0; });
    std::string name = "-";
    if (!v.name.empty())
    {
        name = "\"";
        for (const char c : v.name)
        {
            if (c == '"' || c == '\\') { name += '\\'; }
            name += c;
        }
        name += '"';
    }
    return v.alias + (v.online ? " online " : " offline ") + (has_id ? agent::formatDeviceId(v.device_id) : "-") + ' ' +
           name + ' ' + v.endpoint + " clients=" + std::to_string(v.clients) +
           (v.holder_id ? " holder=" + std::to_string(v.holder_id) + ':' + (v.holder_name.empty() ? "-" : v.holder_name) +
                                  " (" + clientKindName(v.holder_kind) + ')'
                            : "");
}
} // namespace cli
