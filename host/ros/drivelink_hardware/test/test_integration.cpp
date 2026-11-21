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
#include <hardware_interface/loaned_command_interface.hpp>
#include <hardware_interface/loaned_state_interface.hpp>
#include <hardware_interface/resource_manager.hpp>
#include <hardware_interface/types/hardware_interface_return_values.hpp>
#include <lifecycle_msgs/msg/state.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_lifecycle/state.hpp>

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
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
        stopLoop();
        m_commands.clear();
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

    // --- H4b: controller_manager's real-time loop and its mode switches ---

    // A thread calling read() then write() at 100 Hz, as controller_manager's
    // update loop does. Returns when a few cycles have run.
    void startLoop()
    {
        if (m_loop.joinable()) { return; }
        m_stop = false;
        m_loop = std::thread([this] { loopMain(); });
        waitCycles(3, 5s);
    }
    void stopLoop()
    {
        m_stop = true;
        if (m_loop.joinable()) { m_loop.join(); }
    }
    // Waits (bounded) for `n` more loop cycles; false when they did not run.
    bool waitCycles(unsigned n, std::chrono::milliseconds limit = 10s)
    {
        const unsigned target = m_cycles + n;
        const auto deadline = Clock::now() + limit;
        while (m_cycles < target)
        {
            if (Clock::now() >= deadline) { return false; }
            std::this_thread::sleep_for(2ms);
        }
        return true;
    }
    // While paused the loop still calls read() but no longer calls write().
    void pauseWrite(bool paused) { m_write_paused = paused; }
    // True once any read() or write() of the loop failed (ERROR for the component).
    bool failed() const { return m_failed; }
    // Waits (bounded) for failed(); returns whether it happened.
    bool waitFailed(std::chrono::milliseconds limit)
    {
        const auto deadline = Clock::now() + limit;
        while (!m_failed)
        {
            if (Clock::now() >= deadline) { return false; }
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }
    std::string failure()
    {
        std::lock_guard lock(m_mutex);
        return m_failure;
    }
    // Runs `task` on the loop thread (before its next read()) and waits for it;
    // false when it did not run in time (the task may still run later, so it
    // must not hold references a failing test would invalidate).
    bool onLoop(std::function<void()> task, std::chrono::milliseconds limit = 5s)
    {
        auto done = std::make_shared<std::promise<void>>();
        auto result = done->get_future();
        {
            std::lock_guard lock(m_mutex);
            m_tasks.push_back([task = std::move(task), done] {
                try
                {
                    task();
                    done->set_value();
                }
                catch (...)
                {
                    done->set_exception(std::current_exception());
                }
            });
        }
        if (result.wait_for(limit) != std::future_status::ready) { return false; }
        result.get();
        return true;
    }

    // controller_manager's non-real-time half of a switch: the RM's prepare.
    bool prepare(const std::vector<std::string> &start, const std::vector<std::string> &stop)
    {
        return m_rm->prepare_command_mode_switch(start, stop);
    }
    bool perform(const std::vector<std::string> &start, const std::vector<std::string> &stop)
    {
        return m_rm->perform_command_mode_switch(start, stop);
    }
    // A whole switch as controller_manager does it, from the calling thread:
    // prepare, perform, then the controller gives up the stopped interfaces
    // and claims the started ones (their values start unset, NaN).
    bool switchTo(const std::vector<std::string> &start, const std::vector<std::string> &stop)
    {
        if (!prepare(start, stop) || !perform(start, stop)) { return false; }
        for (const auto &key : stop) { m_commands.erase(key); }
        for (const auto &key : start) { m_commands.emplace(key, m_rm->claim_command_interface(key)); }
        return true;
    }
    // Drops the loans, then prepares and performs the stop.
    bool release(const std::vector<std::string> &stop)
    {
        for (const auto &key : stop) { m_commands.erase(key); }
        return prepare({}, stop) && perform({}, stop);
    }
    // Sets a claimed command interface; false when it is not claimed or refused.
    bool command(const std::string &key, double value)
    {
        const auto it = m_commands.find(key);
        return it != m_commands.end() && setLoaned(it->second, value);
    }

  private:
    // set_value returns bool ([[nodiscard]]) in newer ros2_control and void in older.
    template <class Loan> static bool setLoaned(Loan &loan, double value)
    {
        if constexpr (std::is_void_v<decltype(loan.set_value(value))>)
        {
            loan.set_value(value);
            return true;
        }
        else
        {
            return loan.set_value(value);
        }
    }

    void loopMain()
    {
        auto next = Clock::now();
        while (!m_stop)
        {
            std::deque<std::function<void()>> tasks;
            {
                std::lock_guard lock(m_mutex);
                tasks.swap(m_tasks);
            }
            for (auto &task : tasks) { task(); }
            const auto time = m_clock->now();
            const auto period = rclcpp::Duration::from_seconds(0.01);
            if (readFailed(m_rm->read(time, period))) { noteFailure("read()"); }
            if (!m_write_paused && readFailed(m_rm->write(time, period))) { noteFailure("write()"); }
            ++m_cycles;
            next += 10ms;
            if (Clock::now() > next + 100ms) { next = Clock::now(); } // a stall: do not burst to catch up
            std::this_thread::sleep_until(next);
        }
    }
    void noteFailure(const char *where)
    {
        std::lock_guard lock(m_mutex);
        if (!m_failed) { m_failure = std::string(where) + " returned ERROR in loop cycle " + std::to_string(m_cycles.load()); }
        m_failed = true;
    }

    std::string m_name;
    rclcpp::Clock::SharedPtr m_clock;
    std::unique_ptr<hardware_interface::ResourceManager> m_rm;
    std::map<std::string, hardware_interface::LoanedStateInterface> m_loans;
    std::map<std::string, hardware_interface::LoanedCommandInterface> m_commands;
    std::thread m_loop;
    std::atomic<bool> m_stop{false}, m_write_paused{false}, m_failed{false};
    std::atomic<unsigned> m_cycles{0};
    std::mutex m_mutex;
    std::deque<std::function<void()>> m_tasks;
    std::string m_failure;
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

    // H4b: both wheels declare position, velocity and duty commands.
    static std::vector<Harness::JointSpec> commandJoints()
    {
        const std::vector<std::string> commands{"position", "velocity", "duty"};
        return {Harness::JointSpec{"left_wheel", "left", {"position", "velocity"}, commands, {}},
                Harness::JointSpec{"right_wheel", "right", {"position", "velocity"}, commands, {}}};
    }

    Rm &makeCommandRm(const std::string &extra = {})
    {
        rm = std::make_unique<Rm>(h->urdf(kComponent, extra, commandJoints()));
        return *rm;
    }

    // The H4b setup: the component with command interfaces, configured and
    // activated, its state interfaces loaned, controller_manager's loop running.
    // The device stays disarmed until a claim.
    void upCommanded(const std::string &extra = {})
    {
        Rm &r = makeCommandRm(extra);
        ASSERT_NO_FATAL_FAILURE(bringUp(r));
        r.claimAll();
        r.startLoop();
        ASSERT_TRUE(r.waitCycles(5)) << "the loop did not run";
        ASSERT_FALSE(r.failed()) << r.failure();
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

// ---------------------------------------------------------------------------
// H4b: the command path. The plant is actuator.mock (gain 10, tau 0.1 s,
// deadband 0.02) behind a cascade controller whose velocity and position gains
// the harness sets, so a velocity or position setpoint makes it track. The
// measured velocity ripples (the mock encoder is quantised), so velocities are
// judged by their mean over a window.

constexpr const char *kLeftVel = "left_wheel/velocity", *kLeftPos = "left_wheel/position",
                     *kLeftDuty = "left_wheel/duty", *kRightVel = "right_wheel/velocity",
                     *kRightPos = "right_wheel/position";

template <class F> bool waitFor(F predicate, std::chrono::milliseconds limit, std::chrono::milliseconds step = 10ms)
{
    const auto deadline = Clock::now() + limit;
    for (;;)
    {
        if (predicate()) { return true; }
        if (Clock::now() >= deadline) { return false; }
        std::this_thread::sleep_for(step);
    }
}

double state(Rm &r, const std::string &key) { return r.value(key).value_or(NAN); }

// The mean of a state interface over a window of 10 ms samples.
double meanOver(Rm &r, const std::string &key, std::chrono::milliseconds window)
{
    std::vector<double> samples;
    const auto end = Clock::now() + window;
    while (Clock::now() < end)
    {
        const auto v = r.value(key);
        if (v) { samples.push_back(*v); }
        std::this_thread::sleep_for(10ms);
    }
    return samples.empty() ? NAN : std::accumulate(samples.begin(), samples.end(), 0.0) / double(samples.size());
}

// The mock arm blocker's action arguments, placed by the descriptor's names.
std::array<uint32_t, 4> blockerArgs(drivelink::Session &gui, bool disarms, bool active)
{
    drivelink::Client client(gui);
    uint8_t type = 0;
    for (const auto &i : client.instances())
    {
        if (i.label == "left_act") { type = i.type; }
    }
    for (const auto &item : client.items(type))
    {
        if (item.name != "diagnostic") { continue; }
        std::array<uint32_t, 4> args{};
        for (size_t n = 0; n < item.args.size() && n < args.size(); ++n)
        {
            if (item.args[n].value_type != drivelink::ValueType::Bool) { throw std::runtime_error("diagnostic arg is not a bool"); }
            if (item.args[n].name == "disarms") { args[n] = disarms ? 1u : 0u; }
            else if (item.args[n].name == "active") { args[n] = active ? 1u : 0u; }
            else { throw std::runtime_error("unexpected diagnostic arg " + item.args[n].name); }
        }
        return args;
    }
    throw std::runtime_error("left_act has no diagnostic action");
}

// The harness's own GUI-client helpers, with no plugin: the status stream, the
// params and actions by label and name, the fault and the disarm.
TEST_F(Integration, HarnessGuiHelpers)
{
    auto &gui = h->gui();
    const auto first = h->armStatus();
    EXPECT_EQ(first.arm_state, 0) << "the device starts disarmed";
    EXPECT_LE(first.last_disarm_reason, 5);
    std::cout << "[  stats   ] first status: arm_state " << int(first.arm_state) << ", last_disarm_reason "
              << int(first.last_disarm_reason) << ", flags " << int(first.flags) << std::endl;
    EXPECT_NO_THROW(h->waitArm(false, 2s));

    EXPECT_EQ(h->enumParam("left", "mode"), "velocity");
    EXPECT_EQ(h->enumParam("right", "mode"), "velocity");
    EXPECT_EQ(h->param("left", "mode"), 1.0);
    EXPECT_NEAR(h->param("left", "vel.kp"), 0.3, 1e-6);
    EXPECT_NEAR(h->param("right", "vel.ki"), 2.0, 1e-6);
    EXPECT_NEAR(h->param("right", "pos.kp"), 4.0, 1e-6);
    EXPECT_THROW(h->param("left", "no_such_item"), HarnessError);
    EXPECT_THROW(h->param("no_such_label", "mode"), HarnessError);

    // The raised blocker refuses an ARM and the cleared one lets it through; DISARM and the
    // status stream follow. (A command group is needed to ARM at all.)
    Device dev(gui);
    uint16_t velocity_sp = 0xFFFF;
    for (const auto &i : dev.client.items(dev.controller))
    {
        if (i.name == "velocity_sp") { velocity_sp = i.id; }
    }
    ASSERT_NE(velocity_sp, 0xFFFF);
    drivelink::agentLeaseTake(gui);
    dev.client.cmdConfigure(0, dev.rt_loop, std::vector<drivelink::Ref>{{dev.controller, dev.left, velocity_sp}});

    h->action("left_act", "diagnostic", blockerArgs(gui, false, true));
    EXPECT_ANY_THROW(dev.client.arm()) << "ARM was accepted with a blocker raised";
    EXPECT_EQ(h->armStatus().arm_state, 0);
    h->action("left_act", "diagnostic", blockerArgs(gui, false, false));
    ASSERT_NO_THROW(dev.client.arm());
    const auto armed = h->waitArm(true);
    EXPECT_EQ(armed.arm_state, 1);
    h->disarm();
    const auto disarmed = h->waitArm(false);
    EXPECT_NE(disarmed.arm_state, 1);
    const auto after = h->armStatus();
    EXPECT_EQ(after.arm_state, 0);
    EXPECT_EQ(after.last_disarm_reason, 1) << "an explicit DISARM is reason Explicit (1)";

    // A disarming fault: latched, then cleared and acknowledged.
    h->vdevScenario(1, "fault");
    EXPECT_TRUE(waitFor([&] { return (h->armStatus().flags & 1) != 0; }, 3s)) << "no fault_latched flag after a fault";
    h->vdevScenario(2, "fault-clear");
    EXPECT_TRUE(waitFor(
        [&] {
            h->acknowledgeFault();
            return (h->armStatus().flags & 1) == 0;
        },
        5s, 200ms))
        << "the fault stayed latched after fault-clear and ACK_FAULT";
}

// The loop helper of Rm, with a state-only component (no command path needed).
TEST_F(Integration, ControllerManagerLoopHelper)
{
    Rm &r = makeRm("<param name=\"stale_timeout_ms\">0</param>");
    ASSERT_NO_FATAL_FAILURE(bringUp(r));
    r.claimAll();
    r.startLoop();
    ASSERT_TRUE(r.waitCycles(10));
    EXPECT_FALSE(r.failed());
    // A task runs on the loop thread, not on this one.
    const auto main_id = std::this_thread::get_id();
    auto loop_id = std::make_shared<std::atomic<bool>>(false);
    ASSERT_TRUE(r.onLoop([loop_id, main_id] { loop_id->store(std::this_thread::get_id() != main_id); }));
    EXPECT_TRUE(loop_id->load());
    EXPECT_THROW(r.onLoop([] { throw std::runtime_error("from the task"); }), std::runtime_error);
    // write() pauses and resumes; read() goes on (the state stays finite and the loop cycles).
    r.pauseWrite(true);
    EXPECT_TRUE(r.waitCycles(20));
    r.pauseWrite(false);
    EXPECT_TRUE(std::isfinite(state(r, "left_wheel/position")));
    // A stops-only switch of nothing, and an unknown start, as controller_manager would try them.
    EXPECT_TRUE(r.prepare({}, {}));
    EXPECT_FALSE(r.prepare({kLeftVel}, {})) << "an interface the component does not export was accepted";
    // A read() ERROR from the loop is recorded.
    drivelink::agentLeaseTake(h->gui());
    EXPECT_TRUE(r.waitFailed(5s));
    EXPECT_FALSE(r.failure().empty());
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_UNCONFIGURED) << r.stateLabel();
}

TEST_F(Integration, ClaimArmsAndVelocityMovesThePlant)
{
    ASSERT_NO_FATAL_FAILURE(upCommanded());
    Rm &r = *rm;
    ASSERT_TRUE(r.switchTo({kLeftVel, kRightVel}, {})) << "the claim was refused; state " << r.stateLabel();
    EXPECT_TRUE(h->waitArm(true).arm_state == 1);
    EXPECT_EQ(h->enumParam("left", "mode"), "velocity");
    EXPECT_EQ(h->enumParam("right", "mode"), "velocity");

    const double p0 = state(r, "left_wheel/position");
    ASSERT_TRUE(r.command(kLeftVel, 3.0));
    ASSERT_TRUE(r.command(kRightVel, 3.0));
    ASSERT_TRUE(waitFor([&] { return state(r, "left_wheel/position") > p0 + 1.0; }, 4s))
        << "the left wheel's position did not increase: " << state(r, "left_wheel/position") << " from " << p0;
    EXPECT_NEAR(meanOver(r, "left_wheel/velocity", 500ms), 3.0, 0.45) << "left velocity";
    EXPECT_NEAR(meanOver(r, "right_wheel/velocity", 500ms), 3.0, 0.45) << "right velocity";
    EXPECT_FALSE(r.failed()) << r.failure();

    ASSERT_TRUE(r.release({kLeftVel, kRightVel}));
    EXPECT_NO_THROW(h->waitArm(false));
    EXPECT_TRUE(r.waitCycles(30));
    EXPECT_FALSE(r.failed()) << "releasing the claims failed the component: " << r.failure();
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_ACTIVE) << r.stateLabel();
}

TEST_F(Integration, ModeChangeWhileArmed)
{
    ASSERT_NO_FATAL_FAILURE(upCommanded());
    Rm &r = *rm;
    ASSERT_TRUE(r.switchTo({kLeftVel, kRightVel}, {}));
    ASSERT_TRUE(h->waitArm(true).arm_state == 1);
    ASSERT_TRUE(r.command(kRightVel, 2.0));
    ASSERT_TRUE(r.command(kLeftVel, 1.0));
    ASSERT_TRUE(waitFor([&] { return state(r, "right_wheel/velocity") > 1.0; }, 3s));

    // Left velocity -> position in one switch, right keeps running.
    ASSERT_TRUE(r.switchTo({kLeftPos}, {kLeftVel})) << "the mode switch was refused";
    EXPECT_TRUE(h->waitArm(true).arm_state == 1) << "the switch disarmed the device";
    EXPECT_EQ(h->enumParam("left", "mode"), "position");
    EXPECT_EQ(h->enumParam("right", "mode"), "velocity");
    EXPECT_FALSE(r.failed()) << r.failure();

    const double target = state(r, "left_wheel/position") + 1.0;
    ASSERT_TRUE(r.command(kLeftPos, target));
    EXPECT_TRUE(waitFor([&] { return std::abs(state(r, "left_wheel/position") - target) < 0.1; }, 4s))
        << "left position " << state(r, "left_wheel/position") << " did not reach " << target;
    EXPECT_NEAR(meanOver(r, "right_wheel/velocity", 500ms), 2.0, 0.3) << "the right wheel's velocity was disturbed";
    EXPECT_EQ(h->enumParam("right", "mode"), "velocity");
    EXPECT_FALSE(r.failed()) << r.failure();
}

TEST_F(Integration, DutyClaimSetsOpenLoop)
{
    ASSERT_NO_FATAL_FAILURE(upCommanded());
    Rm &r = *rm;
    ASSERT_TRUE(r.switchTo({kLeftDuty}, {}));
    ASSERT_TRUE(h->waitArm(true).arm_state == 1);
    EXPECT_EQ(h->enumParam("left", "mode"), "open_loop");
    ASSERT_TRUE(r.command(kLeftDuty, 0.3));
    ASSERT_TRUE(waitFor([&] { return state(r, "left_wheel/velocity") > 1.0; }, 3s)) << "the wheel did not turn";
    const double mean = meanOver(r, "left_wheel/velocity", 500ms);
    // gain 10 x duty 0.3 = 3 rad/s, less the deadband's share.
    EXPECT_GT(mean, 2.4);
    EXPECT_LT(mean, 3.2);
    EXPECT_FALSE(r.failed()) << r.failure();
}

TEST_F(Integration, NaNIsSentAsHold)
{
    ASSERT_NO_FATAL_FAILURE(upCommanded());
    Rm &r = *rm;
    const double right_position = state(r, "right_wheel/position");
    ASSERT_TRUE(r.switchTo({kLeftVel, kRightPos}, {}));
    ASSERT_TRUE(h->waitArm(true).arm_state == 1);
    // Neither command is ever set: more than the device watchdog (200 ms) passes on holds.
    ASSERT_TRUE(r.waitCycles(100)) << "the loop stopped";
    EXPECT_FALSE(r.failed()) << "the component failed with unset commands: " << r.failure();
    EXPECT_EQ(h->waitArm(true).arm_state, 1) << "the device disarmed";
    EXPECT_NEAR(meanOver(r, "left_wheel/velocity", 300ms), 0.0, 0.2);
    EXPECT_NEAR(state(r, "right_wheel/position"), right_position, 0.05);
}

TEST_F(Integration, TwoInterfacesOnOneJointRefused)
{
    ASSERT_NO_FATAL_FAILURE(upCommanded());
    Rm &r = *rm;
    EXPECT_FALSE(r.prepare({kLeftPos, kLeftVel}, {})) << "two interfaces of one joint were accepted";
    const auto status = h->waitArm(false);
    EXPECT_NE(status.arm_state, 1) << "the refused switch armed the device";
    EXPECT_TRUE(r.waitCycles(20));
    EXPECT_FALSE(r.failed()) << r.failure();
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_ACTIVE) << r.stateLabel();
}

TEST_F(Integration, GuiTakeFailsTheComponent)
{
    ASSERT_NO_FATAL_FAILURE(upCommanded());
    Rm &r = *rm;
    ASSERT_TRUE(r.switchTo({kLeftVel}, {}));
    ASSERT_TRUE(h->waitArm(true).arm_state == 1);
    ASSERT_TRUE(r.command(kLeftVel, 1.0));
    drivelink::agentLeaseTake(h->gui());
    const auto start = Clock::now();
    ASSERT_TRUE(r.waitFailed(5s)) << "no ERROR after the GUI took the lease";
    EXPECT_LT(since(start).count(), 2000);
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_UNCONFIGURED) << r.stateLabel();
    EXPECT_NE(h->waitArm(false).arm_state, 1) << "the device is still armed";
}

TEST_F(Integration, DisarmingFaultFailsTheComponent)
{
    ASSERT_NO_FATAL_FAILURE(upCommanded());
    Rm &r = *rm;
    ASSERT_TRUE(r.switchTo({kLeftVel}, {}));
    ASSERT_TRUE(h->waitArm(true).arm_state == 1);
    ASSERT_TRUE(r.command(kLeftVel, 1.0));
    h->vdevScenario(1, "fault");
    const auto start = Clock::now();
    ASSERT_TRUE(r.waitFailed(5s)) << "no ERROR after a disarming fault";
    EXPECT_LT(since(start).count(), 3000);
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_UNCONFIGURED) << r.stateLabel();
    EXPECT_NE(h->waitArm(false).arm_state, 1);
    // Cleanup: the fault goes away and is acknowledged.
    h->vdevScenario(2, "fault-clear");
    EXPECT_TRUE(waitFor(
        [&] {
            h->acknowledgeFault();
            return (h->armStatus().flags & 1) == 0;
        },
        5s, 200ms))
        << "the fault stayed latched";
}

TEST_F(Integration, WatchdogTripFailsTheComponent)
{
    ASSERT_NO_FATAL_FAILURE(upCommanded());
    Rm &r = *rm;
    ASSERT_TRUE(r.switchTo({kLeftVel}, {}));
    ASSERT_TRUE(h->waitArm(true).arm_state == 1);
    ASSERT_TRUE(r.command(kLeftVel, 1.0));
    // write() stops (read() continues): there are no keepalives, so the 200 ms device watchdog disarms.
    r.pauseWrite(true);
    const auto start = Clock::now();
    const bool failed = r.waitFailed(3s);
    const auto took = since(start);
    r.pauseWrite(false);
    ASSERT_TRUE(failed) << "no ERROR within 3 s of write() stopping";
    EXPECT_GE(took.count(), 100) << "the failure came before the watchdog could have tripped";
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_UNCONFIGURED) << r.stateLabel();
    ASSERT_NO_THROW(h->waitArm(false));
    const auto status = h->armStatus();
    EXPECT_EQ(status.arm_state, 0);
    EXPECT_EQ(status.last_disarm_reason, 2) << "the device was not disarmed by its watchdog";
}

TEST_F(Integration, RefusedArmFailsTheSwitch)
{
    // Before activation nobody holds the lease and the device is disarmed: raise an arm blocker.
    h->action("left_act", "diagnostic", blockerArgs(h->gui(), false, true));
    ASSERT_NO_FATAL_FAILURE(upCommanded());
    Rm &r = *rm;
    EXPECT_FALSE(r.prepare({kLeftVel}, {})) << "the switch was accepted although the device refuses to arm";
    EXPECT_NE(h->waitArm(false).arm_state, 1);
    EXPECT_TRUE(r.waitCycles(20));
    EXPECT_FALSE(r.failed()) << "a refused switch failed the component: " << r.failure();
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_ACTIVE) << r.stateLabel();
    ASSERT_EQ(r.deactivate(), return_type::OK) << r.stateLabel();
    h->action("left_act", "diagnostic", blockerArgs(h->gui(), false, false));
}

TEST_F(Integration, LostArmReplyDisarms)
{
    ASSERT_NO_FATAL_FAILURE(upCommanded("<param name=\"timeout_ms\">1000</param>"));
    Rm &r = *rm;
    h->vdevScenario(1, "drop-arm-reply 1");
    const auto start = Clock::now();
    EXPECT_FALSE(r.prepare({kLeftVel}, {})) << "the switch was accepted without an ARM reply";
    const auto took = since(start);
    EXPECT_GE(took.count(), 800) << "the refusal did not wait for the timeout";
    EXPECT_LT(took.count(), 4000) << "the refusal took " << took.count() << " ms";
    // The ARM did reach the device, so the plugin must have disarmed it again.
    EXPECT_NE(h->waitArm(false, 3s).arm_state, 1);
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_ACTIVE) << r.stateLabel();
    ASSERT_TRUE(r.switchTo({kLeftVel}, {})) << "the next claim was refused";
    EXPECT_EQ(h->waitArm(true).arm_state, 1);
    EXPECT_FALSE(r.failed()) << r.failure();
}

TEST_F(Integration, DeactivateWhileClaimedDisarmsAndReleases)
{
    ASSERT_NO_FATAL_FAILURE(upCommanded());
    Rm &r = *rm;
    ASSERT_TRUE(r.switchTo({kLeftVel, kRightVel}, {}));
    ASSERT_TRUE(h->waitArm(true).arm_state == 1);
    ASSERT_TRUE(r.command(kLeftVel, 1.0));
    ASSERT_EQ(r.deactivate(), return_type::OK) << r.stateLabel();
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_INACTIVE);
    EXPECT_NE(h->waitArm(false).arm_state, 1) << "the device is still armed after deactivate";
    const auto list = clients();
    for (const auto &c : list)
    {
        EXPECT_FALSE(c.holder) << "somebody holds the lease after deactivate:" << describe(list);
    }
}

TEST_F(Integration, AbandonedSwitchFailsTheComponent)
{
    ASSERT_NO_FATAL_FAILURE(upCommanded("<param name=\"timeout_ms\">500</param>"));
    Rm &r = *rm;
    ASSERT_TRUE(r.prepare({kLeftVel}, {})) << "the switch was refused; state " << r.stateLabel();
    // controller_manager never performs it.
    const auto start = Clock::now();
    ASSERT_TRUE(r.waitFailed(2s)) << "no ERROR within 2 s of an unperformed switch";
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_UNCONFIGURED)
        << r.stateLabel() << " after " << since(start).count() << " ms";
    EXPECT_NE(h->waitArm(false).arm_state, 1);
}

TEST_F(Integration, ClaimFromTheRtThreadIsRefused)
{
    ASSERT_NO_FATAL_FAILURE(upCommanded());
    Rm &r = *rm;
    auto accepted = std::make_shared<std::atomic<bool>>(true);
    ASSERT_TRUE(r.onLoop([&r, accepted] { accepted->store(r.prepare({kLeftVel}, {})); })) << "the task did not run";
    EXPECT_FALSE(accepted->load()) << "a claim from the real-time thread was accepted";
    EXPECT_NE(h->waitArm(false).arm_state, 1);
    EXPECT_TRUE(r.waitCycles(20));
    EXPECT_FALSE(r.failed()) << r.failure();
    EXPECT_EQ(r.state(), LifecycleState::PRIMARY_STATE_ACTIVE) << r.stateLabel();
}
} // namespace
