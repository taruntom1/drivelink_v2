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
using hardware_interface::return_type;

// Why the component left the active state. The first fault wins; read()
// reports it with ERROR and on_error() logs it (read() never logs).
enum class Fault : uint8_t
{
    None,
    LeaseLost,
    DeviceOffline,
    AgentShutdown,
    LinkLost,
    Rebooted,
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
    case Fault::Rebooted: return "the device rebooted; the mapping must be loaded again";
    case Fault::Stale: return "no fresh sample within stale_timeout_ms";
    case Fault::Internal: return "internal error in read()";
    }
    return "unknown fault";
}

constexpr uint8_t kClientKindRos = 3;      // AgentClientKind Ros
constexpr uint8_t kEventClosing = 1;       // agent events (docs/protocol/agent.md)
constexpr uint8_t kEventLease = 2;
constexpr uint8_t kCloseDeviceOffline = 1; // AgentCloseReason

// Shared with the library's callback thread (observer), which may outlive a
// teardown by one callback: it holds this by shared_ptr, never the plugin.
struct Watch
{
    std::atomic<uint16_t> client_id{0};
    std::atomic<bool> leased{false}; // a lease end naming us is a fault
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
    switch (kind)
    {
    case 1: return "GUI";
    case 2: return "CLI";
    case 3: return "ROS";
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
    uint64_t generation = 0; // the session link generation the groups belong to

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

DriveLinkSystem::DriveLinkSystem() : m_impl(std::make_unique<Impl>()) {}

DriveLinkSystem::~DriveLinkSystem()
{
    // ros2_control normally deactivates and cleans up first; this only makes
    // sure the connection (and with it the lease) ends.
    if (m_impl->session)
    {
        m_impl->session->close();
    }
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
            for (const auto &i : component.state_interfaces) { spec.state_interfaces.push_back(i.name); }
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

namespace
{
// Ends the connection and forgets everything the device gave us. Idempotent.
void disconnect(auto &m)
{
    m.running = false;
    m.watch->leased = false;
    if (m.session)
    {
        if (m.observer) { m.session->unobserve(m.observer); }
        m.session->close();
    }
    m.observer = 0;
    m.runs.clear();
    m.handles.clear();
    m.client.reset();
    m.session.reset();
    m.resolved = {};
}
} // namespace

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
        options.agent = drivelink::AgentHello{token, m.config.device, info_.name.substr(0, 32), kClientKindRos};
        m.session = std::make_unique<drivelink::Session>(drivelink::resolveTcpEndpoint(m.config.agent), options);
        m.client = std::make_unique<drivelink::Client>(*m.session);
        {
            // The model loads synchronously and is dropped after resolving:
            // a Boot afterwards is caught by the group generation in read().
            drivelink::DeviceModel model(*m.client);
            const auto snapshot = model.snapshotShared();
            checkDevice(*snapshot);
            m.resolved = resolve(*snapshot, m.specs, m.config);
        }
        m.handles.clear();
        for (const auto &state : m.resolved.states)
        {
            m.handles.push_back(get_state_interface_handle(state.component + "/" + state.interface_name));
        }
        const auto hello = m.session->agentHello();
        m.watch->client_id = hello ? hello->client_id : 0;
        // Agent events and the link's end arrive on the library's callback
        // thread; only atomics cross to read().
        m.observer = m.session->observe([watch = m.watch](const drivelink::Frame &frame) {
            if (frame.type == 0)
            {
                watch->raise(Fault::LinkLost);
                return;
            }
            if (frame.type != 0xF0) { return; }
            const auto event = drivelink::agent::decodeEvent(frame.payload);
            if (!event) { return; }
            if (event->event == kEventClosing)
            {
                const auto reason = drivelink::agent::decodeClosing(event->body);
                watch->raise(reason && *reason == kCloseDeviceOffline ? Fault::DeviceOffline : Fault::AgentShutdown);
            }
            else if (event->event == kEventLease && watch->leased)
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

namespace
{
// Clears our subscription groups and gives the lease back. Best effort: a
// lost link fails each call quickly, and the agent clears a closed client's
// groups and lease itself.
void release(auto &m, const rclcpp::Logger &logger)
{
    m.running = false;
    m.watch->leased = false;
    if (!m.client) { return; }
    for (const auto &run : m.runs)
    {
        m.session->removeBuffer(run.id);
        try { m.client->groupClear(run.id, false); }
        catch (const std::exception &e) { RCLCPP_WARN(logger, "clearing group %u: %s", unsigned(run.id), e.what()); }
    }
    m.runs.clear();
    try { drivelink::agentLeaseRelease(*m.session); }
    catch (const drivelink::Error &e)
    {
        if (!(e.result == uint8_t(defs::Result::InvalidArgument) && e.detail == uint8_t(defs::Detail::WrongState)))
        {
            RCLCPP_WARN(logger, "releasing the lease: %s", e.what());
        }
    }
    catch (const std::exception &e) { RCLCPP_WARN(logger, "releasing the lease: %s", e.what()); }
}
} // namespace

hardware_interface::CallbackReturn DriveLinkSystem::on_activate(const rclcpp_lifecycle::State &)
{
    auto &m = *m_impl;
    if (!m.client)
    {
        RCLCPP_ERROR(get_logger(), "activate without a configured connection");
        return CallbackReturn::ERROR;
    }
    if (const auto fault = m.watch->fault.load(); fault != Fault::None)
    {
        RCLCPP_ERROR(get_logger(), "cannot activate: %s; configure again", describe(fault));
        return CallbackReturn::FAILURE;
    }
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
        // Armed before the Take: a lease end naming us can only follow the grant.
        m.watch->leased = true;
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
                                     " subscription group(s), the device has " + std::to_string(free.size()) +
                                     " free for this client");
        }
        m.generation = m.session->generation();
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
            m.runs.push_back(std::move(run)); // recorded before the checks, so release() clears it
            auto &added = m.runs.back();
            if (configured.generation != m.generation)
            {
                throw std::runtime_error("the device rebooted while the groups were set up");
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
            const auto group_deadline =
                deadline + std::chrono::microseconds(uint64_t(10e6 / m.resolved.groups[i].rate_hz));
            while (!run.buffer->latest(m.sample) || m.sample.generation != m.generation)
            {
                if (std::chrono::steady_clock::now() > group_deadline)
                {
                    throw std::runtime_error("no sample from loop " + std::to_string(run.loop) + " within " +
                                             std::to_string(m.config.timeout.count()) + " ms");
                }
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
        RCLCPP_ERROR(get_logger(), "activate failed: %s", e.what());
        release(m, get_logger());
        return CallbackReturn::FAILURE;
    }
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_deactivate(const rclcpp_lifecycle::State &)
{
    release(*m_impl, get_logger());
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_cleanup(const rclcpp_lifecycle::State &)
{
    disconnect(*m_impl);
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_shutdown(const rclcpp_lifecycle::State &)
{
    release(*m_impl, get_logger());
    disconnect(*m_impl);
    return CallbackReturn::SUCCESS;
}

hardware_interface::CallbackReturn DriveLinkSystem::on_error(const rclcpp_lifecycle::State &)
{
    auto &m = *m_impl;
    const auto fault = m.watch->fault.load();
    if (fault == Fault::Stale)
    {
        RCLCPP_ERROR(get_logger(), "%s (loop %u); the component is now unconfigured", describe(fault),
                     unsigned(m.watch->stale_loop.load()));
    }
    else
    {
        RCLCPP_ERROR(get_logger(), "%s; the component is now unconfigured", describe(fault));
    }
    // Unconfigured afterwards: configuring again reconnects and reloads.
    release(m, get_logger());
    disconnect(m);
    return CallbackReturn::SUCCESS;
}

return_type DriveLinkSystem::read(const rclcpp::Time &, const rclcpp::Duration &) noexcept
{
    auto &m = *m_impl;
    if (!m.running.load(std::memory_order_acquire))
    {
        return return_type::OK; // inactive: nothing streams, the values stay as they are
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
            if (m.sample.generation != m.generation)
            {
                m.watch->raise(Fault::Rebooted);
                return return_type::ERROR;
            }
            if (m.sample.counter != run.counter)
            {
                run.counter = m.sample.counter;
                run.seen_us = now;
            }
            // The device time of the sample mapped through the agent's clock
            // model; until that mapping is valid, when read() first saw it.
            const uint64_t sample_us =
                m.sample.mapped && m.sample.host_us > 0 ? uint64_t(m.sample.host_us) : run.seen_us;
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
