#include "drivelink/client.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>
#include <csignal>

namespace
{
using namespace drivelink;
volatile std::sig_atomic_t interrupted = 0;
struct InterruptScope {
    using Handler = void (*)(int);
    Handler previous;
    InterruptScope() : previous(std::signal(SIGINT, [](int) { interrupted = 1; })) { interrupted = 0; }
    ~InterruptScope() { std::signal(SIGINT, previous); }
};
constexpr const char *kHelp = R"(drivelink-cli --endpoint tcp://127.0.0.1:8765|serial://COM3 [--json]
  [--baud 921600] [--timeout-ms 1500] COMMAND [ARGS]
  info | types | items TYPE | enums TYPE ITEM | codes
  instances [pending] | add TYPE LABEL [LOOP] | remove TYPE INSTANCE
  label TYPE INSTANCE LABEL | discard
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
REF is TYPE:INSTANCE:ITEM; IDs accept decimal or 0xHEX.
Values use device descriptors: true/false, integers, floats, enum names.
Writes are never retried. A write timeout has an unknown outcome.
JSON is one envelope per command or one per watched event (NDJSON).
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
        return client.info();
    }
    if (cmd == "types")
    {
        count(0);
        return client.types();
    }
    if (cmd == "items")
    {
        count(1);
        return client.items(static_cast<uint8_t>(id(1, 255)));
    }
    if (cmd == "enums")
    {
        count(2);
        return client.enums(static_cast<uint8_t>(id(1, 255)), static_cast<uint16_t>(id(2, 65535)));
    }
    if (cmd == "codes")
    {
        count(0);
        return client.codes();
    }
    if (cmd == "instances")
    {
        if (args.size() > 2 || (args.size() == 2 && args[1] != "pending"))
        {
            usage("instances [pending]");
        }
        return client.instances(args.size() == 2);
    }
    if (cmd == "add")
    {
        if (args.size() != 3 && args.size() != 4)
        {
            usage("add TYPE LABEL [LOOP]");
        }
        return client.add(static_cast<uint8_t>(id(1, 255)), args[2],
                          static_cast<uint8_t>(args.size() == 4 ? id(3, 255) : 255));
    }
    if (cmd == "remove")
    {
        count(2);
        return client.remove(static_cast<uint8_t>(id(1, 255)), static_cast<uint8_t>(id(2, 254)));
    }
    if (cmd == "label")
    {
        count(3);
        return client.label(static_cast<uint8_t>(id(1, 255)), static_cast<uint8_t>(id(2, 254)), args[3]);
    }
    if (cmd == "discard")
    {
        count(0);
        return client.discard();
    }
    const std::map<std::string, uint8_t> ops{{"arm", 2}, {"disarm", 3}, {"ack-fault", 4}, {"save", 5}, {"revert", 6}, {"factory-reset", 7},
                                             {"reboot", 8}, {"exit-safe-mode", 9}, {"bench-crash", 254}};
    if (auto it = ops.find(cmd); it != ops.end())
    {
        count(0);
        return client.system(it->second);
    }
    if (cmd == "diag-info")
    {
        count(0);
        return client.diagInfo();
    }
    if (cmd == "bench-stall") { count(2); return client.benchStall(static_cast<uint8_t>(id(1,254)),static_cast<uint16_t>(id(2,10000))); }
    if (cmd == "diag-entries")
    {
        count(0);
        return client.diagEntries();
    }
    if (cmd == "diag-clear")
    {
        count(0);
        return client.diagClear();
    }
    if (cmd == "get" || cmd == "get-pending" || cmd == "set")
    {
        if (args.size() < 2 || args.size() > 65)
        {
            usage("batch requires 1..64 refs");
        }
        std::vector<Ref> refs;
        std::vector<uint32_t> values;
        std::vector<Json> descriptors;
        for (size_t i = 1; i < args.size(); ++i)
        {
            const auto equal = args[i].find('=');
            if ((cmd == "set") != (equal != std::string::npos))
            {
                usage("set requires REF=VALUE; get requires REF");
            }
            const auto ref = parseRef(args[i].substr(0, equal));
            const auto d = client.item(ref);
            if (d.at("kind") != 0)
            {
                usage("get/set accepts parameter items");
            }
            refs.push_back(ref);
            descriptors.push_back(d);
            if (cmd == "set")
            {
                auto enums = d.at("value_type") == 6 ? client.enums(ref.type, ref.item) : Json::array();
                values.push_back(parseValue(d, args[i].substr(equal + 1), enums));
            }
        }
        auto result = cmd == "set" ? client.set(refs, values) : client.get(refs, cmd == "get-pending");
        result["typed_values"] = Json::array();
        for (size_t i = 0; i < refs.size(); ++i)
        {
            result["typed_values"].push_back(valueJson(descriptors[i].at("value_type"), result["values"][i]));
        }
        return result;
    }
    if (cmd == "action")
    {
        if (args.size() < 2)
        {
            usage("action REF [ARG...] [--blob HEX]");
        }
        auto ref = parseRef(args[1]);
        auto d = client.item(ref);
        if (d.at("kind") != 3)
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
                if (index >= d.at("args").size() || index >= 4)
                {
                    usage("too many action arguments");
                }
                values[index] = parseValue(d["args"][index], args[i]);
                ++index;
            }
        }
        if (index != d.at("args").size())
        {
            usage("wrong action argument count");
        }
        auto result = client.action(ref, values, blob);
        result["typed_values"] = Json::array();
        for (size_t i = 0; i < d.at("results").size(); ++i)
        {
            result["typed_values"].push_back(valueJson(d["results"][i]["value_type"], result["values"][i]));
        }
        return result;
    }
    if (cmd == "groups") { count(0); return client.groups(); }
    if (cmd == "sub-config" || cmd == "cmd-config") {
        const bool sub = cmd == "sub-config";
        const size_t first = sub ? 4 : 3;
        if (args.size() < first) { usage("missing group configuration arguments"); }
        const auto group = static_cast<uint8_t>(id(1, 255));
        const auto loop = static_cast<uint8_t>(id(2, 254));
        std::vector<Ref> refs;
        for (size_t i = first; i < args.size(); ++i) { refs.push_back(parseRef(args[i])); }
        return sub ? client.subConfigure(group, loop, static_cast<uint16_t>(id(3, 65535)), refs) :
            client.cmdConfigure(group, loop, refs);
    }
    if (cmd == "sub-enable") {
        count(2);
        if (args[2] != "true" && args[2] != "false") { usage("sub-enable expects true or false"); }
        return client.subEnable(static_cast<uint8_t>(id(1, 255)), args[2] == "true");
    }
    if (cmd == "sub-clear" || cmd == "cmd-clear") {
        count(1); return client.groupClear(static_cast<uint8_t>(id(1, 255)), cmd == "cmd-clear");
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
        Json group;
        const auto listing = client.groups();
        for (const auto &g : listing["groups"]) {
            if (g["id"] == group_id && g["kind"] == (send ? "command" : "subscription")) { group = g; break; }
        }
        if (group.is_null() || !group["configured"].get<bool>()) { usage("group is not configured"); }
        Json descriptors = Json::array();
        for (const auto &ref : group["refs"]) { descriptors.push_back(client.item({ref["type"], ref["instance"], ref["item"]})); }
        if (send) {
            const size_t first = repeat ? 4 : 3;
            if (args.size() != first && args.size() - first != descriptors.size()) { usage("command values must match the configured refs"); }
            std::vector<uint32_t> values;
            for (size_t i = first; i < args.size(); ++i) {
                const auto &d = descriptors[i - first];
                const auto &ref = group["refs"][i - first];
                const Json enums = d["value_type"] == 6 ? client.enums(ref["type"], ref["item"]) : Json::array();
                values.push_back(parseValue(d, args[i], enums));
            }
            if (repeat) {
                InterruptScope interrupt_scope;
                const auto period = id(2, 1000), n = id(3, 1000000);
                const Ref watchdog{0,0,4}; const auto limit = client.get({&watchdog,1})["values"][0].get<uint32_t>();
                if (!period || !n || period * 2 >= limit) { usage("period must be less than half watchdog_ms, and count must be positive"); }
                bool owns_arm = false;
                try {
                    if (cmd == "cmd-run") { client.arm(); owns_arm = true; }
                    auto next = std::chrono::steady_clock::now();
                    uint32_t sent = 0;
                    while (sent < n && !interrupted) {
                        if (!client.command(static_cast<uint8_t>(group_id), group["epoch"], static_cast<uint16_t>(sent), values)) {
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
            if (!client.command(static_cast<uint8_t>(group_id), group["epoch"], static_cast<uint16_t>(id(2, 65535)), values)) {
                throw Error(ErrorKind::Connection, "command stream queue full");
            }
            // An ordered read drains the frame before this short-lived session
            // closes. The command stream itself has no acknowledgement.
            client.info(); return {{"sent", true}, {"epoch", group["epoch"]}};
        }
        const auto n = args.size() == 3 ? id(2, 1000000) : 1;
        if (csv) {
            std::cout << "timestamp_us,counter";
            for (const auto &ref : group["refs"]) {
                std::cout << ',' << ref["type"].get<unsigned>() << ':' << ref["instance"].get<unsigned>() << ':' << ref["item"].get<unsigned>();
            }
            std::cout << '\n';
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (uint32_t i = 0; i < n;) {
            Frame frame;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0 || !session.nextEvent(frame, remaining)) { throw Error(ErrorKind::Timeout, "no subscription sample received"); }
            if (frame.type != group["channel"]) { continue; }
            auto sample = decodeSubscription(frame, group, descriptors);
            if (sample.is_null()) { continue; }
            if (csv) {
                std::cout << sample["timestamp_us"] << ',' << sample["counter"];
                for (const auto &value : sample["typed_values"]) { std::cout << ',' << value; }
                std::cout << '\n';
            } else { emit(sample, json); }
            ++i;
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        return nullptr;
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
                const Json enums = d["value_type"] == 6 ? client.enums(trigger.ref.type, trigger.ref.item) : Json::array();
                trigger.value = parseValue(d, args[next++], enums);
            }
        }
        std::vector<Ref> refs;
        for (; next < args.size(); ++next) { refs.push_back(parseRef(args[next])); }
        if (refs.empty()) { usage("capture-config needs at least one REF"); }
        return client.captureConfig(loop, divider, samples, pre, trigger, refs);
    }
    if (cmd == "capture-arm") { count(0); return client.captureArm(); }
    if (cmd == "capture-trigger") { count(0); return client.captureTrigger(); }
    if (cmd == "capture-abort") { count(0); return client.captureAbort(); }
    if (cmd == "capture-status") { count(0); return client.captureStatus(); }
    if (cmd == "capture-wait")
    {
        if (args.size() > 2) { usage("capture-wait [TIMEOUT_MS]"); }
        const auto timeout = std::chrono::milliseconds(args.size() == 2 ? id(1, 3600000) : 10000);
        // Events queue from connect, so one after this STATUS is never missed.
        auto status = client.captureStatus();
        Json event = nullptr;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto next_poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (status["state"] != "done")
        {
            if (status["state"] == "idle") { usage("capture is idle"); }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) { throw Error(ErrorKind::Timeout, "capture did not complete"); }
            // Re-read STATUS on the event, or every 500 ms so a lost event never
            // stalls the wait; other stream frames (status at 10 Hz) are skipped.
            Frame frame;
            const auto until_poll = std::chrono::duration_cast<std::chrono::milliseconds>(next_poll - std::chrono::steady_clock::now());
            const auto slice = std::max(std::chrono::milliseconds(1), std::min(remaining, until_poll));
            const bool got = session.nextEvent(frame, slice);
            if (got && frame.type == protocol::defs::msg::kCoreEvent) { event = client.event(frame); }
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
            if (frame.type == protocol::defs::msg::kCoreEvent) { event = client.event(frame); }
        }
        return {{"event", event}, {"status", status}};
    }
    if (cmd == "capture-read")
    {
        if (args.size() > 2 || (args.size() == 2 && args[1] != "--csv")) { usage("capture-read [--csv]"); }
        client.info(); // learn the payload limit for paging
        auto data = client.captureData();
        Json descriptors = Json::array();
        for (const auto &ref : data["refs"]) { descriptors.push_back(client.item({ref["type"], ref["instance"], ref["item"]})); }
        const auto trigger_index = data["trigger_index"].get<int64_t>();
        const auto period = data["sample_period_us"].get<int64_t>();
        Json rows = Json::array();
        for (size_t i = 0; i < data["data"].size(); ++i)
        {
            Json typed = Json::array();
            for (size_t k = 0; k < descriptors.size(); ++k)
            {
                typed.push_back(valueJson(descriptors[k]["value_type"], data["data"][i][k]));
            }
            rows.push_back({{"index", i}, {"time_us", (static_cast<int64_t>(i) - trigger_index) * period},
                            {"typed_values", typed}});
        }
        if (args.size() == 2)
        {
            std::cout << "index,time_us";
            for (const auto &ref : data["refs"]) {
                std::cout << ',' << ref["type"].get<unsigned>() << ':' << ref["instance"].get<unsigned>() << ':' << ref["item"].get<unsigned>();
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
        data["rows"] = rows;
        return data;
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
            emit(client.event(frame), json);
            ++i;
            deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        }
        return nullptr;
    }
    usage("unknown command: " + cmd);
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
        std::string endpoint;
        SessionOptions options;
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
        Session session(endpoint, options);
        Client client(session);
        client.info();
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
