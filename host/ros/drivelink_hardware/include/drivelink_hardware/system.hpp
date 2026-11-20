#pragma once
// drivelink_hardware/DriveLinkSystem: a ros2_control SystemInterface for one
// DriveLink device behind drivelink-agent (README.md). H4a: state interfaces
// only. It uses only the ros2_control API that Jazzy (4.x) and Lyrical (6.x)
// share: on_init(HardwareComponentInterfaceParams), the default export of the
// interfaces declared in the URDF, and the handle-based set_state.
//
// Threads: the lifecycle callbacks run on the controller_manager's thread and
// may block (bounded by the `timeout_ms` parameter and the lease handoff).
// read() runs in its real-time loop: noexcept, no allocation, no blocking lock, no
// device call. Agent events and link changes reach it through atomics that
// libdrivelink's callback thread sets.
#include <hardware_interface/system_interface.hpp>
#include <hardware_interface/types/hardware_component_interface_params.hpp>
#include <rclcpp_lifecycle/state.hpp>
#include <memory>
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
    hardware_interface::return_type write(const rclcpp::Time &time, const rclcpp::Duration &period) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace drivelink_hardware
