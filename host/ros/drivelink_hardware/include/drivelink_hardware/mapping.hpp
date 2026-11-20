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
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
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

struct Resolved
{
    std::vector<StateBinding> states; // in component order, then state-interface order
    std::vector<LoopGroup> groups;    // ascending loop id
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
// H4a has no command path: a component with command interfaces is refused
// ("command interfaces arrive in H4b"). A component with no state interface
// is refused. Duplicate component names or duplicate interface names within
// a component are refused.
//
// Groups: one per loop that holds a mapped signal. The loop must be in
// snapshot.info.loops with rate_hz > 0 (else refused). A group holds at most
// 64 refs (drivelink::GroupSample::values); more is refused. divider and
// rate_hz as in LoopGroup; stale_timeout_us from staleTimeoutUs.
Resolved resolve(const drivelink::DeviceSnapshot &snapshot, const std::vector<ComponentSpec> &components,
                 const ConnectionConfig &config);

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
