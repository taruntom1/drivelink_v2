// drivelink-agent --list-ports: aliases, device lines and the port scan over a
// temporary /dev and /sys tree.
#include "config.hpp"
#include "ports.hpp"
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace drivelink::agent;
namespace fs = std::filesystem;

namespace
{
struct Tree
{
    fs::path root = fs::temp_directory_path() / ("drivelink-ports-" + std::to_string(::getpid()));
    Tree() { fs::remove_all(root); fs::create_directories(root); }
    ~Tree() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path dev() const { return root / "dev"; }
    fs::path sys() const { return root / "sys"; }
    void file(const fs::path &p, const std::string &content = "") const
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }
    void link(const fs::path &at, const fs::path &target) const
    {
        fs::create_directories(at.parent_path());
        fs::create_directory_symlink(target, at);
    }
    // A USB tty as the kernel shows it: /sys/class/tty/<tty>/device -> the
    // interface directory, whose parent has idVendor and idProduct.
    void usbTty(const std::string &tty, const std::string &vendor, const std::string &product) const
    {
        const auto usb = sys() / "devices/usb1" / tty;
        file(usb / "idVendor", vendor + "\n");
        file(usb / "idProduct", product + "\n");
        fs::create_directories(usb / "1-1:1.0");
        link(sys() / "class/tty" / tty / "device", usb / "1-1:1.0");
        file(dev() / tty);
    }
};

std::string configWith(const std::string &line) { return "{\"token\":\"abcdefgh\",\"devices\":[" + line + "]}"; }
} // namespace

TEST_CASE("ports: aliases are valid, short and unique")
{
    CHECK(aliasFor("/dev/drivelink/5A7B012345", {}) == "esp-7b012345");
    CHECK(aliasFor("/dev/drivelink/AB:CD", {}) == "esp-abcd");
    CHECK(aliasFor("/dev/drivelink/---", {}) == "esp");
    CHECK(aliasFor("/dev/serial0", {}) == "esp-uart");
    CHECK(aliasFor("/dev/ttyAMA0", {}) == "esp-ttyama0");
    CHECK(aliasFor("/dev/serial0", {"esp-uart"}) == "esp-uart-2");
    CHECK(aliasFor("/dev/serial0", {"esp-uart", "esp-uart-2"}) == "esp-uart-3");
    // Every alias the helper makes passes the config's alias rule.
    for (const auto *path : {"/dev/drivelink/0123456789ABCDEF", "/dev/drivelink/x", "/dev/serial0", "/dev/ttyAMA10"})
    {
        const auto alias = aliasFor(path, {});
        CHECK_NOTHROW(validate(parseConfig(configWith(deviceLine(alias, path)))));
    }
}

TEST_CASE("ports: the device line parses as a config entry")
{
    const auto line = deviceLine("esp-uart", "/dev/serial0");
    CHECK(line == R"({"alias":"esp-uart","endpoint":"serial:///dev/serial0","baud":921600})");
    const auto config = parseConfig(configWith(line));
    REQUIRE(config.devices.size() == 1);
    CHECK(config.devices[0].endpoint == "serial:///dev/serial0");
    CHECK(config.devices[0].baud == 921600);
    // A serial number with JSON's special characters stays valid JSON.
    CHECK_NOTHROW(parseConfig(configWith(deviceLine("esp-x", "/dev/drivelink/a\"b\\c"))));
}

TEST_CASE("ports: adapter kinds")
{
    CHECK(usbKind("1a86", "55d3").starts_with("CH343"));
    CHECK(usbKind("303a", "1001").find("console") != std::string::npos);
    CHECK(usbKind("0403", "6001") == "USB serial (0403:6001)");
}

TEST_CASE("ports: an empty host has no ports")
{
    Tree t;
    CHECK(findPorts(t.dev(), t.sys()).empty());
}

TEST_CASE("ports: udev names, serial0 and the other UARTs, in order")
{
    Tree t;
    t.usbTty("ttyACM0", "1a86", "55d3");
    t.usbTty("ttyACM1", "303a", "1001");
    fs::create_directories(t.dev() / "drivelink");
    fs::create_symlink("../ttyACM1", t.dev() / "drivelink/F4122EB0");
    fs::create_symlink("../ttyACM0", t.dev() / "drivelink/5A7B012345");
    t.file(t.dev() / "ttyAMA0");
    t.file(t.dev() / "ttyAMA10");
    fs::create_symlink("ttyAMA0", t.dev() / "serial0");
    t.file(t.dev() / "ttyACM2"); // not named by udev: not listed

    const auto ports = findPorts(t.dev(), t.sys());
    REQUIRE(ports.size() == 4);
    CHECK(ports[0].path == "/dev/drivelink/5A7B012345");
    CHECK(ports[0].kind.starts_with("CH343"));
    CHECK(ports[1].path == "/dev/drivelink/F4122EB0");
    CHECK(ports[1].kind.find("console") != std::string::npos);
    CHECK(ports[2].path == "/dev/serial0");
    CHECK(ports[2].kind == "GPIO UART (ttyAMA0)");
    CHECK(ports[3].path == "/dev/ttyAMA10"); // ttyAMA0 is serial0 already

    const auto text = portListing(ports);
    CHECK(text.find(R"({"alias":"esp-7b012345","endpoint":"serial:///dev/drivelink/5A7B012345","baud":921600})") !=
          std::string::npos);
    CHECK(text.find(R"("alias":"esp-uart")") != std::string::npos);
    CHECK(text.find("systemctl restart drivelink-agent") != std::string::npos);
}

TEST_CASE("ports: a tty without USB parents is a plain serial port")
{
    Tree t;
    t.file(t.dev() / "ttyS0");
    fs::create_directories(t.dev() / "drivelink");
    fs::create_symlink("../ttyS0", t.dev() / "drivelink/abc");
    const auto ports = findPorts(t.dev(), t.sys());
    REQUIRE(ports.size() == 1);
    CHECK(ports[0].kind == "serial port");
}
