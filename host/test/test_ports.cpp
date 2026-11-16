#include "doctest.h"
#include "drivelink/ports.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace drivelink;
namespace fs = std::filesystem;

namespace
{
void write(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text << "\n";
}
struct TempDir
{
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path() /
               ("drivelink-ports-" + std::to_string(reinterpret_cast<uintptr_t>(this)) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
} // namespace

TEST_CASE("serial port hardware ids yield USB VID and PID")
{
    uint16_t vid = 0, pid = 0;
    CHECK(ports_detail::parseHardwareId("USB\\VID_1A86&PID_55D3&REV_0445", vid, pid));
    CHECK(vid == 0x1A86);
    CHECK(pid == 0x55D3);
    CHECK(ports_detail::parseHardwareId("FTDIBUS\\VID_0403+PID_6001+A50285BIA\\0000", vid, pid));
    CHECK(vid == 0x0403);
    CHECK(pid == 0x6001);
    CHECK(ports_detail::parseHardwareId("usb\\vid_303a&pid_1001&mi_00", vid, pid));
    CHECK(pid == 0x1001);
    vid = pid = 7;
    CHECK_FALSE(ports_detail::parseHardwareId("ACPI\\PNP0501", vid, pid));
    CHECK_FALSE(ports_detail::parseHardwareId("USB\\VID_12&PID_55D3", vid, pid));
    CHECK(vid == 7); // unchanged on failure
}

TEST_CASE("serial ports sort in natural order")
{
    CHECK(ports_detail::naturalLess("COM3", "COM10"));
    CHECK_FALSE(ports_detail::naturalLess("COM10", "COM3"));
    CHECK(ports_detail::naturalLess("/dev/ttyACM0", "/dev/ttyUSB0"));
    CHECK(ports_detail::naturalLess("com2", "COM3"));
    CHECK(ports_detail::naturalLess("COM3", "COM3a"));
    CHECK_FALSE(ports_detail::naturalLess("COM3", "COM3"));
    std::vector<SerialPortInfo> ports(4);
    ports[0].name = "COM10";
    ports[1].name = "COM4";
    ports[2].name = "COM3";
    ports[3].name = "COM100";
    ports_detail::sortPorts(ports);
    CHECK(ports[0].name == "COM3");
    CHECK(ports[1].name == "COM4");
    CHECK(ports[2].name == "COM10");
    CHECK(ports[3].name == "COM100");
}

TEST_CASE("Linux sysfs listing finds USB serial ports and skips virtual terminals")
{
    TempDir root;
    const auto tty = root.path / "class" / "tty";
    // A USB adapter: attributes live on an ancestor of the device directory.
    write(tty / "ttyUSB1" / "idVendor", "1a86");
    write(tty / "ttyUSB1" / "idProduct", "55d3");
    write(tty / "ttyUSB1" / "manufacturer", "WCH");
    write(tty / "ttyUSB1" / "product", "USB Single Serial");
    write(tty / "ttyUSB1" / "serial", "5A7B012345");
    fs::create_directories(tty / "ttyUSB1" / "device");
    // A port without USB attributes (on-board UART).
    fs::create_directories(tty / "ttyAMA0" / "device");
    // A virtual terminal has no device directory.
    fs::create_directories(tty / "tty1");
    write(tty / "ttyUSB0" / "idVendor", "303a");
    write(tty / "ttyUSB0" / "idProduct", "1001");
    fs::create_directories(tty / "ttyUSB0" / "device");

    const auto ports = ports_detail::listSysfs(tty, "/dev");
    REQUIRE(ports.size() == 3);
    CHECK(ports[0].name == "/dev/ttyAMA0");
    CHECK(ports[0].endpoint == "serial:///dev/ttyAMA0");
    CHECK_FALSE(ports[0].vid);
    CHECK(ports[1].name == "/dev/ttyUSB0");
    CHECK(ports[1].vid == 0x303a);
    CHECK(ports[1].description.empty());
    CHECK(ports[2].name == "/dev/ttyUSB1");
    CHECK(ports[2].vid == 0x1a86);
    CHECK(ports[2].pid == 0x55d3);
    CHECK(ports[2].description == "WCH USB Single Serial");
    CHECK(ports[2].serial_number == "5A7B012345");
    CHECK(ports_detail::listSysfs(root.path / "missing", "/dev").empty());
}

TEST_CASE("listing serial ports never throws")
{
    const auto ports = listSerialPorts();
    for (const auto &port : ports)
    {
        CHECK_FALSE(port.name.empty());
        CHECK(port.endpoint == "serial://" + port.name);
    }
}
