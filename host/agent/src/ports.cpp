// drivelink-agent --list-ports (see ports.hpp).
#include "ports.hpp"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

namespace drivelink::agent
{
namespace fs = std::filesystem;

namespace
{
std::string readLine(const fs::path &file)
{
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}

// The tty name a /dev entry ends at (ttyACM0 for /dev/drivelink/<serial>).
std::string ttyName(const fs::path &entry)
{
    std::error_code ec;
    const auto real = fs::canonical(entry, ec);
    return (ec ? entry : real).filename().string();
}

// The USB device above /sys/class/tty/<name>: its idVendor/idProduct.
std::string usbKindOf(const fs::path &sys, const std::string &tty)
{
    std::error_code ec;
    auto dir = fs::canonical(sys / "class/tty" / tty / "device", ec);
    if (ec)
        return "serial port";
    for (int up = 0; up < 4 && !dir.empty() && dir != dir.root_path(); ++up, dir = dir.parent_path())
    {
        if (fs::exists(dir / "idVendor", ec) && fs::exists(dir / "idProduct", ec))
            return usbKind(readLine(dir / "idVendor"), readLine(dir / "idProduct"));
    }
    return "serial port";
}

std::vector<fs::path> sortedEntries(const fs::path &dir, std::string_view prefix)
{
    std::vector<fs::path> found;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->path().filename().string().starts_with(prefix))
            found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    return found;
}

bool aliasChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
} // namespace

std::string usbKind(std::string_view vendor, std::string_view product)
{
    if (vendor == "1a86" && product == "55d3")
        return "CH343 USB serial adapter";
    if (vendor == "303a" && product == "1001")
        return "ESP32-S3 native USB (the DriveLink firmware's console, not its protocol port)";
    return "USB serial (" + std::string(vendor) + ":" + std::string(product) + ")";
}

std::vector<PortInfo> findPorts(const fs::path &dev, const fs::path &sys)
{
    std::vector<PortInfo> ports;
    for (const auto &entry : sortedEntries(dev / "drivelink", ""))
        ports.push_back({"/dev/drivelink/" + entry.filename().string(), usbKindOf(sys, ttyName(entry))});
    std::error_code ec;
    std::string serial0;
    if (fs::exists(dev / "serial0", ec))
    {
        serial0 = ttyName(dev / "serial0");
        ports.push_back({"/dev/serial0", "GPIO UART (" + serial0 + ")"});
    }
    for (const auto &entry : sortedEntries(dev, "ttyAMA"))
    {
        const auto name = entry.filename().string();
        if (name != serial0)
            ports.push_back({"/dev/" + name, "UART " + name + " (on a Pi 5, ttyAMA0 is GPIO 14/15 and ttyAMA10 the debug header)"});
    }
    return ports;
}

std::string aliasFor(std::string_view path, const std::set<std::string> &taken)
{
    std::string base;
    if (path == "/dev/serial0")
        base = "esp-uart";
    else
    {
        const auto slash = path.rfind('/');
        std::string tail;
        for (char c : path.substr(slash == std::string_view::npos ? 0 : slash + 1))
        {
            const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            if (aliasChar(lower))
                tail.push_back(lower);
        }
        if (tail.size() > 8)
            tail.erase(0, tail.size() - 8); // the end of a serial number differs between boards
        base = tail.empty() ? "esp" : "esp-" + tail;
    }
    std::string alias = base;
    for (int n = 2; taken.contains(alias); ++n)
        alias = base + "-" + std::to_string(n);
    return alias;
}

std::string deviceLine(const std::string &alias, const std::string &path, unsigned baud)
{
    nlohmann::ordered_json line;
    line["alias"] = alias;
    line["endpoint"] = "serial://" + path;
    if (baud == 0)
        line["baud"] = "auto";
    else
        line["baud"] = baud;
    return line.dump();
}

std::string portListing(const std::vector<PortInfo> &ports)
{
    std::string text = "# Add a line to \"devices\" in /etc/drivelink/agent.json (entries are separated by commas),\n"
                       "# then run: sudo systemctl restart drivelink-agent\n";
    std::set<std::string> taken;
    for (const auto &port : ports)
    {
        const auto alias = aliasFor(port.path, taken);
        taken.insert(alias);
        text += "# " + port.path + ": " + port.kind + "\n" + deviceLine(alias, port.path) + "\n";
    }
    return text;
}
} // namespace drivelink::agent
