// Implements harness.hpp (see there for the contract). POSIX only.
#include "harness.hpp"
#include <fstream>

#include <drivelink/client.hpp>
#include <drivelink/session.hpp>
#include <drivelink/protocol/gen/registry.hpp>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <bit>
#include <filesystem>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <span>
#include <sstream>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

extern char **environ;

namespace drivelink_hardware::test
{
namespace
{
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr size_t kTailLines = 50;
constexpr size_t kMaxOutText = 4u << 20;

std::string requiredEnv(const char *name)
{
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0')
    {
        throw HarnessError(std::string("the environment variable ") + name + " is not set (the test needs the host superbuild programs)");
    }
    return value;
}

// A child process with its stdout and stderr drained by reader threads.
class Child
{
  public:
    Child(std::string name, const std::vector<std::string> &argv) : m_name(std::move(name))
    {
        int out[2], err[2];
        if (::pipe2(out, O_CLOEXEC) != 0 || ::pipe2(err, O_CLOEXEC) != 0)
        {
            throw HarnessError(m_name + ": pipe failed");
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, out[1], 1);
        posix_spawn_file_actions_adddup2(&actions, err[1], 2);
        std::vector<char *> args;
        for (const auto &a : argv) { args.push_back(const_cast<char *>(a.c_str())); }
        args.push_back(nullptr);
        const int rc = ::posix_spawn(&m_pid, argv[0].c_str(), &actions, nullptr, args.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(out[1]);
        ::close(err[1]);
        if (rc != 0)
        {
            ::close(out[0]);
            ::close(err[0]);
            throw HarnessError(m_name + ": cannot start " + argv[0] + ": " + std::strerror(rc));
        }
        m_threads.emplace_back([this, fd = out[0]] { drain(fd, true); });
        m_threads.emplace_back([this, fd = err[0]] { drain(fd, false); });
    }

    ~Child()
    {
        if (m_pid > 0 && !m_reaped)
        {
            ::kill(m_pid, SIGCONT);
            ::kill(m_pid, SIGKILL);
            int status = 0;
            ::waitpid(m_pid, &status, 0);
        }
        for (auto &t : m_threads) { t.join(); }
    }

    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;

    pid_t pid() const { return m_pid; }

    // The next stdout line that starts with prefix.
    std::string waitLine(const std::string &prefix, std::chrono::milliseconds timeout)
    {
        const auto deadline = Clock::now() + timeout;
        std::unique_lock lock(m_mutex);
        for (;;)
        {
            while (!m_lines.empty())
            {
                std::string line = std::move(m_lines.front());
                m_lines.pop_front();
                if (line.rfind(prefix, 0) == 0) { return line; }
            }
            if (m_out_eof)
            {
                lock.unlock();
                throw HarnessError(m_name + " exited before printing '" + prefix + "'\n" + diag());
            }
            if (m_cv.wait_until(lock, deadline) == std::cv_status::timeout && m_lines.empty())
            {
                lock.unlock();
                throw HarnessError(m_name + ": no '" + prefix + "' line within " + std::to_string(timeout.count()) +
                                   " ms\n" + diag());
            }
        }
    }

    // True when the child exited within the timeout.
    bool waitExit(std::chrono::milliseconds timeout)
    {
        const auto deadline = Clock::now() + timeout;
        for (;;)
        {
            if (pollExit()) { return true; }
            if (Clock::now() >= deadline) { return false; }
            std::this_thread::sleep_for(10ms);
        }
    }

    bool alive() { return !pollExit(); }

    // SIGTERM, then SIGKILL after the timeout. Returns whether SIGTERM was enough.
    bool terminate(std::chrono::milliseconds timeout)
    {
        if (m_reaped) { return true; }
        ::kill(m_pid, SIGCONT);
        ::kill(m_pid, SIGTERM);
        if (waitExit(timeout)) { return true; }
        ::kill(m_pid, SIGKILL);
        int status = 0;
        ::waitpid(m_pid, &status, 0);
        std::lock_guard lock(m_mutex);
        m_status = status;
        m_reaped = true;
        return false;
    }

    std::string stdoutText()
    {
        std::lock_guard lock(m_mutex);
        return m_out_text;
    }

    int exitCode() // -1 while running or when killed by a signal
    {
        pollExit();
        std::lock_guard lock(m_mutex);
        return m_reaped && WIFEXITED(m_status) ? WEXITSTATUS(m_status) : -1;
    }

    std::string status()
    {
        pollExit();
        std::lock_guard lock(m_mutex);
        if (!m_reaped) { return "still running"; }
        if (WIFEXITED(m_status)) { return "exited with status " + std::to_string(WEXITSTATUS(m_status)); }
        if (WIFSIGNALED(m_status)) { return "killed by signal " + std::to_string(WTERMSIG(m_status)); }
        return "ended";
    }

    // The last stderr (and stdout) lines and the exit status.
    std::string diag()
    {
        const std::string state = status();
        std::lock_guard lock(m_mutex);
        std::ostringstream out;
        out << "  [" << m_name << "] " << state << "\n";
        for (const auto &l : m_err_tail) { out << "  [" << m_name << " stderr] " << l << "\n"; }
        if (m_err_tail.empty()) { out << "  [" << m_name << " stderr] (empty)\n"; }
        return out.str();
    }

  private:
    bool pollExit()
    {
        std::lock_guard lock(m_mutex);
        if (m_reaped) { return true; }
        int status = 0;
        const pid_t r = ::waitpid(m_pid, &status, WNOHANG);
        if (r == m_pid)
        {
            m_status = status;
            m_reaped = true;
            return true;
        }
        return false;
    }

    void drain(int fd, bool is_out)
    {
        std::string partial;
        char buffer[4096];
        for (;;)
        {
            const ssize_t n = ::read(fd, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) { continue; }
            if (n <= 0) { break; }
            partial.append(buffer, size_t(n));
            std::lock_guard lock(m_mutex);
            if (is_out && m_out_text.size() < kMaxOutText) { m_out_text.append(buffer, size_t(n)); }
            size_t pos;
            while ((pos = partial.find('\n')) != std::string::npos)
            {
                std::string line = partial.substr(0, pos);
                partial.erase(0, pos + 1);
                if (is_out)
                {
                    m_lines.push_back(std::move(line));
                    if (m_lines.size() > 1000) { m_lines.pop_front(); }
                }
                else
                {
                    m_err_tail.push_back(std::move(line));
                    if (m_err_tail.size() > kTailLines) { m_err_tail.pop_front(); }
                }
            }
            m_cv.notify_all();
        }
        {
            std::lock_guard lock(m_mutex);
            if (!partial.empty() && !is_out)
            {
                m_err_tail.push_back(partial);
                if (m_err_tail.size() > kTailLines) { m_err_tail.pop_front(); }
            }
            if (is_out) { m_out_eof = true; }
        }
        m_cv.notify_all();
        ::close(fd);
    }

    std::string m_name;
    pid_t m_pid = -1;
    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_lines, m_err_tail;
    std::string m_out_text;
    bool m_out_eof = false;
    bool m_reaped = false;
    int m_status = 0;
};

std::string randomToken()
{
    std::random_device device;
    std::ostringstream out;
    for (int i = 0; i < 6; ++i) { out << std::hex << std::setw(4) << std::setfill('0') << (device() & 0xFFFF); }
    return out.str(); // 24 hex digits
}
} // namespace

struct Harness::Impl
{
    std::string vdev_program, agent_program, cli_program;
    fs::path dir;
    std::unique_ptr<Child> vdev, agent;
    std::string vdev_endpoint, agent_endpoint, token;
    std::unique_ptr<drivelink::Session> gui;
    // The newest status-stream frame the GUI client has seen (see armStatus).
    struct GuiWatch
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool seen = false;
        uint64_t seq = 0;
        ArmStatus status;
        std::vector<uint8_t> disarms; // last_disarm_reason at each armed -> not armed transition
    };
    std::shared_ptr<GuiWatch> watch = std::make_shared<GuiWatch>();
    uint64_t gui_observer = 0;

    void closeGui()
    {
        if (!gui) { return; }
        try
        {
            if (gui_observer != 0) { gui->unobserve(gui_observer); }
            gui->close();
        }
        catch (...)
        {
        }
        gui_observer = 0;
        gui.reset();
        watch = std::make_shared<GuiWatch>();
    }

    std::string diag()
    {
        std::string text;
        if (vdev) { text += vdev->diag(); }
        if (agent) { text += agent->diag(); }
        return text;
    }

    [[noreturn]] void fail(const std::string &step, const std::string &detail)
    {
        throw HarnessError("harness: " + step + ": " + detail + "\n" + diag());
    }

    // One drivelink-cli process against the vdev (the vdev serves one client at
    // a time and frees it when it notices a close, so a refused connection,
    // exit code 3, is retried: such a command never reached the device).
    std::string cli(const std::vector<std::string> &command)
    {
        std::string last;
        const auto deadline = Clock::now() + 15s;
        for (;;)
        {
            std::vector<std::string> argv{cli_program, "--endpoint", vdev_endpoint, "--json"};
            argv.insert(argv.end(), command.begin(), command.end());
            Child child("cli", argv);
            if (!child.waitExit(30s))
            {
                child.terminate(1s);
                fail("cli " + command.front(), "the CLI did not finish within 30 s");
            }
            const std::string out = child.stdoutText();
            const int code = child.exitCode();
            if (code == 0 && out.find("\"ok\":true") != std::string::npos) { return out; }
            last = "exit " + std::to_string(code) + "\n  stdout: " + out + child.diag();
            if (code != 3 || Clock::now() >= deadline)
            {
                fail("cli " + command.front(), last);
            }
            std::this_thread::sleep_for(200ms);
        }
    }

    // A typed read of the vdev through libdrivelink, with the same retry.
    template <class F> auto readVdev(const std::string &step, F fn)
    {
        std::string last;
        const auto deadline = Clock::now() + 15s;
        for (;;)
        {
            try
            {
                drivelink::SessionOptions options;
                options.timeout = 3000ms;
                options.periodic_time = false;
                drivelink::Session session(vdev_endpoint, options);
                drivelink::Client client(session);
                auto result = fn(client);
                session.close();
                return result;
            }
            catch (const std::exception &e)
            {
                last = e.what();
            }
            if (Clock::now() >= deadline) { fail(step, last); }
            std::this_thread::sleep_for(200ms);
        }
    }

    int instanceOf(const std::string &json, const std::string &step)
    {
        std::smatch m;
        if (!std::regex_search(json, m, std::regex("\"instance\":([0-9]+)"))) { fail(step, "no instance in: " + json); }
        return std::stoi(m[1]);
    }

    void layout()
    {
        struct Lookup
        {
            std::map<std::string, int> types;
            std::map<int, std::map<std::string, int>> items;
            int rt_loop = -1, sensor_loop = -1;
        };
        const std::vector<std::string> wanted{"controller.cascade_pid", "actuator.mock", "feedback.mock", "imu.mock"};
        const Lookup look = readVdev("lookup", [&](drivelink::Client &c) {
            Lookup l;
            uint8_t imu_mask = 0, ctrl_mask = 0;
            for (const auto &t : c.types())
            {
                l.types[t.name] = t.type;
                if (t.name == "imu.mock") { imu_mask = t.loop_mask; }
                if (t.name == "controller.cascade_pid") { ctrl_mask = t.loop_mask; }
            }
            for (const auto &name : wanted)
            {
                const auto it = l.types.find(name);
                if (it == l.types.end()) { throw std::runtime_error("the device has no type " + name); }
                for (const auto &item : c.items(uint8_t(it->second))) { l.items[it->second][item.name] = item.id; }
            }
            // loop_mask bit 0: realtime loops, bit 1: non-realtime loops (core.md).
            for (const auto &loop : c.info().loops)
            {
                if (loop.realtime() && l.rt_loop < 0 && (ctrl_mask & 1)) { l.rt_loop = loop.id; }
                if (!loop.realtime() && l.sensor_loop < 0 && (imu_mask & 2)) { l.sensor_loop = loop.id; }
            }
            return l;
        });
        if (look.rt_loop < 0) { fail("layout", "the device has no realtime loop for the controllers"); }
        if (look.sensor_loop < 0) { fail("layout", "the device has no non-realtime loop for imu.mock"); }
        const int act_t = look.types.at("actuator.mock"), fb_t = look.types.at("feedback.mock"),
                  ctl_t = look.types.at("controller.cascade_pid"), imu_t = look.types.at("imu.mock");

        auto add = [&](int type, const std::string &label, int loop) {
            return instanceOf(cli({"add", std::to_string(type), label, std::to_string(loop)}), "add " + label);
        };
        const int left_act = add(act_t, "left_act", look.rt_loop);
        const int right_act = add(act_t, "right_act", look.rt_loop);
        const int left_fb = add(fb_t, "left_fb", look.rt_loop);
        const int right_fb = add(fb_t, "right_fb", look.rt_loop);
        const int left = add(ctl_t, "left", look.rt_loop);
        const int right = add(ctl_t, "right", look.rt_loop);
        add(imu_t, "imu", look.sensor_loop);

        auto ref = [](int type, int instance, int item) {
            return std::to_string(type) + ":" + std::to_string(instance) + ":" + std::to_string(item);
        };
        auto inst_ref = [](int type, int instance) { return std::to_string((type << 8) | instance); };
        const auto &fb_items = look.items.at(fb_t);
        const auto &ctl_items = look.items.at(ctl_t);
        // H4b: gains so that velocity_sp and position_sp make the mock plant
        // (gain 10, tau 0.1 s, deadband 0.02) track: a 3 rad/s step settles
        // within a few percent of its mean in 1 s, a +1 rad step within 0.01
        // rad in 1 s (measured with the CLI against this layout).
        const std::vector<std::pair<std::string, std::string>> gains{
            {"vel.kp", "0.3"}, {"vel.ki", "2"}, {"pos.kp", "4"}};
        auto item = [&](const std::map<std::string, int> &items, const std::string &name) {
            const auto it = items.find(name);
            if (it == items.end()) { fail("layout", "the type has no item " + name); }
            return it->second;
        };
        std::vector<std::string> set{"set"};
        for (const auto &[fb, act] : {std::pair{left_fb, left_act}, std::pair{right_fb, right_act}})
        {
            set.push_back(ref(fb_t, fb, item(fb_items, "actuator")) + "=" + inst_ref(act_t, act));
        }
        for (const auto &[ctl, act, fb] : {std::tuple{left, left_act, left_fb}, std::tuple{right, right_act, right_fb}})
        {
            set.push_back(ref(ctl_t, ctl, item(ctl_items, "actuator")) + "=" + inst_ref(act_t, act));
            set.push_back(ref(ctl_t, ctl, item(ctl_items, "feedback")) + "=" + inst_ref(fb_t, fb));
            set.push_back(ref(ctl_t, ctl, item(ctl_items, "mode")) + "=velocity");
            for (const auto &[name, value] : gains) { set.push_back(ref(ctl_t, ctl, item(ctl_items, name)) + "=" + value); }
        }
        cli(set);
        cli({"save"});
        cli({"reboot"});

        // 2 loops + 7 instances, all running, no pending reboot or invalid layout.
        const size_t expected = 2 + 7;
        const auto deadline = Clock::now() + 30s;
        std::string state;
        for (;;)
        {
            state = readVdev("wait for the layout", [&](drivelink::Client &c) {
                const auto info = c.info();
                const auto instances = c.instances();
                size_t running = 0;
                for (const auto &i : instances) { running += i.state == 0 ? 1 : 0; }
                return std::to_string(info.flags) + " " + std::to_string(instances.size()) + " " +
                       std::to_string(running);
            });
            std::istringstream in(state);
            int flags = -1;
            size_t count = 0, running = 0;
            in >> flags >> count >> running;
            if ((flags & 0x03) == 0 && count == expected && running == expected) { return; }
            if (Clock::now() >= deadline)
            {
                fail("wait for the layout", "after the reboot: info.flags " + std::to_string(flags) + ", " +
                                                std::to_string(count) + " instances, " + std::to_string(running) +
                                                " running (wanted " + std::to_string(expected) + ")");
            }
            std::this_thread::sleep_for(300ms);
        }
    }
};

Harness::Harness() : m_impl(std::make_unique<Impl>()) {}

std::unique_ptr<Harness> Harness::start(bool pty)
{
    std::unique_ptr<Harness> h(new Harness());
    auto &m = *h->m_impl;
    m.vdev_program = requiredEnv("DRIVELINK_VDEV");
    m.agent_program = requiredEnv("DRIVELINK_AGENT");
    m.cli_program = requiredEnv("DRIVELINK_CLI");
    std::string pattern = (fs::temp_directory_path() / "drivelink-hw-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) { throw HarnessError("harness: cannot create a temporary directory"); }
    m.dir = pattern;
    fs::create_directories(m.dir / "state");

    std::vector<std::string> vdev_args{m.vdev_program};
    if (pty) { vdev_args.push_back("--pty"); }
    else { vdev_args.insert(vdev_args.end(), {"--port", "0"}); }
    vdev_args.insert(vdev_args.end(), {"--state", (m.dir / "state").string(), "--scenario-file",
                                       (m.dir / "scenario").string()});
    m.vdev = std::make_unique<Child>("vdev", vdev_args);
    m.vdev_endpoint = m.vdev->waitLine("READY ", 20s).substr(6);
    m.layout();

    m.token = randomToken();
    m.agent = std::make_unique<Child>(
        "agent", std::vector<std::string>{m.agent_program, "--bind", "127.0.0.1", "--port", "0", "--token", m.token,
                                          "--device", std::string(kDevice) + "=" + m.vdev_endpoint, "--retry-ms",
                                          "300", "--no-advertise"});
    m.agent_endpoint = m.agent->waitLine("READY ", 20s).substr(6);
    const auto deadline = Clock::now() + 30s;
    std::string last = "no device list";
    for (;;)
    {
        try
        {
            for (const auto &d : drivelink::listAgentDevices(m.agent_endpoint, m.token, 3000ms, "harness-lobby"))
            {
                if (d.alias == kDevice && d.online) { return h; }
                last = "'" + d.alias + "' is " + (d.online ? "online" : "offline");
            }
        }
        catch (const std::exception &e)
        {
            last = e.what();
        }
        if (Clock::now() >= deadline) { m.fail("wait for the device to be online", last); }
        std::this_thread::sleep_for(200ms);
    }
}

Harness::~Harness()
{
    auto &m = *m_impl;
    m.closeGui();
    if (m.agent) { m.agent->terminate(5s); }
    if (m.vdev) { m.vdev->terminate(5s); }
    m.agent.reset();
    m.vdev.reset();
    std::error_code ec;
    fs::remove_all(m.dir, ec);
}

const std::string &Harness::agentEndpoint() const { return m_impl->agent_endpoint; }
const std::string &Harness::token() const { return m_impl->token; }

void Harness::vdevScenario(unsigned sequence, const std::string &action)
{
    const fs::path file = m_impl->dir / "scenario";
    const fs::path temp = m_impl->dir / "scenario.tmp";
    {
        std::ofstream out(temp);
        out << sequence << ' ' << action << '\n';
        if (!out) { m_impl->fail("vdev scenario", "cannot write " + temp.string()); }
    }
    fs::rename(temp, file);
    // The vdev polls the file every 100 ms and prints "SCENARIO <sequence> <action>" when it
    // runs the line: wait for it, so the action has run when this returns.
    try
    {
        m_impl->vdev->waitLine("SCENARIO " + std::to_string(sequence) + " ", 5s);
    }
    catch (const HarnessError &e)
    {
        m_impl->fail("vdev scenario '" + action + "'", std::string("the vdev did not run it: ") + e.what());
    }
}

std::string Harness::diag() const { return m_impl->diag(); }

namespace
{
// ros2_control checks each <joint> against a URDF joint (and the URDF parser a link).
std::string wheelJoint(const std::string &name)
{
    return "<link name=\"" + name + "_link\"/><joint name=\"" + name +
           "\" type=\"continuous\"><parent link=\"base_link\"/><child link=\"" + name +
           "_link\"/><axis xyz=\"0 1 0\"/></joint>";
}
} // namespace

std::string Harness::urdf(const std::string &component, const std::string &extra_hardware_params) const
{
    return urdf(component, extra_hardware_params,
                {JointSpec{"left_wheel", "left", {"position", "velocity"}, {}, {}},
                 JointSpec{"right_wheel", "right", {"position", "velocity"}, {}, {}}});
}

std::string Harness::urdf(const std::string &component, const std::string &extra_hardware_params,
                          const std::vector<JointSpec> &joints, bool imu) const
{
    const auto &m = *m_impl;
    std::ostringstream out;
    out << "<?xml version=\"1.0\"?>\n<robot name=\"test\"><link name=\"base_link\"/>";
    for (const auto &joint : joints) { out << wheelJoint(joint.name); }
    out << "<ros2_control name=\"" << component << "\" type=\"system\">"
        << "<hardware><plugin>drivelink_hardware/DriveLinkSystem</plugin>"
        << "<param name=\"agent\">" << m.agent_endpoint << "</param>"
        << "<param name=\"token\">" << m.token << "</param>"
        << "<param name=\"device\">" << kDevice << "</param>" << extra_hardware_params << "</hardware>\n";
    for (const auto &joint : joints)
    {
        out << "<joint name=\"" << joint.name << "\"><param name=\"instance\">" << joint.instance << "</param>"
            << joint.extra_params;
        for (const auto &name : joint.state) { out << "<state_interface name=\"" << name << "\"/>"; }
        for (const auto &name : joint.command) { out << "<command_interface name=\"" << name << "\"/>"; }
        out << "</joint>\n";
    }
    if (imu)
    {
        out << "<sensor name=\"imu\"><param name=\"instance\">imu</param>";
        for (const char *name : {"orientation.x", "orientation.y", "orientation.z", "orientation.w", "angular_velocity.x",
                                 "angular_velocity.y", "angular_velocity.z", "linear_acceleration.x",
                                 "linear_acceleration.y", "linear_acceleration.z"})
        {
            out << "<state_interface name=\"" << name << "\"/>";
        }
        out << "</sensor>";
    }
    out << "</ros2_control></robot>\n";
    return out.str();
}

drivelink::Session &Harness::gui()
{
    auto &m = *m_impl;
    if (!m.gui)
    {
        drivelink::SessionOptions options;
        options.timeout = 5000ms;
        options.agent = drivelink::AgentHello{m.token, kDevice, "harness-gui", 1};
        try
        {
            m.gui = std::make_unique<drivelink::Session>(m.agent_endpoint, options);
            // From here on every status-stream frame updates the watch.
            m.gui_observer = m.gui->observe([watch = m.watch](const drivelink::Frame &frame) {
                if (frame.type != drivelink::protocol::defs::msg::kStatusStream) { return; }
                try
                {
                    const auto event = drivelink::Client::decodeEvent(frame);
                    const auto *status = std::get_if<drivelink::StatusFrame>(&event);
                    if (status == nullptr) { return; }
                    {
                        std::lock_guard lock(watch->mutex);
                        // The reason at an armed -> not armed transition (a later
                        // DISARM of a disarmed device overwrites the status's reason).
                        if (watch->seen && watch->status.arm_state == 1 && status->arm_state != 1)
                        {
                            watch->disarms.push_back(status->last_disarm_reason);
                        }
                        watch->status = ArmStatus{status->arm_state, status->last_disarm_reason, status->flags};
                        watch->seen = true;
                        ++watch->seq;
                    }
                    watch->cv.notify_all();
                }
                catch (...)
                {
                }
            });
        }
        catch (const std::exception &e)
        {
            m.gui.reset();
            m.fail("open the GUI client", e.what());
        }
    }
    return *m.gui;
}

namespace
{
// An instance's item by label and name, read from the device (never literal ids).
struct FoundItem
{
    drivelink::Ref ref;
    drivelink::ItemDescriptor descriptor;
    uint8_t type = 0;
};

FoundItem findItem(drivelink::Client &client, const std::string &label, const std::string &item)
{
    for (const auto &instance : client.instances())
    {
        if (instance.label != label) { continue; }
        for (const auto &descriptor : client.items(instance.type))
        {
            if (descriptor.name == item)
            {
                return {drivelink::Ref{instance.type, instance.instance, descriptor.id}, descriptor, instance.type};
            }
        }
        throw std::runtime_error("the instance '" + label + "' has no item '" + item + "'");
    }
    throw std::runtime_error("the device has no instance labelled '" + label + "'");
}

uint32_t readBits(drivelink::Client &client, const FoundItem &found)
{
    const auto result = client.get(std::span<const drivelink::Ref>(&found.ref, 1));
    if (result.result != 0 || result.values.size() != 1)
    {
        throw std::runtime_error("GET of '" + found.descriptor.name + "' was refused (result " +
                                 std::to_string(int(result.result)) + ")");
    }
    return result.values[0];
}
} // namespace

Harness::ArmStatus Harness::armStatus(std::chrono::milliseconds wait)
{
    gui();
    auto &m = *m_impl;
    const auto watch = m.watch;
    std::unique_lock lock(watch->mutex);
    if (!watch->cv.wait_for(lock, wait, [&] { return watch->seen; }))
    {
        lock.unlock();
        m.fail("armStatus", "the GUI client saw no status frame within " + std::to_string(wait.count()) + " ms");
    }
    return watch->status;
}

std::vector<uint8_t> Harness::disarmReasons()
{
    gui();
    std::lock_guard lock(m_impl->watch->mutex);
    return m_impl->watch->disarms;
}

Harness::ArmStatus Harness::waitArm(bool armed, std::chrono::milliseconds timeout)
{
    gui();
    auto &m = *m_impl;
    const auto watch = m.watch;
    std::unique_lock lock(watch->mutex);
    const uint64_t first = watch->seq;
    // "Not armed" is any arm_state other than Armed (1): Disarmed 0 or Fault 2.
    const auto matches = [&] { return watch->seq > first && (watch->status.arm_state == 1) == armed; };
    if (!watch->cv.wait_for(lock, timeout, matches))
    {
        const ArmStatus last = watch->status;
        const bool seen = watch->seen;
        lock.unlock();
        m.fail(armed ? "waitArm(armed)" : "waitArm(disarmed)",
               std::string("no status frame showing ") + (armed ? "armed" : "not armed") + " within " +
                   std::to_string(timeout.count()) + " ms; the last frame " +
                   (seen ? "had arm_state " + std::to_string(int(last.arm_state)) + ", last_disarm_reason " +
                               std::to_string(int(last.last_disarm_reason))
                         : std::string("never came")));
    }
    return watch->status;
}

double Harness::param(const std::string &label, const std::string &item)
{
    auto &m = *m_impl;
    try
    {
        drivelink::Client client(gui());
        const FoundItem found = findItem(client, label, item);
        const uint32_t bits = readBits(client, found);
        switch (found.descriptor.value_type)
        {
        case drivelink::ValueType::F32: return double(std::bit_cast<float>(bits));
        case drivelink::ValueType::I32: return double(int32_t(bits));
        default: return double(bits);
        }
    }
    catch (const std::exception &e)
    {
        m.fail("param " + label + "." + item, e.what());
    }
}

std::string Harness::enumParam(const std::string &label, const std::string &item)
{
    auto &m = *m_impl;
    try
    {
        drivelink::Client client(gui());
        const FoundItem found = findItem(client, label, item);
        if (found.descriptor.value_type != drivelink::ValueType::Enum)
        {
            throw std::runtime_error("'" + item + "' is not an Enum param");
        }
        const uint32_t bits = readBits(client, found);
        for (const auto &entry : client.enums(found.type, found.ref.item))
        {
            if (entry.value == bits) { return entry.name; }
        }
        throw std::runtime_error("the value " + std::to_string(bits) + " has no entry name");
    }
    catch (const std::exception &e)
    {
        m.fail("enumParam " + label + "." + item, e.what());
    }
}

void Harness::action(const std::string &label, const std::string &item, std::array<uint32_t, 4> args)
{
    auto &m = *m_impl;
    try
    {
        drivelink::Client client(gui());
        const FoundItem found = findItem(client, label, item);
        client.action(found.ref, args);
    }
    catch (const std::exception &e)
    {
        m.fail("action " + label + "." + item, e.what());
    }
}

void Harness::disarm()
{
    auto &m = *m_impl;
    try
    {
        drivelink::Client client(gui());
        client.disarm();
    }
    catch (const std::exception &e)
    {
        m.fail("disarm", e.what());
    }
}

void Harness::acknowledgeFault()
{
    auto &m = *m_impl;
    try
    {
        drivelink::Client client(gui());
        client.acknowledgeFault();
    }
    catch (const std::exception &e)
    {
        m.fail("acknowledgeFault", e.what());
    }
}

void Harness::closeGui() { m_impl->closeGui(); }

void Harness::stopAgent()
{
    auto &m = *m_impl;
    if (!m.agent) { return; }
    if (!m.agent->terminate(10s)) { m.fail("stop the agent", "it did not exit on SIGTERM within 10 s (killed)"); }
}
} // namespace drivelink_hardware::test
