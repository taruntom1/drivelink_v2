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
// ItemDescriptor::flags
constexpr unsigned kItemRebootRequired = 0x01;
constexpr unsigned kItemReadOnly = 0x04;
constexpr unsigned kItemLockedWhileArmed = 0x08;

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

// "a signal", "an input": the kind with its article, for messages.
std::string withArticle(drivelink::ItemKind kind)
{
    const std::string name = kindName(kind);
    return (kind == drivelink::ItemKind::Input || kind == drivelink::ItemKind::Action ? "an " : "a ") + name;
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

std::string describe(const ComponentSpec &component, const std::string &interface_name, const char *what = "state")
{
    return describe(component) + " " + what + " interface " + sq(interface_name);
}

bool contains(const std::vector<std::string> &items, const std::string &item)
{
    return std::find(items.begin(), items.end(), item) != items.end();
}

// The param overrides of one component.
struct ParamSet
{
    std::map<std::string, std::string> state;   // state.<interface> -> signal name
    std::map<std::string, std::string> command; // command.<interface> -> input name
    std::optional<std::string> mode_param;      // mode_param
    std::map<std::string, std::string> mode;    // mode.<interface> -> enum entry name
};

// The default input item of a command interface.
const char *defaultInputName(CommandKind kind)
{
    switch (kind)
    {
    case CommandKind::Position:
        return "position_sp";
    case CommandKind::Velocity:
        return "velocity_sp";
    case CommandKind::Duty:
        return "duty_sp";
    case CommandKind::None:
        break;
    }
    return "";
}

// The default enum entry written to the mode param when a kind is claimed.
const char *defaultModeEntry(CommandKind kind)
{
    switch (kind)
    {
    case CommandKind::Position:
        return "position";
    case CommandKind::Velocity:
        return "velocity";
    case CommandKind::Duty:
        return "open_loop";
    case CommandKind::None:
        break;
    }
    return "";
}

constexpr const char *kCommandNames = "position, velocity, duty";

// Validates the structure of one component and returns its param overrides.
ParamSet checkParams(const ComponentSpec &component)
{
    ParamSet set;
    for (const auto &[name, value] : component.params)
    {
        if (name == "instance")
            continue;
        if (name.starts_with("state."))
        {
            const std::string interface_name = name.substr(6);
            // A position command holds the measured position, which may be a
            // hidden signal: its `state.position` is allowed without the state interface.
            const bool hold_override = interface_name == "position" && !component.sensor &&
                                       contains(component.command_interfaces, "position");
            if (!contains(component.state_interfaces, interface_name) && !hold_override)
                throw MappingError(describe(component) + ": param " + sq(name) + " overrides state interface " +
                                   sq(interface_name) + ", which the component does not declare (declared: " +
                                   join(component.state_interfaces) + ")");
            if (value.empty())
                throw MappingError(describe(component) + ": param " + sq(name) +
                                   " is empty; give the name of a signal of the instance");
            set.state[interface_name] = value;
            continue;
        }
        if (!component.sensor && name.starts_with("command."))
        {
            const std::string interface_name = name.substr(8);
            if (!contains(component.command_interfaces, interface_name))
                throw MappingError(describe(component) + ": param " + sq(name) + " overrides command interface " +
                                   sq(interface_name) + ", which the joint does not declare (declared: " +
                                   join(component.command_interfaces) + ")");
            if (value.empty())
                throw MappingError(describe(component) + ": param " + sq(name) +
                                   " is empty; give the name of an input of the instance");
            set.command[interface_name] = value;
            continue;
        }
        if (!component.sensor && name == "mode_param")
        {
            if (component.command_interfaces.empty())
                throw MappingError(describe(component) + ": param 'mode_param' is set, but the joint declares no " +
                                   "command interface (declared: (none))");
            if (value.empty())
                throw MappingError(describe(component) + ": param 'mode_param' is empty; give the name of an enum " +
                                   "param of the instance, or 'none' for no mode writes");
            set.mode_param = value;
            continue;
        }
        if (!component.sensor && name.starts_with("mode."))
        {
            const std::string interface_name = name.substr(5);
            if (!contains(component.command_interfaces, interface_name))
                throw MappingError(describe(component) + ": param " + sq(name) + " names the mode entry of command " +
                                   "interface " + sq(interface_name) + ", which the joint does not declare " +
                                   "(declared: " + join(component.command_interfaces) + ")");
            if (value.empty())
                throw MappingError(describe(component) + ": param " + sq(name) +
                                   " is empty; give the name of an entry of the mode param");
            set.mode[interface_name] = value;
            continue;
        }
        throw MappingError(describe(component) + ": unknown param " + sq(name) +
                           (component.sensor
                                ? " (known: instance, state.<interface>)"
                                : " (known: instance, state.<interface>, command.<interface>, mode_param, "
                                  "mode.<interface>)"));
    }
    return set;
}

// The item `item_name` of `want` kind of the instance's type (a scalar numeric
// Signal, or an F32 Input), or a MappingError that says what was found.
const drivelink::ModelItem &requireItem(const drivelink::ModelType &type, const std::string &label,
                                        const std::string &item_name, drivelink::ItemKind want,
                                        const std::string &where)
{
    const drivelink::ModelItem *found = nullptr;
    std::vector<std::string> available;
    for (const auto &[id, item] : type.items)
    {
        (void)id;
        if (item.descriptor.name == item_name)
            found = &item;
        if (item.descriptor.kind == want)
            available.push_back(item.descriptor.name);
    }
    if (!found)
    {
        std::sort(available.begin(), available.end());
        throw MappingError(where + ": instance " + sq(label) + " (type " + sq(type.descriptor.name) +
                           ") has no such item; available " + kindName(want) + "s: " + join(available));
    }
    const auto &descriptor = found->descriptor;
    if (descriptor.kind != want)
        throw MappingError(where + ": item is " + withArticle(descriptor.kind) + ", not " + withArticle(want) +
                           " (type " + typeName(descriptor.value_type) + ")");
    if (want == drivelink::ItemKind::Input && descriptor.value_type != drivelink::ValueType::F32)
        throw MappingError(where + ": input has value type " + typeName(descriptor.value_type) +
                           "; command inputs must be f32 (write() sends doubles as f32)");
    if (!isScalarNumeric(descriptor.value_type))
        throw MappingError(where + ": " + kindName(want) + " has value type " + typeName(descriptor.value_type) +
                           ", which is not a scalar number");
    return *found;
}

// The mode param of a joint: nullopt for no mode writes (the mode enum values
// land in `joint.mode_values`).
std::optional<drivelink::Ref> resolveMode(const ComponentSpec &component, const ParamSet &set,
                                          const drivelink::ModelType &type, const drivelink::ModelInstance &instance,
                                          JointCommands &joint)
{
    const bool explicit_param = set.mode_param.has_value();
    const std::string mode_name = explicit_param ? *set.mode_param : "mode";
    const auto find_item = [&](const std::string &name) -> const drivelink::ModelItem * {
        for (const auto &[id, item] : type.items)
        {
            (void)id;
            if (item.descriptor.name == name)
                return &item;
        }
        return nullptr;
    };

    const drivelink::ModelItem *item = nullptr;
    if (mode_name != "none" || !explicit_param)
        item = find_item(mode_name);
    if (!explicit_param && !item)
    {
        if (!set.mode.empty())
            throw MappingError(describe(component) + ": param " + sq("mode." + set.mode.begin()->first) +
                               " is set, but the type " + sq(type.descriptor.name) +
                               " has no item named 'mode' and mode_param is not set, so no mode is written; " +
                               "name the mode param with 'mode_param'");
        return std::nullopt;
    }
    if (explicit_param && mode_name == "none")
    {
        if (!set.mode.empty())
            throw MappingError(describe(component) + ": param " + sq("mode." + set.mode.begin()->first) +
                               " is set, but 'mode_param' = 'none' turns mode writes off");
        return std::nullopt;
    }

    const std::string where = describe(component) + " mode param " + sq(mode_name) +
                              (explicit_param ? " (param 'mode_param')" : " (default)");
    if (!item)
    {
        std::vector<std::string> params;
        for (const auto &[id, other] : type.items)
        {
            (void)id;
            if (other.descriptor.kind == drivelink::ItemKind::Param)
                params.push_back(other.descriptor.name);
        }
        std::sort(params.begin(), params.end());
        throw MappingError(where + ": instance " + sq(component.params.at("instance")) + " (type " +
                           sq(type.descriptor.name) + ") has no such item; available params: " + join(params));
    }
    const auto &descriptor = item->descriptor;
    if (descriptor.kind != drivelink::ItemKind::Param)
        throw MappingError(where + ": item is " + withArticle(descriptor.kind) + ", not a param (type " +
                           typeName(descriptor.value_type) + ")");
    if (descriptor.value_type != drivelink::ValueType::Enum)
        throw MappingError(where + ": param has value type " + typeName(descriptor.value_type) +
                           ", not enum");
    if ((descriptor.flags & kItemReadOnly) != 0)
        throw MappingError(where + ": param is read-only (flag ReadOnly), so the plugin cannot write it");
    if ((descriptor.flags & kItemRebootRequired) != 0)
        throw MappingError(where + ": param needs a reboot to change (flag RebootRequired), so the plugin cannot " +
                           "write it");
    if ((descriptor.flags & kItemLockedWhileArmed) != 0)
        throw MappingError(where + ": param is locked while armed (flag LockedWhileArmed), but the plugin writes " +
                           "it while armed");

    std::vector<std::string> entries;
    for (const auto &entry : item->enums)
        entries.push_back(entry.name);
    for (const auto &interface_name : component.command_interfaces)
    {
        const CommandKind kind = *commandKind(interface_name);
        const auto override_it = set.mode.find(interface_name);
        const bool overridden = override_it != set.mode.end();
        const std::string entry_name = overridden ? override_it->second : defaultModeEntry(kind);
        const auto entry = std::find_if(item->enums.begin(), item->enums.end(),
                                        [&](const drivelink::EnumEntry &e) { return e.name == entry_name; });
        if (entry == item->enums.end())
            throw MappingError(describe(component, interface_name, "command") + " (mode " +
                               (overridden ? "param " + sq("mode." + interface_name) + " = " + sq(entry_name)
                                           : "default entry " + sq(entry_name)) +
                               "): mode param " + sq(mode_name) + " has no such enum entry; available entries: " +
                               join(entries));
        joint.mode_values[static_cast<size_t>(kind)] = entry->value;
    }
    return drivelink::Ref{instance.instance.type, instance.instance.instance, descriptor.id};
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

std::optional<CommandKind> commandKind(std::string_view interface_name) noexcept
{
    if (interface_name == "position")
        return CommandKind::Position;
    if (interface_name == "velocity")
        return CommandKind::Velocity;
    if (interface_name == "duty")
        return CommandKind::Duty;
    return std::nullopt;
}

const char *commandName(CommandKind kind) noexcept
{
    switch (kind)
    {
    case CommandKind::Position:
        return "position";
    case CommandKind::Velocity:
        return "velocity";
    case CommandKind::Duty:
        return "duty";
    case CommandKind::None:
        break;
    }
    return "none";
}

Resolved resolve(const drivelink::DeviceSnapshot &snapshot, const std::vector<ComponentSpec> &components,
                 const ConnectionConfig &config)
{
    if (!(config.state_rate_hz >= 1) || config.state_rate_hz > 10000 || !std::isfinite(config.state_rate_hz))
        throw MappingError("state_rate_hz " + std::to_string(config.state_rate_hz) + " is invalid: expected [1, 10000]");

    // One signal a group must hold: a state interface's, or a position hold's
    // (hidden when no state interface maps it). Kept in order so the hidden
    // slot lands after its component's state refs.
    struct SignalUse
    {
        std::optional<size_t> state; // index into Resolved::states
        std::optional<size_t> joint; // index into Resolved::joints (a position hold)
        drivelink::Ref ref;
        drivelink::ValueType type = drivelink::ValueType::F32;
        uint8_t loop = 0;
        std::string at; // for the 64-refs message
    };
    // The owner of every command input ref taken so far.
    struct InputOwner
    {
        drivelink::Ref ref;
        std::string component;
        std::string what;
    };

    Resolved resolved;
    std::set<std::string> component_names;
    std::map<uint8_t, uint32_t> loop_rates; // loops that hold a mapped signal
    std::vector<SignalUse> uses;
    std::vector<InputOwner> owners;

    for (const auto &component : components)
    {
        if (component.name.empty())
            throw MappingError(std::string(component.sensor ? "a sensor" : "a joint") + " has an empty name");
        if (!component_names.insert(component.name).second)
            throw MappingError(describe(component) + ": duplicate component name");
        if (component.state_interfaces.empty() && component.command_interfaces.empty())
            throw MappingError(describe(component) + ": declares no state interface and no command interface");
        std::set<std::string> interface_names;
        for (const auto &name : component.state_interfaces)
        {
            if (name.empty())
                throw MappingError(describe(component) + ": has a state interface with an empty name");
            if (!interface_names.insert(name).second)
                throw MappingError(describe(component, name) + ": declared more than once");
        }
        std::set<std::string> command_names;
        for (const auto &name : component.command_interfaces)
        {
            if (component.sensor)
                throw MappingError(describe(component, name, "command") +
                                   ": a sensor has no command interfaces (joints only)");
            if (name.empty())
                throw MappingError(describe(component) + ": has a command interface with an empty name");
            if (!commandKind(name))
                throw MappingError(describe(component, name, "command") + ": unknown command interface; known: " +
                                   kCommandNames);
            if (!command_names.insert(name).second)
                throw MappingError(describe(component, name, "command") + ": declared more than once");
        }

        const auto params = checkParams(component);
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
            const auto override_it = params.state.find(interface_name);
            const bool overridden = override_it != params.state.end();
            const std::string item_name = overridden ? override_it->second
                                          : component.sensor ? sensorItemName(interface_name)
                                                             : interface_name;
            const std::string origin = overridden ? "param " + sq("state." + interface_name) + " = " +
                                                        sq(item_name)
                                                  : "default item " + sq(item_name);
            const std::string where = describe(component, interface_name) + " (" + origin + ")";

            const auto &descriptor =
                requireItem(type, label, item_name, drivelink::ItemKind::Signal, where).descriptor;

            StateBinding binding;
            binding.component = component.name;
            binding.interface_name = interface_name;
            binding.ref = drivelink::Ref{instance.instance.type, instance.instance.instance, descriptor.id};
            binding.type = descriptor.value_type;
            binding.loop = instance.instance.loop;
            SignalUse use;
            use.state = resolved.states.size();
            use.ref = binding.ref;
            use.type = binding.type;
            use.loop = binding.loop;
            use.at = sq(component.name) + " state interface " + sq(interface_name);
            uses.push_back(std::move(use));
            resolved.states.push_back(std::move(binding));
            loop_rates[instance.instance.loop] = loop_it->rate_hz;
        }

        if (component.command_interfaces.empty())
            continue;

        // Command interfaces: joints only (checked above).
        JointCommands joint;
        joint.name = component.name;
        const size_t joint_index = resolved.joints.size();
        for (const auto &interface_name : component.command_interfaces)
        {
            const CommandKind kind = *commandKind(interface_name);
            const auto override_it = params.command.find(interface_name);
            const bool overridden = override_it != params.command.end();
            const std::string item_name = overridden ? override_it->second : defaultInputName(kind);
            const std::string origin = overridden ? "param " + sq("command." + interface_name) + " = " +
                                                        sq(item_name)
                                                  : "default item " + sq(item_name);
            const std::string where = describe(component, interface_name, "command") + " (" + origin + ")";

            const auto &descriptor =
                requireItem(type, label, item_name, drivelink::ItemKind::Input, where).descriptor;

            CommandBinding binding;
            binding.component = component.name;
            binding.interface_name = interface_name;
            binding.kind = kind;
            binding.ref = drivelink::Ref{instance.instance.type, instance.instance.instance, descriptor.id};
            binding.type = descriptor.value_type;
            binding.loop = instance.instance.loop;
            binding.joint = joint_index;

            for (const auto &owner : owners)
            {
                if (owner.ref == binding.ref)
                    throw MappingError(where + ": input " + sq(descriptor.name) + " of instance " + sq(label) +
                                       " is already commanded by joint " + owner.what +
                                       "; one input has one command interface");
            }
            owners.push_back({binding.ref, component.name,
                              sq(component.name) + " command interface " + sq(interface_name)});

            joint.commands[static_cast<size_t>(kind)] = resolved.commands.size();
            resolved.commands.push_back(std::move(binding));
        }

        joint.mode = resolveMode(component, params, type, instance, joint);

        if (joint.commands[static_cast<size_t>(CommandKind::Position)])
        {
            const auto override_it = params.state.find("position");
            const bool overridden = override_it != params.state.end();
            const std::string item_name = overridden ? override_it->second : "position";
            const std::string where = describe(component) + " position command hold (" +
                                      (overridden ? "param 'state.position' = " + sq(item_name)
                                                  : "default item " + sq(item_name)) +
                                      ")";
            const drivelink::ItemDescriptor *descriptor = nullptr;
            try
            {
                descriptor = &requireItem(type, label, item_name, drivelink::ItemKind::Signal, where).descriptor;
            }
            catch (const MappingError &e)
            {
                throw MappingError(std::string(e.what()) +
                                   "; the position command needs the measured position for its hold");
            }
            SignalUse use;
            use.joint = joint_index;
            use.ref = drivelink::Ref{instance.instance.type, instance.instance.instance, descriptor->id};
            use.type = descriptor->value_type;
            use.loop = instance.instance.loop;
            use.at = sq(component.name) + " position hold";
            joint.position_group = 0; // set below, with the slot
            joint.position_type = use.type;
            uses.push_back(std::move(use));
            loop_rates[instance.instance.loop] = loop_it->rate_hz;
        }
        resolved.joints.push_back(std::move(joint));
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

    for (const auto &use : uses)
    {
        const size_t group_number = group_index.at(use.loop);
        LoopGroup &group = resolved.groups[group_number];
        size_t slot = 0;
        const auto it = std::find(group.refs.begin(), group.refs.end(), use.ref);
        if (it != group.refs.end())
        {
            slot = static_cast<size_t>(it - group.refs.begin());
        }
        else
        {
            if (group.refs.size() >= kMaxGroupRefs)
                throw MappingError("loop " + std::to_string(group.loop) + " would map more than " +
                                   std::to_string(kMaxGroupRefs) + " distinct signals (at " + use.at +
                                   "); a subscription group holds at most " + std::to_string(kMaxGroupRefs));
            slot = group.refs.size();
            group.refs.push_back(use.ref);
            group.types.push_back(use.type);
        }
        if (use.state)
        {
            resolved.states[*use.state].group = group_number;
            resolved.states[*use.state].slot = slot;
        }
        else
        {
            resolved.joints[*use.joint].position_group = group_number;
            resolved.joints[*use.joint].position_slot = slot;
        }
    }

    // Command groups: one per loop that holds a command input, ascending loop id.
    std::map<uint8_t, size_t> command_group_index;
    {
        std::set<uint8_t> command_loops;
        for (const auto &binding : resolved.commands)
            command_loops.insert(binding.loop);
        for (const uint8_t loop : command_loops)
        {
            CommandLoopGroup group;
            group.loop = loop;
            command_group_index[loop] = resolved.command_groups.size();
            resolved.command_groups.push_back(std::move(group));
        }
    }
    for (auto &binding : resolved.commands)
    {
        binding.group = command_group_index.at(binding.loop);
        CommandLoopGroup &group = resolved.command_groups[binding.group];
        const auto it = std::find(group.refs.begin(), group.refs.end(), binding.ref);
        if (it != group.refs.end())
        {
            binding.slot = static_cast<size_t>(it - group.refs.begin());
            continue;
        }
        if (group.refs.size() >= kMaxGroupRefs)
            throw MappingError("loop " + std::to_string(group.loop) + " would command more than " +
                               std::to_string(kMaxGroupRefs) + " distinct inputs (at " + sq(binding.component) +
                               " command interface " + sq(binding.interface_name) +
                               "); a command group holds at most " + std::to_string(kMaxGroupRefs));
        binding.slot = group.refs.size();
        group.refs.push_back(binding.ref);
        group.types.push_back(binding.type);
    }
    return resolved;
}

SwitchPlan planSwitch(const Resolved &resolved, const Claims &current, const std::vector<std::string> &start,
                      const std::vector<std::string> &stop)
{
    if (current.size() != resolved.joints.size())
        throw MappingError("command switch: " + std::to_string(current.size()) + " claims given for " +
                           std::to_string(resolved.joints.size()) + " joints with command interfaces");

    // "<joint>/<interface>" -> (joint index, kind), for the declared command interfaces only.
    std::map<std::string, std::pair<size_t, CommandKind>> known;
    for (size_t j = 0; j < resolved.joints.size(); ++j)
    {
        for (size_t k = 1; k < kCommandKinds; ++k)
        {
            if (resolved.joints[j].commands[k])
                known[resolved.joints[j].name + "/" + commandName(static_cast<CommandKind>(k))] = {
                    j, static_cast<CommandKind>(k)};
        }
    }

    SwitchPlan plan;
    plan.after = current;
    for (const auto &name : stop)
    {
        const auto it = known.find(name);
        if (it == known.end())
            continue;
        const auto [joint, kind] = it->second;
        if (current[joint] == kind)
            plan.after[joint] = CommandKind::None;
    }

    std::vector<CommandKind> started(resolved.joints.size(), CommandKind::None);
    for (const auto &name : start)
    {
        const auto it = known.find(name);
        if (it == known.end())
            continue;
        const auto [joint, kind] = it->second;
        const std::string &joint_name = resolved.joints[joint].name;
        if (started[joint] != CommandKind::None)
            throw MappingError("command switch: joint " + sq(joint_name) + " is asked to start " +
                               sq(joint_name + "/" + commandName(started[joint])) + " and " + sq(name) +
                               "; a joint has one command interface at a time");
        if (plan.after[joint] != CommandKind::None)
            throw MappingError("command switch: cannot start " + sq(name) + ": joint " + sq(joint_name) +
                               " still claims " + sq(joint_name + "/" + commandName(plan.after[joint])) +
                               "; stop it in the same switch (one command interface per joint at a time)");
        started[joint] = kind;
    }

    bool claimed_before = false;
    bool claimed_after = false;
    for (size_t j = 0; j < resolved.joints.size(); ++j)
    {
        if (started[j] != CommandKind::None)
        {
            plan.after[j] = started[j];
            plan.claimed.push_back(j);
        }
        else if (current[j] != CommandKind::None && plan.after[j] == CommandKind::None)
        {
            plan.released.push_back(j);
        }
        claimed_before = claimed_before || current[j] != CommandKind::None;
        claimed_after = claimed_after || plan.after[j] != CommandKind::None;
    }
    plan.arm = !claimed_before && claimed_after;
    plan.disarm = claimed_before && !claimed_after;
    return plan;
}
} // namespace drivelink_hardware
