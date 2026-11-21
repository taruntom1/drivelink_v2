#pragma once
// Test harness for the drivelink_hardware integration test (Linux only): one
// drivelink-vdev with the example layout, behind one drivelink-agent, plus a
// second agent client that plays the GUI. The programs come from the
// environment (DRIVELINK_VDEV, DRIVELINK_AGENT, DRIVELINK_CLI; CMakeLists.txt
// sets them from the host superbuild). Every wait is bounded; a failure throws
// HarnessError with the step, the child's recent stderr and its exit status.
//
// The example layout (set up on the vdev directly, before the agent starts,
// with the CLI: add, set, save, reboot, then wait for no pending reboot):
//   control loop (loop 0):
//     actuator.mock  "left_act",  feedback.mock "left_fb"  (actuator -> left_act)
//     actuator.mock  "right_act", feedback.mock "right_fb" (actuator -> right_act)
//     controller.cascade_pid "left"  (actuator -> left_act,  feedback -> left_fb),  mode velocity
//     controller.cascade_pid "right" (actuator -> right_act, feedback -> right_fb), mode velocity
//     (H4b: both controllers also get velocity and position gains, saved, so
//     that velocity_sp and position_sp make the mock plant track within the
//     tolerances test_integration uses; duty_sp drives it open loop)
//   sensor loop (the lowest non-realtime loop that imu.mock's loop mask allows):
//     imu.mock "imu"
// Type ids, item ids and ref params come from the device (the CLI's `types`
// and `items` commands), never literals copied from the YAML, except where the
// CLI needs a numeric id it has just looked up.
#include <drivelink/session.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <vector>
#include <memory>
#include <stdexcept>
#include <string>

namespace drivelink_hardware::test
{
struct HarnessError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class Harness
{
  public:
    // Starts the vdev (fresh temporary state dir, a free loopback port; waits
    // for its READY line), lays out the example, then starts the agent with a
    // random 24-hex-digit token, owning the vdev as alias "robot" on a free
    // loopback port (waits for READY and for the device to be online in a
    // lobby device list).
    //
    // pty = false (default): the vdev listens on loopback TCP, and drops the
    // link when it reboots (the agent sees the device go offline). pty = true:
    // `drivelink-vdev --pty`, READY `serial:///dev/pts/N`; the port survives a
    // reboot, so a reboot reaches the device's clients as a Boot event, not as
    // device loss. The CLI, the library and the agent (--device
    // robot=serial:///dev/pts/N) take the serial:// endpoint the same way.
    static std::unique_ptr<Harness> start(bool pty = false);
    ~Harness(); // SIGTERM to the agent, then the vdev; SIGKILL after 5 s; removes the temp dir

    Harness(const Harness &) = delete;
    Harness &operator=(const Harness &) = delete;

    const std::string &agentEndpoint() const; // "tcp://127.0.0.1:<port>"
    const std::string &token() const;
    static constexpr const char *kDevice = "robot";

    // A URDF holding only the <ros2_control> block for the plugin, for
    // hardware_interface::ResourceManager:
    //   <robot name="test"><ros2_control name="<component>" type="system">
    //     <hardware><plugin>drivelink_hardware/DriveLinkSystem</plugin>
    //       agent, token (inline), device=robot, plus `extra_hardware_params`
    //       (raw <param> XML appended verbatim)</hardware>
    //   (with a <link> per wheel, a base_link and continuous <joint>s outside
    //   the block, which the URDF parser needs)
    //     <joint name="left_wheel"><param name="instance">left</param>
    //       <state_interface name="position"/><state_interface name="velocity"/></joint>
    //     <joint name="right_wheel"> ... instance right, the same interfaces </joint>
    //     <sensor name="imu"><param name="instance">imu</param>
    //       orientation.x/y/z/w, angular_velocity.x/y/z, linear_acceleration.x/y/z</sensor>
    //   </ros2_control></robot>
    std::string urdf(const std::string &component = "drivelink", const std::string &extra_hardware_params = {}) const;

    // H4b: a URDF with chosen joints. Each joint is written as
    //   <joint name="NAME"><param name="instance">INSTANCE</param> EXTRA
    //     <state_interface name="S"/>... <command_interface name="C"/>... </joint>
    // (EXTRA: raw <param> XML appended verbatim), with its <link> and
    // continuous <joint> outside the block as in urdf(); the IMU sensor is
    // added as in urdf() when `imu` is true.
    struct JointSpec
    {
        std::string name;
        std::string instance;
        std::vector<std::string> state{"position", "velocity"};
        std::vector<std::string> command;
        std::string extra_params;
    };
    std::string urdf(const std::string &component, const std::string &extra_hardware_params,
                     const std::vector<JointSpec> &joints, bool imu = true) const;

    // --- H4b helpers on the GUI client (gui()); each opens it if needed. ---
    // The newest status-stream frame the GUI client has seen (it observes the
    // status stream from the moment gui() opens it): arm_state (0 disarmed,
    // 1 armed) and last_disarm_reason (core.md: Boot 0, Explicit 1, Watchdog 2,
    // Fault 3, LoopStalled 4, Shutdown 5). waitArm waits (bounded) until a
    // status frame received after the call shows `armed`, else HarnessError.
    struct ArmStatus
    {
        uint8_t arm_state = 0;
        uint8_t last_disarm_reason = 0;
        uint8_t flags = 0; // status flags (fault_latched 1, ...)
    };
    ArmStatus armStatus(std::chrono::milliseconds wait = std::chrono::milliseconds(2000));
    ArmStatus waitArm(bool armed, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
    // A param of an instance found by label, read through the GUI client
    // (Client::get), as a double (enums and integers by value, f32 by bit cast).
    double param(const std::string &label, const std::string &item);
    // The enum entry name of an Enum param (e.g. mode -> "velocity").
    std::string enumParam(const std::string &label, const std::string &item);
    // Runs a loop action of an instance found by label through the GUI client
    // (Client::action), args as f32/bool bits by the action's descriptor order
    // (the test passes them already encoded). Throws on a refusal.
    void action(const std::string &label, const std::string &item, std::array<uint32_t, 4> args = {});
    // DISARM and ACK_FAULT from the GUI client (any client may do both).
    void disarm();
    void acknowledgeFault();

    // A second agent client of the device (a drivelink::Session with
    // AgentHello kind Gui (1), client_name "harness-gui"), opened on demand.
    // The test drives it with the library: agentLeaseTake / agentLeaseRelease,
    // Client::set for live params, Client::subConfigure + subscribeBuffer to
    // observe the same signals the plugin maps.
    drivelink::Session &gui();
    void closeGui();

    // Stops the agent (SIGTERM, waits for exit). Its clients get Closing.
    void stopAgent();

    // The vdev's scenario control file (vdev --scenario-file, host/vdev/README.md):
    // a write of `<sequence> <action>` (e.g. `1 loop-stall 0 4000`) runs the
    // action once; the file is replaced atomically.
    void vdevScenario(unsigned sequence, const std::string &action);

    // The children's recent stderr and exit status, for failure messages.
    std::string diag() const;

  private:
    Harness();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace drivelink_hardware::test
