#pragma once
// Pure parameter parsing and device mapping for the drivelink_hardware plugin.
// No ROS types and no device I/O (readToken reads one local file): the plugin
// turns ros2_control's HardwareInfo into these structs, and the unit tests
// build DeviceSnapshots by hand. Every refusal throws MappingError with a
// message that names the joint or sensor, the interface, the parameter and
// what was found, so a robot's config error is clear from the log alone.
//
// The design is plans/host-architecture.md "drivelink_hardware (ROS 2)" and
// the brief in plans/host-steps.md (H4a). README.md documents the parameters.
#include <drivelink/model.hpp>
#include <drivelink/types.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace drivelink_hardware
{
struct MappingError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// <param name="...">value</param> entries, as ros2_control gives them.
using Params = std::map<std::string, std::string>;

enum class TakeLease
{
    Refuse, // default: on_activate fails while another client holds the lease
    Preempt // take it anyway (LeaseTake always preempts on the wire)
};

// The hardware component's own parameters (<hardware><param>).
struct ConnectionConfig
{
    std::string agent = "tcp://127.0.0.1:47620"; // `agent`: a tcp:// endpoint (host name allowed)
    std::string token;                           // `token`: inline, for tests and vdev; wins over token_file
    std::string token_file = "/etc/drivelink/token"; // `token_file`
    std::string device;                          // `device`: alias or 12-hex device id; required
    TakeLease take_lease = TakeLease::Refuse;    // `take_lease`: "refuse" (default) or "preempt"
    double state_rate_hz = 100;                  // `state_rate_hz`: [1, 10000]; the rate the plugin reads at
    // `stale_timeout_ms`: "auto" (default, nullopt), "0" (off) or a positive
    // number of milliseconds (at most 60000).
    std::optional<double> stale_timeout_ms;
    std::chrono::milliseconds timeout{3000};     // `timeout_ms`: [100, 60000]; agent/device request timeout
};

// Parses the hardware parameters. Unknown parameter names are refused (a typo
// must not silently fall back to a default). Numbers must parse completely
// ("100", "2.5"; not "100hz", not empty, finite). `device` is required and
// non-empty; `agent` must start with "tcp://".
ConnectionConfig parseConnection(const Params &hardware_params);

// The token to send: `token` when non-empty, else the first line of
// `token_file` with trailing whitespace (\r, \n, spaces, tabs) removed. An
// unreadable file throws with the path and the hint that the ROS user must be
// in the `drivelink` group; an empty token throws. The token is never part of
// an error message.
std::string readToken(const ConnectionConfig &config);

// One <joint> or <sensor> of the ros2_control block.
struct ComponentSpec
{
    std::string name;   // the joint or sensor name
    bool sensor = false;
    Params params;      // its <param> entries
    std::vector<std::string> state_interfaces;   // <state_interface name="...">, in URDF order
    std::vector<std::string> command_interfaces; // <command_interface name="...">
};

// One exported state interface, bound to one device signal.
struct StateBinding
{
    std::string component;      // joint or sensor name
    std::string interface_name; // as declared, e.g. "position" or "orientation.x"
    drivelink::Ref ref;
    drivelink::ValueType type = drivelink::ValueType::F32;
    uint8_t loop = 0;           // the instance's loop
    size_t group = 0;           // index into Resolved::groups
    size_t slot = 0;            // index into that group's refs
};

// One subscription group: every mapped signal of one loop.
struct LoopGroup
{
    uint8_t loop = 0;
    uint32_t loop_rate_hz = 0;
    uint16_t divider = 1;          // max(1, floor(loop_rate_hz / state_rate_hz)), at most 65535
    double rate_hz = 0;            // loop_rate_hz / divider
    uint64_t stale_timeout_us = 0; // 0: off
    std::vector<drivelink::Ref> refs;           // distinct refs, in first-use order
    std::vector<drivelink::ValueType> types;    // parallel to refs
};

// The command interfaces a joint may declare (H4b). The numeric values index
// JointCommands::commands and ::mode_values.
enum class CommandKind : uint8_t
{
    None = 0, // unclaimed
    Position = 1,
    Velocity = 2,
    Duty = 3
};
inline constexpr size_t kCommandKinds = 4; // array size, None included

// "position", "velocity", "duty" -> the kind; anything else (including "none"
// and the empty string) -> nullopt.
std::optional<CommandKind> commandKind(std::string_view interface_name) noexcept;
// The interface name of a kind ("position", "velocity", "duty"; "none" for None).
const char *commandName(CommandKind kind) noexcept;

// One exported command interface, bound to one device input.
struct CommandBinding
{
    std::string component;      // the joint name
    std::string interface_name; // "position", "velocity" or "duty"
    CommandKind kind = CommandKind::None;
    drivelink::Ref ref;         // the input item
    drivelink::ValueType type = drivelink::ValueType::F32;
    uint8_t loop = 0;           // the instance's loop
    size_t group = 0;           // index into Resolved::command_groups
    size_t slot = 0;            // index into that group's refs
    size_t joint = 0;           // index into Resolved::joints
};

// One command group: every command input of one loop.
struct CommandLoopGroup
{
    uint8_t loop = 0;
    std::vector<drivelink::Ref> refs;        // distinct input refs, in first-use order
    std::vector<drivelink::ValueType> types; // parallel to refs
};

// A joint that declares at least one command interface.
struct JointCommands
{
    std::string name;
    // Index into Resolved::commands per CommandKind value (index 0, None, is
    // always empty); empty where the joint does not declare that interface.
    std::array<std::optional<size_t>, kCommandKinds> commands{};
    // The mode param to write when an interface is claimed; nullopt: no mode
    // writes for this joint (see resolve()).
    std::optional<drivelink::Ref> mode;
    // The enum value to write per CommandKind value (meaningful only for the
    // declared kinds, and only when `mode` is set).
    std::array<uint8_t, kCommandKinds> mode_values{};
    // Where the measured position lives for the position hold: set exactly
    // when the joint declares a `position` command interface. Indexes
    // Resolved::groups and that group's refs (the slot may be hidden: not
    // bound to any StateBinding).
    std::optional<size_t> position_group;
    size_t position_slot = 0;
    drivelink::ValueType position_type = drivelink::ValueType::F32;
};

struct Resolved
{
    std::vector<StateBinding> states; // in component order, then state-interface order
    std::vector<LoopGroup> groups;    // ascending loop id
    // H4b. In component order, then command-interface order (URDF order).
    std::vector<CommandBinding> commands;
    std::vector<CommandLoopGroup> command_groups; // ascending loop id
    std::vector<JointCommands> joints;            // joints with a command interface, in component order
};

// Refuses a device that is not ready to map: !snapshot.ready, safe mode
// (snapshot.safeMode()), a pending reboot (info.flags bit 0x01) or an
// invalid layout (info.flags bit 0x02), each with its own message saying
// what to do (exit safe mode, reboot to apply the pending layout, fix it in
// the GUI's Layout page).
void checkDevice(const drivelink::DeviceSnapshot &snapshot);

// Resolves every component against the device model (configured layout).
//
// Joints and sensors take a required param `instance`: the label of a
// configured instance (labels are matched exactly; zero or several matches
// are refused, listing the available labels). The instance must be running
// (Instance::state 0); a failed or invalid instance is refused.
//
// Each state interface maps to a signal of that instance. The default item
// name is:
//   joints:  the interface name itself (position -> position,
//            velocity -> velocity, duty -> duty, any other name -> itself)
//   sensors: orientation.{w,x,y,z}      -> quaternion.{w,x,y,z}
//            angular_velocity.{x,y,z}    -> gyro.{x,y,z}
//            linear_acceleration.{x,y,z} -> accel.{x,y,z}
//            any other name              -> itself
// A param `state.<interface>` overrides the item name for that interface
// (e.g. <param name="state.position">output_position</param>). A
// `state.<name>` param for an interface the component does not declare is
// refused, as is any other unknown param name.
//
// The item must exist in the instance's type, be ItemKind::Signal and have a
// scalar numeric value type (Bool, U8, U16, U32, I32, F32, Enum); otherwise
// it is refused with the item's actual kind/type. Two interfaces may map to
// the same ref; they share one group slot.
//
// A component with neither a state nor a command interface is refused.
// Duplicate component names or duplicate interface names within a component
// (state and command interfaces are separate namespaces) are refused.
//
// Command interfaces (H4b), joints only (a sensor's is refused):
//   - The name must be `position`, `velocity` or `duty` (commandKind); any
//     other is refused, naming the three.
//   - Default input item: position -> position_sp, velocity -> velocity_sp,
//     duty -> duty_sp. A param `command.<interface>` overrides it (refused
//     for an interface the joint does not declare as a command interface).
//     The item must exist, be ItemKind::Input and have a scalar numeric value
//     type (as for signals); otherwise refused with what was found.
//   - Mode. A param `mode_param` names the mode param item (default "mode");
//     the value "none" turns mode writes off for the joint. A param
//     `mode.<interface>` (only for a declared command interface) names the
//     enum entry to write when that interface is claimed; defaults: position
//     -> "position", velocity -> "velocity", duty -> "open_loop".
//     * `mode_param` unset and the type has no item named "mode": no mode
//       writes (JointCommands::mode empty), and any `mode.<interface>` param
//       is refused (it would be ignored).
//     * Otherwise the item must exist and be an ItemKind::Param of ValueType
//       Enum without the flags ReadOnly (0x04), RebootRequired (0x01) or
//       LockedWhileArmed (0x08) (it is written while armed), and every
//       declared command interface's entry name must be one of the item's
//       enum entries (ModelItem::enums); refusals list the available entries.
//   - Position hold: a joint that declares a `position` command interface
//     needs its measured position. The signal is the item named by
//     `state.position` if that param is set, else "position" (the state
//     interface's own rules: an existing scalar numeric Signal). It is added
//     to the joint's loop group like a state signal (sharing the slot when a
//     state interface already maps the same ref, else as an extra, hidden
//     slot after that component's state refs). A `state.position` param is
//     therefore allowed on such a joint even without a `position` state
//     interface. Missing or wrong: refused ("the position command needs the
//     measured position for its hold").
//   - Command groups: one per loop that holds a command input, ascending loop
//     id; at most 64 refs each (more is refused). Two command interfaces of
//     different joints that name the same input ref are refused (one input,
//     one owner).
//
// Known joint params: `instance`, `state.<interface>`, `command.<interface>`,
// `mode_param`, `mode.<interface>`; sensors: `instance`, `state.<interface>`.
// Any other param name is refused.
//
// Groups: one per loop that holds a mapped signal (state or hidden). The loop
// must be in snapshot.info.loops with rate_hz > 0 (else refused). A group
// holds at most 64 refs (drivelink::GroupSample::values); more is refused.
// divider and rate_hz as in LoopGroup; stale_timeout_us from staleTimeoutUs.
Resolved resolve(const drivelink::DeviceSnapshot &snapshot, const std::vector<ComponentSpec> &components,
                 const ConnectionConfig &config);

// The claimed CommandKind of every joint, parallel to Resolved::joints.
using Claims = std::vector<CommandKind>;

// What one prepare/perform_command_mode_switch pair changes.
struct SwitchPlan
{
    Claims after;                  // the claims once the switch is performed
    std::vector<size_t> claimed;   // joints (Resolved::joints index) claiming an interface, ascending
    std::vector<size_t> released;  // joints whose claim ends and that claim nothing new, ascending
    bool arm = false;              // no joint claimed before, at least one after
    bool disarm = false;           // at least one joint claimed before, none after
};

// Plans a switch from ros2_control's interface lists ("<joint>/<interface>").
// Names that are not a declared command interface of a joint in `resolved`
// are ignored (other components' interfaces arrive in the same lists). The
// stop list is applied first: a joint whose current interface is stopped
// becomes unclaimed (stopping an interface that is not the joint's current
// claim is ignored). Then each started interface claims its joint. Refused
// with MappingError naming the joint and interfaces: two started interfaces
// of one joint, or a start on a joint that stays claimed (its current
// interface is not stopped) — one command interface per joint at a time.
// A joint both stopped and started (a mode change, or a controller handing
// over the same interface) is in `claimed`, not in `released`. `current`
// must have resolved.joints.size() entries (else MappingError).
SwitchPlan planSwitch(const Resolved &resolved, const Claims &current, const std::vector<std::string> &start,
                      const std::vector<std::string> &stop);

// The value write() sends for one command slot of kind `kind`:
//   - the slot's joint claims this kind and `command` is finite -> command;
//   - otherwise (unclaimed, claimed by another kind, NaN or infinite) the
//     hold: 0 for Velocity and Duty, `position_hold` for Position.
// None gives 0.
constexpr double slotValue(CommandKind kind, bool claimed, double command, double position_hold) noexcept
{
    const bool finite = command == command && command - command == 0; // not NaN, not infinite
    if (claimed && finite && kind != CommandKind::None)
    {
        return command;
    }
    return kind == CommandKind::Position ? position_hold : 0.0;
}

// A group's stale timeout: nullopt (auto) -> max(10 / rate_hz s, 100 ms);
// 0 -> 0 (off); else the given milliseconds. In microseconds, rounded up.
uint64_t staleTimeoutUs(std::optional<double> stale_timeout_ms, double group_rate_hz);

// True when a sample stamped sample_us (host monotonic µs) is older than
// timeout_us at now_us; never stale when timeout_us is 0 or the sample is in
// the future.
constexpr bool isStale(uint64_t now_us, uint64_t sample_us, uint64_t timeout_us) noexcept
{
    return timeout_us != 0 && now_us > sample_us && now_us - sample_us > timeout_us;
}

// A slot's bits as a double: F32 by bit cast, I32 signed, U8/U16/U32/Enum
// unsigned, Bool 0 or 1 (any non-zero bits are 1). Other types give NaN.
double toDouble(drivelink::ValueType type, uint32_t bits) noexcept;
} // namespace drivelink_hardware
