// drivelink_hardware/DriveLinkSystem (README.md; the design is
// plans/host-architecture.md "drivelink_hardware (ROS 2)"). H4a: the state
// path. H4b: the command path (command groups, the mode switch, ARM/DISARM,
// write(), ERROR on a disarm while armed).
#include "drivelink_hardware/system.hpp"
#include "drivelink_hardware/mapping.hpp"

#include <drivelink/buffers.hpp>
#include <drivelink/client.hpp>
#include <drivelink/command.hpp>
#include <drivelink/model.hpp>
#include <drivelink/session.hpp>
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_system.hpp>
#include <hardware_interface/types/hardware_interface_return_values.hpp>
#include <pluginlib/class_list_macros.hpp>
#include <rclcpp/logging.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <variant>

namespace drivelink_hardware
{
namespace
{
namespace defs = drivelink::protocol::defs;
namespace msg = drivelink::protocol::defs::msg;
using hardware_interface::return_type;

// Why the component cannot run. The first fault wins; read() reports it with
// ERROR and on_error() logs it (read() never logs). Faults found while the
// component is inactive make the next activate refuse.
enum class Fault : uint8_t
{
    None,
    LeaseLost,
    DeviceOffline,
    AgentShutdown,
    LinkLost,
    Rebooted,
    EventsLost,
    Stale,
    Disarmed,
    StatusLost,
    SwitchAbandoned,
    SwitchMismatch,
    ModeUncertain,
    DisarmFailed,
    Internal
};

const char *describe(Fault fault)
{
    switch (fault)
    {
    case Fault::None: return "no fault";
    case Fault::LeaseLost: return "another client took the lease (or it was released); nothing re-arms by itself";
    case Fault::DeviceOffline: return "the agent reported the device offline";
    case Fault::AgentShutdown: return "the agent is shutting down";
    case Fault::LinkLost: return "the connection to the agent was lost";
    case Fault::Rebooted: return "the device rebooted; configure again to reload the mapping";
    case Fault::EventsLost: return "agent events were dropped, so the lease state is unknown";
    case Fault::Stale: return "no fresh sample within stale_timeout_ms";
    case Fault::Disarmed: return "the device disarmed while armed; nothing re-arms by itself";
    case Fault::StatusLost:
        return "no status frame from the device for three status periods while armed, so a disarm could go unseen";
    case Fault::SwitchAbandoned:
        return "a prepared command mode switch was not performed within timeout_ms, or another claim was prepared "
               "before it (a joint's mode may not match its controller)";
    case Fault::SwitchMismatch:
        return "perform_command_mode_switch claimed other interfaces than prepare_command_mode_switch planned";
    case Fault::ModeUncertain: return "a mode write while armed got no answer, so a joint's mode is unknown";
    case Fault::DisarmFailed: return "the DISARM after the last release failed";
    case Fault::Internal: return "internal error in read() or write()";
    }
    return "unknown fault";
}

// Status `last_disarm_reason` (core.md).
const char *disarmReason(uint8_t reason)
{
    switch (reason)
    {
    case 0: return "boot";
    case 1: return "explicit DISARM (another client, the agent or a lease change)";
    case 2: return "watchdog (no command frame within system.watchdog_ms: write() stopped)";
    case 3: return "fault";
    case 4: return "loop stall";
    case 5: return "shutdown";
    default: return "unknown reason";
    }
}

// The refusals ARM can answer, by name (registry.hpp Result / Detail).
std::string refusal(uint8_t result, uint8_t detail)
{
    const char *r = nullptr;
    switch (defs::Result(result))
    {
    case defs::Result::InvalidArgument: r = "InvalidArgument"; break;
    case defs::Result::Busy: r = "Busy"; break;
    case defs::Result::Armed: r = "Armed"; break;
    case defs::Result::NotArmed: r = "NotArmed"; break;
    case defs::Result::Fault: r = "Fault"; break;
    default: break;
    }
    const char *d = nullptr;
    switch (defs::Detail(detail))
    {
    case defs::Detail::SafeMode: d = "SafeMode"; break;
    case defs::Detail::FaultLatched: d = "FaultLatched (acknowledge the fault)"; break;
    case defs::Detail::BlocksArm: d = "BlocksArm (a module blocks arming; see the device's faults)"; break;
    case defs::Detail::LayoutInvalid: d = "LayoutInvalid"; break;
    case defs::Detail::NoCommandGroup: d = "NoCommandGroup"; break;
    case defs::Detail::DisarmRaced: d = "DisarmRaced"; break;
    case defs::Detail::FaultActive: d = "FaultActive (a fault condition is still active)"; break;
    case defs::Detail::MonitorUnavailable: d = "MonitorUnavailable"; break;
    case defs::Detail::RebootPending: d = "RebootPending"; break;
    case defs::Detail::OtaActive: d = "OtaActive"; break;
    case defs::Detail::OtaPendingConfirm: d = "OtaPendingConfirm"; break;
    case defs::Detail::WrongState: d = "WrongState"; break;
    case defs::Detail::LeaseRequired: d = "LeaseRequired (the plugin no longer holds the lease)"; break;
    case defs::Detail::DeviceOffline: d = "DeviceOffline"; break;
    default: break;
    }
    return "result " + std::to_string(result) + (r ? std::string(" ") + r : std::string()) + ", detail " +
           std::to_string(detail) + (d ? std::string(" ") + d : std::string());
}

// A mapped sample time further ahead than this is a bad clock model: the time
// read() first saw the sample is used instead.
constexpr uint64_t kFutureToleranceUs = 1000000;
// The observer mailbox: agent events share it with status and diag frames.
constexpr size_t kEventCapacity = 1024;
constexpr uint8_t kArmed = 1;     // status arm_state
constexpr uint8_t kNoStatus = 0xFF;

// Shared with the library's callback thread (observer), which may outlive a
// teardown by one callback: it holds this by shared_ptr, never the plugin.
struct Watch
{
    std::atomic<uint16_t> client_id{0};
    std::atomic<uint64_t> generation{0}; // the device boot the mapping belongs to
    std::atomic<bool> tracking{false};   // generation is set
    std::atomic<bool> leased{false};     // a lease end naming us is a fault
    std::atomic<uint64_t> dropped{0};    // the observer drops counted when leased was armed
    std::atomic<Fault> fault{Fault::None};
    std::atomic<uint8_t> stale_loop{0};

    // The status stream (H4b). last_arm: the newest arm_state processed.
    // arm_stage, for confirming our ARM: 1 = waiting for a disarmed status
    // processed after the request, 2 = waiting for an armed one, 3 = armed
    // seen. Frames are processed in device order and only the lease holder
    // (us) can ARM, so an armed status after a disarmed one is our ARM.
    std::atomic<uint8_t> last_arm{kNoStatus};
    std::atomic<uint8_t> arm_stage{0};
    std::atomic<bool> armed_expected{false}; // a disarmed status is then a fault
    std::atomic<uint8_t> disarm_reason{kNoStatus};
    std::atomic<uint64_t> last_status_us{0}; // host monotonic time of the newest status frame

    void raise(Fault value) noexcept
    {
        Fault expected = Fault::None;
        fault.compare_exchange_strong(expected, value, std::memory_order_acq_rel);
    }

    void status(uint8_t arm_state, uint8_t reason) noexcept
    {
        last_status_us.store(drivelink::monotonicUs());
        last_arm.store(arm_state);
        if (arm_state != kArmed)
        {
            uint8_t stage = 1;
            arm_stage.compare_exchange_strong(stage, 2);
            if (armed_expected.load())
            {
                uint8_t none = kNoStatus;
                disarm_reason.compare_exchange_strong(none, reason);
                raise(Fault::Disarmed);
            }
        }
        else
        {
            uint8_t stage = 2;
            arm_stage.compare_exchange_strong(stage, 3);
        }
    }
};

Params toParams(const std::unordered_map<std::string, std::string> &map)
{
    return Params(map.begin(), map.end());
}

std::string kindName(uint8_t kind)
{
    switch (defs::AgentClientKind(kind))
    {
    case defs::AgentClientKind::Gui: return "GUI";
    case defs::AgentClientKind::Cli: return "CLI";
    case defs::AgentClientKind::Ros: return "ROS";
    default: return "other";
    }
}

bool withdrawn(const drivelink::Error &e)
{
    return e.kind == drivelink::ErrorKind::Rejected && e.result == uint8_t(defs::Result::Busy) &&
           e.detail == uint8_t(defs::Detail::Withdrawn);
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
} // namespace

struct DriveLinkSystem::Impl
{
    ConnectionConfig config;
    std::vector<ComponentSpec> specs;
    std::unique_ptr<drivelink::Session> session;
    std::unique_ptr<drivelink::Client> client;
    Resolved resolved;
    std::shared_ptr<Watch> watch = std::make_shared<Watch>();
    uint64_t observer = 0;
    uint64_t generation = 0; // the device boot the mapping was resolved on

    // Device calls off the real-time loop (lifecycle, prepare, the worker)
    // hold this. The worker only try-locks it, so a holder may join it.
    std::mutex device;
    std::atomic<bool> closing{false}; // on_error: the session is closing; waits stop

    // One configured subscription group and what read() copies out of it.
    struct Copy
    {
        hardware_interface::StateInterface::SharedPtr handle;
        size_t slot = 0;
        drivelink::ValueType type = drivelink::ValueType::F32;
    };
    struct Run
    {
        uint8_t id = 0, loop = 0;
        std::shared_ptr<drivelink::GroupBuffer> buffer;
        uint64_t stale_us = 0;
        uint16_t counter = 0;
        uint64_t seen_us = 0; // host time read() first saw `counter`
        std::vector<Copy> copies;
    };
    std::vector<Run> runs;
    std::vector<hardware_interface::StateInterface::SharedPtr> handles; // parallel to resolved.states
    drivelink::GroupSample sample{};                                     // read()'s scratch
    std::atomic<bool> running{false};

    // --- H4b: the command path ---
    // Per joint (parallel to resolved.joints): the claimed interface, bumped
    // epoch per claim change, and the position hold. Shared between prepare,
    // perform (either thread) and write() (the real-time loop).
    struct JointState
    {
        std::atomic<CommandKind> claim{CommandKind::None};
        std::atomic<uint32_t> epoch{0};
        std::atomic<double> position_hold{0};
    };
    std::unique_ptr<JointState[]> joints;
    size_t joint_count = 0;
    std::vector<hardware_interface::CommandInterface::SharedPtr> command_handles; // parallel to resolved.commands
    std::vector<std::string> command_names;                                       // "<joint>/<interface>", parallel
    // write()'s own: the last command read per binding and the claim epoch it was read under.
    std::vector<double> last_command;
    std::vector<uint32_t> last_epoch;
    struct Slot
    {
        std::array<size_t, kCommandKinds> bindings{}; // indexes into resolved.commands (one joint)
        uint8_t count = 0;
    };
    struct CommandRun
    {
        uint8_t id = 0, loop = 0;
        std::shared_ptr<drivelink::CommandGroup> group;
        std::vector<Slot> slots;
        std::array<double, 64> values{};
    };
    std::vector<CommandRun> command_runs;
    std::atomic<bool> commanding{false}; // write() sends value frames (arming, armed, or until the DISARM)
    std::atomic<uint64_t> command_drops{0};
    std::atomic<uint64_t> write_cycles{0}; // write() calls that sent frames
    // Per binding (parallel to resolved.commands): the NaN reset of a new
    // claim's handle has not happened yet; write() retries it and holds.
    std::unique_ptr<std::atomic<bool>[]> reset_pending;
    // perform_command_mode_switch calls in progress: teardown waits for 0.
    std::atomic<int> performing{0};
    // command_names (and what it indexes) may be read: false while on_configure rebuilds them.
    std::atomic<bool> names_ready{false};
    uint64_t status_timeout_us = 0; // three status periods, at least 300 ms
    // A claim's prepare -> perform. Only a prepare that claims stores a plan
    // (claimed joints and their kinds); stops are applied by perform from its
    // own list. pending is written under the device mutex before pending_ready
    // (release) and read by perform after taking it (acquire).
    SwitchPlan pending;
    std::atomic<bool> pending_ready{false};
    std::atomic<uint64_t> pending_since_us{0};
    std::atomic<std::thread::id> rt_thread{};
    drivelink::GroupSample prepare_sample{}, perform_sample{};
    // The worker: the DISARM after the last release, off the real-time loop.
    std::thread worker;
    std::atomic<uint32_t> worker_signal{0};
    std::atomic<bool> worker_stop{false};
    std::atomic<bool> disarm_requested{false};
};

namespace
{
using Impl = DriveLinkSystem::Impl;

bool anyClaim(const Impl &m) noexcept
{
    for (size_t j = 0; j < m.joint_count; ++j)
    {
        if (m.joints[j].claim.load(std::memory_order_acquire) != CommandKind::None) { return true; }
    }
    return false;
}

Claims currentClaims(const Impl &m)
{
    Claims claims(m.joint_count, CommandKind::None);
    for (size_t j = 0; j < m.joint_count; ++j) { claims[j] = m.joints[j].claim.load(); }
    return claims;
}

// The joint's measured position from its group buffer (the position hold's
// source); nullopt without a readable sample. Allocation-free; `scratch` is
// the caller thread's own.
std::optional<double> measure(const Impl &m, size_t joint, drivelink::GroupSample &scratch) noexcept
{
    if (joint >= m.resolved.joints.size()) { return std::nullopt; }
    const auto &j = m.resolved.joints[joint];
    if (!j.position_group || *j.position_group >= m.runs.size()) { return std::nullopt; }
    if (!m.runs[*j.position_group].buffer->latest(scratch)) { return std::nullopt; }
    const double value = toDouble(j.position_type, scratch.values[j.position_slot]);
    return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

void holdHere(Impl &m, size_t joint, drivelink::GroupSample &scratch) noexcept
{
    if (const auto position = measure(m, joint, scratch))
    {
        m.joints[joint].position_hold.store(*position);
    }
}

// Off the real-time loop: the position hold from a measured position, retried
// until a readable sample (a torn seqlock read retries) or the deadline. True
// for a joint without a position command. False: no measured position, so the
// caller refuses rather than hold at an old or zero position.
bool holdMeasured(Impl &m, size_t joint, std::chrono::steady_clock::time_point deadline)
{
    if (!m.resolved.joints[joint].position_group) { return true; }
    for (;;)
    {
        if (const auto position = measure(m, joint, m.prepare_sample))
        {
            m.joints[joint].position_hold.store(*position);
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline || m.closing.load()) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Teardown (running is already false): waits until no perform_command_mode_switch
// is inside, so the structures it reads can change. perform re-checks running
// after announcing itself, so none starts using them afterwards.
void waitPerforming(const Impl &m)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (m.performing.load() != 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
}

void stopWorker(Impl &m)
{
    if (!m.worker.joinable()) { return; }
    m.worker_stop = true;
    m.worker_signal.fetch_add(1);
    m.worker_signal.notify_all();
    m.worker.join();
    m.worker_stop = false;
}

// DISARM with the device mutex held. Stops our frames first-hand either way:
// without keepalives the device watchdog then disarms it even if this fails.
bool disarmDevice(Impl &m, const rclcpp::Logger &logger)
{
    m.watch->armed_expected = false;
    m.watch->arm_stage = 0;
    m.disarm_requested = false;
    bool ok = true;
    if (m.client)
    {
        try
        {
            m.client->disarm();
        }
        catch (const std::exception &e)
        {
            RCLCPP_ERROR(logger, "DISARM failed: %s (command frames stopped; the device watchdog disarms it)",
                         e.what());
            ok = false;
        }
    }
    m.commanding = false;
    return ok;
}

void workerLoop(Impl &m, rclcpp::Logger logger)
{
    for (;;)
    {
        const uint32_t seen = m.worker_signal.load();
        if (m.worker_stop.load()) { return; }
        if (m.disarm_requested.load())
        {
            if (!m.device.try_lock())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            std::unique_lock lock(m.device, std::adopt_lock);
            if (m.pending_ready.load())
            {
                // A claim is prepared and about to be performed (which clears
                // the request); if it never is, read() fails the component.
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            // A claim prepared meanwhile handled (or superseded) the request.
            if (m.disarm_requested.exchange(false) && !anyClaim(m) && m.commanding.load())
            {
                if (disarmDevice(m, logger))
                {
                    RCLCPP_INFO(logger, "disarmed: the last command interface was released");
                }
                else
                {
                    m.watch->raise(Fault::DisarmFailed);
                }
            }
            continue;
        }
        m.worker_signal.wait(seen);
    }
}

void startWorker(Impl &m, const rclcpp::Logger &logger)
{
    stopWorker(m);
    m.disarm_requested = false;
    m.worker = std::thread([&m, logger] { workerLoop(m, logger); });
}

// Ends the connection and forgets everything the device gave us. Never waits
// on the agent or the device (Session::close does not), so on_error may call
// it from the controller_manager's loop. The agent then clears our groups and
// ends our lease (and a Take still waiting) itself; ending the lease DISARMs.
// The caller holds m.device (or nothing else can use the connection).
// Idempotent.
void disconnect(DriveLinkSystem::Impl &m)
{
    m.running = false;
    m.commanding = false;
    m.watch->leased = false;
    m.watch->armed_expected = false;
    m.closing = true;
    waitPerforming(m);
    if (m.session)
    {
        if (m.observer) { m.session->unobserve(m.observer); } // waits for a running handler
        m.session->close();
    }
    stopWorker(m); // it only try-locks m.device, so it ends while we hold it
    for (auto &run : m.command_runs)
    {
        if (run.group) { run.group->stop(); }
    }
    m.command_runs.clear();
    m.pending_ready = false;
    m.pending_since_us = 0;
    for (size_t j = 0; j < m.joint_count; ++j) { m.joints[j].claim = CommandKind::None; }
    m.observer = 0;
    m.runs.clear();
    m.handles.clear();
    m.client.reset();
    m.session.reset();
    // resolved.joints, the joint states and the command handles stay until the
    // next configure: a perform_command_mode_switch racing a teardown finds
    // them (with no claims and nothing running).
    m.closing = false;
}

// Clears our subscription groups and gives the lease back, on the
// controller_manager's non-real-time path (deactivate). True when every
// group is cleared and the lease is known to be ours no more; otherwise the
// caller disconnects, and the agent clears what is left.
bool release(DriveLinkSystem::Impl &m, const rclcpp::Logger &logger)
{
    m.running = false;
    m.watch->leased = false;
    waitPerforming(m);
    if (!m.client) { return true; }
    bool cleared = true;
    for (const auto &run : m.runs)
    {
        m.session->removeBuffer(run.id);
        try { m.client->groupClear(run.id, false); }
        catch (const std::exception &e)
        {
            RCLCPP_WARN(logger, "clearing group %u: %s", unsigned(run.id), e.what());
            cleared = false;
        }
    }
    m.runs.clear();
    try
    {
        drivelink::agentLeaseRelease(*m.session);
    }
    catch (const drivelink::Error &e)
    {
        // WrongState: we do not hold it (already taken by another client).
        if (e.result == uint8_t(defs::Result::InvalidArgument) && e.detail == uint8_t(defs::Detail::WrongState))
        {
            return cleared;
        }
        RCLCPP_WARN(logger, "releasing the lease: %s", e.what());
        return false;
    }
    catch (const std::exception &e)
    {
        RCLCPP_WARN(logger, "releasing the lease: %s", e.what());
        return false;
    }
    return cleared;
}

// Waits (bounded) until the observer's arm_stage reaches `stage`.
bool waitStage(const Impl &m, uint8_t stage, std::chrono::steady_clock::time_point deadline)
{
    while (m.watch->arm_stage.load() < stage)
    {
        if (std::chrono::steady_clock::now() > deadline || m.closing.load() ||
            m.watch->fault.load() != Fault::None)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// ARM after the mode write, with the device mutex held; write() feeds hold
// frames from the start. True once the status stream shows our ARM.
bool armDevice(Impl &m, const rclcpp::Logger &logger)
{
    auto &w = *m.watch;
    m.commanding = true;
    w.armed_expected = false;
    w.disarm_reason = kNoStatus;
    w.arm_stage = 1;
    if (w.last_arm.load() != kNoStatus && w.last_arm.load() != kArmed)
    {
        uint8_t stage = 1;
        w.arm_stage.compare_exchange_strong(stage, 2);
    }
    if (!waitStage(m, 2, std::chrono::steady_clock::now() + m.config.timeout))
    {
        RCLCPP_ERROR(logger, "not arming: the status stream did not show the device disarmed within timeout_ms");
        w.arm_stage = 0;
        m.commanding = false;
        return false;
    }
    try
    {
        m.client->arm();
    }
    catch (const drivelink::Error &e)
    {
        if (e.kind == drivelink::ErrorKind::Rejected && !e.outcome_unknown)
        {
            RCLCPP_ERROR(logger, "ARM refused: %s", refusal(e.result, e.detail).c_str());
            w.arm_stage = 0;
            m.commanding = false;
            return false;
        }
        RCLCPP_ERROR(logger, "ARM got no definite answer (%s); disarming", e.what());
        disarmDevice(m, logger);
        // A late armed status of that ARM may still be in flight: the next ARM
        // waits for a fresh disarmed one instead of trusting last_arm.
        w.last_arm = kNoStatus;
        return false;
    }
    if (!waitStage(m, 3, std::chrono::steady_clock::now() + m.config.timeout))
    {
        RCLCPP_ERROR(logger, "ARM was accepted but the status stream did not show the device armed within "
                             "timeout_ms (it may have disarmed at once); disarming");
        disarmDevice(m, logger);
        w.last_arm = kNoStatus;
        return false;
    }
    // From here a disarmed status is a fault (a disarm in between still shows
    // in the next periodic status frame).
    w.armed_expected = true;
    w.arm_stage = 0;
    return true;
}

bool namesOurs(const Impl &m, const std::vector<std::string> &names) noexcept
{
    if (!m.names_ready.load()) { return false; } // on_configure is rebuilding them
    for (const auto &name : names)
    {
        for (const auto &ours : m.command_names)
        {
            if (name == ours) { return true; }
        }
    }
    return false;
}
} // namespace

DriveLinkSystem::DriveLinkSystem() : m_impl(std::make_unique<Impl>()) {}

DriveLinkSystem::~DriveLinkSystem()
{
    // ros2_control normally deactivates and cleans up first; this only makes
    // sure the connection (and with it the lease) ends.
    std::lock_guard lock(m_impl->device);
    disconnect(*m_impl);
}

hardware_interface::CallbackReturn DriveLinkSystem::on_init(
    const hardware_interface::HardwareComponentInterfaceParams &params)
{
    if (SystemInterface::on_init(params) != CallbackReturn::SUCCESS)
    {
        return CallbackReturn::ERROR;
    }
    auto &m = *m_impl;
    try
    {
        m.config = parseConnection(toParams(info_.hardware_parameters));
        if (!info_.gpios.empty())
        {
            throw MappingError("<gpio> is not supported: map joints and sensors by instance label");
        }
        if (info_.is_async)
        {
            // prepare_command_mode_switch tells the control loop's thread by
            // the thread that runs read(); an async component reads elsewhere.
            throw MappingError("is_async is not supported: run the component in the controller_manager's loop");
        }
        m.specs.clear();
        auto add = [&](const hardware_interface::ComponentInfo &component, bool sensor) {
            ComponentSpec spec{component.name, sensor, toParams(component.parameters), {}, {}};
            auto doubles = [&](const hardware_interface::InterfaceInfo &i, const char *what) {
                // read() and write() use doubles; another data_type's handle holds no double.
                if (!i.data_type.empty() && i.data_type != "double")
                {
                    throw MappingError(std::string(sensor ? "sensor '" : "joint '") + component.name + "' " + what +
                                       " '" + i.name + "' has data_type '" + i.data_type +
                                       "'; only double is supported");
                }
            };
            for (const auto &i : component.state_interfaces)
            {
                doubles(i, "state interface");
                spec.state_interfaces.push_back(i.name);
            }
            for (const auto &i : component.command_interfaces)
            {
                doubles(i, "command interface");
                spec.command_interfaces.push_back(i.name);
            }
            m.specs.push_back(std::move(spec));
        };
        for (const auto &joint : info_.joints) { add(joint, false); }
        for (const auto &sensor : info_.sensors) { add(sensor, true); }
    }
    catch (const std::exception &e)
    {
        RCLCPP_ERROR(get_logger(), "%s", e.what());
        return CallbackReturn::ERROR;
    }
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_configure(const rclcpp_lifecycle::State &)
{
    auto &m = *m_impl;
    std::lock_guard lock(m.device);
    disconnect(m);
    m.watch = std::make_shared<Watch>();
    try
    {
        const std::string token = readToken(m.config);
        drivelink::SessionOptions options;
        options.timeout = m.config.timeout;
        options.event_capacity = kEventCapacity;
        // A long-lived link: a timed-out request id is reusable later, as on the agent's own links.
        options.retire_quarantine = std::chrono::seconds(30);
        options.agent = drivelink::AgentHello{token, m.config.device, info_.name.substr(0, 32),
                                              uint8_t(defs::AgentClientKind::Ros)};
        m.session = std::make_unique<drivelink::Session>(drivelink::resolveTcpEndpoint(m.config.agent), options);
        m.client = std::make_unique<drivelink::Client>(*m.session);
        const auto hello = m.session->agentHello();
        m.watch->client_id = hello ? hello->client_id : 0;
        // Agent events, the status stream, the device's Boot and the link's
        // end arrive on the library's callback thread; only atomics cross to
        // read() and write(). Installed before the model loads, so a Boot
        // from here on is seen.
        m.observer = m.session->observe([watch = m.watch, session = m.session.get()](const drivelink::Frame &frame) {
            if (frame.type == 0)
            {
                watch->raise(Fault::LinkLost);
                return;
            }
            // Every stream and event frame carries the boot it arrived in; the
            // device's Boot event is the first of a new one.
            if (watch->tracking.load(std::memory_order_acquire) && frame.generation != watch->generation.load())
            {
                watch->raise(Fault::Rebooted);
            }
            // A dropped notification may have been a Lease event (unobserve
            // waits for this handler, so the session outlives it).
            if (watch->leased.load(std::memory_order_acquire) && session->droppedNotifications() != watch->dropped)
            {
                watch->raise(Fault::EventsLost);
            }
            if (frame.type == msg::kStatusStream)
            {
                try
                {
                    const auto event = drivelink::Client::decodeEvent(frame);
                    if (const auto *status = std::get_if<drivelink::StatusFrame>(&event))
                    {
                        watch->status(status->arm_state, status->last_disarm_reason);
                    }
                }
                catch (const std::exception &)
                {
                    // A malformed status frame says nothing about the arm state.
                }
                return;
            }
            if (frame.type != msg::kAgentEvent) { return; }
            const auto event = drivelink::agent::decodeEvent(frame.payload);
            if (!event) { return; }
            if (event->event == uint8_t(defs::AgentEvent::Closing))
            {
                const auto reason = drivelink::agent::decodeClosing(event->body);
                watch->raise(reason && *reason == uint8_t(defs::AgentCloseReason::DeviceOffline) ? Fault::DeviceOffline
                                                                                                 : Fault::AgentShutdown);
            }
            else if (event->event == uint8_t(defs::AgentEvent::Lease) && watch->leased)
            {
                const auto lease = drivelink::agent::decodeLease(event->body);
                const uint16_t me = watch->client_id;
                // agent.md: a client whose id is previous_id has lost the lease.
                if (lease && (lease->previous_id == me || (lease->holder_id != 0 && lease->holder_id != me)))
                {
                    watch->raise(Fault::LeaseLost);
                }
            }
        });
        {
            // The model loads synchronously and is dropped after resolving;
            // the observer catches a Boot afterwards.
            const uint64_t before = m.session->generation();
            drivelink::DeviceModel model(*m.client);
            const auto snapshot = model.snapshotShared();
            checkDevice(*snapshot);
            m.resolved = resolve(*snapshot, m.specs, m.config);
            // The status stream's rate (system.status.rate_hz) for the StatusLost rule.
            double status_hz = 10;
            if (const auto it = snapshot->system_params.find(defs::system::kStatusRateHz);
                it != snapshot->system_params.end())
            {
                if (const auto *hz = std::get_if<uint16_t>(&it->second); hz && *hz > 0) { status_hz = *hz; }
            }
            m.status_timeout_us = std::max<uint64_t>(300000, uint64_t(3e6 / status_hz));
            m.generation = m.session->generation();
            if (m.generation != before)
            {
                throw std::runtime_error("the device rebooted while its model was loading; configure again");
            }
            m.watch->generation = m.generation;
            m.watch->tracking.store(true, std::memory_order_release);
        }
        m.handles.clear();
        for (const auto &state : m.resolved.states)
        {
            m.handles.push_back(get_state_interface_handle(state.component + "/" + state.interface_name));
        }
        // A perform racing this (running is false) reads none of these while
        // names_ready is false.
        m.names_ready = false;
        waitPerforming(m);
        m.command_handles.clear();
        m.command_names.clear();
        for (const auto &command : m.resolved.commands)
        {
            m.command_names.push_back(command.component + "/" + command.interface_name);
            m.command_handles.push_back(get_command_interface_handle(m.command_names.back()));
        }
        m.last_command.assign(m.resolved.commands.size(), kNaN);
        m.last_epoch.assign(m.resolved.commands.size(), std::numeric_limits<uint32_t>::max());
        m.reset_pending = std::make_unique<std::atomic<bool>[]>(m.resolved.commands.size());
        m.joint_count = m.resolved.joints.size();
        m.joints = std::make_unique<Impl::JointState[]>(m.joint_count);
        m.pending.after.assign(m.joint_count, CommandKind::None);
        m.pending.claimed.reserve(m.joint_count);
        m.pending.released.reserve(m.joint_count);
        m.names_ready = true;
        if (const auto fault = m.watch->fault.load(); fault != Fault::None)
        {
            throw std::runtime_error(describe(fault));
        }
        const auto device = hello ? hello->device : drivelink::agent::DeviceRecord{};
        RCLCPP_INFO(get_logger(),
                    "configured: device '%s' (%s) through %s, %zu state and %zu command interfaces, %zu loop "
                    "group(s), %zu command group(s)",
                    device.alias.c_str(), drivelink::agent::formatDeviceId(device.device_id).c_str(),
                    m.config.agent.c_str(), m.resolved.states.size(), m.resolved.commands.size(),
                    m.resolved.groups.size(), m.resolved.command_groups.size());
        for (const auto &group : m.resolved.groups)
        {
            RCLCPP_INFO(get_logger(), "loop %u at %u Hz: divider %u (%.1f Hz), %zu signals, stale after %.1f ms",
                        unsigned(group.loop), unsigned(group.loop_rate_hz), unsigned(group.divider), group.rate_hz,
                        group.refs.size(), double(group.stale_timeout_us) / 1000.0);
        }
    }
    catch (const std::exception &e)
    {
        RCLCPP_ERROR(get_logger(), "configure failed: %s", e.what());
        disconnect(m);
        return CallbackReturn::FAILURE;
    }
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_activate(const rclcpp_lifecycle::State &)
{
    auto &m = *m_impl;
    std::lock_guard lock(m.device);
    if (!m.client)
    {
        RCLCPP_ERROR(get_logger(), "activate without a configured connection; configure again");
        return CallbackReturn::ERROR;
    }
    if (const auto fault = m.watch->fault.load(); fault != Fault::None)
    {
        RCLCPP_ERROR(get_logger(), "cannot activate: %s", describe(fault));
        return CallbackReturn::ERROR; // on_error disconnects: configure again
    }
    // Before the Take: a refusal leaves the component configured and inactive.
    try
    {
        const uint16_t me = m.watch->client_id;
        for (const auto &record : drivelink::agentClients(*m.session))
        {
            if (record.holder && record.client_id != me)
            {
                if (m.config.take_lease == TakeLease::Refuse)
                {
                    RCLCPP_ERROR(get_logger(),
                                 "the lease is held by '%s' (%s, client %u): release it there (the GUI's lease "
                                 "button), or set take_lease to preempt to take it",
                                 record.name.c_str(), kindName(record.kind).c_str(), unsigned(record.client_id));
                    return CallbackReturn::FAILURE;
                }
                RCLCPP_WARN(get_logger(), "taking the lease from '%s' (%s) because take_lease is preempt",
                            record.name.c_str(), kindName(record.kind).c_str());
            }
        }
    }
    catch (const std::exception &e)
    {
        RCLCPP_ERROR(get_logger(), "activate failed: %s", e.what());
        return CallbackReturn::FAILURE;
    }
    // From the Take on, any failure disconnects (returning ERROR, so on_error
    // runs): closing is the only certain end of a Take whose answer was lost.
    try
    {
        // Armed before the Take: a lease end naming us can only follow the grant.
        m.watch->dropped = m.session->droppedNotifications();
        m.watch->leased.store(true, std::memory_order_release);
        drivelink::agentLeaseTake(*m.session);

        auto groups = m.client->groups();
        std::vector<uint8_t> free, free_commands;
        for (const auto &g : groups.groups)
        {
            if (g.configured) { continue; }
            if (g.kind == drivelink::GroupKind::Subscription) { free.push_back(g.id); }
            else { free_commands.push_back(g.id); }
        }
        if (free.size() < m.resolved.groups.size())
        {
            throw std::runtime_error("needs " + std::to_string(m.resolved.groups.size()) +
                                     " subscription group(s), only " + std::to_string(free.size()) + " are free");
        }
        if (free_commands.size() < m.resolved.command_groups.size())
        {
            throw std::runtime_error("needs " + std::to_string(m.resolved.command_groups.size()) +
                                     " command group(s), only " + std::to_string(free_commands.size()) + " are free");
        }
        m.runs.clear();
        for (size_t i = 0; i < m.resolved.groups.size(); ++i)
        {
            const auto &group = m.resolved.groups[i];
            Impl::Run run;
            run.id = free[i];
            run.loop = group.loop;
            run.stale_us = group.stale_timeout_us;
            drivelink::GroupMutation configured;
            for (int attempt = 1;; ++attempt)
            {
                try
                {
                    configured = m.client->subConfigure(run.id, group.loop, group.divider, group.refs);
                    break;
                }
                catch (const drivelink::Error &e)
                {
                    if (withdrawn(e) && attempt < 3) { continue; }
                    throw;
                }
            }
            m.runs.push_back(std::move(run));
            auto &added = m.runs.back();
            // The refs were resolved on configure's boot; on another they may name other signals.
            if (configured.generation != m.generation)
            {
                throw std::runtime_error("the device rebooted since configure; configure again");
            }
            added.buffer = m.session->subscribeBuffer(added.id, configured.epoch, group.refs.size(), 8, configured.generation);
        }
        for (size_t s = 0; s < m.resolved.states.size(); ++s)
        {
            const auto &state = m.resolved.states[s];
            m.runs[state.group].copies.push_back({m.handles[s], state.slot, state.type});
        }
        // The first sample of every group, so read() starts fresh.
        const auto deadline = std::chrono::steady_clock::now() + m.config.timeout;
        for (size_t i = 0; i < m.runs.size(); ++i)
        {
            auto &run = m.runs[i];
            const auto period = std::chrono::microseconds(uint64_t(1e6 / m.resolved.groups[i].rate_hz));
            const auto group_deadline = deadline + 3 * period;
            while (!run.buffer->latest(m.sample))
            {
                if (std::chrono::steady_clock::now() > group_deadline)
                {
                    throw std::runtime_error("no sample from loop " + std::to_string(run.loop) +
                                             " within timeout_ms plus three group periods");
                }
                if (m.watch->fault.load() != Fault::None) { break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            run.counter = m.sample.counter;
            run.seen_us = drivelink::monotonicUs();
        }
        // H4b: one command group per loop with a command input, without
        // keepalives: only write() feeds the device watchdog, so a stalled
        // control loop disarms the device. Configured now, disarmed
        // (CMD_CONFIG is refused while armed); nothing is sent until a claim.
        m.command_runs.clear();
        for (size_t i = 0; i < m.resolved.command_groups.size(); ++i)
        {
            const auto &group = m.resolved.command_groups[i];
            Impl::CommandRun run;
            run.id = free_commands[i];
            run.loop = group.loop;
            run.slots.resize(group.refs.size());
            const std::vector<uint32_t> zeros(group.refs.size(), 0);
            for (int attempt = 1;; ++attempt)
            {
                try
                {
                    run.group = m.client->managedCommand(run.id, group.loop, group.refs, zeros, false);
                    break;
                }
                catch (const drivelink::Error &e)
                {
                    if (withdrawn(e) && attempt < 3) { continue; }
                    throw;
                }
            }
            m.command_runs.push_back(std::move(run));
            if (m.session->generation() != m.generation)
            {
                throw std::runtime_error("the device rebooted since configure; configure again");
            }
        }
        for (size_t c = 0; c < m.resolved.commands.size(); ++c)
        {
            const auto &command = m.resolved.commands[c];
            auto &slot = m.command_runs[command.group].slots[command.slot];
            slot.bindings[slot.count++] = c;
        }
        m.last_command.assign(m.resolved.commands.size(), kNaN);
        m.last_epoch.assign(m.resolved.commands.size(), std::numeric_limits<uint32_t>::max());
        for (size_t j = 0; j < m.joint_count; ++j)
        {
            m.joints[j].claim = CommandKind::None;
            m.joints[j].epoch.fetch_add(1);
            m.joints[j].position_hold = 0;
            holdHere(m, j, m.prepare_sample);
        }
        m.commanding = false;
        m.pending_ready = false;
        m.pending_since_us = 0;
        m.watch->armed_expected = false;
        m.watch->arm_stage = 0;
        // system.status.rate_hz is live: read it again for the StatusLost rule.
        // (Lowered while armed, the rule fails safe with a spurious ERROR.)
        {
            const drivelink::Ref rate{0, 0, defs::system::kStatusRateHz};
            const auto result = m.client->get(std::span<const drivelink::Ref>(&rate, 1));
            if (result.values.size() == 1 && result.values[0] > 0)
            {
                m.status_timeout_us = std::max<uint64_t>(300000, uint64_t(3e6 / double(result.values[0])));
            }
        }
        if (const auto fault = m.watch->fault.load(); fault != Fault::None)
        {
            throw std::runtime_error(describe(fault));
        }
        if (!m.command_runs.empty()) { startWorker(m, get_logger()); }
        m.running = true;
        RCLCPP_INFO(get_logger(), "active: lease held, %zu group(s) streaming, %zu command group(s) ready (disarmed)",
                    m.runs.size(), m.command_runs.size());
    }
    catch (const std::exception &e)
    {
        RCLCPP_ERROR(get_logger(), "activate failed: %s; disconnecting (configure again)", e.what());
        disconnect(m);
        return CallbackReturn::ERROR;
    }
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_deactivate(const rclcpp_lifecycle::State &)
{
    auto &m = *m_impl;
    // No perform may change the claims or read the command groups from here on.
    m.running = false;
    waitPerforming(m);
    stopWorker(m); // before the lock: it may be finishing a DISARM
    std::lock_guard lock(m.device);
    bool ok = true;
    m.pending_ready = false;
    m.pending_since_us = 0;
    if (m.commanding.load() || anyClaim(m) || m.watch->armed_expected.load())
    {
        ok = disarmDevice(m, get_logger());
    }
    for (size_t j = 0; j < m.joint_count; ++j) { m.joints[j].claim = CommandKind::None; }
    for (auto &run : m.command_runs)
    {
        run.group->stop();
        if (!m.client) { continue; }
        try { m.client->groupClear(run.id, true); }
        catch (const std::exception &e)
        {
            RCLCPP_WARN(get_logger(), "clearing command group %u: %s", unsigned(run.id), e.what());
            ok = false;
        }
    }
    m.command_runs.clear();
    if (!release(m, get_logger()) || !ok)
    {
        // The lease may still be ours: closing the connection ends it for certain (and DISARMs).
        RCLCPP_ERROR(get_logger(),
                     "a DISARM, a group clear or the lease release was not confirmed; disconnecting (configure again)");
        disconnect(m);
        return CallbackReturn::ERROR;
    }
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_cleanup(const rclcpp_lifecycle::State &)
{
    std::lock_guard lock(m_impl->device);
    disconnect(*m_impl);
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_shutdown(const rclcpp_lifecycle::State &)
{
    std::lock_guard lock(m_impl->device);
    disconnect(*m_impl);
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_error(const rclcpp_lifecycle::State &)
{
    // Called from the controller_manager's loop after read() returned ERROR:
    // no device call here, only the connection's end (the agent then clears
    // our groups and lease, and the lease end DISARMs). Closing first makes a
    // prepare or the worker holding the device mutex give up at once.
    auto &m = *m_impl;
    m.closing = true;
    m.running = false;
    m.commanding = false;
    if (m.session) { m.session->close(); }
    std::lock_guard lock(m.device);
    const auto fault = m.watch->fault.load();
    if (fault == Fault::Stale)
    {
        RCLCPP_ERROR(get_logger(), "%s (loop %u); disconnected, the component is unconfigured", describe(fault),
                     unsigned(m.watch->stale_loop.load()));
    }
    else if (fault == Fault::Disarmed)
    {
        RCLCPP_ERROR(get_logger(), "%s (%s); disconnected, the component is unconfigured", describe(fault),
                     disarmReason(m.watch->disarm_reason.load()));
    }
    else if (fault != Fault::None)
    {
        RCLCPP_ERROR(get_logger(), "%s; disconnected, the component is unconfigured", describe(fault));
    }
    if (const auto drops = m.command_drops.load(); drops != 0)
    {
        RCLCPP_ERROR(get_logger(), "%llu command frame(s) were refused locally (queue full or an invalid value)",
                     static_cast<unsigned long long>(drops));
    }
    disconnect(m);
    return CallbackReturn::SUCCESS;
}

return_type DriveLinkSystem::read(const rclcpp::Time &, const rclcpp::Duration &) noexcept
{
    auto &m = *m_impl;
    // prepare_command_mode_switch refuses claims made on this thread.
    m.rt_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
    if (!m.running.load(std::memory_order_acquire))
    {
        return return_type::OK; // inactive: nothing streams; a fault found now refuses the next activate
    }
    try
    {
        if (m.watch->fault.load(std::memory_order_acquire) != Fault::None)
        {
            return return_type::ERROR;
        }
        const uint64_t now = drivelink::monotonicUs();
        if (const uint64_t since = m.pending_since_us.load(std::memory_order_acquire);
            since != 0 && now > since && now - since > uint64_t(m.config.timeout.count()) * 1000)
        {
            m.watch->raise(Fault::SwitchAbandoned);
            return return_type::ERROR;
        }
        // Armed, a disarm shows only in the status stream: without it for
        // three periods a disarm could go unseen.
        if (m.watch->armed_expected.load(std::memory_order_acquire))
        {
            const uint64_t last = m.watch->last_status_us.load(std::memory_order_acquire);
            if (last != 0 && now > last && now - last > m.status_timeout_us)
            {
                m.watch->raise(Fault::StatusLost);
                return return_type::ERROR;
            }
        }
        for (auto &run : m.runs)
        {
            if (!run.buffer->latest(m.sample))
            {
                // No readable sample (an invalidated buffer, or a torn read
                // that retries next cycle): the last one seen ages on.
                if (isStale(now, run.seen_us, run.stale_us))
                {
                    m.watch->stale_loop = run.loop;
                    m.watch->raise(Fault::Stale);
                    return return_type::ERROR;
                }
                continue;
            }
            if (m.sample.counter != run.counter)
            {
                run.counter = m.sample.counter;
                run.seen_us = now;
            }
            // The device time of the sample mapped through the agent's clock
            // model; until that mapping is valid (or if it lies far ahead),
            // when read() first saw it.
            uint64_t sample_us = run.seen_us;
            if (m.sample.mapped && m.sample.host_us > 0 && m.sample.host_us <= double(now + kFutureToleranceUs))
            {
                sample_us = uint64_t(m.sample.host_us);
            }
            if (isStale(now, sample_us, run.stale_us))
            {
                m.watch->stale_loop = run.loop;
                m.watch->raise(Fault::Stale);
                return return_type::ERROR;
            }
            for (const auto &copy : run.copies)
            {
                // try_lock: a reader holding the handle keeps last cycle's value.
                std::ignore = set_state(copy.handle, toDouble(copy.type, m.sample.values[copy.slot]), false);
            }
        }
    }
    catch (...)
    {
        m.watch->raise(Fault::Internal);
        return return_type::ERROR;
    }
    return return_type::OK;
}

return_type DriveLinkSystem::write(const rclcpp::Time &, const rclcpp::Duration &) noexcept
{
    auto &m = *m_impl;
    if (!m.running.load(std::memory_order_acquire) || !m.commanding.load(std::memory_order_acquire))
    {
        return return_type::OK; // disarmed and unclaimed: no frames (and no keepalives)
    }
    try
    {
        for (auto &run : m.command_runs)
        {
            for (size_t s = 0; s < run.slots.size(); ++s)
            {
                const auto &slot = run.slots[s];
                // The binding of the interface its joint claims, else the first.
                size_t chosen = slot.bindings[0];
                const size_t joint = m.resolved.commands[chosen].joint;
                auto &state = m.joints[joint];
                const CommandKind claim = state.claim.load(std::memory_order_acquire);
                const uint32_t epoch = state.epoch.load(std::memory_order_acquire);
                bool claimed = false;
                for (uint8_t k = 0; k < slot.count; ++k)
                {
                    if (m.resolved.commands[slot.bindings[k]].kind == claim)
                    {
                        chosen = slot.bindings[k];
                        claimed = true;
                        break;
                    }
                }
                double command = kNaN;
                if (claimed && m.reset_pending[chosen].load(std::memory_order_acquire))
                {
                    // perform could not clear what a previous controller left:
                    // hold until the reset takes the handle.
                    if (set_command(m.command_handles[chosen], kNaN, false))
                    {
                        m.reset_pending[chosen].store(false, std::memory_order_release);
                    }
                }
                else if (claimed)
                {
                    double value = kNaN;
                    // try_lock: a controller holding the handle leaves last cycle's value (same claim only).
                    if (get_command(m.command_handles[chosen], value, false))
                    {
                        m.last_command[chosen] = value;
                        m.last_epoch[chosen] = epoch;
                        command = value;
                    }
                    else if (m.last_epoch[chosen] == epoch)
                    {
                        command = m.last_command[chosen];
                    }
                }
                const CommandKind kind = m.resolved.commands[chosen].kind;
                // Inputs are f32 (resolve): a finite double beyond its range
                // would become infinite and the whole frame refused.
                constexpr double kF32Max = double(std::numeric_limits<float>::max());
                const double value = std::clamp(
                    slotValue(kind, claimed, command, state.position_hold.load()), -kF32Max,
                    kF32Max);
                // A position hold is the last position sent while claimed.
                if (kind == CommandKind::Position && claimed && std::isfinite(command))
                {
                    state.position_hold.store(command, std::memory_order_relaxed);
                }
                run.values[s] = value;
            }
            if (!run.group->write({run.values.data(), run.slots.size()}))
            {
                m.command_drops.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // seq_cst like the hold loads and prepare's hold stores: a call that
        // counts after prepare read the counter also loaded the new holds.
        m.write_cycles.fetch_add(1);
    }
    catch (...)
    {
        m.watch->raise(Fault::Internal);
        return return_type::ERROR;
    }
    return return_type::OK;
}

return_type DriveLinkSystem::prepare_command_mode_switch(const std::vector<std::string> &start,
                                                         const std::vector<std::string> &stop)
{
    // Only a claim does work here (off the real-time loop: the mode write and
    // ARM). Stops need nothing: perform applies them from its own list, so a
    // stop-only prepare (also on the real-time thread, in ros2_control's
    // error and undo paths) only validates.
    auto &m = *m_impl;
    if (!m.running.load())
    {
        if (namesOurs(m, start))
        {
            RCLCPP_ERROR(get_logger(), "cannot claim a command interface: the component is not active");
            return return_type::ERROR;
        }
        return return_type::OK;
    }
    try
    {
        if (planSwitch(m.resolved, currentClaims(m), start, stop).claimed.empty()) { return return_type::OK; }
    }
    catch (const std::exception &e)
    {
        RCLCPP_ERROR(get_logger(), "command mode switch refused: %s", e.what());
        return return_type::ERROR;
    }
    if (std::this_thread::get_id() == m.rt_thread.load(std::memory_order_relaxed))
    {
        RCLCPP_ERROR(get_logger(), "a command interface claim from the real-time loop (a fallback controller) is not "
                                   "supported: claiming needs a mode write and ARM, which would block the loop");
        return return_type::ERROR;
    }
    std::lock_guard lock(m.device);
    if (!m.client || !m.running.load() || m.watch->fault.load() != Fault::None)
    {
        RCLCPP_ERROR(get_logger(), "cannot claim a command interface: the component is failing or inactive");
        return return_type::ERROR;
    }
    if (m.pending_ready.load())
    {
        // An earlier claim was prepared but never performed: its mode writes
        // may not match the running controllers.
        m.watch->raise(Fault::SwitchAbandoned);
        RCLCPP_ERROR(get_logger(), "%s; refusing this switch", describe(Fault::SwitchAbandoned));
        return return_type::ERROR;
    }
    try
    {
        // Planned again under the lock: a stop performed on the real-time
        // thread meanwhile changes the claims.
        SwitchPlan plan = planSwitch(m.resolved, currentClaims(m), start, stop);
        if (plan.claimed.empty()) { return return_type::OK; }
        // A DISARM still owed after the last release goes first, so ARM
        // starts from a known disarmed device.
        if (m.disarm_requested.exchange(false) && m.commanding.load() && !anyClaim(m))
        {
            if (!disarmDevice(m, get_logger()))
            {
                m.watch->raise(Fault::DisarmFailed);
                return return_type::ERROR;
            }
        }
        // Whether this switch arms: the device is not armed by us now (not
        // the plan's view, which a concurrent release or the owed DISARM
        // above may have overtaken).
        const bool arm = !m.commanding.load();
        // Holds before the mode write: until perform the claimed slots are
        // unclaimed, so write() sends these, and they are the first values
        // the controller sees in its new mode. At ARM every position hold is
        // refreshed: an unclaimed joint in position mode holds where it is.
        // A joint without a measured position refuses the switch.
        const auto hold_deadline = std::chrono::steady_clock::now() + m.config.timeout;
        for (size_t j = 0; j < m.joint_count; ++j)
        {
            const bool claiming = std::find(plan.claimed.begin(), plan.claimed.end(), j) != plan.claimed.end();
            if ((arm || claiming) && !holdMeasured(m, j, hold_deadline))
            {
                RCLCPP_ERROR(get_logger(), "command mode switch refused: no readable position of joint '%s' for its "
                                           "hold within timeout_ms",
                             m.resolved.joints[j].name.c_str());
                return return_type::ERROR;
            }
        }
        std::vector<drivelink::Ref> refs;
        std::vector<uint32_t> values;
        std::string modes;
        for (const size_t j : plan.claimed)
        {
            const auto &joint = m.resolved.joints[j];
            if (!joint.mode) { continue; }
            const auto kind = plan.after[j];
            refs.push_back(*joint.mode);
            values.push_back(joint.mode_values[size_t(kind)]);
            modes += (modes.empty() ? "" : ", ") + joint.name + " " + commandName(kind);
        }
        if (!refs.empty() && !arm)
        {
            // Armed: frames built before the new holds may still be queued.
            // One sent after the mode write would release the controller's
            // wait for a fresh command with an old value (a position hold
            // from long ago). Wait until write() has built frames with the
            // new holds, then drop everything queued before them.
            const uint64_t cycles = m.write_cycles.load();
            while (m.write_cycles.load() < cycles + 2)
            {
                if (std::chrono::steady_clock::now() > hold_deadline || m.closing.load() ||
                    m.watch->fault.load() != Fault::None)
                {
                    RCLCPP_ERROR(get_logger(), "command mode switch refused: write() did not run within timeout_ms");
                    return return_type::ERROR;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            for (auto &run : m.command_runs) { run.group->discardQueued(); }
        }
        if (!refs.empty())
        {
            try
            {
                m.client->set(refs, values);
            }
            catch (const drivelink::Error &e)
            {
                if (e.outcome_unknown && m.commanding.load())
                {
                    // While armed a joint may now be in a mode no controller drives.
                    m.watch->raise(Fault::ModeUncertain);
                }
                RCLCPP_ERROR(get_logger(), "command mode switch failed: writing the mode (%s): %s", modes.c_str(),
                             e.what());
                return return_type::ERROR;
            }
        }
        if (arm)
        {
            if (!armDevice(m, get_logger())) { return return_type::ERROR; }
            RCLCPP_INFO(get_logger(), "armed (%s)", modes.empty() ? "no mode writes" : modes.c_str());
        }
        else if (!modes.empty())
        {
            RCLCPP_INFO(get_logger(), "mode switch while armed (%s)", modes.c_str());
        }
        m.pending = std::move(plan);
        m.pending_since_us.store(drivelink::monotonicUs(), std::memory_order_release);
        m.pending_ready.store(true, std::memory_order_release);
    }
    catch (const std::exception &e)
    {
        RCLCPP_ERROR(get_logger(), "command mode switch failed: %s", e.what());
        // An unexpected failure while arming must not leave hold frames
        // feeding the watchdog with nothing claimed.
        if (m.commanding.load() && !anyClaim(m) && !disarmDevice(m, get_logger()))
        {
            m.watch->raise(Fault::DisarmFailed);
        }
        return return_type::ERROR;
    }
    return return_type::OK;
}

namespace
{
// "<joint>/<interface>" -> the binding index, without allocating; nullopt
// for another component's interface.
std::optional<size_t> bindingOf(const Impl &m, const std::string &name) noexcept
{
    if (!m.names_ready.load()) { return std::nullopt; }
    for (size_t i = 0; i < m.command_names.size(); ++i)
    {
        if (m.command_names[i] == name) { return i; }
    }
    return std::nullopt;
}

struct Performing
{
    std::atomic<int> &count;
    explicit Performing(std::atomic<int> &c) : count(c) { count.fetch_add(1); }
    ~Performing() { count.fetch_sub(1); }
};
} // namespace

return_type DriveLinkSystem::perform_command_mode_switch(const std::vector<std::string> &start,
                                                         const std::vector<std::string> &stop)
{
    // Real-time (or the switching thread beside it): no allocation, no lock,
    // no device call. Stops are applied from the list itself; claims only as
    // their prepare planned.
    auto &m = *m_impl;
    const Performing performing(m.performing);
    if (!m.running.load())
    {
        return namesOurs(m, start) ? return_type::ERROR : return_type::OK;
    }
    const bool before = anyClaim(m);
    for (const auto &name : stop)
    {
        const auto index = bindingOf(m, name);
        if (!index) { continue; }
        const auto &binding = m.resolved.commands[*index];
        auto &state = m.joints[binding.joint];
        CommandKind expected = binding.kind;
        if (binding.kind == CommandKind::Position)
        {
            // A released position claim holds where the joint is now (before
            // the release is visible to write()).
            if (state.claim.load() != CommandKind::Position) { continue; }
            holdHere(m, binding.joint, m.perform_sample);
        }
        if (state.claim.compare_exchange_strong(expected, CommandKind::None, std::memory_order_acq_rel))
        {
            state.epoch.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    if (namesOurs(m, start))
    {
        if (!m.pending_ready.load(std::memory_order_acquire))
        {
            m.watch->raise(Fault::SwitchMismatch); // a claim nobody prepared
            return return_type::ERROR;
        }
        // The plan is consumed only after it has been read: until then a
        // concurrent prepare sees it pending and refuses instead of
        // replacing it.
        struct Consume
        {
            Impl &m;
            ~Consume() { m.pending_ready.store(false, std::memory_order_release); }
        } consume{m};
        m.pending_since_us.store(0, std::memory_order_release);
        // The claims must be exactly the prepared ones.
        size_t ours = 0;
        for (const auto &name : start)
        {
            const auto index = bindingOf(m, name);
            if (!index) { continue; }
            const auto &binding = m.resolved.commands[*index];
            ++ours;
            const bool planned = binding.joint < m.pending.after.size() &&
                                 m.pending.after[binding.joint] == binding.kind &&
                                 std::find(m.pending.claimed.begin(), m.pending.claimed.end(), binding.joint) !=
                                     m.pending.claimed.end();
            if (!planned)
            {
                m.watch->raise(Fault::SwitchMismatch);
                return return_type::ERROR;
            }
        }
        if (ours != m.pending.claimed.size())
        {
            m.watch->raise(Fault::SwitchMismatch);
            return return_type::ERROR;
        }
        for (const size_t j : m.pending.claimed)
        {
            auto &state = m.joints[j];
            const auto kind = m.pending.after[j];
            // The new claim starts unset: a value a previous controller left
            // in the handle is not a command. Reset it first, then the epoch,
            // then publish the claim, so write() never pairs the claim with
            // the old contents; a reset that cannot take the handle now is
            // retried by write(), which holds until it succeeds.
            if (const auto index = m.resolved.joints[j].commands[size_t(kind)])
            {
                bool reset = false;
                for (int attempt = 0; attempt < 100 && !reset; ++attempt)
                {
                    reset = set_command(m.command_handles[*index], kNaN, false);
                }
                m.reset_pending[*index].store(!reset, std::memory_order_release);
            }
            state.epoch.fetch_add(1, std::memory_order_acq_rel);
            state.claim.store(kind, std::memory_order_release);
        }
    }
    if (anyClaim(m))
    {
        m.disarm_requested.store(false); // a claim supersedes an owed DISARM
    }
    else if (before)
    {
        m.disarm_requested.store(true);
        m.worker_signal.fetch_add(1);
        m.worker_signal.notify_one();
    }
    return return_type::OK;
}
} // namespace drivelink_hardware

PLUGINLIB_EXPORT_CLASS(drivelink_hardware::DriveLinkSystem, hardware_interface::SystemInterface)
