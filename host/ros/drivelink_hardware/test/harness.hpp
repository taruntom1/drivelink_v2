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
//   sensor loop (the lowest non-realtime loop that imu.mock's loop mask allows):
//     imu.mock "imu"
// Type ids, item ids and ref params come from the device (the CLI's `types`
// and `items` commands), never literals copied from the YAML, except where the
// CLI needs a numeric id it has just looked up.
#include <drivelink/session.hpp>
#include <chrono>
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
