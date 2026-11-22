#pragma once
// Agent discovery over mDNS/DNS-SD (docs/protocol/agent.md, "Discovery").
// IPv4 only. An agent that listens on a LAN address advertises the service
// `_drivelink._tcp.local.`; browseAgents finds such agents. The packet
// building and parsing below are pure (no sockets) so both sides are tested
// on every platform; drivelink-agent (Linux) owns the responder's sockets.
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace drivelink::discovery
{
inline constexpr std::string_view kServiceType = "_drivelink._tcp.local.";
inline constexpr std::string_view kServicesQuery = "_services._dns-sd._udp.local.";
inline constexpr uint16_t kMdnsPort = 5353;
inline constexpr uint32_t kHostTtl = 120;    // SRV and A records, seconds
inline constexpr uint32_t kServiceTtl = 4500; // PTR and TXT records, seconds
inline constexpr unsigned kTxtVersion = 1;

// An agent found on the network.
struct FoundAgent
{
    std::string instance;               // instance label, e.g. "robot-pi"
    std::string host;                   // SRV target, e.g. "drivelink-robot-pi.local."
    uint16_t port = 0;                  // SRV port; 0 when no SRV was received
    std::vector<std::string> addresses; // dotted IPv4 from the host's A records, in order received
    std::string from;                   // dotted IPv4 the first answer came from
    std::optional<unsigned> proto;      // TXT proto= (agent protocol version), when present and numeric
    std::string version;                // TXT version= (DriveLink version), may be empty
    // "a.b.c.d:port": `from` when it is one of `addresses`, else the first
    // address, else `from`. Empty when port is 0 or no address is known.
    std::string address() const;
    // "tcp://" + address(), or empty.
    std::string endpoint() const;
};

// One browse round: a PTR query for kServiceType asking for unicast answers,
// sent from an ephemeral port on every interface listIpv4Interfaces()
// returns (twice when window >= 600 ms), then answers collected until
// `window` ends. Blocking; run it off a UI thread. Network failures are not
// errors: an interface that cannot send is skipped and the result may be
// empty. Agents without a port are left out; a goodbye (TTL 0) received in
// the window removes its records. Sorted by instance, then address. Throws Error{Usage} when window is outside 100..10000 ms.
std::vector<FoundAgent> browseAgents(std::chrono::milliseconds window = std::chrono::milliseconds(1000));

// Up, multicast-capable, non-loopback IPv4 interfaces (host byte order).
struct Ipv4Interface
{
    std::string name;
    uint32_t address = 0;
    uint32_t netmask = 0;
    bool operator==(const Ipv4Interface &) const = default;
};
std::vector<Ipv4Interface> listIpv4Interfaces();

std::string formatIpv4(uint32_t address);                 // host order -> "a.b.c.d"
std::optional<uint32_t> parseIpv4(std::string_view text); // strict dotted quad -> host order

// ---- Responder side (pure) -------------------------------------------------

// What one agent advertises on one interface.
struct Advert
{
    std::string instance; // 1..63 bytes UTF-8, no '.', no control characters
    std::string host;     // one label, without ".local." (e.g. "drivelink-robot-pi")
    uint16_t port = 0;
    uint32_t address = 0; // host order, the A record
    unsigned proto = 0;   // TXT proto=
    std::string version;  // TXT version=
};

// The records a question asks of an advert.
enum class Answer
{
    None,
    Services, // _services._dns-sd._udp PTR -> the service type
    Service,  // service PTR -> instance, with SRV, TXT and A as additionals
    Srv,      // instance SRV, with A as additional
    Txt,      // instance TXT
    A         // host A
};
// rtype is the question's type (PTR 12, A 1, TXT 16, SRV 33, ANY 255); the
// name compares case-insensitively, with or without its trailing dot.
Answer match(const Advert &advert, uint16_t rtype, std::string_view name);

// A question to echo in a legacy-unicast answer (RFC 6762 6.7).
struct Question
{
    std::string name;
    uint16_t rtype = 0;
    uint16_t rclass = 1;
};
// A response packet carrying `answer` for `advert`: id 0 unless echoing a
// legacy-unicast question (then `query_id` and the question are echoed and
// TTLs are capped at 10 s); flags 0x8400; cache-flush on unique records
// (SRV, TXT, A) except in legacy answers; TTL 0 when `goodbye`. Answer::None
// returns an empty vector. An announcement or goodbye uses Answer::Service.
std::vector<uint8_t> buildResponse(const Advert &advert, Answer answer, bool goodbye = false,
                                   uint16_t query_id = 0, const Question *legacy = nullptr);

// Builds a PTR query for `name` (id 0, one question; QU bit when `unicast`).
std::vector<uint8_t> buildQuery(std::string_view name, uint16_t rtype, bool unicast);

// Collects records from response packets and resolves them into agents.
class Collector
{
  public:
    // Parses one received packet; malformed or truncated packets and
    // records are skipped (never throws). `from` is the sender, host order.
    void add(std::span<const uint8_t> packet, uint32_t from);
    // Agents for kServiceType known so far (same rules as browseAgents). A
    // record repeated in several packets counts once.
    std::vector<FoundAgent> agents() const;

  private:
    struct Srv { std::string name, target; uint16_t port; };
    struct Txt { std::string name; std::vector<std::pair<std::string, std::string>> entries; };
    struct A { std::string name; uint32_t address; };
    std::vector<std::pair<std::string, uint32_t>> m_instances; // PTR target, first sender
    std::vector<Srv> m_srv;
    std::vector<Txt> m_txt;
    std::vector<A> m_a;
};

// The question entries of a query packet, for the responder (pure). Stops at
// the first malformed question; never throws.
struct ParsedQuestion
{
    std::string name;
    uint16_t rtype = 0;
    uint16_t rclass = 0; // with the QU bit (0x8000) as received
};
struct ParsedQuery
{
    uint16_t id = 0;
    uint16_t flags = 0;
    std::vector<ParsedQuestion> questions;
};
std::optional<ParsedQuery> parseQuery(std::span<const uint8_t> packet);

// The instance label rules for Advert::instance; empty when valid, else why.
std::string instanceError(std::string_view instance);
// A host label from a host name: lower case, [a-z0-9-] (others become '-'),
// "drivelink-" prefix, at most 63 bytes, no trailing '-'.
std::string hostLabel(std::string_view hostname);
} // namespace drivelink::discovery
