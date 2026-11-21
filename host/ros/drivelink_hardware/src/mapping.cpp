#include "drivelink_hardware/mapping.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <string_view>
#include <system_error>

namespace drivelink_hardware
{
namespace
{
constexpr size_t kMaxGroupRefs = 64; // drivelink::GroupSample::values
constexpr unsigned kLayoutPendingReboot = 0x01;
constexpr unsigned kLayoutInvalid = 0x02;

std::string join(const std::vector<std::string> &items)
{
    if (items.empty())
        return "(none)";
    std::string out;
    for (const auto &item : items)
    {
        if (!out.empty())
            out += ", ";
        out += item;
    }
    return out;
}

std::string sq(const std::string &text)
{
    return "'" + text + "'";
}

std::optional<double> parseNumber(const std::string &text)
{
    if (text.empty())
        return std::nullopt;
    double value = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || !std::isfinite(value))
        return std::nullopt;
    return value;
}

std::optional<int64_t> parseInteger(const std::string &text)
{
    if (text.empty())
        return std::nullopt;
    int64_t value = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end)
        return std::nullopt;
    return value;
}

[[noreturn]] void failParam(const std::string &name, const std::string &value, const std::string &expected)
{
    throw MappingError("hardware parameter " + sq(name) + " = " + sq(value) + " is invalid: " + expected);
}

const char *kindName(drivelink::ItemKind kind)
{
    switch (kind)
    {
    case drivelink::ItemKind::Param:
        return "param";
    case drivelink::ItemKind::Signal:
        return "signal";
    case drivelink::ItemKind::Input:
        return "input";
    case drivelink::ItemKind::Action:
        return "action";
    }
    return "unknown kind";
}

std::string typeName(drivelink::ValueType type)
{
    switch (type)
    {
    case drivelink::ValueType::Bool:
        return "bool";
    case drivelink::ValueType::U8:
        return "u8";
    case drivelink::ValueType::U16:
        return "u16";
    case drivelink::ValueType::U32:
        return "u32";
    case drivelink::ValueType::I32:
        return "i32";
    case drivelink::ValueType::F32:
        return "f32";
    case drivelink::ValueType::Enum:
        return "enum";
    case drivelink::ValueType::None:
        return "none";
    }
    return "unknown type " + std::to_string(static_cast<unsigned>(type));
}

bool isScalarNumeric(drivelink::ValueType type)
{
    switch (type)
    {
    case drivelink::ValueType::Bool:
    case drivelink::ValueType::U8:
    case drivelink::ValueType::U16:
    case drivelink::ValueType::U32:
    case drivelink::ValueType::I32:
    case drivelink::ValueType::F32:
    case drivelink::ValueType::Enum:
        return true;
    default:
        return false;
    }
}

// The default item name of a sensor interface (the IMU convention).
std::string sensorItemName(const std::string &interface_name)
{
    struct Rule
    {
        std::string_view from;
        std::string_view to;
        std::string_view suffixes;
    };
    static constexpr Rule rules[] = {
        {"orientation.", "quaternion.", "wxyz"},
        {"angular_velocity.", "gyro.", "xyz"},
        {"linear_acceleration.", "accel.", "xyz"},
    };
    for (const auto &rule : rules)
    {
        if (interface_name.size() == rule.from.size() + 1 && interface_name.starts_with(rule.from) &&
            rule.suffixes.find(interface_name.back()) != std::string_view::npos)
            return std::string(rule.to) + interface_name.back();
    }
    return interface_name;
}

std::string describe(const ComponentSpec &component)
{
    return std::string(component.sensor ? "sensor " : "joint ") + sq(component.name);
}

std::string describe(const ComponentSpec &component, const std::string &interface_name)
{
    return describe(component) + " state interface " + sq(interface_name);
}

// Validates the structure of one component and returns its item-name overrides.
std::map<std::string, std::string> checkParams(const ComponentSpec &component)
{
    std::map<std::string, std::string> overrides;
    for (const auto &[name, value] : component.params)
    {
        if (name == "instance")
            continue;
        if (name.starts_with("state."))
        {
            const std::string interface_name = name.substr(6);
            if (std::find(component.state_interfaces.begin(), component.state_interfaces.end(), interface_name) ==
                component.state_interfaces.end())
                throw MappingError(describe(component) + ": param " + sq(name) + " overrides state interface " +
                                   sq(interface_name) + ", which the component does not declare (declared: " +
                                   join(component.state_interfaces) + ")");
            if (value.empty())
                throw MappingError(describe(component) + ": param " + sq(name) +
                                   " is empty; give the name of a signal of the instance");
            overrides[interface_name] = value;
            continue;
        }
        throw MappingError(describe(component) + ": unknown param " + sq(name) +
                           " (known: instance, state.<interface>)");
    }
    return overrides;
}

const drivelink::ModelInstance &findInstance(const drivelink::DeviceSnapshot &snapshot,
                                             const ComponentSpec &component)
{
    const auto it = component.params.find("instance");
    if (it == component.params.end() || it->second.empty())
        throw MappingError(describe(component) + ": missing required param 'instance' (the label of a configured " +
                           (it == component.params.end() ? "instance)" : "instance; it is empty)"));
    const std::string &label = it->second;

    std::vector<const drivelink::ModelInstance *> matches;
    std::vector<std::string> labels;
    for (const auto &[key, instance] : snapshot.configured)
    {
        (void)key;
        if (!instance.instance.label.empty())
            labels.push_back(instance.instance.label);
        if (instance.instance.label == label)
            matches.push_back(&instance);
    }
    std::sort(labels.begin(), labels.end());
    if (matches.empty())
        throw MappingError(describe(component) + ": param 'instance' = " + sq(label) +
                           " matches no configured instance; available labels: " + join(labels));
    if (matches.size() > 1)
        throw MappingError(describe(component) + ": param 'instance' = " + sq(label) + " matches " +
                           std::to_string(matches.size()) + " configured instances; labels must be unique (labels: " +
                           join(labels) + ")");
    const drivelink::ModelInstance &instance = *matches.front();
    if (instance.instance.state != 0)
        throw MappingError(describe(component) + ": param 'instance' = " + sq(label) +
                           " is not running (instance state " + std::to_string(instance.instance.state) +
                           "); fix it in the GUI's Layout page and reboot");
    return instance;
}
} // namespace

ConnectionConfig parseConnection(const Params &hardware_params)
{
    static const std::vector<std::string> known = {"agent",           "device",     "state_rate_hz", "stale_timeout_ms",
                                                   "take_lease",      "timeout_ms", "token",         "token_file"};
    for (const auto &[name, value] : hardware_params)
    {
        (void)value; // never echoed: `token` is a secret
        if (std::find(known.begin(), known.end(), name) == known.end())
            throw MappingError("unknown hardware parameter " + sq(name) + " (known: " + join(known) + ")");
    }

    ConnectionConfig config;
    for (const auto &[name, value] : hardware_params)
    {
        if (name == "agent")
        {
            constexpr std::string_view prefix = "tcp://";
            if (!value.starts_with(prefix) || value.size() == prefix.size())
                failParam(name, value, "expected a tcp://host:port endpoint");
            config.agent = value;
        }
        else if (name == "token")
        {
            config.token = value;
        }
        else if (name == "token_file")
        {
            config.token_file = value;
        }
        else if (name == "device")
        {
            if (value.empty())
                failParam(name, value, "expected a device alias or 12-hex device id");
            config.device = value;
        }
        else if (name == "take_lease")
        {
            if (value == "refuse")
                config.take_lease = TakeLease::Refuse;
            else if (value == "preempt")
                config.take_lease = TakeLease::Preempt;
            else
                failParam(name, value, "expected 'refuse' or 'preempt'");
        }
        else if (name == "state_rate_hz")
        {
            const auto number = parseNumber(value);
            if (!number || *number < 1 || *number > 10000)
                failParam(name, value, "expected a number in [1, 10000]");
            config.state_rate_hz = *number;
        }
        else if (name == "stale_timeout_ms")
        {
            if (value == "auto")
            {
                config.stale_timeout_ms = std::nullopt;
                continue;
            }
            const auto number = parseNumber(value);
            if (!number || *number < 0 || *number > 60000)
                failParam(name, value, "expected 'auto', 0 (off) or a number of milliseconds up to 60000");
            config.stale_timeout_ms = *number == 0 ? 0.0 : *number;
        }
        else if (name == "timeout_ms")
        {
            const auto number = parseInteger(value);
            if (!number || *number < 100 || *number > 60000)
                failParam(name, value, "expected an integer number of milliseconds in [100, 60000]");
            config.timeout = std::chrono::milliseconds(*number);
        }
    }
    if (config.device.empty())
        throw MappingError("hardware parameter 'device' is required (a device alias or 12-hex device id)");
    return config;
}

std::string readToken(const ConnectionConfig &config)
{
    if (!config.token.empty())
        return config.token;
    std::ifstream file(config.token_file);
    if (!file.is_open())
        throw MappingError("cannot read token_file " + sq(config.token_file) +
                           ": the ROS user must be in the 'drivelink' group (or use the 'token' parameter)");
    std::string line;
    std::getline(file, line);
    if (file.bad())
        throw MappingError("cannot read token_file " + sq(config.token_file) +
                           ": the ROS user must be in the 'drivelink' group (or use the 'token' parameter)");
    const auto last = line.find_last_not_of(" \t\r\n");
    line.erase(last == std::string::npos ? 0 : last + 1);
    if (line.empty())
        throw MappingError("token_file " + sq(config.token_file) +
                           " holds no token (its first line is empty) and no 'token' parameter is set");
    return line;
}

void checkDevice(const drivelink::DeviceSnapshot &snapshot)
{
    if (!snapshot.ready)
        throw MappingError("device is not ready: its model is still loading or reloading; wait for it or retry");
    if (snapshot.safeMode())
        throw MappingError("device is in safe mode; exit safe mode (fix the fault and reboot) before activating");
    if ((snapshot.info.flags & kLayoutPendingReboot) != 0)
        throw MappingError("device has a pending reboot; reboot it to apply the pending layout before activating");
    if ((snapshot.info.flags & kLayoutInvalid) != 0)
        throw MappingError("device layout is invalid; fix it in the GUI's Layout page before activating");
}

uint64_t staleTimeoutUs(std::optional<double> stale_timeout_ms, double group_rate_hz)
{
    constexpr double kAutoFloorUs = 100000; // 100 ms
    double us = 0;
    if (!stale_timeout_ms)
    {
        const double ten_periods_us = group_rate_hz > 0 && std::isfinite(group_rate_hz) ? 1e7 / group_rate_hz : 0;
        us = std::max(ten_periods_us, kAutoFloorUs);
    }
    else
    {
        const double ms = *stale_timeout_ms;
        if (!std::isfinite(ms) || ms < 0)
            throw MappingError("stale_timeout_ms " + std::to_string(ms) + " is invalid: expected 0 or a positive number");
        if (ms == 0)
            return 0;
        us = ms * 1000.0;
    }
    return static_cast<uint64_t>(std::ceil(us));
}

double toDouble(drivelink::ValueType type, uint32_t bits) noexcept
{
    switch (type)
    {
    case drivelink::ValueType::F32:
        return static_cast<double>(std::bit_cast<float>(bits));
    case drivelink::ValueType::I32:
        return static_cast<double>(std::bit_cast<int32_t>(bits));
    case drivelink::ValueType::U8:
    case drivelink::ValueType::U16:
    case drivelink::ValueType::U32:
    case drivelink::ValueType::Enum:
        return static_cast<double>(bits);
    case drivelink::ValueType::Bool:
        return bits != 0 ? 1.0 : 0.0;
    default:
        return std::numeric_limits<double>::quiet_NaN();
    }
}

Resolved resolve(const drivelink::DeviceSnapshot &snapshot, const std::vector<ComponentSpec> &components,
                 const ConnectionConfig &config)
{
    if (!(config.state_rate_hz >= 1) || config.state_rate_hz > 10000 || !std::isfinite(config.state_rate_hz))
        throw MappingError("state_rate_hz " + std::to_string(config.state_rate_hz) + " is invalid: expected [1, 10000]");

    Resolved resolved;
    std::set<std::string> component_names;
    std::map<uint8_t, uint32_t> loop_rates; // loops that hold a mapped signal

    for (const auto &component : components)
    {
        if (component.name.empty())
            throw MappingError(std::string(component.sensor ? "a sensor" : "a joint") + " has an empty name");
        if (!component_names.insert(component.name).second)
            throw MappingError(describe(component) + ": duplicate component name");
        if (!component.command_interfaces.empty())
            throw MappingError(describe(component) + ": command interface " +
                               sq(component.command_interfaces.front()) +
                               " is not supported; command interfaces arrive in H4b");
        if (component.state_interfaces.empty())
            throw MappingError(describe(component) + ": declares no state interface");
        std::set<std::string> interface_names;
        for (const auto &name : component.state_interfaces)
        {
            if (name.empty())
                throw MappingError(describe(component) + ": has a state interface with an empty name");
            if (!interface_names.insert(name).second)
                throw MappingError(describe(component, name) + ": declared more than once");
        }

        const auto overrides = checkParams(component);
        const drivelink::ModelInstance &instance = findInstance(snapshot, component);
        const std::string &label = component.params.at("instance");

        const auto type_it = snapshot.types.find(instance.instance.type);
        if (type_it == snapshot.types.end())
            throw MappingError(describe(component) + ": instance " + sq(label) + " has type id " +
                               std::to_string(instance.instance.type) + ", which the device does not describe");
        const drivelink::ModelType &type = type_it->second;

        const auto loop_it = std::find_if(snapshot.info.loops.begin(), snapshot.info.loops.end(),
                                          [&](const drivelink::LoopInfo &l) { return l.id == instance.instance.loop; });
        if (loop_it == snapshot.info.loops.end())
            throw MappingError(describe(component) + ": instance " + sq(label) + " runs in loop " +
                               std::to_string(instance.instance.loop) + ", which the device does not list");
        if (loop_it->rate_hz == 0)
            throw MappingError(describe(component) + ": instance " + sq(label) + " runs in loop " +
                               std::to_string(instance.instance.loop) + " (" + loop_it->name +
                               "), whose rate is 0 Hz");

        for (const auto &interface_name : component.state_interfaces)
        {
            const auto override_it = overrides.find(interface_name);
            const bool overridden = override_it != overrides.end();
            const std::string item_name = overridden ? override_it->second
                                          : component.sensor ? sensorItemName(interface_name)
                                                             : interface_name;
            const std::string origin = overridden ? "param " + sq("state." + interface_name) + " = " +
                                                        sq(item_name)
                                                  : "default item " + sq(item_name);
            const std::string where = describe(component, interface_name) + " (" + origin + ")";

            const drivelink::ModelItem *found = nullptr;
            std::vector<std::string> signals;
            for (const auto &[id, item] : type.items)
            {
                (void)id;
                if (item.descriptor.name == item_name)
                    found = &item;
                if (item.descriptor.kind == drivelink::ItemKind::Signal)
                    signals.push_back(item.descriptor.name);
            }
            if (!found)
            {
                std::sort(signals.begin(), signals.end());
                throw MappingError(where + ": instance " + sq(label) + " (type " +
                                   sq(type.descriptor.name) + ") has no such item; available signals: " +
                                   join(signals));
            }
            const auto &descriptor = found->descriptor;
            if (descriptor.kind != drivelink::ItemKind::Signal)
                throw MappingError(where + ": item is a " + kindName(descriptor.kind) + ", not a signal (type " +
                                   typeName(descriptor.value_type) + ")");
            if (!isScalarNumeric(descriptor.value_type))
                throw MappingError(where + ": signal has value type " + typeName(descriptor.value_type) +
                                   ", which is not a scalar number");

            StateBinding binding;
            binding.component = component.name;
            binding.interface_name = interface_name;
            binding.ref = drivelink::Ref{instance.instance.type, instance.instance.instance, descriptor.id};
            binding.type = descriptor.value_type;
            binding.loop = instance.instance.loop;
            resolved.states.push_back(std::move(binding));
            loop_rates[instance.instance.loop] = loop_it->rate_hz;
        }
    }

    // One group per loop, ascending loop id (std::map order).
    std::map<uint8_t, size_t> group_index;
    for (const auto &[loop, rate] : loop_rates)
    {
        LoopGroup group;
        group.loop = loop;
        group.loop_rate_hz = rate;
        const double ratio = std::floor(static_cast<double>(rate) / config.state_rate_hz);
        group.divider = static_cast<uint16_t>(std::clamp(ratio, 1.0, 65535.0));
        group.rate_hz = static_cast<double>(rate) / group.divider;
        group.stale_timeout_us = staleTimeoutUs(config.stale_timeout_ms, group.rate_hz);
        group_index[loop] = resolved.groups.size();
        resolved.groups.push_back(std::move(group));
    }

    for (auto &binding : resolved.states)
    {
        binding.group = group_index.at(binding.loop);
        LoopGroup &group = resolved.groups[binding.group];
        const auto it = std::find(group.refs.begin(), group.refs.end(), binding.ref);
        if (it != group.refs.end())
        {
            binding.slot = static_cast<size_t>(it - group.refs.begin());
            continue;
        }
        if (group.refs.size() >= kMaxGroupRefs)
            throw MappingError("loop " + std::to_string(group.loop) + " would map more than " +
                               std::to_string(kMaxGroupRefs) + " distinct signals (at " + sq(binding.component) +
                               " state interface " + sq(binding.interface_name) +
                               "); a subscription group holds at most " + std::to_string(kMaxGroupRefs));
        binding.slot = group.refs.size();
        group.refs.push_back(binding.ref);
        group.types.push_back(binding.type);
    }
    return resolved;
}
} // namespace drivelink_hardware
