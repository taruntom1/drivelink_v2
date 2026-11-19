#include "format.hpp"
#include "drivelink/ota.hpp"
#include "drivelink/protocol/defs.hpp"
#include "drivelink/protocol/ota_image.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <thread>
#include <csignal>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace
{
using namespace drivelink;
using cli::Json;
using cli::toJson;
using cli::valueJson;
volatile std::sig_atomic_t interrupted = 0;
struct InterruptScope {
    using Handler = void (*)(int);
    Handler previous;
    InterruptScope() : previous(std::signal(SIGINT, [](int) { interrupted = 1; })) { interrupted = 0; }
    ~InterruptScope() { std::signal(SIGINT, previous); }
};
constexpr const char *kHelp = R"(drivelink-cli --endpoint tcp://127.0.0.1:8765|serial://COM3 [--json]
  [--baud 921600] [--timeout-ms 1500] COMMAND [ARGS]
  [--device ALIAS|ID12 --token TOKEN [--client-name NAME]]   (through drivelink-agent)
  [--lease]                    Through an agent: take the control lease first (needs --device)
  agent-devices                Through an agent: list its devices and lease holders (needs --token, no --device)
  agent-clients                Through an agent: list the device's clients and the lease holder (needs --device)
  time-ping | time-sync [DURATION_S]
  ota-status | ota-abort | ota-confirm
  ota-upload IMAGE [--upload-only | --no-confirm] [--window 1..4]
  ota-begin SIZE SHA256_HEX | ota-write OFFSET CHUNK_HEX | ota-finish
  info | types | items TYPE | enums TYPE ITEM | codes
  instances [pending] | add TYPE LABEL [LOOP] | remove TYPE INSTANCE
  label TYPE INSTANCE LABEL | discard
  name [TEXT]                  Read or rename device (empty TEXT clears)
  get REF... | get-pending REF... | set REF=VALUE...
  action REF [ARG...] [--blob HEX]
  save | revert | factory-reset | reboot | exit-safe-mode
  arm | disarm | ack-fault
  status [COUNT] | events [COUNT] | diag-info | diag-entries | diag-clear
  groups | sub-config GROUP LOOP DIVIDER [REF...] | cmd-config GROUP LOOP [REF...]
  sub-enable GROUP true|false | sub-clear GROUP | cmd-clear GROUP
  sub-stream GROUP [COUNT] | cmd-send GROUP COUNTER [VALUE...]
  sub-csv GROUP COUNT | cmd-run GROUP PERIOD_MS COUNT [VALUE...]
  keepalive GROUP PERIOD_MS COUNT
  bench-stall LOOP DURATION_MS (bench-enabled firmware only)
  capture-config LOOP DIVIDER SAMPLES PRE_TRIGGER TRIGGER REF...
    TRIGGER: manual | value_change REF | threshold REF rising|falling VALUE
  capture-arm | capture-trigger | capture-abort | capture-status
  capture-wait [TIMEOUT_MS]   (until done: the CaptureDone event, if seen, and STATUS)
  capture-read [--csv]   (a done capture; CSV time is relative to the trigger)
  testgen-start LOOP REF WAVE OFFSET AMPLITUDE F0 F1 DURATION_MS REPEAT
  testgen-stop LOOP | testgen-status LOOP | testgen-wait LOOP [TIMEOUT_MS]
  testgen-run GROUP PERIOD_MS TIMEOUT_MS LOOP REF WAVE OFFSET AMPLITUDE F0 F1 DURATION_MS REPEAT
    WAVE: step | square | ramp | sine | chirp_linear | chirp_log
    Use zero for unused frequencies. REPEAT=0 runs until stopped/disarmed.
    testgen-run arms, feeds keepalives, runs the test, and disarms on exit.
REF is TYPE:INSTANCE:ITEM; IDs accept decimal or 0xHEX.
Values use device descriptors: true/false, integers, floats, enum names.
Writes are never retried. A write timeout has an unknown outcome.
JSON is one envelope per command or one per watched event (NDJSON).
Through an agent: --endpoint tcp://HOST:47620 --device NAME_OR_12HEX --token TOKEN.
The token may come from DRIVELINK_AGENT_TOKEN; --client-name defaults to drivelink-cli.
--lease takes the device's control lease (preempting a holder) before the command; it
ends when this process exits, and the agent then disarms the device and clears this
client's command groups. Exclusive operations (arm, save, reboot, ...) need the lease.
Exit: 0 success, 1 internal, 2 usage, 3 connection, 4 timeout, 5 rejection, 6 protocol.
)";
[[noreturn]] void usage(const std::string &message)
{
    throw Error(ErrorKind::Usage, message);
}
void readable(const Json &j, const std::string &prefix = "")
{
    if (j.is_object())
    {
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            if (it.value().is_structured())
            {
                std::cout << prefix << it.key() << ":\n";
                readable(it.value(), prefix + "  ");
            }
            else
            {
                std::cout << prefix << it.key() << ": "
                          << (it.value().is_string() ? it.value().get<std::string>() : it.value().dump())
                          << '\n';
            }
        }
    }
    else if (j.is_array())
    {
        if (j.empty())
        {
            std::cout << prefix << "(empty)\n";
        }
        for (size_t i = 0; i < j.size(); ++i)
        {
            std::cout << prefix << '[' << i << "]\n";
            readable(j[i], prefix + "  ");
        }
    }
    else
    {
        std::cout << prefix << j.dump() << '\n';
    }
}
void emit(const Json &j, bool json)
{
    if (json)
    {
        std::cout << Json{{"ok", true}, {"data", j}}.dump() << std::endl;
    }
    else
    {
        readable(j);
    }
}
Json run(Client &client, Session &session, const std::vector<std::string> &args, bool json)
{
    const auto &cmd = args[0];
    auto count = [&](size_t n)
    {
        if (args.size() != n + 1)
        {
            usage("wrong argument count for " + cmd);
        }
    };
    auto id = [&](size_t i, uint32_t max)
    {
        if (i >= args.size())
        {
            usage("missing argument");
        }
        return parseUnsigned(args[i], max);
    };
    if (cmd == "info")
    {
        count(0);
        return toJson(client.info());
    }
    if (cmd == "name")
    {
        if (args.size() > 2) { usage("name takes zero or one argument"); }
        if (args.size() == 2) { (void)client.setName(args[1]); }
        return Json{{"name", client.info().name}};
    }
    if (cmd=="ota-status") { count(0);return toJson(client.otaStatus()); }
    if (cmd=="ota-abort") { count(0);return Json{{"aborted",client.otaAbort().ok}}; }
    if (cmd=="ota-confirm") { count(0);return Json{{"confirmed",client.otaConfirm().ok}}; }
    if (cmd=="ota-finish") { count(0);return Json{{"finished",client.otaFinish().ok}}; }
    if (cmd=="ota-begin") {
        count(2);const auto bytes=parseHex(args[2]);if (bytes.size()!=32) { usage("OTA SHA256 must be 32 bytes"); }
        protocol::Sha256Digest digest{};std::copy(bytes.begin(),bytes.end(),digest.begin());
        return {{"offset",client.otaBegin(id(1,protocol::kOtaSlotBytes),digest)}};
    }
    if (cmd=="ota-write") { count(2);return {{"offset",client.otaWrite(id(1,protocol::kOtaSlotBytes),parseHex(args[2]))}}; }
    if (cmd == "types")
    {
        count(0);
        return toJson(client.types());
    }
    if (cmd == "items")
    {
        count(1);
        return toJson(client.items(static_cast<uint8_t>(id(1, 255))));
    }
    if (cmd == "enums")
    {
        count(2);
        return toJson(client.enums(static_cast<uint8_t>(id(1, 255)), static_cast<uint16_t>(id(2, 65535))));
    }
    if (cmd == "codes")
    {
        count(0);
        return toJson(client.codes());
    }
    if (cmd == "instances")
    {
        if (args.size() > 2 || (args.size() == 2 && args[1] != "pending"))
        {
            usage("instances [pending]");
        }
        return toJson(client.instances(args.size() == 2));
    }
    if (cmd == "add")
    {
        if (args.size() != 3 && args.size() != 4)
        {
            usage("add TYPE LABEL [LOOP]");
        }
        return toJson(client.add(static_cast<uint8_t>(id(1, 255)), args[2],
                          static_cast<uint8_t>(args.size() == 4 ? id(3, 255) : 255)));
    }
    if (cmd == "remove")
    {
        count(2);
        return toJson(client.remove(static_cast<uint8_t>(id(1, 255)), static_cast<uint8_t>(id(2, 254))));
    }
    if (cmd == "label")
    {
        count(3);
        return toJson(client.label(static_cast<uint8_t>(id(1, 255)), static_cast<uint8_t>(id(2, 254)), args[3]));
    }
    if (cmd == "discard")
    {
        count(0);
        return toJson(client.discard());
    }
    const std::map<std::string, uint8_t> ops{{"arm", 2}, {"disarm", 3}, {"ack-fault", 4}, {"save", 5}, {"revert", 6}, {"factory-reset", 7},
                                             {"reboot", 8}, {"exit-safe-mode", 9}, {"bench-crash", 254}};
    if (auto it = ops.find(cmd); it != ops.end())
    {
        count(0);
        return toJson(client.system(it->second));
    }
    if (cmd == "diag-info")
    {
        count(0);
        return toJson(client.diagInfo());
    }
    if (cmd == "bench-stall") { count(2); return toJson(client.benchStall(static_cast<uint8_t>(id(1,254)),static_cast<uint16_t>(id(2,10000)))); }
    if (cmd == "diag-entries")
    {
        count(0);
        return toJson(client.diagEntries());
    }
    if (cmd == "diag-clear")
    {
        count(0);
        return toJson(client.diagClear());
    }
    if (cmd == "get" || cmd == "get-pending" || cmd == "set")
    {
        if (args.size() < 2 || args.size() > 65)
        {
            usage("batch requires 1..64 refs");
        }
        std::vector<Ref> refs;
        std::vector<uint32_t> values;
        std::vector<ItemDescriptor> descriptors;
        for (size_t i = 1; i < args.size(); ++i)
        {
            const auto equal = args[i].find('=');
            if ((cmd == "set") != (equal != std::string::npos))
            {
                usage("set requires REF=VALUE; get requires REF");
            }
            const auto ref = parseRef(args[i].substr(0, equal));
            const auto d = client.item(ref);
            if (d.kind != ItemKind::Param)
            {
                usage("get/set accepts parameter items");
            }
            refs.push_back(ref);
            descriptors.push_back(d);
            if (cmd == "set")
            {
                auto enums = d.value_type == ValueType::Enum ? client.enums(ref.type, ref.item) : std::vector<EnumEntry>{};
                values.push_back(parseValue(d, args[i].substr(equal + 1), enums));
            }
        }
        auto result = cmd == "set" ? client.set(refs, values) : client.get(refs, cmd == "get-pending");
        Json output=toJson(result);
        output["typed_values"] = Json::array();
        for (size_t i = 0; i < refs.size(); ++i)
        {
            output["typed_values"].push_back(valueJson(descriptors[i].value_type, result.values[i]));
        }
        return output;
    }
    if (cmd == "action")
    {
        if (args.size() < 2)
        {
            usage("action REF [ARG...] [--blob HEX]");
        }
        auto ref = parseRef(args[1]);
        auto d = client.item(ref);
        if (d.kind != ItemKind::Action)
        {
            usage("target must be an action");
        }
        std::array<uint32_t, 4> values{};
        size_t index = 0;
        Bytes blob;
        for (size_t i = 2; i < args.size(); ++i)
        {
            if (args[i] == "--blob")
            {
                if (++i >= args.size() || !blob.empty())
                {
                    usage("--blob requires hex bytes once");
                }
                blob = parseHex(args[i]);
            }
            else
            {
                if (index >= d.args.size() || index >= 4)
                {
                    usage("too many action arguments");
                }
                values[index] = parseValue(d.args[index], args[i]);
                ++index;
            }
        }
        if (index != d.args.size())
        {
            usage("wrong action argument count");
        }
        auto result = client.action(ref, values, blob);
        Json output=toJson(result);
        output["typed_values"] = Json::array();
        for (size_t i = 0; i < d.results.size(); ++i)
        {
            output["typed_values"].push_back(valueJson(d.results[i].value_type, result.values[i]));
        }
        return output;
    }
    if (cmd=="time-ping" || cmd=="time-sync") {
        if(args.size()>(cmd=="time-ping"?1U:2U)) { usage("time-ping | time-sync [DURATION_S]"); }
        const auto duration=cmd=="time-ping"?0U:(args.size()==2?id(1,3600):180U);
        if(cmd=="time-sync" && !duration) { usage("duration must be positive"); }
        ClockEstimator estimator;Json samples=Json::array();unsigned timeouts=0;
        InterruptScope interrupt_scope;
        const auto start=std::chrono::steady_clock::now();auto next=start;
        do {
            try {
                const auto s=session.timePing();
                double rtt=0,device=0,host=0;const auto valid=ClockEstimator::measure(s,rtt,device,host);
                const auto before=estimator.model(s.host_rx_us);
                const auto prediction=before.drift_valid?before.map(s.esp_rx_us):0.0;
                const bool accepted=estimator.add(s);
                const auto m=estimator.model(s.host_rx_us);
                // Through an agent the sample measures the agent's clock (esp_* are agent
                // times): report whether the composed device clock is valid, map nothing.
                const bool via_agent=session.agentHello().has_value();
                auto mapped=mapTime(session.clockModel(),s.esp_rx_us,s.boot_generation);
                if(session.generation()!=s.boot_generation) {mapped.valid=false;}
                samples.push_back({{"id",s.id},{"host_tx_us",s.host_tx_us},{"esp_rx_us",s.esp_rx_us},
                    {"esp_tx_us",s.esp_tx_us},{"host_rx_us",s.host_rx_us},{"boot_generation",s.boot_generation},
                    {"accepted",accepted},{"rtt_us",valid?Json(rtt):Json(nullptr)},
                    {"prior_prediction_us",before.drift_valid?Json(prediction):Json(nullptr)}, {"prior_slope",before.slope},
                    {"model",{{"offset_valid",m.offset_valid},{"drift_valid",m.drift_valid},
                        {"device_reference_us",m.device_reference_us},{"host_reference_us",m.host_reference_us},
                        {"slope",m.slope},{"drift_ppm",m.drift_ppm},{"samples",m.samples},
                        {"span_us",m.span_us},{"updated_us",m.updated_us},{"age_us",m.age_us},{"residual_us",m.residual_us},
                        {"min_rtt_us",m.min_rtt_us},{"max_rtt_us",m.max_rtt_us}}}});
                if(json) {
                    auto &sample=samples.back();sample["mapping_valid"]=mapped.valid;sample["mapping_generation"]=mapped.generation;
                    sample["mapped_host_us"]=mapped.valid && !via_agent?Json(mapped.host_us):Json(nullptr);
                    sample["mapped_uncertainty_us"]=mapped.valid && !via_agent?Json(mapped.uncertainty_us):Json(nullptr);
                }
            } catch(const Error &e) { if(e.kind!=ErrorKind::Timeout || !duration) { throw; } ++timeouts; }
            if(!duration || interrupted) { break; }
            const auto elapsed=std::chrono::steady_clock::now()-start;
            next=std::max(next,std::chrono::steady_clock::now())+
                (elapsed<std::chrono::seconds(5)?std::chrono::milliseconds(100):std::chrono::milliseconds(1000));
            const auto end=start+std::chrono::seconds(duration);
            if(next>=end) { break; }
            std::this_thread::sleep_until(next);
        } while(!interrupted);
        return {{"samples",samples},{"timeouts",timeouts},{"dropped_events",session.droppedEvents()}};
    }
    if (cmd == "groups") { count(0); return toJson(client.groups()); }
    if (cmd == "sub-config" || cmd == "cmd-config") {
        const bool sub = cmd == "sub-config";
        const size_t first = sub ? 4 : 3;
        if (args.size() < first) { usage("missing group configuration arguments"); }
        const auto group = static_cast<uint8_t>(id(1, 255));
        const auto loop = static_cast<uint8_t>(id(2, 254));
        std::vector<Ref> refs;
        for (size_t i = first; i < args.size(); ++i) { refs.push_back(parseRef(args[i])); }
        return toJson(sub ? client.subConfigure(group, loop, static_cast<uint16_t>(id(3, 65535)), refs) :
            client.cmdConfigure(group, loop, refs));
    }
    if (cmd == "sub-enable") {
        count(2);
        if (args[2] != "true" && args[2] != "false") { usage("sub-enable expects true or false"); }
        return toJson(client.subEnable(static_cast<uint8_t>(id(1, 255)), args[2] == "true"));
    }
    if (cmd == "sub-clear" || cmd == "cmd-clear") {
        count(1); return toJson(client.groupClear(static_cast<uint8_t>(id(1, 255)), cmd == "cmd-clear"));
    }
    if (cmd == "sub-stream" || cmd == "sub-csv" || cmd == "cmd-send" || cmd == "cmd-run" || cmd == "keepalive") {
        const bool repeat = cmd == "cmd-run" || cmd == "keepalive";
        const bool csv = cmd == "sub-csv";
        const bool send = cmd == "cmd-send" || repeat;
        if (csv && json) { usage("sub-csv writes CSV; omit --json"); }
        if ((repeat && args.size() < 4) || (cmd == "keepalive" && args.size() != 4) ||
            (send && !repeat && args.size() < 3) || (!send && (args.size() < 2 || args.size() > 3))) {
            usage(send ? "cmd-send GROUP COUNTER [VALUE...]" : "sub-stream GROUP [COUNT]");
        }
        const auto group_id = id(1, 255);
        Group group;
        const auto listing = client.groups();
        for (const auto &g : listing.groups) {
            if (g.id == group_id && g.kind == (send ? GroupKind::Command : GroupKind::Subscription)) { group = g; break; }
        }
        if (!group.configured) { usage("group is not configured"); }
        std::vector<ItemDescriptor> descriptors;
        for (const auto &ref : group.refs) { descriptors.push_back(client.item(ref)); }
        if (send) {
            const size_t first = repeat ? 4 : 3;
            if (args.size() != first && args.size() - first != descriptors.size()) { usage("command values must match the configured refs"); }
            std::vector<uint32_t> values;
            for (size_t i = first; i < args.size(); ++i) {
                const auto &d = descriptors[i - first];
                const auto &ref = group.refs[i - first];
                const auto enums = d.value_type == ValueType::Enum ? client.enums(ref.type,ref.item) : std::vector<EnumEntry>{};
                values.push_back(parseValue(d, args[i], enums));
            }
            if (repeat) {
                InterruptScope interrupt_scope;
                const auto period = id(2, 1000), n = id(3, 1000000);
                const Ref watchdog{0,0,4}; const auto limit = client.get({&watchdog,1}).values[0];
                if (!period || !n || period * 2 >= limit) { usage("period must be less than half watchdog_ms, and count must be positive"); }
                bool owns_arm = false;
                try {
                    if (cmd == "cmd-run") { owns_arm = true; client.arm(); }
                    auto next = std::chrono::steady_clock::now();
                    uint32_t sent = 0;
                    while (sent < n && !interrupted) {
                        if (!client.command(static_cast<uint8_t>(group_id), group.epoch, static_cast<uint16_t>(sent), values)) {
                            throw Error(ErrorKind::Connection, "command stream queue full");
                        }
                        ++sent;
                        next += std::chrono::milliseconds(period); std::this_thread::sleep_until(next);
                    }
                    if (owns_arm) { client.disarm(); } else { client.info(); }
                    return {{"sent", sent}, {"interrupted", interrupted != 0}};
                } catch (...) {
                    if (owns_arm) { try { client.disarm(); } catch (...) {} }
                    throw;
                }
            }
            if (!client.command(static_cast<uint8_t>(group_id), group.epoch, static_cast<uint16_t>(id(2, 65535)), values)) {
                throw Error(ErrorKind::Connection, "command stream queue full");
            }
            // An ordered read drains the frame before this short-lived session
            // closes. The command stream itself has no acknowledgement.
            client.info(); return {{"sent", true}, {"epoch", group.epoch}};
        }
        const auto n = args.size() == 3 ? id(2, 1000000) : 1;
        if (csv) {
            std::cout << "timestamp_us,counter";
            for (const auto &ref : group.refs) {
                std::cout << ',' << unsigned(ref.type) << ':' << unsigned(ref.instance) << ':' << unsigned(ref.item);
            }
            std::cout << '\n';
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (uint32_t i = 0; i < n;) {
            Frame frame;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0 || !session.nextEvent(frame, remaining)) { throw Error(ErrorKind::Timeout, "no subscription sample received"); }
            if (frame.type != group.channel) { continue; }
            auto sample = decodeSubscription(frame, group, descriptors);
            if (!sample) { continue; }
            if (csv) {
                std::cout << sample->timestamp_us << ',' << sample->counter;
                for (const auto &value : sample->typed_values) { std::cout << ',' << toJson(value); }
                std::cout << '\n';
            } else { emit(toJson(*sample), json); }
            ++i;
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        return nullptr;
    }
    if (cmd == "testgen-start" || cmd == "testgen-run") {
        const bool run = cmd == "testgen-run";
        const size_t first = run ? 4 : 1;
        if (args.size() != first + 9) { usage("testgen expects LOOP REF WAVE OFFSET AMPLITUDE F0 F1 DURATION_MS REPEAT"); }
        TestgenConfig c;
        c.loop = static_cast<uint8_t>(id(first, 254)); c.target = parseRef(args[first+1]);
        c.waveform = parseTestgenWaveform(args[first+2]);
        const auto scalar=ValueType::F32; // f32; parseValue rejects nonfinite/overflow/trailing data
        c.offset = protocol::defs::f32FromBits(parseValue(scalar, args[first+3]));
        c.amplitude = protocol::defs::f32FromBits(parseValue(scalar, args[first+4]));
        c.frequency_start = protocol::defs::f32FromBits(parseValue(scalar, args[first+5]));
        c.frequency_end = protocol::defs::f32FromBits(parseValue(scalar, args[first+6]));
        c.duration_ms = id(first+7, UINT32_MAX); c.repeat = id(first+8, UINT32_MAX);
        if (!run) { return toJson(client.testgenStart(c)); }
        const auto group_id = static_cast<uint8_t>(id(1, 255));
        const auto period = id(2, 1000), timeout = id(3, 3600000);
        const Ref watchdog{0,0,4}; const auto limit = client.get({&watchdog,1}).values[0];
        if (!period || !timeout || period*2 >= limit) { usage("positive timeout/period required; period must be less than half watchdog_ms"); }
        Group group;
        const auto listing = client.groups();
        for (const auto &g : listing.groups) {
            if (g.kind == GroupKind::Command && g.id == group_id) { group = g; }
        }
        if (!group.configured) { usage("command group is not configured"); }
        InterruptScope interrupt_scope;
        bool owns_arm = false;
        try {
            owns_arm = true; // ARM may apply even if its reply is lost.
            client.arm();
            uint16_t counter = 0;
            const auto feed = [&] {
                if (!client.command(group_id, group.epoch, counter++)) { throw Error(ErrorKind::Connection, "keepalive queue full"); }
            };
            feed();
            const auto started = client.testgenStart(c);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
            auto next = std::chrono::steady_clock::now();
            std::optional<TestgenStatus> status;
            while (!interrupted) {
                feed(); status = client.testgenStatus(c.loop);
                if (status->run != started.run) { throw Error(ErrorKind::Protocol, "testgen run changed"); }
                if (status->state == TestgenState::Done) { break; }
                if (std::chrono::steady_clock::now() >= deadline) { throw Error(ErrorKind::Timeout, "testgen did not complete"); }
                next += std::chrono::milliseconds(period); std::this_thread::sleep_until(next);
            }
            client.disarm(); owns_arm = false;
            return {{"status", toJson(status)}, {"interrupted", interrupted != 0}};
        } catch (...) {
            if (owns_arm) { try { client.disarm(); } catch (...) {} }
            throw;
        }
    }
    if (cmd == "testgen-stop" || cmd == "testgen-status") {
        count(1); const auto loop = static_cast<uint8_t>(id(1, 254));
        return cmd == "testgen-stop" ? toJson(client.testgenStop(loop)) : toJson(client.testgenStatus(loop));
    }
    if (cmd == "testgen-wait") {
        if (args.size() < 2 || args.size() > 3) { usage("testgen-wait LOOP [TIMEOUT_MS]"); }
        const auto loop = static_cast<uint8_t>(id(1, 254));
        const auto timeout = args.size() == 3 ? id(2, 3600000) : 10000;
        auto status = client.testgenStatus(loop); const auto run = status.run;
        if (status.state == TestgenState::Idle) { usage("testgen is idle"); }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        Json done;
        while (status.state == TestgenState::Running) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) { throw Error(ErrorKind::Timeout, "testgen did not complete"); }
            Frame frame;
            if (session.nextEvent(frame, std::min(remaining, std::chrono::milliseconds(500))) &&
                frame.type == protocol::defs::msg::kCoreEvent) {
                auto event = client.event(frame);
                if (const auto *value=std::get_if<TestgenDone>(&event); value && value->loop==loop && value->run==run) { done=toJson(event); }
            }
            status = client.testgenStatus(loop);
            if (status.run != run) { throw Error(ErrorKind::Protocol, "testgen run changed"); }
        }
        return {{"status", toJson(status)}, {"event", done}};
    }
    if (cmd == "capture-config")
    {
        if (args.size() < 7) { usage("capture-config LOOP DIVIDER SAMPLES PRE_TRIGGER TRIGGER REF..."); }
        const auto loop = static_cast<uint8_t>(id(1, 254));
        const auto divider = static_cast<uint16_t>(id(2, 65535));
        const auto samples = id(3, UINT32_MAX), pre = id(4, UINT32_MAX);
        CaptureTrigger trigger;
        trigger.kind = parseCaptureTrigger(args[5]);
        size_t next = 6;
        if (trigger.kind == 2 || trigger.kind == 3)
        {
            if (args.size() < next + (trigger.kind == 3 ? 4U : 2U)) { usage("missing trigger arguments"); }
            trigger.ref = parseRef(args[next++]);
            if (trigger.kind == 3)
            {
                const auto &edge = args[next++];
                if (edge != "rising" && edge != "falling") { usage("edge must be rising or falling"); }
                trigger.edge = edge == "falling" ? 1 : 0;
                const auto d = client.item(trigger.ref);
                const auto enums = d.value_type == ValueType::Enum ? client.enums(trigger.ref.type, trigger.ref.item) : std::vector<EnumEntry>{};
                trigger.value = parseValue(d, args[next++], enums);
            }
        }
        std::vector<Ref> refs;
        for (; next < args.size(); ++next) { refs.push_back(parseRef(args[next])); }
        if (refs.empty()) { usage("capture-config needs at least one REF"); }
        return toJson(client.captureConfig(loop, divider, samples, pre, trigger, refs));
    }
    if (cmd == "capture-arm") { count(0); return toJson(client.captureArm()); }
    if (cmd == "capture-trigger") { count(0); return toJson(client.captureTrigger()); }
    if (cmd == "capture-abort") { count(0); return toJson(client.captureAbort()); }
    if (cmd == "capture-status") { count(0); return toJson(client.captureStatus()); }
    if (cmd == "capture-wait")
    {
        if (args.size() > 2) { usage("capture-wait [TIMEOUT_MS]"); }
        const auto timeout = std::chrono::milliseconds(args.size() == 2 ? id(1, 3600000) : 10000);
        // Events queue from connect, so one after this STATUS is never missed.
        auto status = client.captureStatus();
        Json event = nullptr;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto next_poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (status.state != CaptureState::Done)
        {
            if (status.state == CaptureState::Idle) { usage("capture is idle"); }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) { throw Error(ErrorKind::Timeout, "capture did not complete"); }
            // Re-read STATUS on the event, or every 500 ms so a lost event never
            // stalls the wait; other stream frames (status at 10 Hz) are skipped.
            Frame frame;
            const auto until_poll = std::chrono::duration_cast<std::chrono::milliseconds>(next_poll - std::chrono::steady_clock::now());
            const auto slice = std::max(std::chrono::milliseconds(1), std::min(remaining, until_poll));
            const bool got = session.nextEvent(frame, slice);
            if (got && frame.type == protocol::defs::msg::kCoreEvent) { event = toJson(client.event(frame)); }
            else if (std::chrono::steady_clock::now() < next_poll) { continue; }
            status = client.captureStatus();
            next_poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        }
        // STATUS can report done just before the device sends the event.
        const auto grace = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (event.is_null())
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(grace - std::chrono::steady_clock::now());
            Frame frame;
            if (left.count() <= 0 || !session.nextEvent(frame, left)) { break; }
            if (frame.type == protocol::defs::msg::kCoreEvent) { event = toJson(client.event(frame)); }
        }
        return {{"event", event}, {"status", toJson(status)}};
    }
    if (cmd == "capture-read")
    {
        if (args.size() > 2 || (args.size() == 2 && args[1] != "--csv")) { usage("capture-read [--csv]"); }
        client.info(); // learn the payload limit for paging
        auto data = client.captureData();
        std::vector<ItemDescriptor> descriptors;
        for (const auto &ref : data.status.refs) { descriptors.push_back(client.item(ref)); }
        const auto trigger_index = static_cast<int64_t>(data.status.trigger_index.value_or(0));
        const auto period = static_cast<int64_t>(data.status.sample_period_us);
        Json rows = Json::array();
        for (size_t i = 0; i < data.data.size(); ++i)
        {
            Json typed = Json::array();
            for (size_t k = 0; k < descriptors.size(); ++k)
            {
                typed.push_back(valueJson(descriptors[k].value_type, data.data[i][k]));
            }
            rows.push_back({{"index", i}, {"time_us", (static_cast<int64_t>(i) - trigger_index) * period},
                            {"typed_values", typed}});
        }
        if (args.size() == 2)
        {
            std::cout << "index,time_us";
            for (const auto &ref : data.status.refs) {
                std::cout << ',' << unsigned(ref.type) << ':' << unsigned(ref.instance) << ':' << unsigned(ref.item);
            }
            std::cout << '\n';
            for (const auto &row : rows)
            {
                std::cout << row["index"] << ',' << row["time_us"];
                for (const auto &value : row["typed_values"]) { std::cout << ',' << value; }
                std::cout << '\n';
            }
            return nullptr;
        }
        auto output=toJson(data);output["rows"] = rows;
        return output;
    }
    if (cmd == "status" || cmd == "events")
    {
        if (args.size() > 2)
        {
            usage("status/events [COUNT]");
        }
        const auto n = args.size() == 2 ? id(1, 1000000) : 1;
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        for (uint32_t i = 0; i < n;)
        {
            Frame frame;
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now());
            if (remaining.count()<=0 || !session.nextEvent(frame, remaining))
            {
                throw Error(ErrorKind::Timeout, "no stream/event received");
            }
            if (cmd == "status" && frame.type != protocol::defs::msg::kStatusStream)
            {
                continue;
            }
            emit(toJson(client.event(frame)), json);
            ++i;
            deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        }
        return nullptr;
    }
    usage("unknown command: " + cmd);
}
Json upload(const std::vector<std::string> &args,const std::string &endpoint,SessionOptions options)
{
    if (args.size()<2) { usage("ota-upload IMAGE [--upload-only | --no-confirm] [--window 1..4]"); }
    bool upload_only=false,no_confirm=false;uint8_t window=4;
    for (size_t i=2;i<args.size();++i) {
        if (args[i]=="--upload-only" && !upload_only) { upload_only=true; }
        else if (args[i]=="--no-confirm" && !no_confirm) { no_confirm=true; }
        else if (args[i]=="--window" && i+1<args.size()) { window=static_cast<uint8_t>(parseUnsigned(args[++i],4)); }
        else { usage("unknown/repeated OTA option"); }
    }
    if ((upload_only && no_confirm) || !window) { usage("invalid OTA options"); }
    const auto image=loadOtaImage(args[1]);
    InterruptScope interrupt;unsigned last_percent=101;
    OtaUpdateOptions update;update.upload_only=upload_only;update.confirm=!no_confirm;update.window=window;
    const auto result=otaUpdate(endpoint,options,image,update,[&](const OtaProgress &p) {
        const auto percent=100U*p.offset/p.size;
        if (percent/5!=last_percent/5) { std::cerr<<"OTA "<<percent<<"%\n";last_percent=percent; }
        return !interrupted;
    },[] { return interrupted!=0; });
    auto output=toJson(result.status);output["rebooted"]=result.rebooted;output["confirmed"]=result.confirmed;return output;
}
} // namespace
int main(int argc, char **argv)
{
    bool json = false;
    // Recognize JSON even when another argument is invalid.
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--json")
        {
            json = true;
        }
    }
    try
    {
#ifdef _WIN32
        // main's narrow argv follows the Windows ANSI code page. Device names
        // are UTF-8 even on machines whose system code page is not UTF-8.
        int wide_count = 0;
        const auto wide = CommandLineToArgvW(GetCommandLineW(), &wide_count);
        if (!wide) { throw std::runtime_error("cannot decode Windows command line"); }
        const auto release = [](wchar_t **p) { LocalFree(p); };
        const std::unique_ptr<wchar_t *, decltype(release)> owned(wide, release);
        std::vector<std::string> utf8;
        utf8.reserve(wide_count);
        for (int i = 0; i < wide_count; ++i) {
            const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1,
                                                nullptr, 0, nullptr, nullptr);
            if (!size) { usage("invalid Unicode command-line argument"); }
            std::string value(static_cast<size_t>(size), '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1,
                                     value.data(), size, nullptr, nullptr)) {
                usage("invalid Unicode command-line argument");
            }
            value.pop_back(); utf8.push_back(std::move(value));
        }
        std::vector<char *> pointers;
        for (auto &arg : utf8) { pointers.push_back(arg.data()); }
        argc = wide_count; argv = pointers.data();
#endif
        std::string endpoint, token, device, client_name = "drivelink-cli";
        bool have_token = false, lease = false;
        SessionOptions options;
        // Short CLI commands own their explicit time measurement; background
        // pings would alter recorded/replayed wire conversations.
        options.periodic_time=false;
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            auto value = [&]()
            {
                if (++i >= argc)
                {
                    usage("missing value for " + arg);
                }
                return std::string(argv[i]);
            };
            if (args.empty() && arg == "--json")
            {
                continue;
            }
            if (args.empty() && arg == "--endpoint")
            {
                endpoint = value();
            }
            else if (args.empty() && arg == "--baud")
            {
                options.baud = parseUnsigned(value(), 4000000);
            }
            else if (args.empty() && arg == "--token")
            {
                token = value();
                have_token = true;
            }
            else if (args.empty() && arg == "--device")
            {
                device = value();
                if (device.empty()) { usage("--device needs an alias or device id"); }
            }
            else if (args.empty() && arg == "--lease")
            {
                lease = true;
            }
            else if (args.empty() && arg == "--client-name")
            {
                client_name = value();
            }
            else if (args.empty() && arg == "--timeout-ms")
            {
                options.timeout = std::chrono::milliseconds(parseUnsigned(value(), 60000));
            }
            else if (args.empty() && arg == "--help")
            {
                if (json)
                {
                    emit({{"help", kHelp}}, true);
                }
                else
                {
                    std::cout << kHelp;
                }
                return 0;
            }
            else
            {
                args.push_back(arg);
            }
        }
        if (args.empty() || endpoint.empty())
        {
            usage("--endpoint and COMMAND required; use --help");
        }
        if (!have_token)
        {
            // The environment is only a fallback for an absent option.
            if (const char *env = std::getenv("DRIVELINK_AGENT_TOKEN")) { token = env; }
        }
        const bool tcp = endpoint.rfind("tcp://", 0) == 0;
        if (lease && device.empty()) { usage("--lease needs --device"); }
        if (args[0] == "agent-clients" && device.empty()) { usage("agent-clients needs --device"); }
        if (args[0] == "agent-devices")
        {
            if (args.size() != 1) { usage("agent-devices takes no arguments"); }
            if (!device.empty()) { usage("agent-devices lists devices; omit --device"); }
            if (!tcp) { usage("agent-devices needs a tcp:// agent endpoint"); }
            if (token.empty()) { usage("agent-devices needs --token or DRIVELINK_AGENT_TOKEN"); }
            const auto devices = listAgentDevices(endpoint, token, options.timeout, client_name);
            if (json)
            {
                Json list = Json::array();
                for (const auto &record : devices) { list.push_back(toJson(record)); }
                emit({{"devices", list}}, true);
            }
            else
            {
                for (const auto &record : devices) { std::cout << cli::deviceLine(record) << '\n'; }
            }
            return 0;
        }
        if (!device.empty())
        {
            if (!tcp) { usage("--device needs a tcp:// agent endpoint"); }
            if (token.empty()) { usage("--device needs --token or DRIVELINK_AGENT_TOKEN"); }
            options.agent = AgentHello{token, device, client_name};
        }
        if (args[0]=="ota-upload")
        {
            if (lease) { usage("--lease does not apply to ota-upload"); }
            emit(upload(args,endpoint,options),json);return 0;
        }
        Session session(endpoint, options);
        Client client(session);
        client.info();
        if (lease) { agentLeaseTake(session); }
        if (args[0] == "agent-clients")
        {
            if (args.size() != 1) { usage("agent-clients takes no arguments"); }
            const auto clients = agentClients(session);
            if (json)
            {
                Json list = Json::array();
                for (const auto &record : clients) { list.push_back(toJson(record)); }
                emit({{"clients", list}}, true);
            }
            else
            {
                for (const auto &record : clients) { std::cout << cli::clientLine(record) << '\n'; }
            }
            return 0;
        }
        auto result = run(client, session, args, json);
        if (!result.is_null())
        {
            emit(result, json);
        }
        return 0;
    }
    catch (const Error &e)
    {
        const int code = e.kind == ErrorKind::Usage        ? 2
                         : e.kind == ErrorKind::Connection ? 3
                         : e.kind == ErrorKind::Timeout    ? 4
                         : e.kind == ErrorKind::Rejected   ? 5
                                                           : 6;
        if (json)
        {
            std::cout << Json{{"ok", false},
                              {"error", e.what()},
                              {"exit_code", code},
                              {"result", e.result},
                              {"detail", e.detail}}
                             .dump()
                      << '\n';
        }
        else
        {
            std::cerr << "error: " << e.what() << '\n';
        }
        return code;
    }
    catch (const std::exception &e)
    {
        if (json)
        {
            std::cout << Json{{"ok", false}, {"error", e.what()}, {"exit_code", 1}}.dump() << '\n';
        }
        else
        {
            std::cerr << "error: " << e.what() << '\n';
        }
        return 1;
    }
}
