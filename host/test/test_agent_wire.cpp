#include "doctest.h"
#include "drivelink/agent.hpp"

#include <string>

using namespace drivelink;
using namespace drivelink::agent;

namespace
{
DeviceRecord sampleRecord()
{
    DeviceRecord record;
    record.alias = "m1";
    record.online = true;
    record.device_id = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    record.name = "x";
    record.endpoint = "tcp:";
    record.clients = 3;
    return record;
}

Hello sampleHello()
{
    Hello hello;
    hello.core_version = 10;
    hello.token = "abc";
    hello.device = "m1";
    hello.client_name = "cli";
    return hello;
}

template <typename Decode> void requireEveryPrefixRefused(const Bytes &bytes, Decode decode)
{
    for (size_t length = 0; length < bytes.size(); ++length)
    {
        INFO("prefix length " << length);
        CHECK_FALSE(decode(std::span<const uint8_t>(bytes.data(), length)).has_value());
    }
}

template <typename Decode> void requireTrailingByteRefused(Bytes bytes, Decode decode)
{
    bytes.push_back(0);
    auto result = decode(std::span<const uint8_t>(bytes));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == ErrorKind::Protocol);
}

template <typename Fn> bool throwsUsage(Fn fn)
{
    try
    {
        fn();
    }
    catch (const Error &error)
    {
        return error.kind == ErrorKind::Usage;
    }
    return false;
}

void requireSameRecord(const DeviceRecord &a, const DeviceRecord &b)
{
    CHECK(a.alias == b.alias);
    CHECK(a.online == b.online);
    CHECK(a.device_id == b.device_id);
    CHECK(a.name == b.name);
    CHECK(a.endpoint == b.endpoint);
    CHECK(a.clients == b.clients);
}
} // namespace

TEST_CASE("agent envelopes round trip")
{
    Command command{7, 2, {1, 2, 3}};
    auto bytes = encodeCommand(command);
    CHECK(bytes == Bytes{1, 7, 2, 1, 2, 3});
    auto decoded = decodeCommand(bytes);
    REQUIRE(decoded.has_value());
    CHECK(decoded->seq == 7);
    CHECK(decoded->op == 2);
    CHECK(decoded->body == command.body);

    Response response{9, 1, 3, 4, {5, 6}};
    bytes = encodeResponse(response);
    CHECK(bytes == Bytes{1, 9, 1, 3, 4, 5, 6});
    auto decodedResponse = decodeResponse(bytes);
    REQUIRE(decodedResponse.has_value());
    CHECK(decodedResponse->version == kVersion);
    CHECK(decodedResponse->seq == 9);
    CHECK(decodedResponse->op == 1);
    CHECK(decodedResponse->result == 3);
    CHECK(decodedResponse->detail == 4);
    CHECK(decodedResponse->body == response.body);

    Event event{1, {2}};
    bytes = encodeEvent(event);
    CHECK(bytes == Bytes{1, 1, 2});
    auto decodedEvent = decodeEvent(bytes);
    REQUIRE(decodedEvent.has_value());
    CHECK(decodedEvent->event == 1);
    CHECK(decodedEvent->body == event.body);

    // Empty bodies.
    CHECK(decodeCommand(encodeCommand(Command{1, 2, {}}))->body.empty());
    CHECK(decodeResponse(encodeResponse(Response{}))->body.empty());
    CHECK(decodeEvent(encodeEvent(Event{}))->body.empty());
}

TEST_CASE("agent envelopes refuse short input and wrong versions")
{
    requireEveryPrefixRefused(encodeCommand(Command{1, 2, {}}), [](auto p) { return decodeCommand(p); });
    requireEveryPrefixRefused(encodeResponse(Response{}), [](auto p) { return decodeResponse(p); });
    requireEveryPrefixRefused(encodeEvent(Event{}), [](auto p) { return decodeEvent(p); });

    for (const uint8_t version : {uint8_t{0}, uint8_t{2}, uint8_t{255}})
    {
        INFO("version " << int(version));
        auto command = encodeCommand(Command{1, 2, {9}});
        command[0] = version;
        auto badCommand = decodeCommand(command);
        REQUIRE_FALSE(badCommand.has_value());
        CHECK(badCommand.error().kind == ErrorKind::Protocol);

        auto event = encodeEvent(Event{1, {2}});
        event[0] = version;
        auto badEvent = decodeEvent(event);
        REQUIRE_FALSE(badEvent.has_value());
        CHECK(badEvent.error().kind == ErrorKind::Protocol);

        // A response from another agent version is still decoded and reports it.
        auto response = encodeResponse(Response{3, 1, 4, 5, encodeVersionRefusal({1, 10})});
        response[0] = version;
        auto decoded = decodeResponse(response);
        REQUIRE(decoded.has_value());
        CHECK(decoded->version == version);
        CHECK(decoded->seq == 3);
        CHECK(decoded->result == 4);
        CHECK(decoded->detail == 5);
        CHECK(decoded->body == Bytes{1, 10});
    }
}

TEST_CASE("agent Hello golden bytes and round trip")
{
    const Bytes golden{0x01, 0x05, 0x01, // version, seq, op
                       0x0a,             // core_version
                       0x03, 'a', 'b', 'c', // token
                       0x00,             // device (lobby)
                       0x03, 'c', 'l', 'i'}; // client_name
    Hello hello;
    hello.core_version = 10;
    hello.token = "abc";
    hello.client_name = "cli";
    CHECK(encodeCommand(Command{5, 1, encodeHello(hello)}) == golden);

    auto command = decodeCommand(golden);
    REQUIRE(command.has_value());
    auto decoded = decodeHello(command->body);
    REQUIRE(decoded.has_value());
    CHECK(decoded->core_version == 10);
    CHECK(decoded->token == "abc");
    CHECK(decoded->device.empty());
    CHECK(decoded->client_name == "cli");

    auto full = decodeHello(encodeHello(sampleHello()));
    REQUIRE(full.has_value());
    CHECK(full->device == "m1");
    CHECK(full->client_name == "cli");

    // The token content is not checked, only its length; bytes are arbitrary.
    Hello binary = sampleHello();
    binary.token = std::string("\x00\xff\x01", 3);
    auto roundTrip = decodeHello(encodeHello(binary));
    REQUIRE(roundTrip.has_value());
    CHECK(roundTrip->token == binary.token);
}

TEST_CASE("agent Hello bounds")
{
    auto withToken = [](size_t size) { auto h = sampleHello(); h.token.assign(size, 't'); return h; };
    auto withDevice = [](size_t size) { auto h = sampleHello(); h.device.assign(size, 'd'); return h; };
    auto withName = [](size_t size) { auto h = sampleHello(); h.client_name.assign(size, 'n'); return h; };

    // Token 1..128.
    CHECK(throwsUsage([&] { encodeHello(withToken(0)); }));
    CHECK(decodeHello(encodeHello(withToken(1))).has_value());
    CHECK(decodeHello(encodeHello(withToken(kMaxToken))).has_value());
    CHECK(throwsUsage([&] { encodeHello(withToken(kMaxToken + 1)); }));
    // Device 0..64.
    CHECK(decodeHello(encodeHello(withDevice(0))).has_value());
    CHECK(decodeHello(encodeHello(withDevice(kMaxDevice))).has_value());
    CHECK(throwsUsage([&] { encodeHello(withDevice(kMaxDevice + 1)); }));
    // Client name 0..32.
    CHECK(decodeHello(encodeHello(withName(0))).has_value());
    CHECK(decodeHello(encodeHello(withName(kMaxClientName))).has_value());
    CHECK(throwsUsage([&] { encodeHello(withName(kMaxClientName + 1)); }));

    // Decoders refuse what the encoder refuses, built by hand.
    auto raw = [](size_t token, size_t device, size_t name) {
        Bytes out{10};
        for (const size_t size : {token, device, name})
        {
            out.push_back(static_cast<uint8_t>(size));
            out.insert(out.end(), size, 'x');
        }
        return out;
    };
    CHECK_FALSE(decodeHello(raw(0, 0, 0)).has_value());
    CHECK(decodeHello(raw(1, 0, 0)).has_value());
    CHECK(decodeHello(raw(kMaxToken, kMaxDevice, kMaxClientName)).has_value());
    CHECK_FALSE(decodeHello(raw(kMaxToken + 1, 0, 0)).has_value());
    CHECK_FALSE(decodeHello(raw(1, kMaxDevice + 1, 0)).has_value());
    CHECK_FALSE(decodeHello(raw(1, 0, kMaxClientName + 1)).has_value());
}

TEST_CASE("agent Hello refuses truncation and trailing bytes")
{
    const auto body = encodeHello(sampleHello());
    requireEveryPrefixRefused(body, [](auto p) { return decodeHello(p); });
    requireTrailingByteRefused(body, [](auto p) { return decodeHello(p); });
}

TEST_CASE("agent HelloAccepted device golden bytes and round trip")
{
    HelloAccepted accepted;
    accepted.client_id = 0x0102;
    accepted.lobby = false;
    accepted.device = sampleRecord();
    const Bytes golden{0x02, 0x01,                               // client_id
                       0x01,                                     // kind: device
                       0x02, 'm', '1',                           // alias
                       0x01,                                     // online
                       0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,       // device_id
                       0x01, 'x',                                // name
                       0x04, 't', 'c', 'p', ':',                 // endpoint
                       0x03};                                    // clients
    CHECK(encodeHelloAccepted(accepted) == golden);

    auto decoded = decodeHelloAccepted(golden);
    REQUIRE(decoded.has_value());
    CHECK(decoded->client_id == 0x0102);
    CHECK_FALSE(decoded->lobby);
    requireSameRecord(decoded->device, sampleRecord());

    requireEveryPrefixRefused(golden, [](auto p) { return decodeHelloAccepted(p); });
    requireTrailingByteRefused(golden, [](auto p) { return decodeHelloAccepted(p); });
}

TEST_CASE("agent HelloAccepted lobby")
{
    HelloAccepted lobby;
    lobby.client_id = 0xBEEF;
    lobby.lobby = true;
    const auto body = encodeHelloAccepted(lobby);
    CHECK(body == Bytes{0xEF, 0xBE, 0});
    auto decoded = decodeHelloAccepted(body);
    REQUIRE(decoded.has_value());
    CHECK(decoded->client_id == 0xBEEF);
    CHECK(decoded->lobby);
    requireEveryPrefixRefused(body, [](auto p) { return decodeHelloAccepted(p); });
    // A lobby with record bytes is refused.
    auto withRecord = body;
    appendDeviceRecord(withRecord, sampleRecord());
    CHECK_FALSE(decodeHelloAccepted(withRecord).has_value());
    requireTrailingByteRefused(body, [](auto p) { return decodeHelloAccepted(p); });
}

TEST_CASE("agent HelloAccepted refuses a bad kind or online flag")
{
    HelloAccepted accepted;
    accepted.lobby = false;
    accepted.device = sampleRecord();
    auto body = encodeHelloAccepted(accepted);
    auto badKind = body;
    badKind[2] = 2;
    CHECK_FALSE(decodeHelloAccepted(badKind).has_value());
    auto badKindLobby = Bytes{1, 0, 2};
    CHECK_FALSE(decodeHelloAccepted(badKindLobby).has_value());
    auto badOnline = body;
    badOnline[3 + 1 + 2] = 2; // after kind, alias length byte and "m1"
    CHECK_FALSE(decodeHelloAccepted(badOnline).has_value());
}

TEST_CASE("agent device record string bounds")
{
    for (int field = 0; field < 3; ++field)
    {
        auto make = [&](size_t size) {
            DeviceRecord record = sampleRecord();
            (field == 0 ? record.alias : field == 1 ? record.name : record.endpoint).assign(size, 's');
            return record;
        };
        INFO("field " << field);
        for (const size_t size : {size_t{0}, kMaxString})
        {
            Bytes out;
            appendDeviceRecord(out, make(size));
            HelloAccepted accepted;
            accepted.lobby = false;
            accepted.device = make(size);
            auto decoded = decodeHelloAccepted(encodeHelloAccepted(accepted));
            REQUIRE(decoded.has_value());
            requireSameRecord(decoded->device, make(size));
            auto list = decodeDeviceList(encodeDeviceList(std::vector{make(size)}));
            REQUIRE(list.has_value());
            REQUIRE(list->size() == 1);
            requireSameRecord((*list)[0], make(size));
        }
        const auto over = make(kMaxString + 1);
        Bytes out{9};
        CHECK(throwsUsage([&] { appendDeviceRecord(out, over); }));
        CHECK(out == Bytes{9}); // untouched on a refusal
        HelloAccepted accepted;
        accepted.lobby = false;
        accepted.device = over;
        CHECK(throwsUsage([&] { encodeHelloAccepted(accepted); }));
        CHECK(throwsUsage([&] { encodeDeviceList(std::vector{over}); }));
        // A lobby ignores the device record.
        accepted.lobby = true;
        CHECK(encodeHelloAccepted(accepted).size() == 3);
    }
}

TEST_CASE("agent device list round trip, bounds and malformed input")
{
    CHECK(encodeDeviceList({}) == Bytes{0});
    auto empty = decodeDeviceList(Bytes{0});
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    std::vector<DeviceRecord> devices{sampleRecord(), DeviceRecord{}, sampleRecord()};
    devices[1].alias = "offline";
    devices[2].online = false;
    devices[2].clients = 64;
    const auto body = encodeDeviceList(devices);
    auto decoded = decodeDeviceList(body);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == 3);
    for (size_t i = 0; i < devices.size(); ++i)
        requireSameRecord((*decoded)[i], devices[i]);

    requireEveryPrefixRefused(body, [](auto p) { return decodeDeviceList(p); });
    requireTrailingByteRefused(body, [](auto p) { return decodeDeviceList(p); });
    // A count larger than the records present is refused.
    auto shortList = body;
    shortList[0] = 4;
    CHECK_FALSE(decodeDeviceList(shortList).has_value());
    // A count smaller leaves trailing bytes.
    auto longList = body;
    longList[0] = 2;
    CHECK_FALSE(decodeDeviceList(longList).has_value());

    // 255 entries is the limit.
    std::vector<DeviceRecord> many(255, sampleRecord());
    auto manyBody = encodeDeviceList(many);
    CHECK(manyBody[0] == 255);
    auto manyDecoded = decodeDeviceList(manyBody);
    REQUIRE(manyDecoded.has_value());
    CHECK(manyDecoded->size() == 255);
    many.push_back(sampleRecord());
    CHECK(throwsUsage([&] { encodeDeviceList(many); }));
}

TEST_CASE("agent device record appends and rejects a bad online flag")
{
    Bytes out{0x7f};
    appendDeviceRecord(out, sampleRecord());
    CHECK(out.size() == 1 + 3 + 1 + 6 + 2 + 5 + 1);
    CHECK(out[0] == 0x7f);
    Bytes list{1};
    appendDeviceRecord(list, sampleRecord());
    CHECK(decodeDeviceList(list).has_value());
    list[1 + 3] = 2;
    CHECK_FALSE(decodeDeviceList(list).has_value());
}

TEST_CASE("agent version refusal and Closing bodies")
{
    const auto refusal = encodeVersionRefusal({1, 10});
    CHECK(refusal == Bytes{1, 10});
    auto decoded = decodeVersionRefusal(refusal);
    REQUIRE(decoded.has_value());
    CHECK(decoded->agent_version == 1);
    CHECK(decoded->core_version == 10);
    requireEveryPrefixRefused(refusal, [](auto p) { return decodeVersionRefusal(p); });
    requireTrailingByteRefused(refusal, [](auto p) { return decodeVersionRefusal(p); });

    CHECK(encodeClosing(2) == Bytes{2});
    auto closing = decodeClosing(Bytes{2});
    REQUIRE(closing.has_value());
    CHECK(*closing == 2);
    requireEveryPrefixRefused(Bytes{2}, [](auto p) { return decodeClosing(p); });
    requireTrailingByteRefused(Bytes{2}, [](auto p) { return decodeClosing(p); });
}

TEST_CASE("agent messages compose: Hello refusal response and Closing event")
{
    auto frame = encodeResponse(Response{4, 1, 3, 2, encodeVersionRefusal({1, 10})});
    auto response = decodeResponse(frame);
    REQUIRE(response.has_value());
    auto refusal = decodeVersionRefusal(response->body);
    REQUIRE(refusal.has_value());
    CHECK(refusal->core_version == 10);

    auto event = decodeEvent(encodeEvent(Event{1, encodeClosing(1)}));
    REQUIRE(event.has_value());
    auto reason = decodeClosing(event->body);
    REQUIRE(reason.has_value());
    CHECK(*reason == 1);
}

TEST_CASE("agent device id formatting and parsing")
{
    const std::array<uint8_t, 6> id{0x00, 0xab, 0xCD, 0x09, 0xf0, 0xff};
    CHECK(formatDeviceId(id) == "00abcd09f0ff");
    CHECK(formatDeviceId({}) == "000000000000");

    std::array<uint8_t, 6> parsed{};
    CHECK(parseDeviceId("00abcd09f0ff", parsed));
    CHECK(parsed == id);
    parsed = {};
    CHECK(parseDeviceId("00ABCD09F0FF", parsed));
    CHECK(parsed == id);
    parsed = {};
    CHECK(parseDeviceId("00AbCd09f0Ff", parsed));
    CHECK(parsed == id);

    const std::array<uint8_t, 6> sentinel{1, 2, 3, 4, 5, 6};
    for (const char *bad : {"", "00abcd09f0f", "00abcd09f0ff0", "00abcd09f0fg", "00abcd09f0f ", "00:bcd09f0ff",
                            "0xabcd09f0ff", "-0abcd09f0ff"})
    {
        INFO("text '" << bad << "'");
        parsed = sentinel;
        CHECK_FALSE(parseDeviceId(bad, parsed));
        CHECK(parsed == sentinel);
    }
    // A bad digit in the last position must not leave a partial result.
    parsed = sentinel;
    CHECK_FALSE(parseDeviceId(std::string("00abcd09f0f\0", 12), parsed));
    CHECK(parsed == sentinel);
}

TEST_CASE("agent tokenEquals")
{
    CHECK(tokenEquals("secret", "secret"));
    CHECK_FALSE(tokenEquals("secret", "secreT"));
    CHECK_FALSE(tokenEquals("secret", "Secret"));
    CHECK_FALSE(tokenEquals("secret", "secre"));
    CHECK_FALSE(tokenEquals("secre", "secret"));
    CHECK_FALSE(tokenEquals("secret", ""));
    CHECK_FALSE(tokenEquals("", "secret"));
    CHECK(tokenEquals("", ""));
    CHECK(tokenEquals(std::string_view("a\0b", 3), std::string_view("a\0b", 3)));
    CHECK_FALSE(tokenEquals(std::string_view("a\0b", 3), std::string_view("a\0c", 3)));
}
