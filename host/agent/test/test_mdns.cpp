// The mDNS responder: pure rules, then a unicast round trip on a real interface.
#include "mdns.hpp"
#include <arpa/inet.h>
#include <doctest.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace drivelink::agent;
namespace d = drivelink::discovery;

namespace
{
const std::vector<d::Ipv4Interface> kInterfaces = {{"eth0", 0xC0A80114, 0xFFFFFF00}, {"wlan0", 0x0A000005, 0xFF000000}};

d::Advert advert() { return {"robot-pi", "drivelink-robot-pi", 47620, 0xC0A80114, 3, "0.1.0"}; }

d::ParsedQuery query(std::vector<d::ParsedQuestion> questions, uint16_t flags = 0, uint16_t id = 0)
{
    return {id, flags, std::move(questions)};
}
} // namespace

TEST_CASE("mdns: default instance and interfaces per bind")
{
    CHECK(defaultInstance("robot-pi.lan") == "robot-pi");
    CHECK(defaultInstance("") == "drivelink-agent");
    CHECK(defaultInstance(std::string(70, 'x')) == "drivelink-agent");
    CHECK(advertisedInterfaces("0.0.0.0", kInterfaces) == kInterfaces);
    CHECK(advertisedInterfaces("::", kInterfaces) == kInterfaces);
    CHECK(advertisedInterfaces("10.0.0.5", kInterfaces) == std::vector<d::Ipv4Interface>{kInterfaces[1]});
    CHECK(advertisedInterfaces("10.0.0.6", kInterfaces).empty());
    CHECK(advertisedInterfaces("127.0.0.1", kInterfaces).empty());
    CHECK(advertisedInterfaces("::1", kInterfaces).empty());
    CHECK(advertisedInterfaces("fe80::1", kInterfaces).empty());
}

TEST_CASE("mdns: interface re-scan diff")
{
    auto after = kInterfaces;
    after[1].address = 0x0A000006; // DHCP gave wlan0 a new address
    after.push_back({"eth1", 0xAC100001, 0xFFFF0000});
    const auto diff = diffInterfaces(kInterfaces, after);
    REQUIRE(diff.removed.size() == 1);
    CHECK(diff.removed[0].address == 0x0A000005);
    REQUIRE(diff.added.size() == 2);
    CHECK(diff.added[0].address == 0x0A000006);
    CHECK(diff.added[1].name == "eth1");
    auto netmask_only = kInterfaces;
    netmask_only[0].netmask = 0xFFFF0000;
    const auto same = diffInterfaces(kInterfaces, netmask_only);
    CHECK(same.added.empty());
    CHECK(same.removed.empty());
}

TEST_CASE("mdns: replies per question, port and QU bit")
{
    const auto a = advert();
    SUBCASE("multicast question from 5353")
    {
        const auto r = replies(a, query({{"_drivelink._tcp.local.", 12, 1}}), 5353);
        REQUIRE(r.size() == 1);
        CHECK_FALSE(r[0].unicast);
        CHECK(r[0].answer == d::Answer::Service);
        CHECK(r[0].packet == d::buildResponse(a, d::Answer::Service));
    }
    SUBCASE("QU question from 5353")
    {
        const auto r = replies(a, query({{"_drivelink._tcp.local.", 12, 0x8001}}), 5353);
        REQUIRE(r.size() == 1);
        CHECK(r[0].unicast);
        CHECK(r[0].packet == d::buildResponse(a, d::Answer::Service));
    }
    SUBCASE("legacy unicast from another port echoes id and question")
    {
        const auto r = replies(a, query({{"_drivelink._tcp.local.", 12, 1}}, 0, 0x4242), 50000);
        REQUIRE(r.size() == 1);
        CHECK(r[0].unicast);
        const d::Question echo{"_drivelink._tcp.local.", 12, 1};
        CHECK(r[0].packet == d::buildResponse(a, d::Answer::Service, false, 0x4242, &echo));
    }
    SUBCASE("several questions, duplicates once, others ignored")
    {
        const auto r = replies(a,
                               query({{"robot-pi._drivelink._tcp.local.", 33, 1},
                                      {"robot-pi._drivelink._tcp.local.", 16, 1},
                                      {"robot-pi._drivelink._tcp.local.", 33, 1},
                                      {"_http._tcp.local.", 12, 1},
                                      {"drivelink-robot-pi.local.", 1, 1}}),
                               5353);
        REQUIRE(r.size() == 3);
        CHECK(r[0].answer == d::Answer::Srv);
        CHECK(r[1].answer == d::Answer::Txt);
        CHECK(r[2].answer == d::Answer::A);
    }
    SUBCASE("responses and foreign services get nothing")
    {
        CHECK(replies(a, query({{"_drivelink._tcp.local.", 12, 1}}, 0x8400), 5353).empty());
        CHECK(replies(a, query({{"_ssh._tcp.local.", 12, 1}}), 5353).empty());
        CHECK(replies(a, query({}), 5353).empty());
    }
}

TEST_CASE("mdns: the advertiser answers a unicast query on a real interface")
{
    const auto interfaces = d::listIpv4Interfaces();
    if (interfaces.empty())
    {
        MESSAGE("skipped: no non-loopback IPv4 interface");
        return;
    }
    const auto iface = interfaces.front();
    std::mutex lock;
    std::vector<std::string> lines;
    Advertiser advertiser({"agent-test-" + std::to_string(::getpid()), "drivelink-test", "0.0.0.0", 47999, 3, "9.9.9"},
                          [&](const std::string &line) {
                              const std::lock_guard g(lock);
                              lines.push_back(line);
                          },
                          [&] { return std::vector<d::Ipv4Interface>{iface}; });
    advertiser.start();
    bool advertising = false;
    for (int i = 0; i < 100 && !advertising; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const std::lock_guard g(lock);
        for (const auto &l : lines) { advertising = advertising || l.find("advertising") != std::string::npos; }
    }
    {
        const std::lock_guard g(lock);
        for (const auto &l : lines) { MESSAGE(l); }
    }
    REQUIRE(advertising);

    const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(s >= 0);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(5353);
    to.sin_addr.s_addr = htonl(iface.address);
    const auto packet = d::buildQuery(d::kServiceType, 12, false);
    std::vector<d::FoundAgent> found;
    for (int attempt = 0; attempt < 3 && found.empty(); ++attempt)
    {
        ::sendto(s, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
        d::Collector collector;
        pollfd fd{s, POLLIN, 0};
        while (::poll(&fd, 1, 1000) > 0)
        {
            std::array<uint8_t, 9000> buffer{};
            sockaddr_in from{};
            socklen_t size = sizeof(from);
            const auto n = ::recvfrom(s, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr *>(&from), &size);
            if (n <= 0) { break; }
            collector.add(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(n)), ntohl(from.sin_addr.s_addr));
            found = collector.agents();
            if (!found.empty()) { break; }
        }
    }
    ::close(s);
    advertiser.stop();
    REQUIRE(found.size() == 1);
    CHECK(found[0].instance == "agent-test-" + std::to_string(::getpid()));
    CHECK(found[0].port == 47999);
    CHECK(found[0].proto == 3u);
    CHECK(found[0].version == "9.9.9");
    CHECK(found[0].addresses == std::vector<std::string>{d::formatIpv4(iface.address)});
    advertiser.stop(); // idempotent
}
