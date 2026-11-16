#include "drivelink/ports.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#endif

namespace drivelink
{
namespace ports_detail
{
namespace
{
bool hexValue(std::string_view text, uint16_t &out)
{
    if (text.size() < 4)
    {
        return false;
    }
    uint32_t value = 0;
    for (char ch : text.substr(0, 4))
    {
        const auto c = static_cast<unsigned char>(ch);
        if (!std::isxdigit(c))
        {
            return false;
        }
        value = value * 16 + static_cast<uint32_t>(std::isdigit(c) ? c - '0' : std::tolower(c) - 'a' + 10);
    }
    out = static_cast<uint16_t>(value);
    return true;
}
std::string upper(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}
std::string readLine(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::string line;
    if (in)
    {
        std::getline(in, line);
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
    {
        line.pop_back();
    }
    return line;
}
} // namespace

bool parseHardwareId(std::string_view id, uint16_t &vid, uint16_t &pid)
{
    const auto text = upper(id);
    const auto v = text.find("VID_");
    const auto p = text.find("PID_");
    if (v == std::string::npos || p == std::string::npos)
    {
        return false;
    }
    uint16_t a = 0, b = 0;
    if (!hexValue(std::string_view(text).substr(v + 4), a) || !hexValue(std::string_view(text).substr(p + 4), b))
    {
        return false;
    }
    vid = a;
    pid = b;
    return true;
}

bool naturalLess(std::string_view a, std::string_view b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size())
    {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb))
        {
            size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei])))
            {
                ++ei;
            }
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej])))
            {
                ++ej;
            }
            auto da = a.substr(i, ei - i), db = b.substr(j, ej - j);
            while (da.size() > 1 && da.front() == '0')
            {
                da.remove_prefix(1);
            }
            while (db.size() > 1 && db.front() == '0')
            {
                db.remove_prefix(1);
            }
            if (da.size() != db.size())
            {
                return da.size() < db.size();
            }
            if (da != db)
            {
                return da < db;
            }
            i = ei;
            j = ej;
            continue;
        }
        const auto la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb)
        {
            return la < lb;
        }
        ++i;
        ++j;
    }
    if ((a.size() - i) != (b.size() - j))
    {
        return (a.size() - i) < (b.size() - j);
    }
    return a < b; // stable tie-break for case/leading-zero differences
}

void sortPorts(std::vector<SerialPortInfo> &ports)
{
    std::ranges::sort(ports, [](const auto &x, const auto &y) { return naturalLess(x.name, y.name); });
}

std::vector<SerialPortInfo> listSysfs(const std::filesystem::path &sys_class_tty, const std::filesystem::path &dev)
{
    namespace fs = std::filesystem;
    std::vector<SerialPortInfo> out;
    std::error_code ec;
    fs::directory_iterator it(sys_class_tty, ec);
    if (ec)
    {
        return out;
    }
    // Increment with an error code: a vanishing entry must not throw.
    for (; it != fs::directory_iterator(); it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        const auto &entry = *it;
        const auto name = entry.path().filename().string();
        const auto device = entry.path() / "device";
        if (!fs::exists(device, ec))
        {
            continue; // virtual terminal, pty
        }
        if (name.starts_with("ttyS"))
        {
            // Legacy 8250 placeholders exist for absent UARTs; keep ports
            // whose device sits on a real bus (pnp, pci, amba).
            const auto subsystem = fs::canonical(device / "subsystem", ec);
            if (!ec && subsystem.filename() == "platform")
            {
                continue;
            }
            ec.clear();
        }
        SerialPortInfo port;
        port.name = (dev / name).generic_string();
        port.endpoint = "serial://" + port.name;
        auto node = fs::canonical(device, ec);
        if (ec)
        {
            node = device;
            ec.clear();
        }
        for (int depth = 0; depth < 8 && !node.empty(); ++depth)
        {
            if (fs::exists(node / "idVendor", ec))
            {
                uint16_t value = 0;
                if (hexValue(readLine(node / "idVendor"), value))
                {
                    port.vid = value;
                }
                if (hexValue(readLine(node / "idProduct"), value))
                {
                    port.pid = value;
                }
                const auto maker = readLine(node / "manufacturer");
                const auto product = readLine(node / "product");
                port.description = maker.empty() ? product : product.empty() ? maker : maker + " " + product;
                port.serial_number = readLine(node / "serial");
                break;
            }
            if (node == node.parent_path())
            {
                break;
            }
            node = node.parent_path();
        }
        out.push_back(std::move(port));
    }
    sortPorts(out);
    return out;
}
} // namespace ports_detail

#ifdef _WIN32
namespace
{
std::string property(HDEVINFO set, SP_DEVINFO_DATA &data, DWORD which)
{
    char buffer[512]{};
    DWORD type = 0;
    if (!::SetupDiGetDeviceRegistryPropertyA(set, &data, which, &type, reinterpret_cast<PBYTE>(buffer),
                                             sizeof(buffer) - 2, nullptr))
    {
        return {};
    }
    return std::string(buffer); // first string of a REG_MULTI_SZ
}
} // namespace

std::vector<SerialPortInfo> listSerialPorts()
{
    std::vector<SerialPortInfo> out;
    const HDEVINFO set = ::SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
    {
        return out;
    }
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD i = 0; ::SetupDiEnumDeviceInfo(set, i, &data); ++i)
    {
        const HKEY key = ::SetupDiOpenDevRegKey(set, &data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE)
        {
            continue;
        }
        char name[64]{};
        DWORD size = sizeof(name) - 1, type = 0;
        const auto status = ::RegQueryValueExA(key, "PortName", nullptr, &type, reinterpret_cast<LPBYTE>(name), &size);
        ::RegCloseKey(key);
        if (status != ERROR_SUCCESS || type != REG_SZ || std::string_view(name).substr(0, 3) != "COM")
        {
            continue; // LPT and other non-serial entries of the Ports class
        }
        SerialPortInfo port;
        port.name = name;
        port.endpoint = "serial://" + port.name;
        port.description = property(set, data, SPDRP_FRIENDLYNAME);
        if (const auto suffix = " (" + port.name + ")"; port.description.ends_with(suffix))
        {
            port.description.resize(port.description.size() - suffix.size());
        }
        uint16_t vid = 0, pid = 0;
        if (ports_detail::parseHardwareId(property(set, data, SPDRP_HARDWAREID), vid, pid))
        {
            port.vid = vid;
            port.pid = pid;
            char instance[256]{};
            if (::SetupDiGetDeviceInstanceIdA(set, &data, instance, sizeof(instance), nullptr))
            {
                // USB\VID_xxxx&PID_xxxx\SERIAL; a composite child has '&' in
                // the last part, which is a generated id, not a serial number.
                const std::string_view id(instance);
                if (const auto slash = id.rfind('\\'); slash != std::string_view::npos &&
                                                       id.substr(slash + 1).find('&') == std::string_view::npos)
                {
                    port.serial_number = std::string(id.substr(slash + 1));
                }
            }
        }
        out.push_back(std::move(port));
    }
    ::SetupDiDestroyDeviceInfoList(set);
    ports_detail::sortPorts(out);
    return out;
}
#elif defined(__linux__)
std::vector<SerialPortInfo> listSerialPorts()
{
    return ports_detail::listSysfs("/sys/class/tty", "/dev");
}
#else
std::vector<SerialPortInfo> listSerialPorts()
{
    return {};
}
#endif
} // namespace drivelink
