#include "recovery.hpp"

#include <controller_manager_msgs/srv/list_controllers.hpp>
#include <controller_manager_msgs/srv/list_hardware_components.hpp>
#include <controller_manager_msgs/srv/set_hardware_component_state.hpp>
#include <controller_manager_msgs/srv/switch_controller.hpp>
#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <drivelink/client.hpp>
#include <drivelink/protocol/gen/registry.hpp>
#include <lifecycle_msgs/msg/state.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <optional>
#include <set>
#include <utility>

namespace drivelink_bringup
{
namespace
{
namespace cm = controller_manager_msgs::srv;
namespace defs = drivelink::protocol::defs;
namespace msg = defs::msg;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Diagnostic = diagnostic_msgs::msg::DiagnosticStatus;
using State = lifecycle_msgs::msg::State;
using Trigger = std_srvs::srv::Trigger;

constexpr uint8_t kRos = uint8_t(defs::AgentClientKind::Ros);
constexpr auto kDeviceTimeout = 1500ms;
constexpr size_t kEventCapacity = 256;

void add(Diagnostic &diagnostic, std::string key, std::string value)
{
    diagnostic_msgs::msg::KeyValue field;
    field.key = std::move(key);
    field.value = std::move(value);
    diagnostic.values.push_back(std::move(field));
}

// Replies are dispatched by the executor's other thread. Remove a timed-out
// request from the client, but leave its outcome uncertain and never repeat it.
template<class Service>
auto call(rclcpp::Client<Service> &client, typename Service::Request::SharedPtr request,
          std::chrono::milliseconds timeout = 3000ms)
{
    const std::string name = client.get_service_name();
    if (!client.wait_for_service(1s)) { throw std::runtime_error(name + " is unavailable"); }
    auto result = client.async_send_request(std::move(request));
    if (result.wait_for(timeout) != std::future_status::ready)
    {
        client.remove_pending_request(result);
        throw std::runtime_error(name + " timed out; outcome unknown");
    }
    return result.get();
}
} // namespace

class Diagnostics : public rclcpp::Node
{
  public:
    Diagnostics() : Node("drivelink_diagnostics")
    {
        endpoint_ = declare_parameter<std::string>("agent", "tcp://127.0.0.1:47620");
        device_ = declare_parameter<std::string>("device", "dev");
        token_file_ = declare_parameter<std::string>("token_file", "/etc/drivelink/token");
        token_ = declare_parameter<std::string>("token", "");
        hardware_ = declare_parameter<std::string>("hardware", "drivelink");
        wanted_ = declare_parameter("recovery_controllers", std::vector<std::string>{});
        const auto manager = declare_parameter<std::string>("controller_manager", "/controller_manager");
        const auto service_timeout_s = declare_parameter<double>("service_timeout_s", 30.0);
        if (!std::isfinite(service_timeout_s) || service_timeout_s < 0.1)
        {
            throw std::runtime_error("service_timeout_s must be finite and at least 0.1 s");
        }
        service_timeout_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double>(service_timeout_s));
        if (device_.empty() || hardware_.empty()) { throw std::runtime_error("device and hardware are required"); }
        const std::set<std::string> unique(wanted_.begin(), wanted_.end());
        if (unique.empty() || unique.contains("") || unique.size() != wanted_.size())
        {
            throw std::runtime_error("recovery_controllers must contain distinct, nonempty controller names");
        }

        // The default mutually exclusive group owns the session and serializes
        // recovery, acknowledgement and diagnostics. Only ROS replies use this group.
        replies_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
        list_hardware_ = managerClient<cm::ListHardwareComponents>(manager + "/list_hardware_components");
        list_controllers_ = managerClient<cm::ListControllers>(manager + "/list_controllers");
        set_hardware_ = managerClient<cm::SetHardwareComponentState>(manager + "/set_hardware_component_state");
        switch_ = managerClient<cm::SwitchController>(manager + "/switch_controller");
        publisher_ = create_publisher<diagnostic_msgs::msg::DiagnosticArray>("/diagnostics", 10);
        ack_ = create_service<Trigger>("~/ack_fault", [this](const Trigger::Request::SharedPtr,
                                                            Trigger::Response::SharedPtr response) {
            respond(*response, [this] { acknowledge(); }, "fault acknowledged");
        });
        recover_ = create_service<Trigger>("~/recover", [this](const Trigger::Request::SharedPtr,
                                                              Trigger::Response::SharedPtr response) {
            respond(*response, [this] { recover(); }, "hardware and configured controllers active");
        });
        timer_ = create_wall_timer(200ms, [this] { tick(); });
    }

  private:
    template<class Service>
    typename rclcpp::Client<Service>::SharedPtr managerClient(const std::string &name)
    {
        return create_client<Service>(name, rclcpp::ServicesQoS(), replies_);
    }

    template<class Operation>
    void respond(Trigger::Response &response, Operation operation, const char *success)
    {
        try
        {
            operation();
            response.success = true;
            response.message = success;
        }
        catch (const std::exception &error)
        {
            response.message = error.what();
            RCLCPP_WARN(get_logger(), "%s", response.message.c_str());
        }
    }

    std::string readToken() const
    {
        if (!token_.empty()) { return token_; }
        std::ifstream input(token_file_);
        std::string token;
        if (!std::getline(input, token)) { throw std::runtime_error("could not read agent token file"); }
        const auto last = token.find_last_not_of(" \t\r\n");
        token.erase(last == std::string::npos ? 0 : last + 1);
        if (token.empty()) { throw std::runtime_error("agent token file is empty"); }
        return token;
    }

    void connect()
    {
        drivelink::SessionOptions options;
        options.timeout = kDeviceTimeout;
        options.event_capacity = kEventCapacity;
        options.periodic_time = false;
        options.retire_quarantine = 30s;
        options.agent = drivelink::AgentHello{readToken(), device_, "ros-diagnostics", kRos};
        auto session = std::make_unique<drivelink::Session>(drivelink::resolveTcpEndpoint(endpoint_), options);
        const auto hello = session->agentHello();
        if (!hello || hello->lobby || !hello->device.online) { throw std::runtime_error("device is offline"); }
        device_id_ = drivelink::agent::formatDeviceId(hello->device.device_id);
        holder_ = hello->device.holder_name;
        holder_id_ = hello->device.holder_id;
        holder_kind_ = hello->device.holder_kind;
        generation_ = session->generation();
        session_ = std::move(session);
        probe_after_ = Clock::now() + 2s;
    }

    void refreshHolder()
    {
        const auto clients = drivelink::agentClients(*session_);
        holder_.clear();
        holder_id_ = 0;
        holder_kind_ = 0;
        for (const auto &client : clients)
        {
            if (!client.holder) { continue; }
            holder_ = client.name;
            holder_id_ = client.client_id;
            holder_kind_ = client.kind;
            break;
        }
        probe_after_ = Clock::now() + 2s;
    }

    void drainEvents()
    {
        drivelink::Frame frame;
        // nextEvent also contains agent frames. Decode them from their own
        // queue, which never loses a lease event to device traffic.
        for (size_t i = 0; i < drivelink::Session::kAgentEventCapacity && session_->nextAgentEvent(frame); ++i)
        {
            const auto event = drivelink::agent::decodeEvent(frame.payload);
            if (!event) { continue; }
            if (event->event == uint8_t(defs::AgentEvent::Closing))
            {
                throw std::runtime_error("agent closed the device connection");
            }
            if (event->event == uint8_t(defs::AgentEvent::Lease))
            {
                if (const auto lease = drivelink::agent::decodeLease(event->body))
                {
                    holder_ = lease->holder_name;
                    holder_id_ = lease->holder_id;
                    holder_kind_ = lease->holder_kind;
                }
            }
        }
        const auto generation = session_->generation();
        if (generation != generation_)
        {
            generation_ = generation;
            status_.reset();
            record_.reset();
        }
        for (size_t i = 0; i < kEventCapacity && session_->nextEvent(frame, 0ms); ++i)
        {
            if (frame.generation != generation_ || (frame.type != msg::kStatusStream && frame.type != msg::kDiagEvent))
            {
                continue;
            }
            try
            {
                const auto event = drivelink::Client::decodeEvent(frame);
                if (const auto *status = std::get_if<drivelink::StatusFrame>(&event))
                {
                    status_ = *status;
                    last_status_ = Clock::now();
                }
                else if (const auto *record = std::get_if<drivelink::DiagRecord>(&event)) { record_ = *record; }
            }
            catch (const drivelink::Error &)
            {
                // A malformed frame cannot update the last valid state.
            }
        }
    }

    void tick()
    {
        try
        {
            if (!session_ && Clock::now() >= probe_after_) { connect(); }
            if (session_)
            {
                drainEvents();
                if (Clock::now() >= probe_after_) { refreshHolder(); }
            }
        }
        catch (const std::exception &error)
        {
            RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "%s", error.what());
            session_.reset();
            status_.reset();
            record_.reset();
            holder_id_ = 0;
            holder_.clear();
            holder_kind_ = 0;
            probe_after_ = Clock::now() + 1s;
        }
        publish();
    }

    void publish()
    {
        Diagnostic diagnostic;
        diagnostic.name = "DriveLink " + device_;
        diagnostic.hardware_id = device_id_.empty() ? device_ : device_id_;
        diagnostic.level = Diagnostic::STALE;
        diagnostic.message = session_ ? "Waiting for fresh device status" : "Device offline";
        add(diagnostic, "online", session_ ? "true" : "false");
        add(diagnostic, "lease_holder", holder_id_ ? holder_ : "none");
        add(diagnostic, "lease_holder_id", std::to_string(holder_id_));
        add(diagnostic, "lease_holder_kind", std::to_string(holder_kind_));
        if (status_ && Clock::now() - last_status_ < 3s)
        {
            const auto &status = *status_;
            diagnostic.level = status.arm_state == 2 || status.active_errors ? Diagnostic::ERROR : Diagnostic::OK;
            diagnostic.message = status.arm_state == 2 ? "Fault latched" :
                                 status.active_errors ? "Device has active errors" :
                                 status.arm_state == 1 ? "Armed" : "Disarmed";
            add(diagnostic, "arm_state", std::to_string(status.arm_state));
            add(diagnostic, "active_errors", std::to_string(status.active_errors));
            add(diagnostic, "last_disarm_reason", std::to_string(status.last_disarm_reason));
        }
        if (record_)
        {
            add(diagnostic, "last_event_source", std::to_string(record_->source));
            add(diagnostic, "last_event_instance", std::to_string(record_->instance));
            add(diagnostic, "last_event_code", std::to_string(record_->code));
            add(diagnostic, "last_event_severity", std::to_string(record_->severity));
        }
        diagnostic_msgs::msg::DiagnosticArray array;
        array.header.stamp = now();
        array.status.push_back(std::move(diagnostic));
        publisher_->publish(std::move(array));
    }

    void acknowledge()
    {
        if (!session_) { throw std::runtime_error("device offline"); }
        drivelink::Client(*session_).acknowledgeFault();
    }

    void requireFreeLease()
    {
        if (!session_) { throw std::runtime_error("device offline"); }
        refreshHolder();
        if (holder_id_) { throw std::runtime_error("lease held by " + holder_ + "; release it before recovery"); }
    }

    controller_manager_msgs::msg::HardwareComponentState hardwareComponent()
    {
        const auto response = call(*list_hardware_, std::make_shared<cm::ListHardwareComponents::Request>());
        for (const auto &component : response->component)
        {
            if (component.name == hardware_) { return component; }
        }
        throw std::runtime_error("controller manager has no hardware component " + hardware_);
    }

    void setHardware(uint8_t target, const std::string &label)
    {
        auto request = std::make_shared<cm::SetHardwareComponentState::Request>();
        request->name = hardware_;
        request->target_state.id = target;
        const auto response = call(*set_hardware_, request, service_timeout_);
        if (!response->ok || response->state.id != target)
        {
            throw std::runtime_error("hardware transition to " + label + " failed; hardware state is " +
                                     response->state.label);
        }
    }

    std::vector<std::string> pendingControllers()
    {
        const auto response = call(*list_controllers_, std::make_shared<cm::ListControllers::Request>());
        return controllersToActivate(wanted_, response->controller);
    }

    std::vector<std::string> activeTargets(const std::vector<std::string> &attempted)
    {
        const auto response = call(*list_controllers_, std::make_shared<cm::ListControllers::Request>());
        return activeSwitchTargets(attempted, response->controller);
    }

    void switchControllers(const std::vector<std::string> &activate, const std::vector<std::string> &deactivate,
                           int32_t strictness = cm::SwitchController::Request::STRICT)
    {
        auto request = std::make_shared<cm::SwitchController::Request>();
        request->activate_controllers = activate;
        request->deactivate_controllers = deactivate;
        request->strictness = strictness;
        request->timeout.sec = 10;
        const auto response = call(*switch_, request, service_timeout_);
        if (!response->ok) { throw std::runtime_error("controller switch refused: " + response->message); }
    }

    void recover()
    {
        requireFreeLease();
        const auto state = hardwareComponent().state;
        if (state.id != State::PRIMARY_STATE_UNCONFIGURED && state.id != State::PRIMARY_STATE_INACTIVE)
        {
            throw std::runtime_error("hardware state is " + state.label + "; recovery needs inactive or unconfigured");
        }
        pendingControllers(); // Reject an invalid controller list before any mutation.
        acknowledge();
        if (state.id == State::PRIMARY_STATE_UNCONFIGURED)
        {
            setHardware(State::PRIMARY_STATE_INACTIVE, "inactive");
        }
        requireFreeLease();
        const auto inactive = hardwareComponent();
        if (inactive.state.id != State::PRIMARY_STATE_INACTIVE)
        {
            throw std::runtime_error("hardware must be inactive before activation; current state is " +
                                     inactive.state.label);
        }
        for (const auto &interface : inactive.command_interfaces)
        {
            if (interface.is_claimed)
            {
                throw std::runtime_error("command interface " + interface.name +
                                         " is still claimed; deactivate its controller before recovery");
            }
        }
        std::vector<std::string> attempted;
        try
        {
            setHardware(State::PRIMARY_STATE_ACTIVE, "active");
            attempted = pendingControllers();
            if (!attempted.empty()) { switchControllers(attempted, {}); }
            if (!pendingControllers().empty() || hardwareComponent().state.id != State::PRIMARY_STATE_ACTIVE)
            {
                throw std::runtime_error("hardware or controllers did not stay active");
            }
        }
        catch (const std::exception &error)
        {
            std::string failure = error.what();
            if (!attempted.empty())
            {
                try
                {
                    auto active = attempted;
                    auto strictness = cm::SwitchController::Request::BEST_EFFORT;
                    try
                    {
                        active = activeTargets(attempted);
                        strictness = cm::SwitchController::Request::STRICT;
                    }
                    catch (const std::exception &)
                    {
                        // Still stop attempted controllers if the list service is unavailable.
                    }
                    if (!active.empty()) { switchControllers({}, active, strictness); }
                    const auto remaining = activeTargets(attempted);
                    for (const auto &name : remaining) { failure += "; controller still active: " + name; }
                }
                catch (const std::exception &cleanup)
                {
                    failure += "; controller cleanup unconfirmed: ";
                    failure += cleanup.what();
                }
            }
            try
            {
                // Use the same service even if the ACTIVE response was lost.
                // The plugin disarms and releases the lease on deactivation.
                setHardware(State::PRIMARY_STATE_INACTIVE, "inactive");
                failure += "; hardware returned to inactive";
            }
            catch (const std::exception &cleanup)
            {
                failure += "; hardware deactivation unconfirmed: ";
                failure += cleanup.what();
            }
            throw std::runtime_error(failure);
        }
    }

    std::string endpoint_, device_, token_file_, token_, hardware_, device_id_, holder_;
    std::vector<std::string> wanted_;
    std::chrono::milliseconds service_timeout_{30000ms};
    std::unique_ptr<drivelink::Session> session_;
    uint64_t generation_ = 0;
    uint16_t holder_id_ = 0;
    uint8_t holder_kind_ = 0;
    Clock::time_point probe_after_{}, last_status_{};
    std::optional<drivelink::StatusFrame> status_;
    std::optional<drivelink::DiagRecord> record_;
    rclcpp::CallbackGroup::SharedPtr replies_;
    rclcpp::Client<cm::ListHardwareComponents>::SharedPtr list_hardware_;
    rclcpp::Client<cm::ListControllers>::SharedPtr list_controllers_;
    rclcpp::Client<cm::SetHardwareComponentState>::SharedPtr set_hardware_;
    rclcpp::Client<cm::SwitchController>::SharedPtr switch_;
    rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr publisher_;
    rclcpp::Service<Trigger>::SharedPtr ack_, recover_;
    rclcpp::TimerBase::SharedPtr timer_;
};
} // namespace drivelink_bringup

int main(int argc, char **argv)
{
    rclcpp::init(argc, argv);
    int result = 0;
    try
    {
        rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions(), 2);
        auto node = std::make_shared<drivelink_bringup::Diagnostics>();
        executor.add_node(node);
        executor.spin();
    }
    catch (const std::exception &error)
    {
        RCLCPP_ERROR(rclcpp::get_logger("drivelink_diagnostics"), "%s", error.what());
        result = 1;
    }
    rclcpp::shutdown();
    return result;
}
