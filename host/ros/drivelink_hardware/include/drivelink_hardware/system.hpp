#pragma once
// drivelink_hardware/DriveLinkSystem: a ros2_control SystemInterface for one
// DriveLink device behind drivelink-agent (README.md): state interfaces (H4a)
// and the command path (H4b). It uses only the ros2_control API that Jazzy
// (4.x) and Lyrical (6.x) share: on_init(HardwareComponentInterfaceParams),
// the default export of the interfaces declared in the URDF, the handle-based
// set_state/get_command/set_command, and prepare/perform_command_mode_switch.
//
// Threads: the lifecycle callbacks and prepare_command_mode_switch run off the
// real-time loop and may block (bounded by the `timeout_ms` parameter and the
// lease handoff); they serialize on one device mutex with the plugin's worker
// thread, which sends the DISARM after the last release. read(), write() and
// perform_command_mode_switch run in (or beside) the real-time loop: no
// allocation, blocking lock or device call. Agent events, the status stream
// and link changes reach them through atomics that libdrivelink's callback
// thread sets.
#include <hardware_interface/system_interface.hpp>
#include <hardware_interface/types/hardware_component_interface_params.hpp>
#include <rclcpp_lifecycle/state.hpp>
#include <memory>
#include <string>
#include <vector>

namespace drivelink_hardware
{
class DriveLinkSystem : public hardware_interface::SystemInterface
{
  public:
    DriveLinkSystem();
    ~DriveLinkSystem() override;

    CallbackReturn on_init(const hardware_interface::HardwareComponentInterfaceParams &params) override;
    CallbackReturn on_configure(const rclcpp_lifecycle::State &previous_state) override;
    CallbackReturn on_cleanup(const rclcpp_lifecycle::State &previous_state) override;
    CallbackReturn on_activate(const rclcpp_lifecycle::State &previous_state) override;
    CallbackReturn on_deactivate(const rclcpp_lifecycle::State &previous_state) override;
    CallbackReturn on_shutdown(const rclcpp_lifecycle::State &previous_state) override;
    CallbackReturn on_error(const rclcpp_lifecycle::State &previous_state) override;

    hardware_interface::return_type read(const rclcpp::Time &time, const rclcpp::Duration &period) noexcept override;
    hardware_interface::return_type write(const rclcpp::Time &time, const rclcpp::Duration &period) noexcept override;

    hardware_interface::return_type prepare_command_mode_switch(const std::vector<std::string> &start_interfaces,
                                                                const std::vector<std::string> &stop_interfaces) override;
    hardware_interface::return_type perform_command_mode_switch(const std::vector<std::string> &start_interfaces,
                                                                const std::vector<std::string> &stop_interfaces) override;

    struct Impl; // opaque: the connection, mapping and read() state (system.cpp)

  private:
    std::unique_ptr<Impl> m_impl;
};
} // namespace drivelink_hardware
