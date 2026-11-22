#pragma once
// The agent's mDNS/DNS-SD responder (docs/protocol/agent.md, "Discovery").
// It runs on its own thread and shares nothing with the agent's Asio thread:
// it only answers questions about this agent's service from fixed settings,
// so it never touches a device, a client or the lease. IPv4 only.
#include <drivelink/discovery.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace drivelink::agent
{
struct AdvertiseSettings
{
    std::string instance; // valid by discovery::instanceError
    std::string host;     // host label (discovery::hostLabel)
    std::string bind;     // the agent's bind address
    uint16_t port = 0;    // the agent's TCP port
    unsigned proto = 0;   // agent protocol version
    std::string version;  // DriveLink version
};

// The instance name when the config gives none: the host name's first label
// if it is a valid instance, else "drivelink-agent".
std::string defaultInstance(std::string_view hostname);

// Interfaces to advertise on for a bind address: every interface for
// 0.0.0.0 or ::, the interface holding a specific IPv4 address, none for a
// loopback, another IPv6 or an unknown address (pure).
std::vector<discovery::Ipv4Interface> advertisedInterfaces(std::string_view bind,
                                                           const std::vector<discovery::Ipv4Interface> &interfaces);

struct InterfaceDiff
{
    std::vector<discovery::Ipv4Interface> added, removed; // removed: also when the address changed
};
InterfaceDiff diffInterfaces(const std::vector<discovery::Ipv4Interface> &before,
                             const std::vector<discovery::Ipv4Interface> &after);

// The packets answering one received query on one interface (pure). A query
// from a port other than 5353 gets a legacy-unicast answer echoing the id and
// the question; a QU question gets a unicast answer; else multicast. One
// answer per question that matches; responses (QR set) get none.
struct MdnsReply
{
    std::vector<uint8_t> packet;
    bool unicast = false;
    discovery::Answer answer = discovery::Answer::None;
};
std::vector<MdnsReply> replies(const discovery::Advert &advert, const discovery::ParsedQuery &query, uint16_t source_port);

class Advertiser
{
  public:
    using Log = std::function<void(const std::string &)>;
    // `list` lists interfaces (discovery::listIpv4Interfaces; tests pass a fake).
    Advertiser(AdvertiseSettings settings, Log log,
               std::function<std::vector<discovery::Ipv4Interface>()> list = discovery::listIpv4Interfaces);
    ~Advertiser(); // stop()
    Advertiser(const Advertiser &) = delete;
    Advertiser &operator=(const Advertiser &) = delete;
    // Starts the thread: opens 5353 on each advertised interface, announces
    // twice 1 s apart, answers queries, re-scans interfaces every 10 s.
    void start();
    // Sends goodbyes on every open interface and joins the thread. Idempotent.
    void stop();

    static constexpr std::chrono::seconds kRescan{10};
    static constexpr std::chrono::milliseconds kMulticastGap{1000}; // per answer kind and interface

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace drivelink::agent
