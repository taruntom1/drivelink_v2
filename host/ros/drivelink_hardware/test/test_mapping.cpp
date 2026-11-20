#include "drivelink_hardware/mapping.hpp"
#include <gtest/gtest.h>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
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

TEST(Resolve, CommandInterfacesRefused)
{
    auto spec = joint("j", "left", {"position"});
    spec.command_interfaces = {"velocity"};
    expectRefused([&] { resolve(makeDevice(), {spec}, config()); },
                  {"joint", "'j'", "command interface", "'velocity'", "H4b"});
}

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
