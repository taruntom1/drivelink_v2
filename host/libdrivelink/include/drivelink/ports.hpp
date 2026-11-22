#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace drivelink
{
// A serial port seen by the operating system. Listing never opens a port:
// opening an ESP32 auto-reset adapter can toggle DTR/RTS and reset the board.
struct SerialPortInfo
{
    std::string name;        // COM3, /dev/ttyUSB0
    std::string endpoint;    // serial://COM3, serial:///dev/ttyUSB0
    std::string description; // driver/USB product text; may be empty
    std::optional<uint16_t> vid;
    std::optional<uint16_t> pid;
    std::string serial_number;
};

// Ports present now, sorted by name in natural order (COM3 before COM10).
// Returns an empty list when the platform offers no enumeration.
std::vector<SerialPortInfo> listSerialPorts();

namespace ports_detail
{
// Parses VID/PID from a Windows hardware id such as
// "USB\VID_1A86&PID_55D3&REV_0445" or "FTDIBUS\VID_0403+PID_6001+...".
bool parseHardwareId(std::string_view id, uint16_t &vid, uint16_t &pid);
// Natural ordering: digit runs compare by value, other text case-insensitively.
bool naturalLess(std::string_view a, std::string_view b);
// Linux sysfs listing below `sys_class_tty` (normally /sys/class/tty), naming
// devices below `dev`. Pure filesystem reads; used by listSerialPorts on Linux
// and by tests on every platform.
std::vector<SerialPortInfo> listSysfs(const std::filesystem::path &sys_class_tty,
                                      const std::filesystem::path &dev);
void sortPorts(std::vector<SerialPortInfo> &ports);
} // namespace ports_detail
} // namespace drivelink
