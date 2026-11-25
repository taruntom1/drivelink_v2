#pragma once
// drivelink-agent --list-ports: the device lines for the ESP ports on this
// host, ready to paste into the config's "devices" (host/agent/README.md,
// "Install"). It only reads names under /dev and /sys: it never opens a port,
// because opening one toggles DTR/RTS, which can reset an ESP.
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace drivelink::agent
{
struct PortInfo
{
    std::string path; // the stable name to put in the config, e.g. /dev/drivelink/<serial>
    std::string kind; // what it is, for the human reading the list
};

// The ports, in this order: /dev/drivelink/* (the udev rule's names for USB
// ESPs, sorted), /dev/serial0, then /dev/ttyAMA* (sorted) that serial0 does
// not already name. `dev` and `sys` are the roots (tests use temporary trees).
std::vector<PortInfo> findPorts(const std::filesystem::path &dev = "/dev", const std::filesystem::path &sys = "/sys");

// What a USB serial device is, from its vendor and product ids (lowercase hex).
std::string usbKind(std::string_view vendor, std::string_view product);

// A valid, unused alias for the port at `path` ("esp-" and the end of the USB
// serial, "esp-uart" for serial0, "esp-ttyama0"; "-2", "-3"... when taken).
std::string aliasFor(std::string_view path, const std::set<std::string> &taken);

// The device line: {"alias": ..., "endpoint": "serial://<path>", "baud": "auto"} (or a fixed rate when nonzero).
std::string deviceLine(const std::string &alias, const std::string &path, unsigned baud = 0);

// The whole --list-ports text for `ports` (a comment line per port, then its device line).
std::string portListing(const std::vector<PortInfo> &ports);
} // namespace drivelink::agent
