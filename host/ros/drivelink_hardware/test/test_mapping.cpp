#include "drivelink_hardware/mapping.hpp"
#include <gtest/gtest.h>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace drivelink_hardware;
using drivelink::ItemKind;
using drivelink::ValueType;

namespace
{
// Runs fn, expects a MappingError whose message contains every needle.
template <typename Fn>
void expectRefused(Fn &&fn, const std::vector<std::string> &needles)
{
    try
    {
        fn();
        FAIL() << "expected a MappingError";
    }
    catch (const MappingError &e)
    {
        const std::string message = e.what();
        for (const auto &needle : needles)
            EXPECT_NE(message.find(needle), std::string::npos) << "'" << needle << "' missing from: " << message;
        EXPECT_EQ(message.find('\n'), std::string::npos) << "not a single line: " << message;
    }
}

ConnectionConfig parseOk(Params params)
{
    return parseConnection(params);
}

// ---- fixtures ----------------------------------------------------------

struct Item
{
    uint16_t id;
    std::string name;
    ItemKind kind;
    ValueType type;
};

void addType(drivelink::DeviceSnapshot &snap, uint8_t id, const char *name, const std::vector<Item> &items)
{
    drivelink::ModelType type;
    type.descriptor.type = id;
    type.descriptor.name = name;
    for (const auto &item : items)
    {
        drivelink::ModelItem model;
        model.descriptor.id = item.id;
        model.descriptor.name = item.name;
        model.descriptor.kind = item.kind;
        model.descriptor.value_type = item.type;
        type.items[item.id] = std::move(model);
    }
    snap.types[id] = std::move(type);
}

void addInstance(drivelink::DeviceSnapshot &snap, uint8_t type, uint8_t instance, const char *label, uint8_t loop,
                 uint8_t state = 0)
{
    drivelink::ModelInstance model;
    model.instance.type = type;
    model.instance.instance = instance;
    model.instance.label = label;
    model.instance.loop = loop;
    model.instance.state = state;
    snap.configured[{type, instance}] = std::move(model);
}

void addLoop(drivelink::DeviceSnapshot &snap, uint8_t id, uint32_t rate)
{
    drivelink::LoopInfo loop;
    loop.id = id;
    loop.name = "loop" + std::to_string(id);
    loop.rate_hz = rate;
    snap.info.loops.push_back(loop);
}

constexpr uint8_t kPid = 1;
constexpr uint8_t kImu = 2;

// A device with two controllers on loop 0 (1000 Hz) and an IMU on loop 1 (100 Hz).
drivelink::DeviceSnapshot makeDevice()
{
    drivelink::DeviceSnapshot snap;
    snap.ready = true;
    addType(snap, kPid, "controller.cascade_pid",
            {
                {0, "mode", ItemKind::Param, ValueType::Enum},
                {1, "velocity_sp", ItemKind::Input, ValueType::F32},
                {2, "position", ItemKind::Signal, ValueType::F32},
                {3, "velocity", ItemKind::Signal, ValueType::F32},
                {4, "duty", ItemKind::Signal, ValueType::F32},
                {5, "output_position", ItemKind::Signal, ValueType::F32},
                {6, "saturated", ItemKind::Signal, ValueType::Bool},
                {7, "ticks", ItemKind::Signal, ValueType::I32},
                {8, "level", ItemKind::Signal, ValueType::U8},
                {9, "big", ItemKind::Signal, ValueType::U32},
                {10, "phase", ItemKind::Signal, ValueType::Enum},
                {11, "blob", ItemKind::Signal, ValueType::None},
                {12, "mid", ItemKind::Signal, ValueType::U16},
                {13, "reset", ItemKind::Action, ValueType::None},
            });
    addType(snap, kImu, "imu.mock",
            {
                {0, "quaternion.w", ItemKind::Signal, ValueType::F32},
                {1, "quaternion.x", ItemKind::Signal, ValueType::F32},
                {2, "quaternion.y", ItemKind::Signal, ValueType::F32},
                {3, "quaternion.z", ItemKind::Signal, ValueType::F32},
                {4, "gyro.x", ItemKind::Signal, ValueType::F32},
                {5, "gyro.y", ItemKind::Signal, ValueType::F32},
                {6, "gyro.z", ItemKind::Signal, ValueType::F32},
                {7, "accel.x", ItemKind::Signal, ValueType::F32},
                {8, "accel.y", ItemKind::Signal, ValueType::F32},
                {9, "accel.z", ItemKind::Signal, ValueType::F32},
                {10, "calibrated", ItemKind::Signal, ValueType::Bool},
            });
    addLoop(snap, 0, 1000);
    addLoop(snap, 1, 100);
    addInstance(snap, kPid, 0, "left", 0);
    addInstance(snap, kPid, 1, "right", 0);
    addInstance(snap, kImu, 0, "imu", 1);
    return snap;
}

ComponentSpec joint(const char *name, const char *instance, std::vector<std::string> states)
{
    ComponentSpec spec;
    spec.name = name;
    spec.params["instance"] = instance;
    spec.state_interfaces = std::move(states);
    return spec;
}

ComponentSpec sensor(const char *name, const char *instance, std::vector<std::string> states)
{
    ComponentSpec spec = joint(name, instance, std::move(states));
    spec.sensor = true;
    return spec;
}

ConnectionConfig config(double rate = 100)
{
    ConnectionConfig c;
    c.device = "robot";
    c.state_rate_hz = rate;
    return c;
}

drivelink::Ref ref(uint8_t type, uint8_t instance, uint16_t item)
{
    return drivelink::Ref{type, instance, item};
}

// A scratch file that removes itself.
struct TempFile
{
    std::filesystem::path path;
    explicit TempFile(const std::string &name, const std::string &content, bool create = true)
        : path(std::filesystem::temp_directory_path() / ("drivelink_test_mapping_" + name))
    {
        if (create)
        {
            std::ofstream out(path, std::ios::binary);
            out << content;
        }
    }
    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};
} // namespace

// ---- parseConnection ---------------------------------------------------

TEST(ParseConnection, DefaultsWithDeviceOnly)
{
    const auto c = parseOk({{"device", "robot"}});
    EXPECT_EQ(c.agent, "tcp://127.0.0.1:47620");
    EXPECT_EQ(c.token, "");
    EXPECT_EQ(c.token_file, "/etc/drivelink/token");
    EXPECT_EQ(c.device, "robot");
    EXPECT_EQ(c.take_lease, TakeLease::Refuse);
    EXPECT_EQ(c.state_rate_hz, 100);
    EXPECT_FALSE(c.stale_timeout_ms.has_value());
    EXPECT_EQ(c.timeout, std::chrono::milliseconds(3000));
}

TEST(ParseConnection, AllParametersAccepted)
{
    const auto c = parseOk({{"agent", "tcp://robot.local:5000"},
                            {"token", "abc"},
                            {"token_file", "/tmp/t"},
                            {"device", "0123456789ab"},
                            {"take_lease", "preempt"},
                            {"state_rate_hz", "2.5"},
                            {"stale_timeout_ms", "250"},
                            {"timeout_ms", "500"}});
    EXPECT_EQ(c.agent, "tcp://robot.local:5000");
    EXPECT_EQ(c.token, "abc");
    EXPECT_EQ(c.token_file, "/tmp/t");
    EXPECT_EQ(c.device, "0123456789ab");
    EXPECT_EQ(c.take_lease, TakeLease::Preempt);
    EXPECT_EQ(c.state_rate_hz, 2.5);
    ASSERT_TRUE(c.stale_timeout_ms.has_value());
    EXPECT_EQ(*c.stale_timeout_ms, 250);
    EXPECT_EQ(c.timeout, std::chrono::milliseconds(500));
}

TEST(ParseConnection, MissingOrEmptyDeviceRefused)
{
    expectRefused([] { parseOk({}); }, {"device", "required"});
    expectRefused([] { parseOk({{"agent", "tcp://h:1"}}); }, {"device", "required"});
    expectRefused([] { parseOk({{"device", ""}}); }, {"device"});
}

TEST(ParseConnection, UnknownParameterRefused)
{
    expectRefused([] { parseOk({{"device", "robot"}, {"state_rate", "100"}}); }, {"state_rate", "unknown"});
    expectRefused([] { parseOk({{"device", "robot"}, {"Device", "x"}}); }, {"Device"});
}

TEST(ParseConnection, AgentMustBeTcp)
{
    expectRefused([] { parseOk({{"device", "r"}, {"agent", "serial:///dev/ttyUSB0"}}); }, {"agent", "serial:///dev/ttyUSB0"});
    expectRefused([] { parseOk({{"device", "r"}, {"agent", "127.0.0.1:47620"}}); }, {"agent", "tcp://"});
    expectRefused([] { parseOk({{"device", "r"}, {"agent", ""}}); }, {"agent"});
    expectRefused([] { parseOk({{"device", "r"}, {"agent", "tcp://"}}); }, {"agent"});
    EXPECT_EQ(parseOk({{"device", "r"}, {"agent", "tcp://host-name:1"}}).agent, "tcp://host-name:1");
}

TEST(ParseConnection, TakeLeaseExactlyRefuseOrPreempt)
{
    EXPECT_EQ(parseOk({{"device", "r"}, {"take_lease", "refuse"}}).take_lease, TakeLease::Refuse);
    EXPECT_EQ(parseOk({{"device", "r"}, {"take_lease", "preempt"}}).take_lease, TakeLease::Preempt);
    for (const char *bad : {"", "Preempt", "true", "take", " preempt", "refuse "})
        expectRefused([&] { parseOk({{"device", "r"}, {"take_lease", bad}}); }, {"take_lease", "'refuse' or 'preempt'"});
}

TEST(ParseConnection, StateRateRange)
{
    EXPECT_EQ(parseOk({{"device", "r"}, {"state_rate_hz", "100"}}).state_rate_hz, 100);
    EXPECT_EQ(parseOk({{"device", "r"}, {"state_rate_hz", "10000"}}).state_rate_hz, 10000);
    EXPECT_EQ(parseOk({{"device", "r"}, {"state_rate_hz", "1"}}).state_rate_hz, 1);
    for (const char *bad : {"0", "0.5", "-1", "10000.5", "100hz", "", "nan", "inf", "-inf", "1e400", " 100", "100 ", "abc", "1,5"})
        expectRefused([&] { parseOk({{"device", "r"}, {"state_rate_hz", bad}}); }, {"state_rate_hz", bad});
}

TEST(ParseConnection, StaleTimeout)
{
    EXPECT_FALSE(parseOk({{"device", "r"}, {"stale_timeout_ms", "auto"}}).stale_timeout_ms.has_value());
    const auto off = parseOk({{"device", "r"}, {"stale_timeout_ms", "0"}});
    ASSERT_TRUE(off.stale_timeout_ms.has_value());
    EXPECT_EQ(*off.stale_timeout_ms, 0);
    EXPECT_EQ(*parseOk({{"device", "r"}, {"stale_timeout_ms", "2.5"}}).stale_timeout_ms, 2.5);
    EXPECT_EQ(*parseOk({{"device", "r"}, {"stale_timeout_ms", "60000"}}).stale_timeout_ms, 60000);
    for (const char *bad : {"-1", "60001", "", "Auto", "50ms", "nan", "inf", "never"})
        expectRefused([&] { parseOk({{"device", "r"}, {"stale_timeout_ms", bad}}); }, {"stale_timeout_ms", bad});
}

TEST(ParseConnection, TimeoutMs)
{
    EXPECT_EQ(parseOk({{"device", "r"}, {"timeout_ms", "100"}}).timeout, std::chrono::milliseconds(100));
    EXPECT_EQ(parseOk({{"device", "r"}, {"timeout_ms", "60000"}}).timeout, std::chrono::milliseconds(60000));
    for (const char *bad : {"99", "60001", "0", "-5", "", "1000.0", "2e3", "1s", "abc", "99999999999999999999"})
        expectRefused([&] { parseOk({{"device", "r"}, {"timeout_ms", bad}}); }, {"timeout_ms", bad});
}

TEST(ParseConnection, TokenNeverInMessages)
{
    expectRefused(
        [] { parseOk({{"device", "r"}, {"token", "s3cretvalue"}, {"state_rate_hz", "bad"}}); }, {"state_rate_hz"});
    try
    {
        parseOk({{"device", "r"}, {"token", "s3cretvalue"}, {"bogus", "1"}});
        FAIL();
    }
    catch (const MappingError &e)
    {
        EXPECT_EQ(std::string(e.what()).find("s3cretvalue"), std::string::npos);
    }
    try
    {
        parseOk({{"token", "s3cretvalue"}});
        FAIL();
    }
    catch (const MappingError &e)
    {
        EXPECT_EQ(std::string(e.what()).find("s3cretvalue"), std::string::npos);
    }
}

// ---- readToken ---------------------------------------------------------

TEST(ReadToken, InlineWinsOverFile)
{
    TempFile file("inline_wins", "fromfile\n");
    ConnectionConfig c = config();
    c.token = "inline";
    c.token_file = file.path.string();
    EXPECT_EQ(readToken(c), "inline");
}

TEST(ReadToken, InlineWinsEvenWhenFileMissing)
{
    ConnectionConfig c = config();
    c.token = "inline";
    c.token_file = (std::filesystem::temp_directory_path() / "drivelink_test_mapping_no_such_file").string();
    EXPECT_EQ(readToken(c), "inline");
}

TEST(ReadToken, FileFirstLineTrailingWhitespaceStripped)
{
    for (const auto &[name, content] : std::vector<std::pair<std::string, std::string>>{
             {"plain", "abc123"},
             {"lf", "abc123\n"},
             {"crlf", "abc123\r\n"},
             {"spaces", "abc123 \t \r\n"},
             {"two_lines", "abc123\nsecond line\n"},
             {"two_lines_crlf", "abc123\r\nsecond\r\n"}})
    {
        TempFile file("strip_" + name, content);
        ConnectionConfig c = config();
        c.token_file = file.path.string();
        EXPECT_EQ(readToken(c), "abc123") << name;
    }
}

TEST(ReadToken, LeadingWhitespaceKept)
{
    TempFile file("leading", "  abc\n");
    ConnectionConfig c = config();
    c.token_file = file.path.string();
    EXPECT_EQ(readToken(c), "  abc");
}

TEST(ReadToken, MissingFileNamesPathAndGroup)
{
    TempFile file("missing", "", false);
    ConnectionConfig c = config();
    c.token_file = file.path.string();
    expectRefused([&] { readToken(c); }, {file.path.string(), "drivelink", "group", "token"});
}

TEST(ReadToken, EmptyFileRefused)
{
    for (const auto &[name, content] : std::vector<std::pair<std::string, std::string>>{
             {"empty", ""}, {"newline", "\n"}, {"blank", "  \t\r\n"}, {"blank_then_token", "\ntoken\n"}})
    {
        TempFile file("empty_" + name, content);
        ConnectionConfig c = config();
        c.token_file = file.path.string();
        expectRefused([&] { readToken(c); }, {file.path.string(), "no token"});
    }
}

// ---- checkDevice -------------------------------------------------------

TEST(CheckDevice, ReadyDeviceAccepted)
{
    EXPECT_NO_THROW(checkDevice(makeDevice()));
}

TEST(CheckDevice, NotReadyRefused)
{
    auto snap = makeDevice();
    snap.ready = false;
    expectRefused([&] { checkDevice(snap); }, {"not ready"});
}

TEST(CheckDevice, SafeModeRefused)
{
    auto snap = makeDevice();
    snap.info.flags = 0x08;
    ASSERT_TRUE(snap.safeMode());
    expectRefused([&] { checkDevice(snap); }, {"safe mode"});
}

TEST(CheckDevice, PendingRebootRefused)
{
    auto snap = makeDevice();
    snap.info.flags = 0x01;
    expectRefused([&] { checkDevice(snap); }, {"pending", "reboot"});
}

TEST(CheckDevice, InvalidLayoutRefused)
{
    auto snap = makeDevice();
    snap.info.flags = 0x02;
    expectRefused([&] { checkDevice(snap); }, {"layout", "invalid", "Layout page"});
}

TEST(CheckDevice, UnrelatedFlagBitsIgnored)
{
    auto snap = makeDevice();
    snap.info.flags = 0x04 | 0x10 | 0x80;
    EXPECT_NO_THROW(checkDevice(snap));
}

TEST(CheckDevice, EachRefusalHasItsOwnMessage)
{
    auto message = [](drivelink::DeviceSnapshot snap) {
        try
        {
            checkDevice(snap);
        }
        catch (const MappingError &e)
        {
            return std::string(e.what());
        }
        return std::string();
    };
    auto a = makeDevice();
    a.ready = false;
    auto b = makeDevice();
    b.info.flags = 0x08;
    auto c = makeDevice();
    c.info.flags = 0x01;
    auto d = makeDevice();
    d.info.flags = 0x02;
    const std::vector<std::string> messages = {message(a), message(b), message(c), message(d)};
    for (size_t i = 0; i < messages.size(); ++i)
    {
        EXPECT_FALSE(messages[i].empty());
        for (size_t j = i + 1; j < messages.size(); ++j)
            EXPECT_NE(messages[i], messages[j]);
    }
}

// ---- resolve: conventions ----------------------------------------------

TEST(Resolve, JointConventionIsIdentity)
{
    const auto r = resolve(makeDevice(), {joint("left_wheel", "left", {"position", "velocity", "duty"})}, config());
    ASSERT_EQ(r.states.size(), 3u);
    EXPECT_EQ(r.states[0].component, "left_wheel");
    EXPECT_EQ(r.states[0].interface_name, "position");
    EXPECT_EQ(r.states[0].ref, ref(kPid, 0, 2));
    EXPECT_EQ(r.states[1].interface_name, "velocity");
    EXPECT_EQ(r.states[1].ref, ref(kPid, 0, 3));
    EXPECT_EQ(r.states[2].interface_name, "duty");
    EXPECT_EQ(r.states[2].ref, ref(kPid, 0, 4));
    for (const auto &s : r.states)
    {
        EXPECT_EQ(s.type, ValueType::F32);
        EXPECT_EQ(s.loop, 0);
        EXPECT_EQ(s.group, 0u);
    }
    EXPECT_EQ(r.states[0].slot, 0u);
    EXPECT_EQ(r.states[1].slot, 1u);
    EXPECT_EQ(r.states[2].slot, 2u);
    ASSERT_EQ(r.groups.size(), 1u);
    EXPECT_EQ(r.groups[0].loop, 0);
    EXPECT_EQ(r.groups[0].refs, (std::vector<drivelink::Ref>{ref(kPid, 0, 2), ref(kPid, 0, 3), ref(kPid, 0, 4)}));
    EXPECT_EQ(r.groups[0].types.size(), 3u);
}

TEST(Resolve, JointOtherNameMapsToItself)
{
    const auto r = resolve(makeDevice(), {joint("j", "left", {"saturated", "ticks", "level"})}, config());
    ASSERT_EQ(r.states.size(), 3u);
    EXPECT_EQ(r.states[0].ref, ref(kPid, 0, 6));
    EXPECT_EQ(r.states[0].type, ValueType::Bool);
    EXPECT_EQ(r.states[1].type, ValueType::I32);
    EXPECT_EQ(r.states[2].type, ValueType::U8);
}

TEST(Resolve, JointDoesNotApplySensorConvention)
{
    // A joint named orientation.x gets no IMU mapping: the device has no such item.
    expectRefused([] { resolve(makeDevice(), {joint("j", "imu", {"orientation.x"})}, config()); },
                  {"joint", "'j'", "orientation.x"});
}

TEST(Resolve, SensorImuConvention)
{
    const auto r = resolve(makeDevice(),
                           {sensor("imu_sensor", "imu",
                                   {"orientation.w", "orientation.x", "orientation.y", "orientation.z",
                                    "angular_velocity.x", "angular_velocity.y", "angular_velocity.z",
                                    "linear_acceleration.x", "linear_acceleration.y", "linear_acceleration.z"})},
                           config());
    ASSERT_EQ(r.states.size(), 10u);
    const std::vector<uint16_t> items = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}; // quaternion.wxyz, gyro.xyz, accel.xyz
    for (size_t i = 0; i < items.size(); ++i)
    {
        EXPECT_EQ(r.states[i].ref, ref(kImu, 0, items[i])) << r.states[i].interface_name;
        EXPECT_EQ(r.states[i].loop, 1);
        EXPECT_EQ(r.states[i].slot, i);
    }
    EXPECT_EQ(r.states[0].interface_name, "orientation.w");
    ASSERT_EQ(r.groups.size(), 1u);
    EXPECT_EQ(r.groups[0].loop, 1);
}

TEST(Resolve, SensorOtherNameMapsToItself)
{
    const auto r = resolve(makeDevice(), {sensor("s", "imu", {"calibrated", "quaternion.w", "gyro.x"})}, config());
    ASSERT_EQ(r.states.size(), 3u);
    EXPECT_EQ(r.states[0].ref, ref(kImu, 0, 10));
    EXPECT_EQ(r.states[1].ref, ref(kImu, 0, 0));
    EXPECT_EQ(r.states[2].ref, ref(kImu, 0, 4));
}

TEST(Resolve, SensorConventionOnlyForKnownSuffixes)
{
    // orientation.v is not w/x/y/z: mapped to itself, which the device lacks.
    expectRefused([] { resolve(makeDevice(), {sensor("s", "imu", {"orientation.v"})}, config()); },
                  {"orientation.v", "no such item"});
    expectRefused([] { resolve(makeDevice(), {sensor("s", "imu", {"angular_velocity.w"})}, config()); },
                  {"angular_velocity.w"});
}

TEST(Resolve, StateOverrideSelectsAnotherSignal)
{
    auto spec = joint("j", "left", {"position", "velocity"});
    spec.params["state.position"] = "output_position";
    const auto r = resolve(makeDevice(), {spec}, config());
    ASSERT_EQ(r.states.size(), 2u);
    EXPECT_EQ(r.states[0].interface_name, "position");
    EXPECT_EQ(r.states[0].ref, ref(kPid, 0, 5));
    EXPECT_EQ(r.states[1].ref, ref(kPid, 0, 3));
}

TEST(Resolve, StateOverrideOnSensor)
{
    auto spec = sensor("s", "imu", {"orientation.w", "orientation.x"});
    spec.params["state.orientation.x"] = "gyro.z";
    const auto r = resolve(makeDevice(), {spec}, config());
    EXPECT_EQ(r.states[0].ref, ref(kImu, 0, 0));
    EXPECT_EQ(r.states[1].ref, ref(kImu, 0, 6));
}

TEST(Resolve, OverrideForUndeclaredInterfaceRefused)
{
    auto spec = joint("j", "left", {"position"});
    spec.params["state.velocity"] = "velocity";
    expectRefused([&] { resolve(makeDevice(), {spec}, config()); }, {"joint", "'j'", "state.velocity", "does not declare"});
}

TEST(Resolve, OverrideToMissingItemNamesParamAndSignals)
{
    auto spec = joint("j", "left", {"position"});
    spec.params["state.position"] = "nonexistent";
    expectRefused([&] { resolve(makeDevice(), {spec}, config()); },
                  {"'j'", "'position'", "state.position", "nonexistent", "available signals", "duty", "velocity"});
}

TEST(Resolve, EmptyOverrideRefused)
{
    auto spec = joint("j", "left", {"position"});
    spec.params["state.position"] = "";
    expectRefused([&] { resolve(makeDevice(), {spec}, config()); }, {"state.position", "empty"});
}

TEST(Resolve, UnknownParamRefused)
{
    auto spec = joint("j", "left", {"position"});
    spec.params["gain"] = "1";
    expectRefused([&] { resolve(makeDevice(), {spec}, config()); }, {"joint", "'j'", "'gain'", "unknown"});
}

// ---- resolve: instance lookup ------------------------------------------

TEST(Resolve, MissingInstanceParamRefused)
{
    ComponentSpec spec;
    spec.name = "j";
    spec.state_interfaces = {"position"};
    expectRefused([&] { resolve(makeDevice(), {spec}, config()); }, {"'j'", "'instance'", "required"});
}

TEST(Resolve, EmptyInstanceParamRefused)
{
    expectRefused([] { resolve(makeDevice(), {joint("j", "", {"position"})}, config()); }, {"'j'", "'instance'"});
}

TEST(Resolve, UnknownLabelListsAvailableSorted)
{
    expectRefused([] { resolve(makeDevice(), {joint("front", "middle", {"position"})}, config()); },
                  {"joint", "'front'", "'instance'", "'middle'", "available labels: imu, left, right"});
    // Labels are matched exactly.
    expectRefused([] { resolve(makeDevice(), {joint("j", "Left", {"position"})}, config()); },
                  {"'Left'", "imu, left, right"});
    expectRefused([] { resolve(makeDevice(), {joint("j", "left ", {"position"})}, config()); }, {"'left '"});
}

TEST(Resolve, NoConfiguredInstances)
{
    auto snap = makeDevice();
    snap.configured.clear();
    expectRefused([&] { resolve(snap, {joint("j", "left", {"position"})}, config()); }, {"'left'", "(none)"});
}

TEST(Resolve, AmbiguousLabelRefused)
{
    auto snap = makeDevice();
    addInstance(snap, kPid, 2, "left", 0); // second instance labelled "left"
    expectRefused([&] { resolve(snap, {joint("j", "left", {"position"})}, config()); },
                  {"joint", "'j'", "'left'", "2 configured instances"});
}

TEST(Resolve, AmbiguousAcrossTypesRefused)
{
    auto snap = makeDevice();
    addInstance(snap, kImu, 1, "left", 1);
    expectRefused([&] { resolve(snap, {joint("j", "left", {"position"})}, config()); }, {"'left'", "2 configured"});
}

TEST(Resolve, FailedInstanceRefused)
{
    auto snap = makeDevice();
    addInstance(snap, kPid, 1, "right", 0, 1);
    expectRefused([&] { resolve(snap, {joint("rj", "right", {"position"})}, config()); },
                  {"joint", "'rj'", "'right'", "not running", "state 1"});
}

TEST(Resolve, InvalidInstanceStateRefused)
{
    auto snap = makeDevice();
    addInstance(snap, kPid, 1, "right", 0, 2);
    expectRefused([&] { resolve(snap, {joint("rj", "right", {"position"})}, config()); }, {"not running", "state 2"});
}

TEST(Resolve, UnknownTypeRefused)
{
    auto snap = makeDevice();
    addInstance(snap, 9, 0, "ghost", 0);
    expectRefused([&] { resolve(snap, {joint("j", "ghost", {"position"})}, config()); }, {"'ghost'", "type id 9"});
}

// ---- resolve: item checks ----------------------------------------------

TEST(Resolve, MissingItemRefusedWithSignalsListed)
{
    expectRefused([] { resolve(makeDevice(), {joint("j", "left", {"torque"})}, config()); },
                  {"joint", "'j'", "'torque'", "'left'", "controller.cascade_pid", "available signals: big, blob, duty"});
}

TEST(Resolve, ParamIsNotASignal)
{
    expectRefused([] { resolve(makeDevice(), {joint("j", "left", {"mode"})}, config()); },
                  {"'j'", "'mode'", "param", "not a signal"});
}

TEST(Resolve, InputIsNotASignal)
{
    expectRefused([] { resolve(makeDevice(), {joint("j", "left", {"velocity_sp"})}, config()); },
                  {"'velocity_sp'", "input", "not a signal", "f32"});
}

TEST(Resolve, ActionIsNotASignal)
{
    expectRefused([] { resolve(makeDevice(), {joint("j", "left", {"reset"})}, config()); }, {"'reset'", "action"});
}

TEST(Resolve, SignalWithNoValueTypeRefused)
{
    expectRefused([] { resolve(makeDevice(), {joint("j", "left", {"blob"})}, config()); },
                  {"'j'", "'blob'", "value type none"});
}

TEST(Resolve, EveryScalarTypeAccepted)
{
    const auto r = resolve(makeDevice(),
                           {joint("j", "left", {"saturated", "level", "mid", "big", "ticks", "position", "phase"})},
                           config());
    const std::vector<ValueType> expected = {ValueType::Bool, ValueType::U8, ValueType::U16, ValueType::U32,
                                             ValueType::I32,  ValueType::F32, ValueType::Enum};
    ASSERT_EQ(r.states.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(r.states[i].type, expected[i]);
        EXPECT_EQ(r.groups[0].types[i], expected[i]);
    }
}

// ---- resolve: component structure --------------------------------------

TEST(Resolve, NoStateInterfacesRefused)
{
    expectRefused([] { resolve(makeDevice(), {joint("j", "left", {})}, config()); }, {"'j'", "no state interface"});
}

TEST(Resolve, DuplicateComponentNamesRefused)
{
    expectRefused([] {
        resolve(makeDevice(), {joint("j", "left", {"position"}), joint("j", "right", {"position"})}, config());
    },
                  {"'j'", "duplicate"});
    // A joint and a sensor share one namespace too.
    expectRefused([] {
        resolve(makeDevice(), {joint("x", "left", {"position"}), sensor("x", "imu", {"orientation.w"})}, config());
    },
                  {"'x'", "duplicate"});
}

TEST(Resolve, DuplicateInterfaceNamesRefused)
{
    expectRefused([] { resolve(makeDevice(), {joint("j", "left", {"position", "velocity", "position"})}, config()); },
                  {"'j'", "'position'", "more than once"});
}

TEST(Resolve, EmptyComponentNameRefused)
{
    expectRefused([] { resolve(makeDevice(), {joint("", "left", {"position"})}, config()); }, {"empty name"});
}

TEST(Resolve, NoComponentsGivesNoGroups)
{
    const auto r = resolve(makeDevice(), {}, config());
    EXPECT_TRUE(r.states.empty());
    EXPECT_TRUE(r.groups.empty());
}

// ---- resolve: groups and slots -----------------------------------------

TEST(Resolve, SameRefSharesOneSlot)
{
    auto spec = joint("j", "left", {"position", "velocity"});
    spec.params["state.velocity"] = "position"; // both interfaces read the same signal
    const auto r = resolve(makeDevice(), {spec}, config());
    ASSERT_EQ(r.states.size(), 2u);
    EXPECT_EQ(r.states[0].ref, r.states[1].ref);
    EXPECT_EQ(r.states[0].slot, 0u);
    EXPECT_EQ(r.states[1].slot, 0u);
    ASSERT_EQ(r.groups.size(), 1u);
    EXPECT_EQ(r.groups[0].refs.size(), 1u);
    EXPECT_EQ(r.groups[0].types.size(), 1u);
}

TEST(Resolve, SameRefAcrossComponentsSharesSlot)
{
    const auto r = resolve(makeDevice(), {joint("a", "left", {"position", "velocity"}), joint("b", "left", {"velocity", "duty"})},
                           config());
    ASSERT_EQ(r.states.size(), 4u);
    EXPECT_EQ(r.states[0].slot, 0u);
    EXPECT_EQ(r.states[1].slot, 1u);
    EXPECT_EQ(r.states[2].slot, 1u); // b.velocity == a.velocity
    EXPECT_EQ(r.states[3].slot, 2u);
    EXPECT_EQ(r.groups[0].refs.size(), 3u);
}

TEST(Resolve, GroupsAscendingByLoopAndRefsInFirstUseOrder)
{
    auto snap = makeDevice();
    addLoop(snap, 2, 50);
    addInstance(snap, kPid, 3, "slow", 2);
    // Component order touches loop 1 first, then 2, then 0; groups still come out 0, 1, 2.
    const auto r = resolve(snap,
                           {sensor("imu", "imu", {"orientation.w", "angular_velocity.x"}),
                            joint("slow_j", "slow", {"duty", "position"}),
                            joint("left_j", "left", {"velocity"}),
                            joint("right_j", "right", {"position", "velocity"}),
                            sensor("imu2", "imu", {"angular_velocity.x", "linear_acceleration.z"})},
                           config());
    ASSERT_EQ(r.groups.size(), 3u);
    EXPECT_EQ(r.groups[0].loop, 0);
    EXPECT_EQ(r.groups[1].loop, 1);
    EXPECT_EQ(r.groups[2].loop, 2);
    EXPECT_EQ(r.groups[0].refs, (std::vector<drivelink::Ref>{ref(kPid, 0, 3), ref(kPid, 1, 2), ref(kPid, 1, 3)}));
    EXPECT_EQ(r.groups[1].refs, (std::vector<drivelink::Ref>{ref(kImu, 0, 0), ref(kImu, 0, 4), ref(kImu, 0, 9)}));
    EXPECT_EQ(r.groups[2].refs, (std::vector<drivelink::Ref>{ref(kPid, 3, 4), ref(kPid, 3, 2)}));

    ASSERT_EQ(r.states.size(), 9u);
    struct Expected
    {
        const char *component;
        const char *interface_name;
        size_t group;
        size_t slot;
    };
    const Expected expected[] = {
        {"imu", "orientation.w", 1, 0},       {"imu", "angular_velocity.x", 1, 1}, {"slow_j", "duty", 2, 0},
        {"slow_j", "position", 2, 1},         {"left_j", "velocity", 0, 0},        {"right_j", "position", 0, 1},
        {"right_j", "velocity", 0, 2},        {"imu2", "angular_velocity.x", 1, 1}, {"imu2", "linear_acceleration.z", 1, 2},
    };
    for (size_t i = 0; i < 9; ++i)
    {
        EXPECT_EQ(r.states[i].component, expected[i].component) << i;
        EXPECT_EQ(r.states[i].interface_name, expected[i].interface_name) << i;
        EXPECT_EQ(r.states[i].group, expected[i].group) << i;
        EXPECT_EQ(r.states[i].slot, expected[i].slot) << i;
        // The binding's ref is where its group and slot point.
        EXPECT_EQ(r.groups[r.states[i].group].refs[r.states[i].slot], r.states[i].ref) << i;
        EXPECT_EQ(r.groups[r.states[i].group].types[r.states[i].slot], r.states[i].type) << i;
        EXPECT_EQ(r.groups[r.states[i].group].loop, r.states[i].loop) << i;
    }
}

TEST(Resolve, UnusedLoopsGetNoGroup)
{
    const auto r = resolve(makeDevice(), {joint("j", "left", {"position"})}, config());
    ASSERT_EQ(r.groups.size(), 1u);
    EXPECT_EQ(r.groups[0].loop, 0);
}

TEST(Resolve, LoopMissingFromInfoRefused)
{
    auto snap = makeDevice();
    addInstance(snap, kPid, 5, "lost", 7); // loop 7 is not listed
    expectRefused([&] { resolve(snap, {joint("j", "lost", {"position"})}, config()); },
                  {"joint", "'j'", "'lost'", "loop 7"});
}

TEST(Resolve, LoopWithZeroRateRefused)
{
    auto snap = makeDevice();
    addLoop(snap, 3, 0);
    addInstance(snap, kPid, 5, "idle", 3);
    expectRefused([&] { resolve(snap, {joint("j", "idle", {"position"})}, config()); }, {"'j'", "'idle'", "loop 3", "0 Hz"});
}

TEST(Resolve, ExactlySixtyFourRefsAccepted)
{
    auto snap = makeDevice();
    std::vector<Item> items;
    std::vector<std::string> names;
    for (uint16_t i = 0; i < 70; ++i)
    {
        names.push_back("s" + std::to_string(i));
        items.push_back({i, names.back(), ItemKind::Signal, ValueType::F32});
    }
    addType(snap, 5, "many", items);
    addInstance(snap, 5, 0, "many", 0);
    auto ok = joint("j", "many", std::vector<std::string>(names.begin(), names.begin() + 64));
    const auto r = resolve(snap, {ok}, config());
    ASSERT_EQ(r.groups.size(), 1u);
    EXPECT_EQ(r.groups[0].refs.size(), 64u);
    EXPECT_EQ(r.states.back().slot, 63u);

    auto too_many = joint("j", "many", std::vector<std::string>(names.begin(), names.begin() + 65));
    expectRefused([&] { resolve(snap, {too_many}, config()); }, {"loop 0", "64", "'s64'"});
}

TEST(Resolve, MoreThanSixtyFourAcrossComponentsRefusedPerLoop)
{
    auto snap = makeDevice();
    std::vector<Item> items;
    std::vector<std::string> names;
    for (uint16_t i = 0; i < 40; ++i)
    {
        names.push_back("v" + std::to_string(i));
        items.push_back({i, names.back(), ItemKind::Signal, ValueType::F32});
    }
    addType(snap, 5, "forty", items);
    addInstance(snap, 5, 0, "a", 0);
    addInstance(snap, 5, 1, "b", 0);
    addInstance(snap, 5, 2, "c", 1);
    // 40 + 40 distinct refs on loop 0: refused. The same on two loops: fine.
    expectRefused([&] { resolve(snap, {joint("ja", "a", names), joint("jb", "b", names)}, config()); },
                  {"loop 0", "64", "'jb'"});
    EXPECT_NO_THROW(resolve(snap, {joint("ja", "a", names), joint("jc", "c", names)}, config()));
    // Sharing refs does not count twice.
    EXPECT_NO_THROW(resolve(snap, {joint("ja", "a", names), joint("jb", "a", names)}, config()));
}

// ---- resolve: divider, rate and stale timeout --------------------------

TEST(Resolve, DividerMath)
{
    struct Case
    {
        uint32_t loop_rate;
        double state_rate;
        uint16_t divider;
        double rate_hz;
    };
    const Case cases[] = {
        {1000, 100, 10, 100},   {100, 100, 1, 100},   {30, 100, 1, 30},      {1000, 30, 33, 1000.0 / 33},
        {1000, 10000, 1, 1000}, {1000, 1, 1000, 1},   {200, 1.5, 133, 200.0 / 133}, {1000, 400, 2, 500},
    };
    for (const auto &c : cases)
    {
        auto snap = makeDevice();
        snap.info.loops[0].rate_hz = c.loop_rate;
        const auto r = resolve(snap, {joint("j", "left", {"position"})}, config(c.state_rate));
        ASSERT_EQ(r.groups.size(), 1u);
        EXPECT_EQ(r.groups[0].loop_rate_hz, c.loop_rate);
        EXPECT_EQ(r.groups[0].divider, c.divider) << c.loop_rate << "@" << c.state_rate;
        EXPECT_DOUBLE_EQ(r.groups[0].rate_hz, c.rate_hz) << c.loop_rate << "@" << c.state_rate;
    }
}

TEST(Resolve, HugeRatioClampsDividerTo65535)
{
    auto snap = makeDevice();
    snap.info.loops[0].rate_hz = 4000000000u;
    const auto r = resolve(snap, {joint("j", "left", {"position"})}, config(1));
    ASSERT_EQ(r.groups.size(), 1u);
    EXPECT_EQ(r.groups[0].divider, 65535);
    EXPECT_DOUBLE_EQ(r.groups[0].rate_hz, 4000000000.0 / 65535);

    // Exactly at the limit is not clamped.
    snap.info.loops[0].rate_hz = 65535;
    const auto edge = resolve(snap, {joint("j", "left", {"position"})}, config(1));
    EXPECT_EQ(edge.groups[0].divider, 65535);
    EXPECT_DOUBLE_EQ(edge.groups[0].rate_hz, 1);
}

TEST(Resolve, EachLoopHasItsOwnDivider)
{
    const auto r = resolve(makeDevice(),
                           {joint("j", "left", {"position"}), sensor("s", "imu", {"orientation.w"})}, config(50));
    ASSERT_EQ(r.groups.size(), 2u);
    EXPECT_EQ(r.groups[0].divider, 20); // 1000 Hz loop
    EXPECT_DOUBLE_EQ(r.groups[0].rate_hz, 50);
    EXPECT_EQ(r.groups[1].divider, 2); // 100 Hz loop
    EXPECT_DOUBLE_EQ(r.groups[1].rate_hz, 50);
}

TEST(Resolve, StaleTimeoutAutoOffAndExplicit)
{
    auto run = [](std::optional<double> ms) {
        auto c = config(100);
        c.stale_timeout_ms = ms;
        return resolve(makeDevice(), {joint("j", "left", {"position"}), sensor("s", "imu", {"orientation.w"})}, c);
    };
    // Auto: both groups run at 100 Hz, so 10 periods is 100 ms.
    const auto auto_r = run(std::nullopt);
    EXPECT_EQ(auto_r.groups[0].stale_timeout_us, 100000u);
    EXPECT_EQ(auto_r.groups[1].stale_timeout_us, 100000u);
    const auto off = run(0.0);
    EXPECT_EQ(off.groups[0].stale_timeout_us, 0u);
    EXPECT_EQ(off.groups[1].stale_timeout_us, 0u);
    const auto fixed = run(2.5);
    EXPECT_EQ(fixed.groups[0].stale_timeout_us, 2500u);
    EXPECT_EQ(fixed.groups[1].stale_timeout_us, 2500u);
}

TEST(Resolve, AutoStaleTimeoutFollowsGroupRate)
{
    auto snap = makeDevice();
    snap.info.loops[1].rate_hz = 30; // the IMU's loop: 30 Hz, divider 1
    const auto r = resolve(snap, {joint("j", "left", {"position"}), sensor("s", "imu", {"orientation.w"})}, config(100));
    EXPECT_EQ(r.groups[0].stale_timeout_us, 100000u);            // 100 Hz: 10 periods = 100 ms
    EXPECT_EQ(r.groups[1].stale_timeout_us, 333334u);            // 30 Hz: 10 periods = 333.33 ms, rounded up
    // A slow state rate on the fast loop: 1 Hz -> 10 s.
    const auto slow = resolve(snap, {joint("j", "left", {"position"})}, config(1));
    EXPECT_EQ(slow.groups[0].stale_timeout_us, 10000000u);
}

TEST(Resolve, BadConfigRateRefused)
{
    for (double rate : {0.0, -1.0, 10001.0, std::numeric_limits<double>::quiet_NaN()})
        expectRefused([&] { resolve(makeDevice(), {joint("j", "left", {"position"})}, config(rate)); },
                      {"state_rate_hz"});
}

TEST(Resolve, RefusalsNameFirstOffendingComponentInOrder)
{
    // The second component is the broken one; the first is fine.
    expectRefused([] {
        resolve(makeDevice(), {joint("good", "left", {"position"}), joint("bad", "nobody", {"position"})}, config());
    },
                  {"'bad'", "'nobody'"});
}

// ---- staleTimeoutUs ----------------------------------------------------

TEST(StaleTimeout, Auto)
{
    EXPECT_EQ(staleTimeoutUs(std::nullopt, 100), 100000u);   // 10 periods = 100 ms = the floor
    EXPECT_EQ(staleTimeoutUs(std::nullopt, 1000), 100000u);  // 10 ms, floored to 100 ms
    EXPECT_EQ(staleTimeoutUs(std::nullopt, 10000), 100000u);
    EXPECT_EQ(staleTimeoutUs(std::nullopt, 50), 200000u);    // 10 periods = 200 ms
    EXPECT_EQ(staleTimeoutUs(std::nullopt, 1), 10000000u);   // 10 s
    EXPECT_EQ(staleTimeoutUs(std::nullopt, 30), 333334u);    // 333333.33 rounded up
    EXPECT_EQ(staleTimeoutUs(std::nullopt, 7), 1428572u);    // 1428571.43 rounded up
}

TEST(StaleTimeout, AutoWithBadRateUsesFloor)
{
    EXPECT_EQ(staleTimeoutUs(std::nullopt, 0), 100000u);
    EXPECT_EQ(staleTimeoutUs(std::nullopt, -5), 100000u);
}

TEST(StaleTimeout, ZeroIsOff)
{
    EXPECT_EQ(staleTimeoutUs(0.0, 100), 0u);
    EXPECT_EQ(staleTimeoutUs(0.0, 1), 0u);
}

TEST(StaleTimeout, ExplicitMilliseconds)
{
    EXPECT_EQ(staleTimeoutUs(250.0, 100), 250000u);
    EXPECT_EQ(staleTimeoutUs(2.5, 1000), 2500u);
    EXPECT_EQ(staleTimeoutUs(60000.0, 100), 60000000u);
    EXPECT_EQ(staleTimeoutUs(0.0004, 100), 1u); // a non-zero timeout never rounds down to off
    EXPECT_EQ(staleTimeoutUs(0.0015, 100), 2u); // rounded up
}

TEST(StaleTimeout, InvalidExplicitRefused)
{
    expectRefused([] { staleTimeoutUs(-1.0, 100); }, {"stale_timeout_ms"});
    expectRefused([] { staleTimeoutUs(std::numeric_limits<double>::quiet_NaN(), 100); }, {"stale_timeout_ms"});
}

// ---- isStale -----------------------------------------------------------

TEST(IsStale, Edges)
{
    static_assert(isStale(1000, 0, 999));
    EXPECT_FALSE(isStale(1000, 0, 1000)); // exactly the timeout: not yet stale
    EXPECT_TRUE(isStale(1001, 0, 1000));
    EXPECT_FALSE(isStale(1000, 900, 100));
    EXPECT_TRUE(isStale(1001, 900, 100));
}

TEST(IsStale, DisabledWhenTimeoutZero)
{
    EXPECT_FALSE(isStale(1'000'000'000, 0, 0));
    EXPECT_FALSE(isStale(5, 5, 0));
}

TEST(IsStale, SampleInTheFutureNeverStale)
{
    EXPECT_FALSE(isStale(100, 200, 1));
    EXPECT_FALSE(isStale(100, 100, 1)); // same instant: age 0
    EXPECT_FALSE(isStale(0, std::numeric_limits<uint64_t>::max(), 1));
}

TEST(IsStale, LargeValuesDoNotOverflow)
{
    constexpr uint64_t big = std::numeric_limits<uint64_t>::max();
    EXPECT_TRUE(isStale(big, 0, big - 1));
    EXPECT_FALSE(isStale(big, 0, big));
}

// ---- toDouble ----------------------------------------------------------

TEST(ToDouble, F32)
{
    EXPECT_EQ(toDouble(ValueType::F32, std::bit_cast<uint32_t>(1.5f)), 1.5);
    EXPECT_EQ(toDouble(ValueType::F32, std::bit_cast<uint32_t>(-0.25f)), -0.25);
    EXPECT_EQ(toDouble(ValueType::F32, 0), 0.0);
    EXPECT_TRUE(std::isnan(toDouble(ValueType::F32, std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN()))));
    EXPECT_TRUE(std::isinf(toDouble(ValueType::F32, std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity()))));
    // A float bit pattern is not read as an integer.
    EXPECT_NE(toDouble(ValueType::F32, 0x3FC00000u), static_cast<double>(0x3FC00000u));
}

TEST(ToDouble, I32IsSigned)
{
    EXPECT_EQ(toDouble(ValueType::I32, 5), 5.0);
    EXPECT_EQ(toDouble(ValueType::I32, 0xFFFFFFFFu), -1.0);
    EXPECT_EQ(toDouble(ValueType::I32, 0x80000000u), -2147483648.0);
    EXPECT_EQ(toDouble(ValueType::I32, 0x7FFFFFFFu), 2147483647.0);
}

TEST(ToDouble, UnsignedTypes)
{
    EXPECT_EQ(toDouble(ValueType::U8, 200), 200.0);
    EXPECT_EQ(toDouble(ValueType::U16, 65535), 65535.0);
    EXPECT_EQ(toDouble(ValueType::U32, 0xFFFFFFFFu), 4294967295.0);
    EXPECT_EQ(toDouble(ValueType::U32, 0x80000000u), 2147483648.0);
    EXPECT_EQ(toDouble(ValueType::Enum, 3), 3.0);
    EXPECT_EQ(toDouble(ValueType::Enum, 0), 0.0);
}

TEST(ToDouble, BoolIsZeroOrOne)
{
    EXPECT_EQ(toDouble(ValueType::Bool, 0), 0.0);
    EXPECT_EQ(toDouble(ValueType::Bool, 1), 1.0);
    EXPECT_EQ(toDouble(ValueType::Bool, 2), 1.0);
    EXPECT_EQ(toDouble(ValueType::Bool, 0xFFFFFFFFu), 1.0);
    EXPECT_EQ(toDouble(ValueType::Bool, 0x100), 1.0);
}

TEST(ToDouble, NoneAndUnknownGiveNaN)
{
    EXPECT_TRUE(std::isnan(toDouble(ValueType::None, 0)));
    EXPECT_TRUE(std::isnan(toDouble(ValueType::None, 42)));
    EXPECT_TRUE(std::isnan(toDouble(static_cast<ValueType>(0x42), 1)));
}

// ---- H4b: command interfaces -------------------------------------------

namespace
{
constexpr uint8_t kCascade = 3; // controller.cascade_pid as the device describes it
constexpr uint8_t kPlain = 4;   // inputs but no `mode` and no `position`
constexpr uint8_t kInputs = 5;  // 70 inputs and nothing else
constexpr uint16_t kFlagRebootRequired = 0x01;
constexpr uint16_t kFlagReadOnly = 0x04;
constexpr uint16_t kFlagLockedWhileArmed = 0x08;

drivelink::ModelItem &modelItem(drivelink::DeviceSnapshot &snap, uint8_t type, uint16_t id)
{
    return snap.types.at(type).items.at(id);
}

void setEnums(drivelink::DeviceSnapshot &snap, uint8_t type, uint16_t id, std::vector<drivelink::EnumEntry> entries)
{
    modelItem(snap, type, id).enums = std::move(entries);
}

// makeDevice() plus: controller.cascade_pid (type 3) with instances "wl" and "wr" on loop 0 and
// "slow" on loop 1 (item ids: 0 mode, 2 position, 3 velocity, 4 duty, 5 output_position,
// 512 velocity_sp, 513 position_sp, 514 duty_sp, 515 alt_sp, 516 blob_sp, and extra params
// 20 alt_mode (idle, run), 21 ro_mode, 22 reboot_mode, 23 locked_mode, 24 gain), a type
// "plain" (4) with velocity_sp and duty_sp but no mode and no position, instance "plain" on loop 0.
drivelink::DeviceSnapshot makeCommandDevice()
{
    auto snap = makeDevice();
    addType(snap, kCascade, "controller.cascade_pid",
            {
                {0, "mode", ItemKind::Param, ValueType::Enum},
                {2, "position", ItemKind::Signal, ValueType::F32},
                {3, "velocity", ItemKind::Signal, ValueType::F32},
                {4, "duty", ItemKind::Signal, ValueType::F32},
                {5, "output_position", ItemKind::Signal, ValueType::F32},
                {512, "velocity_sp", ItemKind::Input, ValueType::F32},
                {513, "position_sp", ItemKind::Input, ValueType::F32},
                {514, "duty_sp", ItemKind::Input, ValueType::F32},
                {515, "alt_sp", ItemKind::Input, ValueType::F32},
                {516, "blob_sp", ItemKind::Input, ValueType::None},
                {20, "alt_mode", ItemKind::Param, ValueType::Enum},
                {21, "ro_mode", ItemKind::Param, ValueType::Enum},
                {22, "reboot_mode", ItemKind::Param, ValueType::Enum},
                {23, "locked_mode", ItemKind::Param, ValueType::Enum},
                {24, "gain", ItemKind::Param, ValueType::F32},
            });
    const std::vector<drivelink::EnumEntry> cascade_modes = {{0, "open_loop"}, {1, "velocity"}, {2, "position"}};
    for (const uint16_t id : {0, 21, 22, 23})
        setEnums(snap, kCascade, id, cascade_modes);
    setEnums(snap, kCascade, 20, {{0, "idle"}, {1, "run"}});
    modelItem(snap, kCascade, 21).descriptor.flags = kFlagReadOnly;
    modelItem(snap, kCascade, 22).descriptor.flags = kFlagRebootRequired;
    modelItem(snap, kCascade, 23).descriptor.flags = kFlagLockedWhileArmed;
    addType(snap, kPlain, "plain",
            {
                {1, "velocity", ItemKind::Signal, ValueType::F32},
                {2, "velocity_sp", ItemKind::Input, ValueType::F32},
                {3, "duty_sp", ItemKind::Input, ValueType::F32},
            });
    addInstance(snap, kCascade, 0, "wl", 0);
    addInstance(snap, kCascade, 1, "wr", 0);
    addInstance(snap, kCascade, 2, "slow", 1);
    addInstance(snap, kPlain, 0, "plain", 0);
    return snap;
}

ComponentSpec cjoint(const char *name, const char *instance, std::vector<std::string> states,
                     std::vector<std::string> commands, Params extra = {})
{
    ComponentSpec spec = joint(name, instance, std::move(states));
    spec.command_interfaces = std::move(commands);
    for (auto &[key, value] : extra)
        spec.params[key] = std::move(value);
    return spec;
}

constexpr size_t idx(CommandKind kind)
{
    return static_cast<size_t>(kind);
}

Resolved resolveCommands(const std::vector<ComponentSpec> &components)
{
    return resolve(makeCommandDevice(), components, config());
}
} // namespace

TEST(ResolveCommands, DefaultItemsAndModeEntries)
{
    const auto r = resolveCommands({cjoint("left_wheel", "wl", {"velocity"}, {"position", "velocity", "duty"})});
    ASSERT_EQ(r.commands.size(), 3u);
    const CommandKind kinds[] = {CommandKind::Position, CommandKind::Velocity, CommandKind::Duty};
    const char *names[] = {"position", "velocity", "duty"};
    const uint16_t items[] = {513, 512, 514};
    for (size_t i = 0; i < 3; ++i)
    {
        EXPECT_EQ(r.commands[i].component, "left_wheel");
        EXPECT_EQ(r.commands[i].interface_name, names[i]);
        EXPECT_EQ(r.commands[i].kind, kinds[i]);
        EXPECT_EQ(r.commands[i].ref, ref(kCascade, 0, items[i]));
        EXPECT_EQ(r.commands[i].type, ValueType::F32);
        EXPECT_EQ(r.commands[i].loop, 0);
        EXPECT_EQ(r.commands[i].group, 0u);
        EXPECT_EQ(r.commands[i].slot, i);
        EXPECT_EQ(r.commands[i].joint, 0u);
    }
    ASSERT_EQ(r.command_groups.size(), 1u);
    EXPECT_EQ(r.command_groups[0].loop, 0);
    EXPECT_EQ(r.command_groups[0].refs,
              (std::vector<drivelink::Ref>{ref(kCascade, 0, 513), ref(kCascade, 0, 512), ref(kCascade, 0, 514)}));
    EXPECT_EQ(r.command_groups[0].types, (std::vector<ValueType>{ValueType::F32, ValueType::F32, ValueType::F32}));

    ASSERT_EQ(r.joints.size(), 1u);
    const auto &j = r.joints[0];
    EXPECT_EQ(j.name, "left_wheel");
    EXPECT_FALSE(j.commands[idx(CommandKind::None)].has_value());
    EXPECT_EQ(j.commands[idx(CommandKind::Position)], std::optional<size_t>(0));
    EXPECT_EQ(j.commands[idx(CommandKind::Velocity)], std::optional<size_t>(1));
    EXPECT_EQ(j.commands[idx(CommandKind::Duty)], std::optional<size_t>(2));
    ASSERT_TRUE(j.mode.has_value());
    EXPECT_EQ(*j.mode, ref(kCascade, 0, 0));
    EXPECT_EQ(j.mode_values[idx(CommandKind::Position)], 2);
    EXPECT_EQ(j.mode_values[idx(CommandKind::Velocity)], 1);
    EXPECT_EQ(j.mode_values[idx(CommandKind::Duty)], 0);
}

TEST(ResolveCommands, SubsetOfInterfacesLeavesTheRestEmpty)
{
    const auto r = resolveCommands({cjoint("j", "wl", {}, {"duty"})});
    ASSERT_EQ(r.joints.size(), 1u);
    EXPECT_FALSE(r.joints[0].commands[idx(CommandKind::Position)].has_value());
    EXPECT_FALSE(r.joints[0].commands[idx(CommandKind::Velocity)].has_value());
    EXPECT_EQ(r.joints[0].commands[idx(CommandKind::Duty)], std::optional<size_t>(0));
    EXPECT_FALSE(r.joints[0].position_group.has_value()); // no position command, no hold
}

TEST(ResolveCommands, CommandParamOverridesTheInput)
{
    const auto r = resolveCommands(
        {cjoint("j", "wl", {}, {"velocity", "duty"}, {{"command.velocity", "alt_sp"}, {"command.duty", "velocity_sp"}})});
    ASSERT_EQ(r.commands.size(), 2u);
    EXPECT_EQ(r.commands[0].ref, ref(kCascade, 0, 515));
    EXPECT_EQ(r.commands[1].ref, ref(kCascade, 0, 512));
}

TEST(ResolveCommands, ModeEntryOverride)
{
    const auto r = resolveCommands({cjoint("j", "wl", {}, {"velocity", "duty"},
                                           {{"mode.velocity", "position"}, {"mode.duty", "velocity"}})});
    ASSERT_EQ(r.joints.size(), 1u);
    EXPECT_EQ(r.joints[0].mode_values[idx(CommandKind::Velocity)], 2);
    EXPECT_EQ(r.joints[0].mode_values[idx(CommandKind::Duty)], 1);
}

TEST(ResolveCommands, ModeParamNamesAnotherEnum)
{
    const auto r = resolveCommands({cjoint("j", "wl", {}, {"velocity", "duty"},
                                           {{"mode_param", "alt_mode"}, {"mode.velocity", "run"}, {"mode.duty", "idle"}})});
    ASSERT_TRUE(r.joints[0].mode.has_value());
    EXPECT_EQ(*r.joints[0].mode, ref(kCascade, 0, 20));
    EXPECT_EQ(r.joints[0].mode_values[idx(CommandKind::Velocity)], 1);
    EXPECT_EQ(r.joints[0].mode_values[idx(CommandKind::Duty)], 0);
}

TEST(ResolveCommands, ModeParamNoneTurnsModeWritesOff)
{
    const auto r = resolveCommands({cjoint("j", "wl", {}, {"velocity"}, {{"mode_param", "none"}})});
    ASSERT_EQ(r.joints.size(), 1u);
    EXPECT_FALSE(r.joints[0].mode.has_value());
    ASSERT_EQ(r.commands.size(), 1u);
    // `none` also skips the checks of the default mode item: a read-only "mode" is fine then.
    auto snap = makeCommandDevice();
    modelItem(snap, kCascade, 0).descriptor.flags = kFlagReadOnly;
    const auto off = resolve(snap, {cjoint("j", "wl", {}, {"velocity"}, {{"mode_param", "none"}})}, config());
    EXPECT_FALSE(off.joints[0].mode.has_value());
}

TEST(ResolveCommands, TypeWithoutModeWritesNoMode)
{
    const auto r = resolveCommands({cjoint("j", "plain", {"velocity"}, {"velocity", "duty"})});
    ASSERT_EQ(r.joints.size(), 1u);
    EXPECT_FALSE(r.joints[0].mode.has_value());
    EXPECT_EQ(r.commands[0].ref, ref(kPlain, 0, 2));
    EXPECT_EQ(r.commands[1].ref, ref(kPlain, 0, 3));
    // A mode.* param would be ignored: refused.
    expectRefused([] { resolveCommands({cjoint("j", "plain", {}, {"velocity"}, {{"mode.velocity", "velocity"}})}); },
                  {"'j'", "'mode.velocity'", "no item named 'mode'", "mode_param"});
}

TEST(ResolveCommands, ModeEntryParamsNeedAModeWrite)
{
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"velocity"}, {{"mode_param", "none"}, {"mode.velocity", "velocity"}})}); },
                  {"'j'", "'mode.velocity'", "'none'"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {"velocity"}, {}, {{"mode_param", "alt_mode"}})}); },
                  {"'j'", "'mode_param'", "no command interface"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {"velocity"}, {"velocity"}, {{"mode_param", ""}})}); },
                  {"'j'", "'mode_param'", "empty"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"velocity"}, {{"mode.velocity", ""}})}); },
                  {"'j'", "'mode.velocity'", "empty"});
}

TEST(ResolveCommands, StateOnlyJointNeverTouchesTheModeItem)
{
    // The type's `mode` is not checked for a joint without command interfaces.
    auto snap = makeCommandDevice();
    modelItem(snap, kCascade, 0).descriptor.flags = kFlagReadOnly;
    const auto r = resolve(snap, {cjoint("j", "wl", {"velocity"}, {})}, config());
    EXPECT_TRUE(r.joints.empty());
    EXPECT_TRUE(r.commands.empty());
    EXPECT_TRUE(r.command_groups.empty());
}

// ---- mode param refusals -----------------------------------------------

TEST(ResolveCommands, ModeParamRefusals)
{
    struct Case
    {
        const char *mode_param; // nullptr: the default item "mode"
        std::function<void(drivelink::DeviceSnapshot &)> tweak;
        std::vector<std::string> needles;
    };
    const std::vector<Case> cases = {
        {"position", nullptr, {"'j'", "mode param 'position'", "'mode_param'", "not a param", "signal"}},
        {"velocity_sp", nullptr, {"not a param", "input"}},
        {"gain", nullptr, {"mode param 'gain'", "f32", "not enum"}},
        {"ro_mode", nullptr, {"mode param 'ro_mode'", "read-only", "ReadOnly"}},
        {"reboot_mode", nullptr, {"mode param 'reboot_mode'", "RebootRequired"}},
        {"locked_mode", nullptr, {"mode param 'locked_mode'", "LockedWhileArmed", "while armed"}},
        {"no_such", nullptr, {"mode param 'no_such'", "no such item", "available params:", "alt_mode", "gain", "mode"}},
        {nullptr,
         [](drivelink::DeviceSnapshot &snap) { modelItem(snap, kCascade, 0).descriptor.flags = kFlagReadOnly; },
         {"mode param 'mode'", "(default)", "ReadOnly"}},
        {nullptr,
         [](drivelink::DeviceSnapshot &snap) { modelItem(snap, kCascade, 0).descriptor.kind = ItemKind::Signal; },
         {"mode param 'mode'", "not a param"}},
        {nullptr,
         [](drivelink::DeviceSnapshot &snap) { modelItem(snap, kCascade, 0).descriptor.value_type = ValueType::U8; },
         {"mode param 'mode'", "u8", "not enum"}},
    };
    for (const auto &c : cases)
    {
        auto snap = makeCommandDevice();
        if (c.tweak)
            c.tweak(snap);
        Params extra;
        if (c.mode_param)
            extra["mode_param"] = c.mode_param;
        SCOPED_TRACE(c.mode_param ? c.mode_param : "(default mode item)");
        expectRefused([&] { resolve(snap, {cjoint("j", "wl", {}, {"velocity"}, extra)}, config()); }, c.needles);
    }
}

TEST(ResolveCommands, ModeEntryMissingListsTheEntries)
{
    // The default entry of velocity is "velocity": alt_mode has idle and run only.
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"velocity"}, {{"mode_param", "alt_mode"}})}); },
                  {"'j'", "command interface 'velocity'", "default entry 'velocity'", "mode param 'alt_mode'",
                   "no such enum entry", "available entries: idle, run"});
    // An override that names a missing entry says where it came from.
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"duty"}, {{"mode.duty", "coast"}})}); },
                  {"command interface 'duty'", "param 'mode.duty' = 'coast'", "available entries: open_loop, velocity, position"});
    // Only declared interfaces are checked: velocity alone does not need "open_loop".
    auto snap = makeCommandDevice();
    setEnums(snap, kCascade, 0, {{1, "velocity"}});
    EXPECT_NO_THROW(resolve(snap, {cjoint("j", "wl", {}, {"velocity"})}, config()));
    expectRefused([&] { resolve(snap, {cjoint("j", "wl", {}, {"velocity", "duty"})}, config()); },
                  {"command interface 'duty'", "'open_loop'", "available entries: velocity"});
    // A mode item with no entries at all.
    setEnums(snap, kCascade, 0, {});
    expectRefused([&] { resolve(snap, {cjoint("j", "wl", {}, {"velocity"})}, config()); },
                  {"available entries: (none)"});
}

// ---- command interface refusals ----------------------------------------

TEST(ResolveCommands, UnknownCommandInterfaceRefused)
{
    expectRefused([] { resolveCommands({cjoint("j", "wl", {"velocity"}, {"effort"})}); },
                  {"joint", "'j'", "command interface 'effort'", "known: position, velocity, duty"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {"velocity"}, {"velocity_sp"})}); },
                  {"'velocity_sp'", "known: position, velocity, duty"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {"velocity"}, {"Velocity"})}); }, {"'Velocity'"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {"velocity"}, {"none"})}); }, {"'none'"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {"velocity"}, {""})}); }, {"'j'", "empty name"});
}

TEST(ResolveCommands, SensorCommandInterfaceRefused)
{
    auto spec = sensor("imu_sensor", "imu", {"orientation.w"});
    spec.command_interfaces = {"velocity"};
    expectRefused([&] { resolveCommands({spec}); },
                  {"sensor", "'imu_sensor'", "command interface 'velocity'", "joints only"});
    // Also with no state interface at all.
    auto bare = sensor("imu_sensor", "imu", {});
    bare.command_interfaces = {"duty"};
    expectRefused([&] { resolveCommands({bare}); }, {"sensor", "'imu_sensor'", "joints only"});
}

TEST(ResolveCommands, CommandParamsOnASensorAreUnknown)
{
    auto spec = sensor("s", "imu", {"orientation.w"});
    spec.params["mode_param"] = "none";
    expectRefused([&] { resolveCommands({spec}); }, {"sensor", "'s'", "'mode_param'", "unknown"});
    auto spec2 = sensor("s", "imu", {"orientation.w"});
    spec2.params["command.velocity"] = "velocity_sp";
    expectRefused([&] { resolveCommands({spec2}); }, {"sensor", "'command.velocity'", "unknown"});
}

TEST(ResolveCommands, CommandItemMustBeAnInput)
{
    // A signal named as the input.
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"velocity"}, {{"command.velocity", "velocity"}})}); },
                  {"joint", "'j'", "command interface 'velocity'", "param 'command.velocity' = 'velocity'",
                   "item is a signal, not an input", "f32"});
    // A param.
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"velocity"}, {{"command.velocity", "gain"}})}); },
                  {"item is a param, not an input"});
    // The default item missing from the type: the message lists the inputs.
    expectRefused([] { resolveCommands({cjoint("j", "plain", {}, {"position"})}); },
                  {"'j'", "command interface 'position'", "default item 'position_sp'", "'plain'", "no such item",
                   "available inputs: duty_sp, velocity_sp"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"velocity"}, {{"command.velocity", "nope"}})}); },
                  {"'nope'", "available inputs: alt_sp, blob_sp, duty_sp, position_sp, velocity_sp"});
}

TEST(ResolveCommands, CommandInputMustBeNumeric)
{
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"velocity"}, {{"command.velocity", "blob_sp"}})}); },
                  {"'j'", "'blob_sp'", "input has value type none", "not a scalar number"});
}

TEST(ResolveCommands, EveryScalarInputTypeAccepted)
{
    auto snap = makeCommandDevice();
    const std::vector<std::pair<const char *, ValueType>> inputs = {
        {"in_bool", ValueType::Bool}, {"in_u8", ValueType::U8},   {"in_u16", ValueType::U16}, {"in_u32", ValueType::U32},
        {"in_i32", ValueType::I32},   {"in_f32", ValueType::F32}, {"in_enum", ValueType::Enum}};
    std::vector<Item> items;
    uint16_t id = 600;
    for (const auto &[name, type] : inputs)
        items.push_back({id++, name, ItemKind::Input, type});
    addType(snap, 8, "typed", items);
    addInstance(snap, 8, 0, "typed", 0);
    const auto r = resolve(snap,
                           {cjoint("a", "typed", {}, {"velocity", "duty"}, {{"command.velocity", "in_bool"}, {"command.duty", "in_u8"}}),
                            cjoint("b", "typed", {}, {"velocity", "duty"}, {{"command.velocity", "in_u16"}, {"command.duty", "in_u32"}}),
                            cjoint("c", "typed", {}, {"velocity", "duty"}, {{"command.velocity", "in_i32"}, {"command.duty", "in_f32"}}),
                            cjoint("d", "typed", {}, {"velocity"}, {{"command.velocity", "in_enum"}})},
                           config());
    ASSERT_EQ(r.commands.size(), inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        EXPECT_EQ(r.commands[i].type, inputs[i].second) << inputs[i].first;
        EXPECT_EQ(r.command_groups[0].types[i], inputs[i].second) << inputs[i].first;
    }
}

TEST(ResolveCommands, DuplicateCommandInterfaceRefused)
{
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"velocity", "duty", "velocity"})}); },
                  {"'j'", "command interface 'velocity'", "more than once"});
}

TEST(ResolveCommands, StateAndCommandInterfacesAreSeparateNamespaces)
{
    EXPECT_NO_THROW(resolveCommands({cjoint("j", "wl", {"velocity", "duty"}, {"velocity", "duty"})}));
}

TEST(ResolveCommands, TwoJointsCommandingOneInputRefused)
{
    expectRefused([] { resolveCommands({cjoint("a", "wl", {}, {"velocity"}), cjoint("b", "wl", {}, {"velocity"})}); },
                  {"joint 'b'", "command interface 'velocity'", "'velocity_sp'", "'wl'", "joint 'a' command interface 'velocity'",
                   "one owner"});
    // Different interfaces mapped onto the same input are caught too.
    expectRefused([] {
        resolveCommands({cjoint("a", "wl", {}, {"velocity"}),
                         cjoint("b", "wl", {}, {"duty"}, {{"command.duty", "velocity_sp"}})});
    },
                  {"joint 'b'", "command interface 'duty'", "joint 'a'", "one owner"});
    // The same input item of different instances is two inputs.
    EXPECT_NO_THROW(resolveCommands({cjoint("a", "wl", {}, {"velocity"}), cjoint("b", "wr", {}, {"velocity"})}));
    // Different inputs of one instance are fine too.
    EXPECT_NO_THROW(resolveCommands({cjoint("a", "wl", {}, {"velocity"}), cjoint("b", "wl", {}, {"duty"})}));
}

TEST(ResolveCommands, CommandOverrideForUndeclaredInterfaceRefused)
{
    expectRefused([] { resolveCommands({cjoint("j", "wl", {"velocity"}, {"velocity"}, {{"command.duty", "duty_sp"}})}); },
                  {"joint", "'j'", "'command.duty'", "does not declare", "declared: velocity"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {"velocity"}, {}, {{"command.velocity", "velocity_sp"}})}); },
                  {"'command.velocity'", "declared: (none)"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"velocity"}, {{"command.velocity", ""}})}); },
                  {"'command.velocity'", "empty"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"velocity"}, {{"mode.duty", "open_loop"}})}); },
                  {"'mode.duty'", "does not declare", "declared: velocity"});
}

TEST(ResolveCommands, UnknownParamListsTheJointParams)
{
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"velocity"}, {{"gain", "1"}})}); },
                  {"'gain'", "unknown", "command.<interface>", "mode_param", "mode.<interface>"});
}

TEST(ResolveCommands, ComponentWithNothingRefused)
{
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {})}); },
                  {"joint", "'j'", "no state interface", "no command interface"});
    expectRefused([] { resolveCommands({sensor("s", "imu", {})}); }, {"sensor", "'s'", "no state interface"});
}

TEST(ResolveCommands, CommandOnlyJointAccepted)
{
    const auto r = resolveCommands({cjoint("j", "wl", {}, {"velocity"})});
    EXPECT_TRUE(r.states.empty());
    EXPECT_TRUE(r.groups.empty()); // nothing to subscribe to without a position command
    ASSERT_EQ(r.joints.size(), 1u);
    ASSERT_EQ(r.command_groups.size(), 1u);
    EXPECT_EQ(r.command_groups[0].refs, (std::vector<drivelink::Ref>{ref(kCascade, 0, 512)}));
    // The loop's rate is still checked.
    auto snap = makeCommandDevice();
    snap.info.loops[0].rate_hz = 0;
    expectRefused([&] { resolve(snap, {cjoint("j", "wl", {}, {"velocity"})}, config()); }, {"'j'", "'wl'", "0 Hz"});
}

// ---- position hold -----------------------------------------------------

TEST(ResolveCommands, PositionHoldSharesTheStateSlot)
{
    const auto r = resolveCommands({cjoint("j", "wl", {"velocity", "position"}, {"position"})});
    ASSERT_EQ(r.states.size(), 2u);
    ASSERT_EQ(r.groups.size(), 1u);
    EXPECT_EQ(r.groups[0].refs.size(), 2u); // no extra slot
    ASSERT_TRUE(r.joints[0].position_group.has_value());
    EXPECT_EQ(*r.joints[0].position_group, 0u);
    EXPECT_EQ(r.joints[0].position_slot, 1u);
    EXPECT_EQ(r.joints[0].position_slot, r.states[1].slot);
    EXPECT_EQ(r.joints[0].position_type, ValueType::F32);
    EXPECT_EQ(r.groups[0].refs[r.joints[0].position_slot], ref(kCascade, 0, 2));
}

TEST(ResolveCommands, PositionHoldHiddenWithoutPositionState)
{
    const auto r = resolveCommands({cjoint("j", "wl", {"velocity"}, {"position"})});
    ASSERT_EQ(r.states.size(), 1u);
    EXPECT_EQ(r.states[0].interface_name, "velocity"); // the hidden slot is not a state binding
    ASSERT_EQ(r.groups.size(), 1u);
    EXPECT_EQ(r.groups[0].refs, (std::vector<drivelink::Ref>{ref(kCascade, 0, 3), ref(kCascade, 0, 2)}));
    EXPECT_EQ(r.groups[0].types.size(), 2u);
    ASSERT_TRUE(r.joints[0].position_group.has_value());
    EXPECT_EQ(*r.joints[0].position_group, 0u);
    EXPECT_EQ(r.joints[0].position_slot, 1u);
    EXPECT_EQ(r.groups[0].types[1], r.joints[0].position_type);
}

TEST(ResolveCommands, PositionHoldOfACommandOnlyJoint)
{
    const auto r = resolveCommands({cjoint("j", "wl", {}, {"position"})});
    EXPECT_TRUE(r.states.empty());
    ASSERT_EQ(r.groups.size(), 1u);
    EXPECT_EQ(r.groups[0].refs, (std::vector<drivelink::Ref>{ref(kCascade, 0, 2)}));
    ASSERT_TRUE(r.joints[0].position_group.has_value());
    EXPECT_EQ(r.joints[0].position_slot, 0u);
    EXPECT_EQ(r.groups[0].divider, 10);
}

TEST(ResolveCommands, StatePositionOverrideSelectsTheHoldSignal)
{
    // Hidden: no position state interface, the override is allowed and used.
    const auto hidden = resolveCommands({cjoint("j", "wl", {"velocity"}, {"position"}, {{"state.position", "output_position"}})});
    EXPECT_EQ(hidden.groups[0].refs, (std::vector<drivelink::Ref>{ref(kCascade, 0, 3), ref(kCascade, 0, 5)}));
    EXPECT_EQ(hidden.joints[0].position_slot, 1u);
    ASSERT_EQ(hidden.states.size(), 1u);

    // Shared: the state interface and the hold both use the override.
    const auto shared = resolveCommands({cjoint("j", "wl", {"position"}, {"position"}, {{"state.position", "output_position"}})});
    EXPECT_EQ(shared.groups[0].refs, (std::vector<drivelink::Ref>{ref(kCascade, 0, 5)}));
    EXPECT_EQ(shared.states[0].ref, ref(kCascade, 0, 5));
    EXPECT_EQ(shared.joints[0].position_slot, shared.states[0].slot);
}

TEST(ResolveCommands, StatePositionOverrideStillNeedsAPositionCommandOrState)
{
    // The exception covers `state.position` on a joint with a position command only.
    expectRefused([] { resolveCommands({cjoint("j", "wl", {"velocity"}, {"velocity"}, {{"state.position", "position"}})}); },
                  {"'j'", "'state.position'", "does not declare", "declared: velocity"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"duty"}, {{"state.position", "position"}})}); },
                  {"'state.position'", "does not declare"});
    // state.velocity is never covered by it.
    expectRefused([] { resolveCommands({cjoint("j", "wl", {"duty"}, {"position"}, {{"state.velocity", "velocity"}})}); },
                  {"'state.velocity'", "does not declare"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"position"}, {{"state.position", ""}})}); },
                  {"'state.position'", "empty"});
}

TEST(ResolveCommands, MissingPositionSignalRefused)
{
    // The type has no signal named "position".
    expectRefused([] { resolveCommands({cjoint("j", "plain", {}, {"position"}, {{"command.position", "velocity_sp"}})}); },
                  {"joint", "'j'", "position command hold", "default item 'position'", "'plain'", "no such item",
                   "available signals: velocity", "the position command needs the measured position for its hold"});
    // An override naming a missing item, or a non-signal.
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"position"}, {{"state.position", "nope"}})}); },
                  {"param 'state.position' = 'nope'", "no such item", "for its hold"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"position"}, {{"state.position", "position_sp"}})}); },
                  {"param 'state.position' = 'position_sp'", "item is an input, not a signal", "for its hold"});
    expectRefused([] { resolveCommands({cjoint("j", "wl", {}, {"position"}, {{"state.position", "mode"}})}); },
                  {"item is a param, not a signal", "for its hold"});
    // A velocity command needs no position.
    EXPECT_NO_THROW(resolveCommands({cjoint("j", "plain", {}, {"velocity"})}));
}

TEST(ResolveCommands, HiddenSlotFollowsItsComponentsStateRefs)
{
    // a: state velocity + hidden position; b: state duty. Refs: a.velocity, a.position, b.duty.
    const auto r = resolveCommands({cjoint("a", "wl", {"velocity"}, {"position"}),
                                    cjoint("b", "wr", {"duty"}, {"velocity"})});
    ASSERT_EQ(r.groups.size(), 1u);
    EXPECT_EQ(r.groups[0].refs,
              (std::vector<drivelink::Ref>{ref(kCascade, 0, 3), ref(kCascade, 0, 2), ref(kCascade, 1, 4)}));
    EXPECT_EQ(r.states[0].slot, 0u);
    EXPECT_EQ(r.states[1].slot, 2u);
    EXPECT_EQ(r.joints[0].position_slot, 1u);
    EXPECT_FALSE(r.joints[1].position_group.has_value());
}

TEST(ResolveCommands, PositionHoldOnItsOwnLoopAddsThatLoopsGroup)
{
    // The state signals are on loop 1 ("slow"), the position hold's joint on loop 0.
    const auto r = resolveCommands({cjoint("s", "slow", {"velocity"}, {"velocity"}),
                                    cjoint("w", "wl", {}, {"position"})});
    ASSERT_EQ(r.groups.size(), 2u);
    EXPECT_EQ(r.groups[0].loop, 0);
    EXPECT_EQ(r.groups[1].loop, 1);
    EXPECT_EQ(*r.joints[1].position_group, 0u);
    EXPECT_EQ(r.joints[1].position_slot, 0u);
    EXPECT_EQ(r.states[0].group, 1u);
}

TEST(ResolveCommands, HiddenSlotCountsTowardTheSixtyFourLimit)
{
    auto snap = makeCommandDevice();
    std::vector<Item> items;
    std::vector<std::string> names;
    for (uint16_t i = 0; i < 64; ++i)
    {
        names.push_back("s" + std::to_string(i));
        items.push_back({i, names.back(), ItemKind::Signal, ValueType::F32});
    }
    items.push_back({100, "position", ItemKind::Signal, ValueType::F32});
    items.push_back({200, "position_sp", ItemKind::Input, ValueType::F32});
    addType(snap, 6, "full", items);
    addInstance(snap, 6, 0, "full", 0);
    EXPECT_NO_THROW(resolve(snap, {cjoint("j", "full", names, {})}, config()));
    expectRefused([&] { resolve(snap, {cjoint("j", "full", names, {"position"})}, config()); },
                  {"loop 0", "64", "'j' position hold"});
}

// ---- command groups ----------------------------------------------------

TEST(ResolveCommands, CommandGroupsPerLoopAscending)
{
    // slow is on loop 1, wl and wr on loop 0.
    const auto r = resolveCommands({cjoint("slow_j", "slow", {}, {"velocity"}),
                                    cjoint("left", "wl", {}, {"velocity", "duty"}),
                                    cjoint("right", "wr", {}, {"duty"})});
    ASSERT_EQ(r.command_groups.size(), 2u);
    EXPECT_EQ(r.command_groups[0].loop, 0);
    EXPECT_EQ(r.command_groups[1].loop, 1);
    EXPECT_EQ(r.command_groups[0].refs,
              (std::vector<drivelink::Ref>{ref(kCascade, 0, 512), ref(kCascade, 0, 514), ref(kCascade, 1, 514)}));
    EXPECT_EQ(r.command_groups[1].refs, (std::vector<drivelink::Ref>{ref(kCascade, 2, 512)}));
    ASSERT_EQ(r.commands.size(), 4u);
    struct Expected
    {
        const char *component;
        const char *interface_name;
        size_t group;
        size_t slot;
        size_t joint;
    };
    const Expected expected[] = {
        {"slow_j", "velocity", 1, 0, 0},
        {"left", "velocity", 0, 0, 1},
        {"left", "duty", 0, 1, 1},
        {"right", "duty", 0, 2, 2},
    };
    for (size_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(r.commands[i].component, expected[i].component) << i;
        EXPECT_EQ(r.commands[i].interface_name, expected[i].interface_name) << i;
        EXPECT_EQ(r.commands[i].group, expected[i].group) << i;
        EXPECT_EQ(r.commands[i].slot, expected[i].slot) << i;
        EXPECT_EQ(r.commands[i].joint, expected[i].joint) << i;
        EXPECT_EQ(r.command_groups[r.commands[i].group].refs[r.commands[i].slot], r.commands[i].ref) << i;
        EXPECT_EQ(r.command_groups[r.commands[i].group].types[r.commands[i].slot], r.commands[i].type) << i;
        EXPECT_EQ(r.command_groups[r.commands[i].group].loop, r.commands[i].loop) << i;
    }
    ASSERT_EQ(r.joints.size(), 3u);
    EXPECT_EQ(r.joints[0].name, "slow_j");
    EXPECT_EQ(r.joints[1].name, "left");
    EXPECT_EQ(r.joints[2].name, "right");
    EXPECT_EQ(r.joints[1].commands[idx(CommandKind::Velocity)], std::optional<size_t>(1));
    EXPECT_EQ(r.joints[1].commands[idx(CommandKind::Duty)], std::optional<size_t>(2));
    EXPECT_EQ(r.joints[2].commands[idx(CommandKind::Duty)], std::optional<size_t>(3));
    // Every joint index and command index point at each other.
    for (size_t j = 0; j < r.joints.size(); ++j)
    {
        for (size_t k = 1; k < kCommandKinds; ++k)
        {
            const auto &index = r.joints[j].commands[k];
            if (!index)
                continue;
            EXPECT_EQ(r.commands[*index].joint, j);
            EXPECT_EQ(static_cast<size_t>(r.commands[*index].kind), k);
            EXPECT_EQ(r.commands[*index].component, r.joints[j].name);
        }
    }
}

TEST(ResolveCommands, SameJointSharingAnInputSharesTheSlot)
{
    // Two interfaces of one joint on one input: the refs are deduplicated.
    const auto r = resolveCommands({cjoint("j", "wl", {}, {"velocity", "duty"}, {{"command.duty", "velocity_sp"}})});
    ASSERT_EQ(r.commands.size(), 2u);
    EXPECT_EQ(r.commands[0].slot, 0u);
    EXPECT_EQ(r.commands[1].slot, 0u);
    ASSERT_EQ(r.command_groups.size(), 1u);
    EXPECT_EQ(r.command_groups[0].refs.size(), 1u);
    EXPECT_EQ(r.command_groups[0].types.size(), 1u);
}

TEST(ResolveCommands, JointsListHoldsOnlyCommandJoints)
{
    const auto r = resolveCommands({sensor("imu", "imu", {"orientation.w"}), cjoint("state_only", "wl", {"velocity"}, {}),
                                    cjoint("driven", "wr", {"velocity"}, {"velocity"})});
    ASSERT_EQ(r.joints.size(), 1u);
    EXPECT_EQ(r.joints[0].name, "driven");
    ASSERT_EQ(r.commands.size(), 1u);
    EXPECT_EQ(r.commands[0].joint, 0u);
    EXPECT_EQ(r.states.size(), 3u);
}

TEST(ResolveCommands, SixtyFourCommandRefsPerLoop)
{
    auto snap = makeCommandDevice();
    std::vector<Item> items;
    for (uint16_t i = 0; i < 70; ++i)
        items.push_back({i, "i" + std::to_string(i), ItemKind::Input, ValueType::F32});
    addType(snap, kInputs, "inputs", items);
    addInstance(snap, kInputs, 0, "many", 0);
    addInstance(snap, kInputs, 1, "slowmany", 1);
    // Joint k commands inputs 2k (velocity) and 2k + 1 (duty).
    auto joints = [&](size_t count, const char *instance) {
        std::vector<ComponentSpec> list;
        for (size_t k = 0; k < count; ++k)
            list.push_back(cjoint(("j" + std::to_string(k)).c_str(), instance, {}, {"velocity", "duty"},
                                  {{"command.velocity", "i" + std::to_string(2 * k)},
                                   {"command.duty", "i" + std::to_string(2 * k + 1)}}));
        return list;
    };
    const auto ok = resolve(snap, joints(32, "many"), config());
    ASSERT_EQ(ok.command_groups.size(), 1u);
    EXPECT_EQ(ok.command_groups[0].refs.size(), 64u);
    EXPECT_EQ(ok.commands.back().slot, 63u);
    expectRefused([&] { resolve(snap, joints(33, "many"), config()); },
                  {"loop 0", "64", "command", "'j32'", "'velocity'"});
    // 64 on each of two loops is fine.
    auto both = joints(32, "many");
    for (auto &spec : joints(32, "slowmany"))
    {
        spec.name = "s" + spec.name;
        both.push_back(std::move(spec));
    }
    const auto two = resolve(snap, both, config());
    ASSERT_EQ(two.command_groups.size(), 2u);
    EXPECT_EQ(two.command_groups[1].refs.size(), 64u);
}

// ---- planSwitch --------------------------------------------------------

namespace
{
constexpr CommandKind N = CommandKind::None;
constexpr CommandKind P = CommandKind::Position;
constexpr CommandKind V = CommandKind::Velocity;
constexpr CommandKind D = CommandKind::Duty;

// Joints 0: left_wheel (position, velocity, duty), 1: right_wheel (the same).
Resolved switchResolved()
{
    return resolveCommands({cjoint("left_wheel", "wl", {"velocity"}, {"position", "velocity", "duty"}),
                            cjoint("right_wheel", "wr", {"velocity"}, {"position", "velocity", "duty"})});
}
} // namespace

TEST(PlanSwitch, Table)
{
    struct Case
    {
        const char *name;
        Claims current;
        std::vector<std::string> start;
        std::vector<std::string> stop;
        Claims after;
        std::vector<size_t> claimed;
        std::vector<size_t> released;
        bool arm;
        bool disarm;
    };
    const std::vector<Case> cases = {
        {"empty switch", {N, N}, {}, {}, {N, N}, {}, {}, false, false},
        {"empty switch while claimed", {V, D}, {}, {}, {V, D}, {}, {}, false, false},
        {"first claim arms", {N, N}, {"left_wheel/velocity"}, {}, {V, N}, {0}, {}, true, false},
        {"second claim does not arm", {V, N}, {"right_wheel/duty"}, {}, {V, D}, {1}, {}, false, false},
        {"last release disarms", {V, N}, {}, {"left_wheel/velocity"}, {N, N}, {}, {0}, false, true},
        {"partial release does not disarm", {V, D}, {}, {"left_wheel/velocity"}, {N, D}, {}, {0}, false, false},
        {"release both", {V, D}, {}, {"right_wheel/duty", "left_wheel/velocity"}, {N, N}, {}, {0, 1}, false, true},
        {"mode change is a claim, not a release",
         {V, N},
         {"left_wheel/position"},
         {"left_wheel/velocity"},
         {P, N},
         {0},
         {},
         false,
         false},
        {"mode change on the only claimed joint of two does not arm or disarm",
         {N, D},
         {"left_wheel/position"},
         {"right_wheel/duty"},
         {P, N},
         {0},
         {1},
         false,
         false},
        {"controller hands over the same interface",
         {V, N},
         {"left_wheel/velocity"},
         {"left_wheel/velocity"},
         {V, N},
         {0},
         {},
         false,
         false},
        {"joints differ", {N, N}, {"left_wheel/position", "right_wheel/velocity"}, {}, {P, V}, {0, 1}, {}, true, false},
        {"claimed is ascending whatever the list order",
         {N, N},
         {"right_wheel/velocity", "left_wheel/position"},
         {},
         {P, V},
         {0, 1},
         {},
         true,
         false},
        {"claim moves between joints", {V, N}, {"right_wheel/velocity"}, {"left_wheel/velocity"}, {N, V}, {1}, {0}, false, false},
        {"stop of a non-current interface is ignored", {V, N}, {}, {"left_wheel/position"}, {V, N}, {}, {}, false, false},
        {"stop of an unclaimed joint is ignored", {N, N}, {}, {"right_wheel/velocity"}, {N, N}, {}, {}, false, false},
        {"stop of a non-current interface does not free the joint",
         {V, N},
         {},
         {"left_wheel/duty", "left_wheel/position"},
         {V, N},
         {},
         {},
         false,
         false},
        {"foreign names are ignored",
         {N, N},
         {"other_robot/velocity", "left_wheel/effort", "imu/orientation.x", "left_wheel", "/velocity", "left_wheel/",
          "left_wheel/velocity/x", "Left_wheel/velocity", ""},
         {"other_robot/velocity", "imu/orientation.x", "left_wheel/effort"},
         {N, N},
         {},
         {},
         false,
         false},
        {"foreign names do not hide the real ones",
         {N, N},
         {"imu/orientation.x", "right_wheel/duty", "left_wheel/effort"},
         {"other_robot/velocity"},
         {N, D},
         {1},
         {},
         true,
         false},
        {"the same stop twice is one stop", {V, N}, {}, {"left_wheel/velocity", "left_wheel/velocity"}, {N, N}, {}, {0}, false, true},
    };
    const auto resolved = switchResolved();
    for (const auto &c : cases)
    {
        SCOPED_TRACE(c.name);
        const auto plan = planSwitch(resolved, c.current, c.start, c.stop);
        EXPECT_EQ(plan.after, c.after);
        EXPECT_EQ(plan.claimed, c.claimed);
        EXPECT_EQ(plan.released, c.released);
        EXPECT_EQ(plan.arm, c.arm);
        EXPECT_EQ(plan.disarm, c.disarm);
    }
}

TEST(PlanSwitch, UndeclaredInterfacesOfAJointAreForeign)
{
    // Only velocity is declared: "p/duty" and "p/position" are other components' business.
    const auto resolved = resolveCommands({cjoint("p", "wl", {}, {"velocity"})});
    const Claims none = {N};
    auto plan = planSwitch(resolved, none, {"p/duty", "p/position"}, {"p/duty"});
    EXPECT_EQ(plan.after, none);
    EXPECT_TRUE(plan.claimed.empty());
    EXPECT_FALSE(plan.arm);
    plan = planSwitch(resolved, none, {"p/velocity"}, {});
    EXPECT_EQ(plan.after, (Claims{V}));
    EXPECT_TRUE(plan.arm);
    // A state interface of the same joint is not a command interface.
    plan = planSwitch(switchResolved(), Claims{N, N}, {"left_wheel/velocity.x"}, {});
    EXPECT_TRUE(plan.claimed.empty());
}

TEST(PlanSwitch, NoJointsIsANoOp)
{
    const auto resolved = resolveCommands({cjoint("s", "wl", {"velocity"}, {})});
    const auto plan = planSwitch(resolved, {}, {"s/velocity", "other/duty"}, {"s/duty"});
    EXPECT_TRUE(plan.after.empty());
    EXPECT_TRUE(plan.claimed.empty());
    EXPECT_TRUE(plan.released.empty());
    EXPECT_FALSE(plan.arm);
    EXPECT_FALSE(plan.disarm);
}

TEST(PlanSwitch, TwoStartsOnOneJointRefused)
{
    const auto resolved = switchResolved();
    expectRefused([&] { planSwitch(resolved, {N, N}, {"left_wheel/position", "left_wheel/velocity"}, {}); },
                  {"joint 'left_wheel'", "'left_wheel/position'", "'left_wheel/velocity'", "one command interface"});
    // Also across a stop: stopping does not make two claims one.
    expectRefused([&] { planSwitch(resolved, {V, N}, {"left_wheel/duty", "left_wheel/position"}, {"left_wheel/velocity"}); },
                  {"joint 'left_wheel'", "'left_wheel/duty'", "'left_wheel/position'"});
    // The same start twice is also refused: it is not one request.
    expectRefused([&] { planSwitch(resolved, {N, N}, {"left_wheel/velocity", "left_wheel/velocity"}, {}); },
                  {"joint 'left_wheel'", "'left_wheel/velocity'"});
}

TEST(PlanSwitch, StartOnAJointThatStaysClaimedRefused)
{
    const auto resolved = switchResolved();
    expectRefused([&] { planSwitch(resolved, {V, N}, {"left_wheel/position"}, {}); },
                  {"'left_wheel/position'", "joint 'left_wheel'", "still claims 'left_wheel/velocity'", "stop it"});
    // Starting the interface it already holds, without stopping it, is the same.
    expectRefused([&] { planSwitch(resolved, {V, N}, {"left_wheel/velocity"}, {}); },
                  {"'left_wheel/velocity'", "still claims"});
    // A stop of another interface does not release it.
    expectRefused([&] { planSwitch(resolved, {V, N}, {"left_wheel/position"}, {"left_wheel/duty"}); },
                  {"still claims 'left_wheel/velocity'"});
    // The other joint's stop does not either.
    expectRefused([&] { planSwitch(resolved, {V, D}, {"left_wheel/position"}, {"right_wheel/duty"}); },
                  {"joint 'left_wheel'"});
}

TEST(PlanSwitch, CurrentOfTheWrongSizeRefused)
{
    const auto resolved = switchResolved();
    expectRefused([&] { planSwitch(resolved, {}, {}, {}); }, {"0 claims", "2 joints"});
    expectRefused([&] { planSwitch(resolved, {N}, {"left_wheel/velocity"}, {}); }, {"1 claims", "2 joints"});
    expectRefused([&] { planSwitch(resolved, {N, N, N}, {}, {}); }, {"3 claims", "2 joints"});
    expectRefused([] { planSwitch(Resolved{}, {N}, {}, {}); }, {"1 claims", "0 joints"});
}

// ---- slotValue ---------------------------------------------------------

TEST(SlotValue, ClaimedFiniteCommandIsSent)
{
    for (const auto kind : {CommandKind::Position, CommandKind::Velocity, CommandKind::Duty})
    {
        EXPECT_EQ(slotValue(kind, true, 1.5, 9.0), 1.5);
        EXPECT_EQ(slotValue(kind, true, -2.25, 9.0), -2.25);
        EXPECT_EQ(slotValue(kind, true, 0.0, 9.0), 0.0);
        EXPECT_EQ(slotValue(kind, true, 1e300, 9.0), 1e300);
        EXPECT_EQ(slotValue(kind, true, -std::numeric_limits<double>::max(), 9.0), -std::numeric_limits<double>::max());
    }
}

TEST(SlotValue, UnclaimedHoldsPerKind)
{
    EXPECT_EQ(slotValue(CommandKind::Velocity, false, 3.0, 9.0), 0.0);
    EXPECT_EQ(slotValue(CommandKind::Duty, false, 3.0, 9.0), 0.0);
    EXPECT_EQ(slotValue(CommandKind::Position, false, 3.0, 9.0), 9.0);
    EXPECT_EQ(slotValue(CommandKind::Position, false, 3.0, -4.5), -4.5);
}

TEST(SlotValue, NonFiniteCommandHolds)
{
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();
    for (const double bad : {nan, inf, -inf})
    {
        EXPECT_EQ(slotValue(CommandKind::Velocity, true, bad, 9.0), 0.0);
        EXPECT_EQ(slotValue(CommandKind::Duty, true, bad, 9.0), 0.0);
        EXPECT_EQ(slotValue(CommandKind::Position, true, bad, 9.0), 9.0);
        EXPECT_EQ(slotValue(CommandKind::Position, false, bad, 9.0), 9.0);
        EXPECT_EQ(slotValue(CommandKind::Velocity, false, bad, 9.0), 0.0);
    }
}

TEST(SlotValue, NoneIsZero)
{
    EXPECT_EQ(slotValue(CommandKind::None, true, 5.0, 9.0), 0.0);
    EXPECT_EQ(slotValue(CommandKind::None, false, 5.0, 9.0), 0.0);
    EXPECT_EQ(slotValue(CommandKind::None, true, std::numeric_limits<double>::quiet_NaN(), 9.0), 0.0);
}

TEST(SlotValue, IsConstexpr)
{
    static_assert(slotValue(CommandKind::Velocity, true, 2.0, 0.0) == 2.0);
    static_assert(slotValue(CommandKind::Position, true, std::numeric_limits<double>::quiet_NaN(), 7.0) == 7.0);
    static_assert(slotValue(CommandKind::Position, false, 1.0, 7.0) == 7.0);
    // (An infinite command is not constant-evaluable with the header's `command - command == 0` test.)
}

// ---- commandKind and commandName ---------------------------------------

TEST(CommandKindNames, RoundTrip)
{
    EXPECT_EQ(commandKind("position"), std::optional<CommandKind>(CommandKind::Position));
    EXPECT_EQ(commandKind("velocity"), std::optional<CommandKind>(CommandKind::Velocity));
    EXPECT_EQ(commandKind("duty"), std::optional<CommandKind>(CommandKind::Duty));
    for (const char *bad : {"none", "", "Position", "effort", "position ", "velocity_sp"})
        EXPECT_FALSE(commandKind(bad).has_value()) << bad;
    EXPECT_STREQ(commandName(CommandKind::Position), "position");
    EXPECT_STREQ(commandName(CommandKind::Velocity), "velocity");
    EXPECT_STREQ(commandName(CommandKind::Duty), "duty");
    EXPECT_STREQ(commandName(CommandKind::None), "none");
    for (const auto kind : {CommandKind::Position, CommandKind::Velocity, CommandKind::Duty})
        EXPECT_EQ(commandKind(commandName(kind)), std::optional<CommandKind>(kind));
}
