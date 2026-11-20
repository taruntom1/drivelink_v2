// drivelink_hardware/DriveLinkSystem against drivelink-vdev behind drivelink-agent
// (harness.hpp), loaded by hardware_interface::ResourceManager through pluginlib
// from the URDF the harness builds. The plugin API is used only as ros2_control
// Jazzy and Lyrical share it.
#include "harness.hpp"

#include <drivelink/agent.hpp>
#include <drivelink/buffers.hpp>
#include <drivelink/client.hpp>
#include <drivelink/session.hpp>

#include <gtest/gtest.h>
#include <hardware_interface/loaned_state_interface.hpp>
#include <hardware_interface/resource_manager.hpp>
#include <hardware_interface/types/hardware_interface_return_values.hpp>
#include <lifecycle_msgs/msg/state.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_lifecycle/state.hpp>

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
using drivelink_hardware::test::Harness;
using drivelink_hardware::test::HarnessError;
using hardware_interface::return_type;
using LifecycleState = lifecycle_msgs::msg::State;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr const char *kComponent = "drivelink";
constexpr uint8_t kKindGui = 1, kKindRos = 3;
constexpr uint8_t kEventLease = 2;

struct RclEnvironment : ::testing::Environment
{
    void SetUp() override { rclcpp::init(0, nullptr); }
    void TearDown() override { rclcpp::shutdown(); }
};
[[maybe_unused]] ::testing::Environment *const g_rcl_environment = ::testing::AddGlobalTestEnvironment(new RclEnvironment);

// HardwareReadWriteStatus is {result, failed_hardware_names} in Jazzy and has an
// `ok` flag in newer releases; this reads whichever the build has.
template <class Status> bool readFailed(const Status &status)
{
    bool failed = !status.failed_hardware_names.empty();
    if constexpr (requires { status.ok; })
    {
        failed = failed || !status.ok;
    }
    else if constexpr (requires { status.result; })
    {
        failed = failed || status.result != return_type::OK;
    }
    return failed;
}

// The ResourceManager under test with a 10 ms read cycle.
class Rm
{
  public:
    explicit Rm(const std::string &urdf, std::string name = kComponent)
        : m_name(std::move(name)), m_clock(std::make_shared<rclcpp::Clock>(RCL_STEADY_TIME)),
          m_rm(std::make_unique<hardware_interface::ResourceManager>(urdf, m_clock, rclcpp::get_logger("drivelink_it"),
                                                                     false, 100))
    {
    }
    ~Rm()
    {
        m_loans.clear();
        try
        {
            m_rm->shutdown_components();
        }
        catch (...)
        {
        }
        m_rm.reset();
    }

    bool exists() { return m_rm->get_components_status().count(m_name) != 0; }

    return_type go(uint8_t id, const char *label)
    {
        rclcpp_lifecycle::State target(id, label);
        return m_rm->set_component_state(m_name, target);
    }
    return_type configure() { return go(LifecycleState::PRIMARY_STATE_INACTIVE, "inactive"); }
    return_type activate() { return go(LifecycleState::PRIMARY_STATE_ACTIVE, "active"); }
    return_type deactivate() { return go(LifecycleState::PRIMARY_STATE_INACTIVE, "inactive"); }
    return_type cleanup() { return go(LifecycleState::PRIMARY_STATE_UNCONFIGURED, "unconfigured"); }

    uint8_t state() { return m_rm->get_components_status().at(m_name).state.id(); }
    std::string stateLabel() { return m_rm->get_components_status().at(m_name).state.label(); }

    // One read cycle: read, then sleep to make the cycle 10 ms.
    bool cycle()
    {
        const auto start = Clock::now();
        const auto status = m_rm->read(m_clock->now(), rclcpp::Duration::from_seconds(0.01));
        const bool failed = readFailed(status);
        std::this_thread::sleep_until(start + 10ms);
        return !failed;
    }

    // Cycles until a read fails or the time is up; returns the number of cycles
    // run, or -1 when none failed.
    int cyclesUntilFailure(std::chrono::milliseconds limit)
    {
        const auto deadline = Clock::now() + limit;
        for (int n = 1; Clock::now() < deadline; ++n)
        {
            if (!cycle()) { return n; }
        }
        return -1;
    }

    std::vector<std::string> keys() { return m_rm->state_interface_keys(); }

    void claimAll()
    {
        m_loans.clear();
        for (const auto &key : keys()) { m_loans.emplace(key, m_rm->claim_state_interface(key)); }
    }
    std::optional<double> value(const std::string &key) const
    {
        const auto it = m_loans.find(key);
        return it == m_loans.end() ? std::nullopt : it->second.get_optional();
    }

  private:
    std::string m_name;
    rclcpp::Clock::SharedPtr m_clock;
    std::unique_ptr<hardware_interface::ResourceManager> m_rm;
    std::map<std::string, hardware_interface::LoanedStateInterface> m_loans;
};

// The example device through the GUI client: type, item and loop ids by name.
struct Device
{
    drivelink::Client client;
    uint8_t controller = 0, feedback = 0, rt_loop = 0;
    uint16_t position = 0, velocity = 0, noise = 0;
    uint8_t left = 0, left_fb = 0, right_fb = 0;

    explicit Device(drivelink::Session &gui) : client(gui)
    {
        std::map<std::string, uint8_t> types;
        for (const auto &t : client.types()) { types[t.name] = t.type; }
        controller = types.at("controller.cascade_pid");
        feedback = types.at("feedback.mock");
        for (const auto &i : client.items(controller))
        {
            if (i.name == "position") { position = i.id; }
            if (i.name == "velocity") { velocity = i.id; }
        }
        for (const auto &i : client.items(feedback))
        {
            if (i.name == "noise_counts") { noise = i.id; }
        }
        for (const auto &loop : client.info().loops)
        {
            if (loop.realtime()) { rt_loop = loop.id; break; }
        }
        for (const auto &i : client.instances())
        {
            if (i.type == controller && i.label == "left") { left = i.instance; }
            if (i.type == feedback && i.label == "left_fb") { left_fb = i.instance; }
            if (i.type == feedback && i.label == "right_fb") { right_fb = i.instance; }
        }
    }

    void setNoise(float counts)
    {
        for (uint8_t fb : {left_fb, right_fb})
        {
            const drivelink::Ref ref{feedback, fb, noise};
            const uint32_t bits = std::bit_cast<uint32_t>(counts);
            client.set(std::span<const drivelink::Ref>(&ref, 1), std::span<const uint32_t>(&bits, 1));
        }
    }
};

struct Stats
{
    double min = 0, max = 0, mean = 0;
    size_t n = 0;
    double spread() const { return max - min; }
};
Stats stats(const std::vector<double> &v)
{
    Stats s;
    s.n = v.size();
    if (v.empty()) { return s; }
    s.min = *std::min_element(v.begin(), v.end());
    s.max = *std::max_element(v.begin(), v.end());
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
    return s;
}

std::string str(const Stats &s)
{
    std::ostringstream o;
    o << "n=" << s.n << " min=" << s.min << " max=" << s.max << " mean=" << s.mean;
    return o.str();
}

class Integration : public ::testing::Test
{
  protected:
    // TCP vdev by default; IntegrationPty serves it on a PTY, which survives a reboot.
    virtual bool pty() const { return false; }
    void SetUp() override
    {
        try
        {
            h = Harness::start(pty());
        }
        catch (const HarnessError &e)
        {
            FAIL() << e.what();
        }
    }
    void TearDown() override
    {
        rm.reset(); // the plugin first: it closes its connection
        if (HasFailure() && h) { std::cerr << h->diag(); }
        h.reset();
    }

    Rm &makeRm(const std::string &extra = {})
    {
        rm = std::make_unique<Rm>(h->urdf(kComponent, extra));
        return *rm;
    }

    // Configure and activate; fails the test with the state when it does not work.
    void bringUp(Rm &r)
    {
        ASSERT_TRUE(r.exists()) << "the plugin was not loaded or on_init refused the URDF";
        ASSERT_EQ(r.configure(), return_type::OK) << "configure failed; state " << r.stateLabel();
        ASSERT_EQ(r.activate(), return_type::OK) << "activate failed; state " << r.stateLabel();
        ASSERT_EQ(r.state(), LifecycleState::PRIMARY_STATE_ACTIVE);
    }

    // The clients the agent lists for the device (through the GUI client).
    std::vector<drivelink::agent::ClientRecord> clients() { return drivelink::agentClients(h->gui()); }

    static std::string describe(const std::vector<drivelink::agent::ClientRecord> &list)
    {
        std::ostringstream o;
        for (const auto &c : list)
        {
            o << " [" << c.client_id << " '" << c.name << "' kind " << int(c.kind) << (c.holder ? " holder" : "") << "]";
        }
        return o.str();
    }

    // The agent's client list, retried while the agent re-opens the device.
    std::vector<drivelink::agent::ClientRecord> clientsRetry()
    {
        const auto deadline = Clock::now() + 10s;
        for (;;)
        {
            try
            {
                return clients();
            }
            catch (const std::exception &)
            {
                if (Clock::now() >= deadline) { throw; }
                h->closeGui();
                std::this_thread::sleep_for(200ms);
            }
        }
    }

    static bool holds(const std::vector<drivelink::agent::ClientRecord> &list, uint8_t kind)
    {
        for (const auto &c : list)
        {
            if (c.holder && c.kind == kind) { return true; }
        }
        return false;
    }

    // The failure tests: a read failed (cycles >= 1) in time and the component is UNCONFIGURED.
    void expectFailedAndUnconfigured(Rm &r, int cycles, std::chrono::milliseconds took, std::chrono::milliseconds limit,
                                     const std::string &what)
    {
        ASSERT_GE(cycles, 1) << "no read failed: " << what;
        EXPECT_LT(took.count(), limit.count()) << what << ": the failure took " << took.count() << " ms";
        EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_UNCONFIGURED)
            << what << ": after the failure the component is " << r.stateLabel();
    }

    std::unique_ptr<Harness> h;
    std::unique_ptr<Rm> rm;
};

class IntegrationPty : public Integration
{
  protected:
    bool pty() const override { return true; }
};

std::chrono::milliseconds since(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
}

TEST_F(Integration, ConfigureActivateAndReadFiniteValues)
{
    Rm &r = makeRm();
    bringUp(r);
    EXPECT_EQ(r.keys().size(), 14u);
    r.claimAll();
    for (int i = 0; i < 60; ++i) { ASSERT_TRUE(r.cycle()) << "read failed in cycle " << i; }
    for (const auto &key : r.keys())
    {
        const auto v = r.value(key);
        ASSERT_TRUE(v.has_value()) << key << " has no value";
        EXPECT_TRUE(std::isfinite(*v)) << key << " = " << *v;
    }
    double norm2 = 0;
    for (const char *axis : {"x", "y", "z", "w"})
    {
        const double q = r.value(std::string("imu/orientation.") + axis).value_or(NAN);
        norm2 += q * q;
    }
    EXPECT_NEAR(std::sqrt(norm2), 1.0, 0.05) << "the IMU quaternion is not a unit quaternion";
}

TEST_F(Integration, StateFollowsTheDevice)
{
    // The independent observer is the GUI client (no lease) on its own group.
    auto &gui = h->gui();
    Device dev(gui);
    const auto groups = dev.client.groups();
    uint8_t slot = 255;
    for (const auto &g : groups.groups)
    {
        if (g.kind == drivelink::GroupKind::Subscription && !g.configured) { slot = g.id; break; }
    }
    ASSERT_NE(slot, 255) << "no free subscription group for the observer";
    const std::vector<drivelink::Ref> refs{{dev.controller, dev.left, dev.position}, {dev.controller, dev.left, dev.velocity}};
    const auto configured = dev.client.subConfigure(slot, dev.rt_loop, 10, refs);
    auto buffer = gui.subscribeBuffer(slot, configured.epoch, refs.size(), 512, configured.generation);

    Rm &r = makeRm();
    bringUp(r);
    r.claimAll();

    std::vector<double> plugin_pos, plugin_vel, obs_pos, obs_vel;
    drivelink::GroupSample sample;
    auto run = [&](int cycles) {
        plugin_pos.clear(); plugin_vel.clear(); obs_pos.clear(); obs_vel.clear();
        while (buffer->pop(sample)) {} // drop what queued before
        for (int i = 0; i < cycles; ++i)
        {
            ASSERT_TRUE(r.cycle()) << "read failed in cycle " << i;
            plugin_pos.push_back(r.value("left_wheel/position").value_or(NAN));
            plugin_vel.push_back(r.value("left_wheel/velocity").value_or(NAN));
            while (buffer->pop(sample))
            {
                obs_pos.push_back(double(std::bit_cast<float>(sample.values[0])));
                obs_vel.push_back(double(std::bit_cast<float>(sample.values[1])));
            }
        }
    };

    run(40);
    ASSERT_FALSE(HasFatalFailure());
    const Stats quiet_pos = stats(plugin_pos);
    EXPECT_LT(quiet_pos.spread(), 1e-4) << "position is not steady without noise: " << str(quiet_pos);
    EXPECT_GE(obs_pos.size(), 10u) << "the observer received almost no samples";

    dev.setNoise(40.0f); // counts of encoder jitter on both feedbacks
    run(40);             // settle
    ASSERT_FALSE(HasFatalFailure());
    run(150);
    ASSERT_FALSE(HasFatalFailure());

    const Stats p = stats(plugin_pos), v = stats(plugin_vel), op = stats(obs_pos), ov = stats(obs_vel);
    std::cout << "[  stats   ] plugin position " << str(p) << " | observer position " << str(op)
              << "\n[  stats   ] plugin velocity " << str(v) << " | observer velocity " << str(ov) << std::endl;
    SCOPED_TRACE("plugin position " + str(p) + "\nobserver position " + str(op) + "\nplugin velocity " + str(v) +
                 "\nobserver velocity " + str(ov));
    ASSERT_GE(op.n, 50u) << "the observer received too few samples";
    EXPECT_GT(op.spread(), 1e-3) << "the noise did not move the device's position";
    EXPECT_GT(p.spread(), 1e-3) << "the plugin's position did not change with the plant";
    EXPECT_GT(p.spread(), 0.3 * op.spread());
    const double slack = 0.25 * op.spread() + 1e-4;
    EXPECT_GE(p.min, op.min - slack);
    EXPECT_LE(p.max, op.max + slack);
    EXPECT_LE(std::abs(p.mean - op.mean), 0.5 * op.spread() + 1e-4);
    const double vslack = 0.25 * ov.spread() + 1e-4;
    EXPECT_GE(v.min, ov.min - vslack);
    EXPECT_LE(v.max, ov.max + vslack);
}

TEST_F(Integration, ActivationRefusedWhileAnotherClientHoldsTheLease)
{
    drivelink::agentLeaseTake(h->gui());
    Rm &r = makeRm();
    ASSERT_TRUE(r.exists());
    ASSERT_EQ(r.configure(), return_type::OK) << r.stateLabel();
    EXPECT_NE(r.activate(), return_type::OK) << "activation succeeded although the GUI holds the lease";
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_INACTIVE) << r.stateLabel();
    const auto list = clients();
    bool gui_holds = false;
    for (const auto &c : list) { gui_holds = gui_holds || (c.holder && c.kind == kKindGui); }
    EXPECT_TRUE(gui_holds) << "the GUI lost its lease:" << describe(list);
}

TEST_F(Integration, PreemptTakesTheLeaseFromAnotherClient)
{
    auto &gui = h->gui();
    const uint16_t gui_id = gui.agentHello().value().client_id;
    drivelink::agentLeaseTake(gui);
    Rm &r = makeRm("<param name=\"take_lease\">preempt</param>");
    bringUp(r);

    bool lost = false, granted = false;
    std::ostringstream seen;
    const auto deadline = Clock::now() + 5s;
    drivelink::Frame frame;
    while (!granted && Clock::now() < deadline)
    {
        if (!gui.nextAgentEvent(frame))
        {
            std::this_thread::sleep_for(20ms);
            continue;
        }
        const auto event = drivelink::agent::decodeEvent(frame.payload);
        if (!event || event->event != kEventLease) { continue; }
        const auto lease = drivelink::agent::decodeLease(event->body);
        if (lease)
        {
            seen << " [holder " << lease->holder_id << " kind " << int(lease->holder_kind) << ", previous "
                 << lease->previous_id << ", reason " << int(lease->reason) << "]";
        }
        // The agent ends the old lease first (holder 0, previous = the old holder), then grants the Take.
        if (lease && lease->previous_id == gui_id) { lost = true; }
        if (lease && lost && lease->holder_kind == kKindRos) { granted = true; }
    }
    EXPECT_TRUE(lost) << "the GUI (client " << gui_id
                      << ") saw no Lease event naming it as the previous holder; Lease events seen:" << seen.str();
    EXPECT_TRUE(granted) << "the GUI saw no Lease event granting the lease to a ROS client; Lease events seen:"
                         << seen.str();
    const auto list = clients();
    bool ros_holds = false;
    for (const auto &c : list) { ros_holds = ros_holds || (c.holder && c.kind == kKindRos); }
    EXPECT_TRUE(ros_holds) << "the plugin does not hold the lease:" << describe(list);
}

TEST_F(Integration, LeaseLossFailsTheComponent)
{
    // No stale check: only the Lease observer can fail this.
    Rm &r = makeRm("<param name=\"stale_timeout_ms\">0</param>");
    bringUp(r);
    for (int i = 0; i < 20; ++i) { ASSERT_TRUE(r.cycle()) << "read failed before the lease was taken, cycle " << i; }
    drivelink::agentLeaseTake(h->gui());
    const auto start = Clock::now();
    const int n = r.cyclesUntilFailure(5s);
    expectFailedAndUnconfigured(r, n, since(start), 1000ms, "another client took the lease");
    // The plugin must have let go: its connection is gone or at least not the holder.
    const auto deadline = Clock::now() + 3s;
    std::vector<drivelink::agent::ClientRecord> list;
    bool ros_holds = true, gui_holds = false;
    do
    {
        list = clients();
        ros_holds = holds(list, kKindRos);
        gui_holds = holds(list, kKindGui);
        if (!ros_holds && gui_holds) { break; }
        std::this_thread::sleep_for(100ms);
    } while (Clock::now() < deadline);
    EXPECT_FALSE(ros_holds) << "the plugin still holds the lease:" << describe(list);
    EXPECT_TRUE(gui_holds) << "the GUI does not hold the lease:" << describe(list);
}

TEST_F(Integration, AgentStopFailsTheComponent)
{
    // No stale check: only the Closing event or the lost link can fail this.
    Rm &r = makeRm("<param name=\"stale_timeout_ms\">0</param>");
    bringUp(r);
    for (int i = 0; i < 20; ++i) { ASSERT_TRUE(r.cycle()) << "read failed in cycle " << i; }
    h->stopAgent();
    const auto start = Clock::now();
    const int n = r.cyclesUntilFailure(5s);
    expectFailedAndUnconfigured(r, n, since(start), 1000ms, "the agent stopped");
}

TEST_F(Integration, StaleDataFailsTheComponent)
{
    Rm &r = makeRm("<param name=\"stale_timeout_ms\">300</param>");
    bringUp(r);
    for (int i = 0; i < 30; ++i) { ASSERT_TRUE(r.cycle()) << "read failed before the stall, cycle " << i; }
    // Stall the control loop's ticks: the link and the agent stay up, the group's samples stop.
    h->vdevScenario(1, "loop-stall 0 4000");
    const auto start = Clock::now();
    const int n = r.cyclesUntilFailure(3500ms);
    expectFailedAndUnconfigured(r, n, since(start), 2500ms, "a 4 s loop stall with stale_timeout_ms 300");
}

TEST_F(IntegrationPty, BootFailsTheComponent)
{
    // No stale check: only the device's Boot (a new frame generation) can fail this.
    Rm &r = makeRm("<param name=\"stale_timeout_ms\">0</param>");
    bringUp(r);
    for (int i = 0; i < 20; ++i) { ASSERT_TRUE(r.cycle()) << "read failed before the reboot, cycle " << i; }
    h->vdevScenario(1, "reboot");
    const auto start = Clock::now();
    const int n = r.cyclesUntilFailure(5s);
    expectFailedAndUnconfigured(r, n, since(start), 3000ms, "the device rebooted");
}

TEST_F(IntegrationPty, RebootBetweenConfigureAndActivateIsRefused)
{
    Rm &r = makeRm();
    ASSERT_TRUE(r.exists());
    ASSERT_EQ(r.configure(), return_type::OK) << r.stateLabel();
    ASSERT_EQ(r.state(), LifecycleState::PRIMARY_STATE_INACTIVE);
    h->vdevScenario(1, "reboot");
    std::this_thread::sleep_for(1500ms);
    EXPECT_NE(r.activate(), return_type::OK) << "activation succeeded after the device rebooted since configure";
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_UNCONFIGURED) << r.stateLabel();
    const auto list = clientsRetry();
    EXPECT_FALSE(holds(list, kKindRos)) << "the plugin holds the lease:" << describe(list);
}

TEST_F(Integration, DeactivateReleasesTheLeaseAndReactivates)
{
    Rm &r = makeRm();
    bringUp(r);
    for (int i = 0; i < 20; ++i) { ASSERT_TRUE(r.cycle()) << "read failed in cycle " << i; }
    ASSERT_EQ(r.deactivate(), return_type::OK) << r.stateLabel();
    ASSERT_EQ(r.state(), LifecycleState::PRIMARY_STATE_INACTIVE);
    auto list = clients();
    EXPECT_FALSE(holds(list, kKindRos)) << "the plugin still holds the lease after deactivate:" << describe(list);
    for (const auto &c : list)
    {
        EXPECT_FALSE(c.holder) << "somebody holds the lease after deactivate:" << describe(list);
    }
    ASSERT_EQ(r.activate(), return_type::OK) << "reactivation failed; state " << r.stateLabel();
    ASSERT_EQ(r.state(), LifecycleState::PRIMARY_STATE_ACTIVE);
    r.claimAll();
    for (int i = 0; i < 30; ++i) { ASSERT_TRUE(r.cycle()) << "read failed after reactivation, cycle " << i; }
    for (const auto &key : r.keys())
    {
        const auto v = r.value(key);
        ASSERT_TRUE(v.has_value()) << key << " has no value";
        EXPECT_TRUE(std::isfinite(*v)) << key << " = " << *v;
    }
    list = clients();
    EXPECT_TRUE(holds(list, kKindRos)) << "the plugin does not hold the lease after reactivation:" << describe(list);
}

TEST_F(Integration, CleanupAndConfigureAgain)
{
    Rm &r = makeRm();
    ASSERT_TRUE(r.exists());
    ASSERT_EQ(r.configure(), return_type::OK) << r.stateLabel();
    ASSERT_EQ(r.cleanup(), return_type::OK) << r.stateLabel();
    ASSERT_EQ(r.state(), LifecycleState::PRIMARY_STATE_UNCONFIGURED);
    ASSERT_EQ(r.configure(), return_type::OK) << "the second configure failed; state " << r.stateLabel();
    ASSERT_EQ(r.activate(), return_type::OK) << "activate after the second configure failed; state " << r.stateLabel();
    r.claimAll();
    for (int i = 0; i < 30; ++i) { ASSERT_TRUE(r.cycle()) << "read failed, cycle " << i; }
    for (const auto &key : r.keys())
    {
        const auto v = r.value(key);
        ASSERT_TRUE(v.has_value()) << key << " has no value";
        EXPECT_TRUE(std::isfinite(*v)) << key << " = " << *v;
    }
}

std::string replaceAll(std::string text, const std::string &from, const std::string &to)
{
    for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
    {
        text.replace(pos, from.size(), to);
    }
    return text;
}

TEST_F(Integration, ConfigureRefusesAnUnknownInstanceLabel)
{
    const std::string urdf = replaceAll(h->urdf(), "<param name=\"instance\">left</param>",
                                        "<param name=\"instance\">no_such_label</param>");
    Rm r(urdf);
    ASSERT_TRUE(r.exists());
    EXPECT_NE(r.configure(), return_type::OK);
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_UNCONFIGURED) << r.stateLabel();
}

TEST_F(Integration, ConfigureRefusesAWrongToken)
{
    std::string wrong = h->token();
    for (auto &c : wrong) { c = c == '0' ? '1' : '0'; }
    ASSERT_NE(wrong, h->token());
    const std::string urdf = replaceAll(h->urdf(), h->token(), wrong);
    Rm r(urdf);
    ASSERT_TRUE(r.exists());
    EXPECT_NE(r.configure(), return_type::OK);
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_UNCONFIGURED) << r.stateLabel();
}
} // namespace
