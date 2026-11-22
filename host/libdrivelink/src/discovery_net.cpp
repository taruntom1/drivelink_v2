// Agent discovery: interface listing and the browse round (sockets).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "drivelink/discovery.hpp"
#include "drivelink/error.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace drivelink::discovery
{
namespace
{
#ifdef _WIN32
using Socket = SOCKET;
using Length = int; // sendto/recvfrom buffer length
constexpr Socket kInvalid = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
struct NetInit
{
    bool ok;
    NetInit()
    {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~NetInit()
    {
        if (ok) { WSACleanup(); }
    }
};
#else
using Socket = int;
using Length = size_t;
constexpr Socket kInvalid = -1;
void closeSocket(Socket s) { ::close(s); }
struct NetInit
{
    bool ok = true;
};
#endif

constexpr uint32_t kGroup = 0xE00000FB; // 224.0.0.251

sockaddr_in ipv4(uint32_t address, uint16_t port)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(address);
    return a;
}

// A querier socket sending from an ephemeral port on one interface
// (address 0: the default interface). Unicast answers come back to it.
Socket openQuerier(uint32_t interface_address)
{
    const Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) { return kInvalid; }
    const int ttl = 255, loop = 1; // RFC 6762 11; loop: an agent on this host answers too
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char *>(&ttl), sizeof(ttl));
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char *>(&loop), sizeof(loop));
    if (interface_address != 0)
    {
        in_addr ifa{};
        ifa.s_addr = htonl(interface_address);
        if (setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char *>(&ifa), sizeof(ifa)) != 0)
        {
            closeSocket(s);
            return kInvalid;
        }
    }
    const auto local = ipv4(interface_address, 0);
    if (::bind(s, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0)
    {
        closeSocket(s);
        return kInvalid;
    }
    return s;
}

void sendQuery(Socket s)
{
    const auto query = buildQuery(kServiceType, 12, true);
    const auto group = ipv4(kGroup, kMdnsPort);
    ::sendto(s, reinterpret_cast<const char *>(query.data()), static_cast<Length>(query.size()), 0,
             reinterpret_cast<const sockaddr *>(&group), sizeof(group));
}
} // namespace

std::vector<Ipv4Interface> listIpv4Interfaces()
{
    std::vector<Ipv4Interface> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt)
    {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
    }
    if (result != NO_ERROR) { return out; }
    for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()); adapter; adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            (adapter->Flags & IP_ADAPTER_NO_MULTICAST))
        {
            continue;
        }
        std::string name;
        if (adapter->FriendlyName)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1)
            {
                name.resize(static_cast<size_t>(n - 1));
                WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, name.data(), n, nullptr, nullptr);
            }
        }
        for (auto *u = adapter->FirstUnicastAddress; u; u = u->Next)
        {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) { continue; }
            const auto *sin = reinterpret_cast<const sockaddr_in *>(u->Address.lpSockaddr);
            const uint32_t address = ntohl(sin->sin_addr.s_addr);
            if ((address >> 24) == 127) { continue; }
            const auto prefix = std::min<unsigned>(u->OnLinkPrefixLength, 32);
            const uint32_t mask = prefix == 0 ? 0 : 0xFFFFFFFFu << (32 - prefix);
            out.push_back({name, address, mask});
        }
    }
#else
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) { return out; }
    for (auto *i = list; i; i = i->ifa_next)
    {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) { continue; }
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_MULTICAST)) { continue; }
        const uint32_t address = ntohl(reinterpret_cast<const sockaddr_in *>(i->ifa_addr)->sin_addr.s_addr);
        if ((address >> 24) == 127) { continue; }
        const uint32_t mask =
            i->ifa_netmask ? ntohl(reinterpret_cast<const sockaddr_in *>(i->ifa_netmask)->sin_addr.s_addr) : 0;
        out.push_back({i->ifa_name ? i->ifa_name : "", address, mask});
    }
    freeifaddrs(list);
#endif
    return out;
}

std::vector<FoundAgent> browseAgents(std::chrono::milliseconds window)
{
    if (window < std::chrono::milliseconds(100) || window > std::chrono::milliseconds(10000))
    {
        throw Error(ErrorKind::Usage, "browse window must be 100..10000 ms");
    }
    const NetInit net;
    if (!net.ok) { return {}; }
    std::vector<Socket> sockets;
    for (const auto &i : listIpv4Interfaces())
    {
        if (const auto s = openQuerier(i.address); s != kInvalid) { sockets.push_back(s); }
    }
    if (sockets.empty())
    {
        if (const auto s = openQuerier(0); s != kInvalid) { sockets.push_back(s); }
    }
    Collector collector;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto deadline = start + window;
    auto resend = window >= std::chrono::milliseconds(600) ? start + std::chrono::milliseconds(300) : Clock::time_point::max();
    for (const auto s : sockets) { sendQuery(s); }
    std::array<uint8_t, 9000> buffer{};
    for (auto now = Clock::now(); now < deadline && !sockets.empty(); now = Clock::now())
    {
        if (now >= resend)
        {
            for (const auto s : sockets) { sendQuery(s); }
            resend = Clock::time_point::max();
        }
        const auto wait = std::min(deadline, resend) - now;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(wait).count();
        timeval tv{};
        tv.tv_sec = static_cast<decltype(tv.tv_sec)>(us / 1000000);
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>(us % 1000000);
        fd_set set;
        FD_ZERO(&set);
        Socket highest = 0;
        for (const auto s : sockets)
        {
            FD_SET(s, &set);
            highest = std::max(highest, s);
        }
        const int ready = ::select(static_cast<int>(highest + 1), &set, nullptr, nullptr, &tv);
        if (ready <= 0) { continue; }
        for (const auto s : sockets)
        {
            if (!FD_ISSET(s, &set)) { continue; }
            sockaddr_in from{};
            socklen_t from_size = sizeof(from);
            const auto n = ::recvfrom(s, reinterpret_cast<char *>(buffer.data()), static_cast<Length>(buffer.size()), 0,
                                      reinterpret_cast<sockaddr *>(&from), &from_size);
            if (n <= 0 || from.sin_family != AF_INET) { continue; }
            collector.add(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(n)), ntohl(from.sin_addr.s_addr));
        }
    }
    for (const auto s : sockets) { closeSocket(s); }
    return collector.agents();
}
} // namespace drivelink::discovery
