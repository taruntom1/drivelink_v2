#include "doctest.h"
#include "drivelink/discovery.hpp"
#include "drivelink/error.hpp"

#include <chrono>

using namespace drivelink;
using namespace drivelink::discovery;

namespace
{
Advert advert()
{
    return {"robot-pi", "drivelink-robot-pi", 47620, 0xC0A80114 /* 192.168.1.20 */, 3, "0.1.0"};
}

std::vector<FoundAgent> collect(const std::vector<std::vector<uint8_t>> &packets, uint32_t from)
{
    Collector c;
    for (const auto &p : packets) { c.add(p, from); }
    return c.agents();
}

uint16_t at16(const std::vector<uint8_t> &p, size_t i) { return static_cast<uint16_t>((p[i] << 8) | p[i + 1]); }
} // namespace

TEST_CASE("discovery: dotted IPv4 round trip and strict parsing")
{
    CHECK(formatIpv4(0xC0A80114) == "192.168.1.20");
    CHECK(parseIpv4("192.168.1.20") == 0xC0A80114u);
    CHECK(parseIpv4("0.0.0.0") == 0u);
    CHECK(parseIpv4("255.255.255.255") == 0xFFFFFFFFu);
    for (const auto *bad : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "01.2.3.4", "1..3.4", "a.b.c.d", "1.2.3.4 ", "-1.2.3.4"})
    {
        CAPTURE(bad);
        CHECK_FALSE(parseIpv4(bad).has_value());
    }
}

TEST_CASE("discovery: instance names and host labels")
{
    CHECK(instanceError("robot-pi").empty());
    CHECK(instanceError("Bench agent 2").empty());
    CHECK(instanceError("Roboter-\xC3\x9C").empty());
    CHECK(instanceError(std::string(63, 'a')).empty());
    CHECK_FALSE(instanceError("").empty());
    CHECK_FALSE(instanceError(std::string(64, 'a')).empty());
    CHECK_FALSE(instanceError("a.b").empty());
    CHECK_FALSE(instanceError("tab\there").empty());
    CHECK_FALSE(instanceError("del\x7F").empty());
    CHECK_FALSE(instanceError("bad\xC3").empty());         // truncated sequence
    CHECK_FALSE(instanceError("bad\xC0\xAF").empty());     // overlong
    CHECK_FALSE(instanceError("bad\xED\xA0\x80").empty()); // surrogate
    CHECK_FALSE(instanceError("c1\xC2\x85").empty());      // C1 control
    CHECK(hostLabel("Robot-Pi.lan") == "drivelink-robot-pi");
    CHECK(hostLabel("my_host") == "drivelink-my-host");
    CHECK(hostLabel("") == "drivelink");
    CHECK(hostLabel(std::string(80, 'x')).size() == 63);
}

TEST_CASE("discovery: which records a question asks for")
{
    const auto a = advert();
    CHECK(match(a, 12, "_drivelink._tcp.local.") == Answer::Service);
    CHECK(match(a, 12, "_DriveLink._TCP.local") == Answer::Service);
    CHECK(match(a, 255, "_drivelink._tcp.local") == Answer::Service);
    CHECK(match(a, 12, "_services._dns-sd._udp.local.") == Answer::Services);
    CHECK(match(a, 33, "robot-pi._drivelink._tcp.local.") == Answer::Srv);
    CHECK(match(a, 16, "Robot-Pi._drivelink._tcp.local") == Answer::Txt);
    CHECK(match(a, 255, "robot-pi._drivelink._tcp.local") == Answer::Service);
    CHECK(match(a, 1, "drivelink-robot-pi.local.") == Answer::A);
    CHECK(match(a, 28, "drivelink-robot-pi.local.") == Answer::None); // no AAAA (IPv4 only)
    CHECK(match(a, 12, "_http._tcp.local.") == Answer::None);
    CHECK(match(a, 33, "other._drivelink._tcp.local.") == Answer::None);
    CHECK(match(a, 1, "robot-pi.local.") == Answer::None);
}

TEST_CASE("discovery: a service answer resolves into one agent")
{
    const auto a = advert();
    const auto packet = buildResponse(a, Answer::Service);
    CHECK(at16(packet, 0) == 0);      // id
    CHECK(at16(packet, 2) == 0x8400); // response, authoritative
    CHECK(at16(packet, 4) == 0);      // no question
    CHECK(at16(packet, 6) == 1);      // PTR
    CHECK(at16(packet, 10) == 3);     // SRV, TXT, A
    const auto found = collect({packet}, 0xC0A80114);
    REQUIRE(found.size() == 1);
    CHECK(found[0].instance == "robot-pi");
    CHECK(found[0].host == "drivelink-robot-pi.local.");
    CHECK(found[0].port == 47620);
    CHECK(found[0].addresses == std::vector<std::string>{"192.168.1.20"});
    CHECK(found[0].from == "192.168.1.20");
    CHECK(found[0].proto == 3u);
    CHECK(found[0].version == "0.1.0");
    CHECK(found[0].address() == "192.168.1.20:47620");
    CHECK(found[0].endpoint() == "tcp://192.168.1.20:47620");
}

TEST_CASE("discovery: the endpoint prefers the sender, then the first address")
{
    auto a = advert();
    auto other = a;
    other.address = 0x0A000005; // 10.0.0.5: a second interface of the same host
    {
        Collector c;
        c.add(buildResponse(a, Answer::Service), 0x0A000005);
        c.add(buildResponse(other, Answer::A), 0x0A000005);
        const auto found = c.agents();
        REQUIRE(found.size() == 1);
        CHECK(found[0].addresses.size() == 2);
        CHECK(found[0].address() == "10.0.0.5:47620");
    }
    {
        const auto found = collect({buildResponse(a, Answer::Service)}, 0xC0A80163); // a relay
        REQUIRE(found.size() == 1);
        CHECK(found[0].address() == "192.168.1.20:47620");
    }
    {
        // PTR and SRV only (no A): the sender is the address.
        Collector c;
        c.add(buildResponse(a, Answer::Service), 0xC0A80114);
        c.add(buildResponse(a, Answer::A, true), 0xC0A80114); // the A record says goodbye
        const auto found = c.agents();
        REQUIRE(found.size() == 1);
        CHECK(found[0].addresses.empty());
        CHECK(found[0].address() == "192.168.1.20:47620");
    }
}

TEST_CASE("discovery: repeats count once, goodbyes remove, unknown services are ignored")
{
    const auto a = advert();
    auto b = advert();
    b.instance = "bench agent";
    b.host = "drivelink-bench";
    b.address = 0xC0A80115;
    b.proto = 2;
    {
        const auto found = collect({buildResponse(a, Answer::Service), buildResponse(a, Answer::Service),
                                    buildResponse(b, Answer::Service)},
                                   0);
        REQUIRE(found.size() == 2);
        CHECK(found[0].instance == "bench agent"); // sorted
        CHECK(found[0].proto == 2u);
        CHECK(found[0].from.empty());
        CHECK(found[0].address() == "192.168.1.21:47620");
        CHECK(found[1].addresses.size() == 1);
    }
    {
        const auto found = collect({buildResponse(a, Answer::Service), buildResponse(a, Answer::Service, true)}, 0);
        CHECK(found.empty());
    }
    {
        // Services answers and a bare TXT alone make no agent.
        const auto found = collect({buildResponse(a, Answer::Services), buildResponse(a, Answer::Txt)}, 0);
        CHECK(found.empty());
    }
    {
        // A query packet (QR clear) is not a response.
        const auto found = collect({buildQuery(kServiceType, 12, false)}, 0);
        CHECK(found.empty());
    }
}

TEST_CASE("discovery: legacy unicast answers echo the question")
{
    const auto a = advert();
    const Question q{"_drivelink._tcp.local.", 12, 0x8001};
    const auto packet = buildResponse(a, Answer::Service, false, 0x1234, &q);
    CHECK(at16(packet, 0) == 0x1234);
    CHECK(at16(packet, 4) == 1);
    const auto parsed = parseQuery(packet);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->questions.size() == 1);
    CHECK(parsed->questions[0].name == "_drivelink._tcp.local.");
    CHECK(parsed->questions[0].rclass == 1); // QU bit cleared in the echo
    // The first record after the question: no cache-flush bit, TTL at most 10 s.
    size_t at = 12 + 23 /* name */ + 4;
    at += 23; // owner name of the PTR (uncompressed)
    CHECK(at16(packet, at) == 12);
    CHECK(at16(packet, at + 2) == 1);
    CHECK(((uint32_t{at16(packet, at + 4)} << 16) | at16(packet, at + 6)) == 10u);
    const auto found = collect({packet}, 0xC0A80114);
    REQUIRE(found.size() == 1);
    CHECK(found[0].port == 47620);
}

TEST_CASE("discovery: TTLs and cache-flush bits in multicast answers")
{
    const auto packet = buildResponse(advert(), Answer::Srv);
    // SRV owner "robot-pi._drivelink._tcp.local" = 9+11+5+6+1 bytes.
    const size_t at = 12 + 32;
    CHECK(at16(packet, at) == 33);
    CHECK(at16(packet, at + 2) == 0x8001);
    CHECK(((uint32_t{at16(packet, at + 4)} << 16) | at16(packet, at + 6)) == kHostTtl);
    const auto goodbye = buildResponse(advert(), Answer::Srv, true);
    CHECK(((uint32_t{at16(goodbye, at + 4)} << 16) | at16(goodbye, at + 6)) == 0u);
    CHECK(buildResponse(advert(), Answer::None).empty());
}

TEST_CASE("discovery: queries round trip, with and without the QU bit")
{
    const auto unicast = parseQuery(buildQuery("_drivelink._tcp.local.", 12, true));
    REQUIRE(unicast.has_value());
    CHECK(unicast->id == 0);
    REQUIRE(unicast->questions.size() == 1);
    CHECK(unicast->questions[0].name == "_drivelink._tcp.local.");
    CHECK(unicast->questions[0].rtype == 12);
    CHECK(unicast->questions[0].rclass == 0x8001);
    const auto multicast = parseQuery(buildQuery("drivelink-robot-pi.local", 1, false));
    REQUIRE(multicast.has_value());
    CHECK(multicast->questions[0].rclass == 1);
    CHECK_FALSE(parseQuery(std::vector<uint8_t>(11, 0)).has_value());
}

TEST_CASE("discovery: compressed names from another responder parse")
{
    // Hand-built: PTR _drivelink._tcp.local -> "lab" + pointer to the service
    // name; SRV owner as a pointer to the PTR target; target "pi.local".
    std::vector<uint8_t> p = {0, 0, 0x84, 0, 0, 0, 0, 2, 0, 0, 0, 1};
    const size_t service = p.size();
    for (const char *label : {"_drivelink", "_tcp", "local"})
    {
        p.push_back(static_cast<uint8_t>(std::string_view(label).size()));
        p.insert(p.end(), label, label + std::string_view(label).size());
    }
    p.push_back(0);
    p.insert(p.end(), {0, 12, 0, 1, 0, 0, 0x11, 0x94, 0, 6});
    const size_t instance = p.size();
    p.insert(p.end(), {3, 'l', 'a', 'b', static_cast<uint8_t>(0xC0), static_cast<uint8_t>(service)});
    p.insert(p.end(), {static_cast<uint8_t>(0xC0), static_cast<uint8_t>(instance)});
    p.insert(p.end(), {0, 33, 0x80, 1, 0, 0, 0, 120, 0, 16, 0, 0, 0, 0, 0x1F, 0x90});
    const size_t host = p.size();
    p.insert(p.end(), {2, 'p', 'i', static_cast<uint8_t>(0xC0), static_cast<uint8_t>(service + 16)});
    p.insert(p.end(), {static_cast<uint8_t>(0xC0), static_cast<uint8_t>(host), 0, 1, 0x80, 1, 0, 0, 0, 120, 0, 4, 10, 1, 2, 3});
    const auto found = collect({p}, 0x0A010203);
    REQUIRE(found.size() == 1);
    CHECK(found[0].instance == "lab");
    CHECK(found[0].host == "pi.local.");
    CHECK(found[0].port == 8080);
    CHECK(found[0].address() == "10.1.2.3:8080");
    CHECK_FALSE(found[0].proto.has_value());
}

TEST_CASE("discovery: truncated and looping packets never crash or invent agents")
{
    const auto packet = buildResponse(advert(), Answer::Service);
    for (size_t size = 0; size < packet.size(); ++size)
    {
        Collector c;
        c.add(std::span<const uint8_t>(packet.data(), size), 0);
        for (const auto &agent : c.agents())
        {
            CAPTURE(size);
            CHECK(agent.port == 47620); // only complete records count
        }
        (void)parseQuery(std::span<const uint8_t>(packet.data(), size));
    }
    // A name that points at itself.
    std::vector<uint8_t> loop = {0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0xC0, 12, 0, 12, 0, 1, 0, 0, 0, 1, 0, 2, 0xC0, 12};
    Collector c;
    c.add(loop, 0);
    CHECK(c.agents().empty());
    std::vector<uint8_t> question_loop = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 12, 0, 1};
    const auto q = parseQuery(question_loop);
    REQUIRE(q.has_value());
    CHECK(q->questions.size() <= 1);
}

TEST_CASE("discovery: browse refuses a window out of range")
{
    CHECK_THROWS_AS(browseAgents(std::chrono::milliseconds(99)), Error);
    CHECK_THROWS_AS(browseAgents(std::chrono::milliseconds(10001)), Error);
}
