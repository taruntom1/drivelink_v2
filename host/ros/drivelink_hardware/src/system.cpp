// drivelink_hardware/DriveLinkSystem (README.md; the design is
// plans/host-architecture.md "drivelink_hardware (ROS 2)"). H4a: the state path.
#include "drivelink_hardware/system.hpp"
#include "drivelink_hardware/mapping.hpp"

#include <drivelink/buffers.hpp>
#include <drivelink/client.hpp>
#include <drivelink/model.hpp>
#include <drivelink/session.hpp>
#include <drivelink/protocol/gen/registry.hpp>
#include <hardware_interface/types/hardware_interface_return_values.hpp>
#include <pluginlib/class_list_macros.hpp>
#include <rclcpp/logging.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <tuple>

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
    case Fault::Internal: return "internal error in read()";
    }
    return "unknown fault";
}

// A mapped sample time further ahead than this is a bad clock model: the time
// read() first saw the sample is used instead.
constexpr uint64_t kFutureToleranceUs = 1000000;
// The observer mailbox: agent events share it with status and diag frames.
constexpr size_t kEventCapacity = 1024;

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

    void raise(Fault value) noexcept
    {
        Fault expected = Fault::None;
        fault.compare_exchange_strong(expected, value, std::memory_order_acq_rel);
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
};

namespace
{
// Ends the connection and forgets everything the device gave us. Never waits
// on the agent or the device (Session::close does not), so on_error may call
// it from the controller_manager's loop. The agent then clears our groups and
// ends our lease (and a Take still waiting) itself. Idempotent.
void disconnect(DriveLinkSystem::Impl &m)
{
    m.running = false;
    m.watch->leased = false;
    if (m.session)
    {
        if (m.observer) { m.session->unobserve(m.observer); } // waits for a running handler
        m.session->close();
    }
    m.observer = 0;
    m.runs.clear();
    m.handles.clear();
    m.client.reset();
    m.session.reset();
    m.resolved = {};
}

// Clears our subscription groups and gives the lease back, on the
// controller_manager's non-real-time path (deactivate). True when the lease
// is known to be ours no more.
bool release(DriveLinkSystem::Impl &m, const rclcpp::Logger &logger)
{
    m.running = false;
    m.watch->leased = false;
    if (!m.client) { return true; }
    for (const auto &run : m.runs)
    {
        m.session->removeBuffer(run.id);
        try { m.client->groupClear(run.id, false); }
        catch (const std::exception &e) { RCLCPP_WARN(logger, "clearing group %u: %s", unsigned(run.id), e.what()); }
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
            return true;
        }
        RCLCPP_WARN(logger, "releasing the lease: %s", e.what());
        return false;
    }
    catch (const std::exception &e)
    {
        RCLCPP_WARN(logger, "releasing the lease: %s", e.what());
        return false;
    }
    return true;
}
} // namespace

DriveLinkSystem::DriveLinkSystem() : m_impl(std::make_unique<Impl>()) {}

DriveLinkSystem::~DriveLinkSystem()
{
    // ros2_control normally deactivates and cleans up first; this only makes
    // sure the connection (and with it the lease) ends.
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
        m.specs.clear();
        auto add = [&](const hardware_interface::ComponentInfo &component, bool sensor) {
            ComponentSpec spec{component.name, sensor, toParams(component.parameters), {}, {}};
            for (const auto &i : component.state_interfaces)
            {
                // read() writes doubles; another data_type's handle holds no double.
                if (!i.data_type.empty() && i.data_type != "double")
                {
                    throw MappingError(std::string(sensor ? "sensor '" : "joint '") + component.name +
                                       "' state interface '" + i.name + "' has data_type '" + i.data_type +
                                       "'; only double is supported");
                }
                spec.state_interfaces.push_back(i.name);
            }
            for (const auto &i : component.command_interfaces) { spec.command_interfaces.push_back(i.name); }
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
        // Agent events, the device's Boot and the link's end arrive on the
        // library's callback thread; only atomics cross to read(). Installed
        // before the model loads, so a Boot from here on is seen.
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
        const auto device = hello ? hello->device : drivelink::agent::DeviceRecord{};
        RCLCPP_INFO(get_logger(), "configured: device '%s' (%s) through %s, %zu state interfaces in %zu loop group(s)",
                    device.alias.c_str(), drivelink::agent::formatDeviceId(device.device_id).c_str(),
                    m.config.agent.c_str(), m.resolved.states.size(), m.resolved.groups.size());
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
        std::vector<uint8_t> free;
        for (const auto &g : groups.groups)
        {
            if (g.kind == drivelink::GroupKind::Subscription && !g.configured) { free.push_back(g.id); }
        }
        if (free.size() < m.resolved.groups.size())
        {
            throw std::runtime_error("needs " + std::to_string(m.resolved.groups.size()) +
                                     " subscription group(s), only " + std::to_string(free.size()) + " are free");
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
        if (const auto fault = m.watch->fault.load(); fault != Fault::None)
        {
            throw std::runtime_error(describe(fault));
        }
        m.running = true;
        RCLCPP_INFO(get_logger(), "active: lease held, %zu group(s) streaming", m.runs.size());
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
    if (!release(*m_impl, get_logger()))
    {
        // The lease may still be ours: closing the connection ends it for certain.
        RCLCPP_ERROR(get_logger(), "the lease release was not confirmed; disconnecting (configure again)");
        disconnect(*m_impl);
        return CallbackReturn::ERROR;
    }
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_cleanup(const rclcpp_lifecycle::State &)
{
    disconnect(*m_impl);
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_shutdown(const rclcpp_lifecycle::State &)
{
    disconnect(*m_impl);
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_error(const rclcpp_lifecycle::State &)
{
    // Called from the controller_manager's loop after read() returned ERROR:
    // no device call here, only the connection's end (the agent then clears
    // our groups and lease).
    auto &m = *m_impl;
    const auto fault = m.watch->fault.load();
    if (fault == Fault::Stale)
    {
        RCLCPP_ERROR(get_logger(), "%s (loop %u); disconnected, the component is unconfigured", describe(fault),
                     unsigned(m.watch->stale_loop.load()));
    }
    else if (fault != Fault::None)
    {
        RCLCPP_ERROR(get_logger(), "%s; disconnected, the component is unconfigured", describe(fault));
    }
    disconnect(m);
    return CallbackReturn::SUCCESS;
}

return_type DriveLinkSystem::read(const rclcpp::Time &, const rclcpp::Duration &) noexcept
{
    auto &m = *m_impl;
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
            if (m.sample.mapped && m.sample.host_us > 0 && uint64_t(m.sample.host_us) <= now + kFutureToleranceUs)
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

return_type DriveLinkSystem::write(const rclcpp::Time &, const rclcpp::Duration &)
{
    return return_type::OK; // H4a has no command interfaces
}
} // namespace drivelink_hardware

PLUGINLIB_EXPORT_CLASS(drivelink_hardware::DriveLinkSystem, hardware_interface::SystemInterface)
