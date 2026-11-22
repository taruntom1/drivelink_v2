// The agent's mDNS/DNS-SD responder (mdns.hpp).
#include "mdns.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mdns.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <map>

namespace drivelink::agent
{
using discovery::Answer;
using discovery::Ipv4Interface;

std::string defaultInstance(std::string_view hostname)
{
    const auto label = hostname.substr(0, hostname.find('.'));
    return discovery::instanceError(label).empty() ? std::string(label) : std::string("drivelink-agent");
}

std::vector<Ipv4Interface> advertisedInterfaces(std::string_view bind, const std::vector<Ipv4Interface> &interfaces)
{
    if (bind == "0.0.0.0" || bind == "::") { return interfaces; }
    const auto address = discovery::parseIpv4(bind);
    if (!address || (*address >> 24) == 127) { return {}; }
    std::vector<Ipv4Interface> out;
    for (const auto &i : interfaces)
    {
        if (i.address == *address) { out.push_back(i); }
    }
    return out;
}

InterfaceDiff diffInterfaces(const std::vector<Ipv4Interface> &before, const std::vector<Ipv4Interface> &after)
{
    const auto same = [](const Ipv4Interface &a, const Ipv4Interface &b) { return a.name == b.name && a.address == b.address; };
    InterfaceDiff diff;
    for (const auto &b : before)
    {
        if (std::none_of(after.begin(), after.end(), [&](const auto &a) { return same(a, b); })) { diff.removed.push_back(b); }
    }
    for (const auto &a : after)
    {
        if (std::none_of(before.begin(), before.end(), [&](const auto &b) { return same(a, b); })) { diff.added.push_back(a); }
    }
    return diff;
}

std::vector<MdnsReply> replies(const discovery::Advert &advert, const discovery::ParsedQuery &query, uint16_t source_port)
{
    std::vector<MdnsReply> out;
    if (query.flags & 0x8000) { return out; } // a response, not a query
    const bool legacy = source_port != discovery::kMdnsPort;
    for (const auto &q : query.questions)
    {
        const auto answer = discovery::match(advert, q.rtype, q.name);
        if (answer == Answer::None) { continue; }
        if (std::any_of(out.begin(), out.end(), [&](const MdnsReply &r) { return r.answer == answer; })) { continue; }
        MdnsReply reply;
        reply.answer = answer;
        if (legacy)
        {
            const discovery::Question echo{q.name, q.rtype, q.rclass};
            reply.packet = discovery::buildResponse(advert, answer, false, query.id, &echo);
            reply.unicast = true;
        }
        else
        {
            reply.packet = discovery::buildResponse(advert, answer);
            reply.unicast = (q.rclass & 0x8000) != 0;
        }
        out.push_back(std::move(reply));
    }
    return out;
}

namespace
{
using Clock = std::chrono::steady_clock;
constexpr uint32_t kGroup = 0xE00000FB; // 224.0.0.251

sockaddr_in ipv4(uint32_t address, uint16_t port)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(address);
    return a;
}

std::string describe(const Ipv4Interface &i) { return i.name + " " + discovery::formatIpv4(i.address); }
} // namespace

struct Advertiser::Impl
{
    struct Open
    {
        Ipv4Interface info;
        int sock = -1;
        Clock::time_point announce_again = Clock::time_point::max();
        std::map<Answer, Clock::time_point> last_multicast;
    };

    AdvertiseSettings settings;
    Log log;
    std::function<std::vector<Ipv4Interface>()> list;
    std::thread thread;
    std::array<int, 2> wake{-1, -1};
    std::atomic<bool> stopping{false};
    bool started = false;
    // Owned by the thread:
    std::vector<Open> open;
    std::vector<std::string> failed; // interfaces whose 5353 failed: logged once
    bool told_none = false;

    discovery::Advert advert(const Ipv4Interface &i) const
    {
        return {settings.instance, settings.host, settings.port, i.address, settings.proto, settings.version};
    }

    void send(const Open &o, const std::vector<uint8_t> &packet, const sockaddr_in &to)
    {
        ::sendto(o.sock, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
    }

    void multicast(Open &o, const std::vector<uint8_t> &packet) { send(o, packet, ipv4(kGroup, discovery::kMdnsPort)); }

    bool openSocket(Open &o)
    {
        const int s = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        if (s < 0) { return false; }
        // Only this interface's group traffic (Linux delivers every joined
        // group to every socket on the port otherwise).
        const int zero = 0, ttl = 255;
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof(zero));
        const auto local = ipv4(o.info.address, discovery::kMdnsPort);
        if (mdns_socket_setup_ipv4(s, &local) != 0)
        {
            const int error = errno;
            ::close(s);
            errno = error;
            return false;
        }
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)); // RFC 6762 11
        o.sock = s;
        return true;
    }

    void close(Open &o, bool goodbye)
    {
        if (o.sock < 0) { return; }
        if (goodbye) { multicast(o, discovery::buildResponse(advert(o.info), Answer::Service, true)); }
        ::close(o.sock);
        o.sock = -1;
    }

    void rescan()
    {
        const auto now = advertisedInterfaces(settings.bind, list());
        std::vector<Ipv4Interface> before;
        for (const auto &o : open) { before.push_back(o.info); }
        const auto diff = diffInterfaces(before, now);
        for (const auto &gone : diff.removed)
        {
            const auto it = std::find_if(open.begin(), open.end(), [&](const Open &o) {
                return o.info.name == gone.name && o.info.address == gone.address;
            });
            if (it == open.end()) { continue; }
            close(*it, true);
            log("mDNS: stopped advertising on " + describe(gone));
            open.erase(it);
        }
        for (const auto &added : diff.added)
        {
            Open o{added, -1, {}, {}};
            const auto key = describe(added);
            if (!openSocket(o))
            {
                if (std::find(failed.begin(), failed.end(), key) == failed.end())
                {
                    log("mDNS: cannot open UDP port 5353 on " + key + " (" + std::strerror(errno) +
                        "); not advertised there, TCP is unaffected");
                    failed.push_back(key);
                }
                continue;
            }
            std::erase(failed, key);
            multicast(o, discovery::buildResponse(advert(added), Answer::Service));
            o.announce_again = Clock::now() + std::chrono::seconds(1);
            log("mDNS: advertising \"" + settings.instance + "\" (" + std::string(discovery::kServiceType) + ") on " +
                key + ":" + std::to_string(settings.port));
            open.push_back(std::move(o));
        }
        if (now.empty() && !told_none)
        {
            log("mDNS: no interface holds " + settings.bind + " yet; advertising starts when one does");
            told_none = true;
        }
        if (!now.empty()) { told_none = false; }
    }

    void receive(Open &o)
    {
        alignas(4) std::array<uint8_t, 9000> buffer{};
        for (int i = 0; i < 64; ++i) // bounded per wake-up
        {
            sockaddr_in from{};
            socklen_t size = sizeof(from);
            const auto n = ::recvfrom(o.sock, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr *>(&from), &size);
            if (n <= 0) { return; }
            if (from.sin_family != AF_INET) { continue; }
            const auto query = discovery::parseQuery(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(n)));
            if (!query) { continue; }
            const auto now = Clock::now();
            for (auto &reply : replies(advert(o.info), *query, ntohs(from.sin_port)))
            {
                if (reply.unicast)
                {
                    send(o, reply.packet, from);
                    continue;
                }
                auto &last = o.last_multicast[reply.answer];
                if (last != Clock::time_point{} && now - last < kMulticastGap) { continue; } // RFC 6762 6
                last = now;
                multicast(o, reply.packet);
            }
        }
    }

    void run()
    {
        rescan();
        auto next_rescan = Clock::now() + kRescan;
        std::vector<pollfd> fds;
        while (!stopping.load())
        {
            auto wake_at = next_rescan;
            for (const auto &o : open) { wake_at = std::min(wake_at, o.announce_again); }
            const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(wake_at - Clock::now()).count();
            fds.clear();
            fds.push_back({wake[0], POLLIN, 0});
            for (const auto &o : open) { fds.push_back({o.sock, POLLIN, 0}); }
            const int ready = ::poll(fds.data(), fds.size(), static_cast<int>(std::clamp<long long>(wait, 0, 10000)));
            if (stopping.load()) { break; }
            if (ready > 0)
            {
                for (size_t i = 1; i < fds.size(); ++i)
                {
                    if (fds[i].revents & POLLIN) { receive(open[i - 1]); }
                }
            }
            const auto now = Clock::now();
            for (auto &o : open)
            {
                if (now >= o.announce_again)
                {
                    multicast(o, discovery::buildResponse(advert(o.info), Answer::Service));
                    o.announce_again = Clock::time_point::max();
                }
            }
            if (now >= next_rescan)
            {
                rescan();
                next_rescan = now + kRescan;
            }
        }
        for (auto &o : open) { close(o, true); }
        open.clear();
    }
};

Advertiser::Advertiser(AdvertiseSettings settings, Log log, std::function<std::vector<Ipv4Interface>()> list)
    : m(std::make_unique<Impl>())
{
    m->settings = std::move(settings);
    m->log = std::move(log);
    m->list = std::move(list);
}

Advertiser::~Advertiser() { stop(); }

void Advertiser::start()
{
    if (m->started) { return; }
    if (::pipe2(m->wake.data(), O_NONBLOCK | O_CLOEXEC) != 0) { throw std::runtime_error("mDNS: pipe failed"); }
    m->started = true;
    m->thread = std::thread([impl = m.get()] { impl->run(); });
}

void Advertiser::stop()
{
    if (!m->started) { return; }
    m->started = false;
    m->stopping.store(true);
    const char byte = 1;
    [[maybe_unused]] const auto ignored = ::write(m->wake[1], &byte, 1);
    if (m->thread.joinable()) { m->thread.join(); }
    ::close(m->wake[0]);
    ::close(m->wake[1]);
    m->wake = {-1, -1};
}
} // namespace drivelink::agent
