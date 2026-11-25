#include "doctest.h"
#include "test_options.hpp"
#include "drivelink/link.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "drivelink/protocol/framing.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/identity_codec.hpp"
#include "drivelink/protocol/link.hpp"
#include "drivelink/protocol/wire_codec.hpp"
#include <asio.hpp>
#include <atomic>
#include <mutex>
#include <thread>

using namespace drivelink;
namespace p = drivelink::protocol;
namespace
{
using Socket = asio::ip::tcp::socket;
const p::DeviceId kId{2, 0x44, 0x4c, 0, 0, 1};
const LinkSettings kOld{921600, 43, 44}, kNew{115200, 43, 44};

// What LINK_CONFIRM does on the fake device.
enum class Confirm { Ok, LostReply, LostRequest, Late, StorageFailed, Malformed, RevertNoReply };

// A device on a TCP connection, one client at a time, for as long as it lives.
struct LinkDevice
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
    std::mutex mutex;
    p::DeviceId id = kId;
    LinkSettings running = kNew, next = kOld;
    uint32_t trial_ms = 20000;
    uint8_t flags = 0;
    Confirm confirm = Confirm::Ok;
    unsigned confirms = 0;
    std::atomic<bool> stopping = false;
    std::thread worker{[this] { serve(); }};

    ~LinkDevice()
    {
        stopping = true;
        asio::io_context wake;
        Socket socket(wake);
        asio::error_code ec;
        socket.connect(acceptor.local_endpoint(), ec);
        worker.join();
    }
    std::string endpoint() const { return "tcp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()); }
    void settle(bool confirmed)
    {
        trial_ms = 0;
        if (confirmed) { next = running; }
        else { running = next; }
    }
    static void put(p::wire::Writer &w, const LinkSettings &link)
    {
        w.appendU8(3);
        w.appendU32Le(*p::linkBaudValue(link.baud));
        w.appendU32Le(link.tx_pin);
        w.appendU32Le(link.rx_pin);
    }
    // The answer, or nothing for a lost one.
    std::optional<Bytes> handle(const Bytes &request)
    {
        std::lock_guard lock(mutex);
        Bytes out(256);
        p::wire::Writer w(out.data(), out.size());
        uint8_t result = 0, detail = 0;
        if (request[2] == 1 && request[3] == 1)
        {
            w.appendU8(p::core_wire::kVersion);
            p::core_wire::string(w, "test");
            p::core_wire::string(w, "build");
            w.appendU32Le(1);
            w.appendU32Le(0);
            w.appendU16Le(1024);
            w.appendU8(0);
            w.appendU8(flags);
            w.appendU8(0);
            w.appendBytes(id.data(), id.size());
            w.appendU8(0);
            w.appendU32Le(trial_ms);
        }
        else if (request[2] == 4 && (request[3] == 1 || request[3] == 3)) { put(w, request[3] == 1 ? running : next); }
        else if (request[2] == 1 && request[3] == 11)
        {
            ++confirms;
            switch (confirm)
            {
            case Confirm::Ok: settle(true); break;
            case Confirm::LostReply: settle(true); return std::nullopt;
            case Confirm::LostRequest: confirm = Confirm::Ok; return std::nullopt;
            case Confirm::Late:
                result = static_cast<uint8_t>(p::defs::Result::InvalidArgument);
                detail = static_cast<uint8_t>(p::defs::Detail::NoLinkTrial);
                settle(false);
                break;
            case Confirm::StorageFailed:
                result = static_cast<uint8_t>(p::defs::Result::Fault);
                detail = static_cast<uint8_t>(p::defs::Detail::StorageFailed);
                break;
            case Confirm::Malformed: settle(true); w.appendU8(1); break;
            case Confirm::RevertNoReply: settle(false); return std::nullopt;
            }
        }
        out.resize(w.size());
        Bytes reply{p::core_wire::kVersion, request[1], request[2], request[3], result, detail};
        reply.insert(reply.end(), out.begin(), out.end());
        return reply;
    }
    void serve()
    {
        while (!stopping)
        {
            Socket socket(io);
            asio::error_code ec;
            acceptor.accept(socket, ec);
            if (ec || stopping) { return; }
            socket.set_option(asio::ip::tcp::no_delay(true), ec);
            for (;;)
            {
                std::array<uint8_t, 7> header{};
                asio::read(socket, asio::buffer(header), ec);
                if (ec) { break; }
                Bytes body(header[4] + header[5] * 256U + 2U);
                asio::read(socket, asio::buffer(body), ec);
                if (ec) { break; }
                body.resize(body.size() - 2);
                const auto reply = handle(body);
                if (!reply) { continue; }
                Bytes bytes(p::frameSize(reply->size()));
                p::encodeFrame(bytes.data(), bytes.size(), 0x41, reply->data(), reply->size());
                asio::write(socket, asio::buffer(bytes), ec);
                if (ec) { break; }
            }
        }
    }
};
SessionOptions quick()
{
    auto options = testOptions();
    options.timeout = std::chrono::milliseconds(150);
    options.read_retries = 1;
    return options;
}
LinkState state(const LinkSettings &running, const LinkSettings &next, uint32_t trial_ms, uint8_t flags = 0)
{
    LinkState s;
    s.info.device_id = kId;
    s.info.link_trial_ms = trial_ms;
    s.info.flags = flags;
    s.running = running;
    s.next = next;
    return s;
}
} // namespace

TEST_CASE("link: the expectation comes from a waiting change or a running trial, and covers the trial")
{
    CHECK_FALSE(expectLinkTrial(state(kOld, kOld, 0)));
    const auto before = expectLinkTrial(state(kOld, kNew, 0));
    const auto during = expectLinkTrial(state(kNew, kOld, 1000));
    REQUIRE((before && during));
    for (const auto &e : {*before, *during})
    {
        CHECK(e.device == kId);
        CHECK(e.trial == kNew);
        CHECK(e.confirmed == kOld);
        CHECK(e.window() == std::chrono::seconds(45));
    }
    const LinkSettings moved{921600, 17, 18};
    CHECK(expectLinkTrial(state(kOld, moved, 0))->window() == std::chrono::seconds(135));
}

TEST_CASE("link: every outcome of a trial")
{
    const LinkExpectation e{kId, kNew, kOld};
    CHECK(classifyLinkTrial(e, state(kNew, kOld, 1)) == LinkOutcome::Unconfirmed);
    CHECK(classifyLinkTrial(e, state(kNew, kNew, 0)) == LinkOutcome::Confirmed);
    CHECK(classifyLinkTrial(e, state(kNew, kOld, 0)) == LinkOutcome::Confirmed); // and a next change staged
    CHECK(classifyLinkTrial(e, state(kOld, kOld, 0)) == LinkOutcome::Reverted);
    CHECK(classifyLinkTrial(e, state(kOld, kNew, 0)) == LinkOutcome::NotRebooted);
    CHECK(classifyLinkTrial(e, state(kOld, kNew, 0, 8)) == LinkOutcome::Deferred);
    const LinkSettings other{3000000, 43, 44};
    CHECK(classifyLinkTrial(e, state(other, kOld, 1)) == LinkOutcome::OtherLink);
    CHECK(classifyLinkTrial(e, state(other, other, 0)) == LinkOutcome::OtherLink);
    CHECK(classifyLinkTrial(e, state(kOld, kNew, 1)) == LinkOutcome::OtherLink);
    auto stranger = state(kNew, kOld, 1);
    stranger.info.device_id[5] = 9;
    CHECK(classifyLinkTrial(e, stranger) == LinkOutcome::WrongDevice);
    for (unsigned i = 0; i <= static_cast<unsigned>(LinkOutcome::NotReachable); ++i)
    {
        CHECK_FALSE(linkOutcomeText(static_cast<LinkOutcome>(i)).empty());
    }
}

TEST_CASE("link: LINK_CONFIRM goes out once, to the right device in its trial")
{
    LinkDevice device;
    Session session(device.endpoint(), quick());
    Client client(session);
    SUBCASE("confirmed")
    {
        CHECK(session.baud() == 0);
        CHECK(confirmLinkTrial(client, kId) == LinkConfirmation::Confirmed);
        CHECK(confirmLinkTrial(client, kId) == LinkConfirmation::NoTrial);
        CHECK(device.confirms == 1);
        const auto after = linkState(client);
        CHECK(after.running == kNew);
        CHECK(after.next == kNew);
        CHECK_FALSE(after.trial());
        CHECK_FALSE(after.changed());
    }
    SUBCASE("another device")
    {
        auto other = kId;
        other[0] = 9;
        CHECK(confirmLinkTrial(client, other) == LinkConfirmation::WrongDevice);
        CHECK(device.confirms == 0);
    }
    SUBCASE("a lost reply is settled by reading")
    {
        device.confirm = Confirm::LostReply;
        CHECK(confirmLinkTrial(client, kId) == LinkConfirmation::Confirmed);
        CHECK(device.confirms == 1);
    }
    SUBCASE("a malformed reply too")
    {
        device.confirm = Confirm::Malformed;
        CHECK(confirmLinkTrial(client, kId) == LinkConfirmation::Confirmed);
        CHECK(device.confirms == 1);
    }
    SUBCASE("a lost request leaves the trial")
    {
        device.confirm = Confirm::LostRequest;
        CHECK(confirmLinkTrial(client, kId) == LinkConfirmation::Unconfirmed);
        CHECK(device.confirms == 1);
        CHECK(confirmLinkTrial(client, kId) == LinkConfirmation::Confirmed);
        CHECK(device.confirms == 2);
    }
    SUBCASE("a trial that ends with the reply")
    {
        device.confirm = Confirm::RevertNoReply;
        CHECK(confirmLinkTrial(client, kId) == LinkConfirmation::Reverted);
    }
    SUBCASE("too late")
    {
        device.confirm = Confirm::Late;
        CHECK(confirmLinkTrial(client, kId) == LinkConfirmation::NoTrial);
    }
    SUBCASE("the device could not store it")
    {
        device.confirm = Confirm::StorageFailed;
        CHECK_THROWS_AS(confirmLinkTrial(client, kId), DeviceError);
        CHECK(device.confirms == 1);
    }
}

TEST_CASE("link: following a trial ends with its outcome")
{
    LinkDevice device;
    const LinkExpectation e{kId, kNew, kOld};
    LinkFollowOptions follow;
    follow.window = std::chrono::milliseconds(3000);
    SUBCASE("confirmed")
    {
        const auto r = followLinkTrial(device.endpoint(), quick(), e, follow);
        CHECK(r.outcome == LinkOutcome::Confirmed);
        CHECK(r.baud == 0);
        REQUIRE(r.state);
        CHECK(r.state->next == kNew);
        CHECK(device.confirms == 1);
    }
    SUBCASE("confirmed at the second request")
    {
        device.confirm = Confirm::LostRequest;
        CHECK(followLinkTrial(device.endpoint(), quick(), e, follow).outcome == LinkOutcome::Confirmed);
        CHECK(device.confirms == 2);
    }
    SUBCASE("left unconfirmed")
    {
        follow.confirm = false;
        CHECK(followLinkTrial(device.endpoint(), quick(), e, follow).outcome == LinkOutcome::Unconfirmed);
        CHECK(device.confirms == 0);
    }
    SUBCASE("too late")
    {
        device.confirm = Confirm::Late;
        CHECK(followLinkTrial(device.endpoint(), quick(), e, follow).outcome == LinkOutcome::Reverted);
    }
    SUBCASE("a device that cannot store it")
    {
        device.confirm = Confirm::StorageFailed;
        follow.window = std::chrono::milliseconds(600);
        const auto r = followLinkTrial(device.endpoint(), quick(), e, follow);
        CHECK(r.outcome == LinkOutcome::Unconfirmed);
        CHECK_FALSE(r.problem.empty());
        CHECK(device.confirms >= 2);
    }
    SUBCASE("another device")
    {
        device.id[3] = 7;
        CHECK(followLinkTrial(device.endpoint(), quick(), e, follow).outcome == LinkOutcome::WrongDevice);
        CHECK(device.confirms == 0);
    }
    SUBCASE("cancelled")
    {
        CHECK_THROWS_AS(followLinkTrial(device.endpoint(), quick(), e, follow, [] { return true; }), Error);
    }
    SUBCASE("nothing to follow")
    {
        CHECK_THROWS_AS(followLinkTrial(device.endpoint(), quick(), {kId, kOld, kOld}, follow), Error);
    }
}

TEST_CASE("link: nobody answers")
{
    asio::io_context io;
    asio::ip::tcp::acceptor taken(io, {asio::ip::address_v4::loopback(), 0});
    const auto endpoint = "tcp://127.0.0.1:" + std::to_string(taken.local_endpoint().port());
    taken.close();
    LinkFollowOptions follow;
    follow.window = std::chrono::milliseconds(300);
    const auto r = followLinkTrial(endpoint, quick(), {kId, kNew, kOld}, follow);
    CHECK(r.outcome == LinkOutcome::NotReachable);
    CHECK_FALSE(r.state);
    CHECK_FALSE(r.problem.empty());
}

TEST_CASE("link: options and settings a device cannot have are refused locally")
{
    LinkDevice device;
    auto options = quick();
    options.baud = kBaudAuto;
    options.detect_timeout = std::chrono::milliseconds(0);
    CHECK_THROWS_AS(Session(device.endpoint(), options), Error);
    Session session(device.endpoint(), quick());
    Client client(session);
    CHECK_THROWS_AS(stageLink(client, {57600, 43, 44}), Error);
}
