#include "doctest.h"
#include "drivelink/agent.hpp"

#include <cmath>
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
    record.holder_id = 0x0304;
    record.holder_name = "cli";
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
    CHECK(a.holder_id == b.holder_id);
    CHECK(a.holder_name == b.holder_name);
}
} // namespace

TEST_CASE("agent envelopes round trip")
{
    Command command{7, 2, {1, 2, 3}};
    auto bytes = encodeCommand(command);
    CHECK(bytes == Bytes{2, 7, 2, 1, 2, 3});
    auto decoded = decodeCommand(bytes);
    REQUIRE(decoded.has_value());
    CHECK(decoded->seq == 7);
    CHECK(decoded->op == 2);
    CHECK(decoded->body == command.body);

    Response response{9, 1, 3, 4, {5, 6}};
    bytes = encodeResponse(response);
    CHECK(bytes == Bytes{2, 9, 1, 3, 4, 5, 6});
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
    CHECK(bytes == Bytes{2, 1, 2});
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

    for (const uint8_t version : {uint8_t{0}, uint8_t{1}, uint8_t{255}})
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
    const Bytes golden{0x02, 0x05, 0x01, // version, seq, op
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
                       0x03,                                     // clients
                       0x04, 0x03,                               // holder_id
                       0x03, 'c', 'l', 'i'};                     // holder_name
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
    for (int field = 0; field < 4; ++field)
    {
        auto make = [&](size_t size) {
            DeviceRecord record = sampleRecord();
            (field == 0 ? record.alias : field == 1 ? record.name : field == 2 ? record.endpoint : record.holder_name).assign(size, 's');
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
    CHECK(out.size() == 1 + 3 + 1 + 6 + 2 + 5 + 1 + 2 + 4);
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

namespace
{
ClockModel sampleModel()
{
    ClockModel model;
    model.offset_valid = true;
    model.drift_valid = true;
    model.device_reference_us = 1000;
    model.host_reference_us = 5000;
    model.span_us = 3000000;
    model.age_us = 250;
    model.samples = 8;
    model.slope = 1.0 + 20e-6;
    model.residual_us = 1.5;
    model.min_rtt_us = 400;
    model.max_rtt_us = 900;
    return model;
}

HistoryEntry entry(uint8_t kind, uint64_t unix_ms, Bytes body)
{
    return HistoryEntry{kind, unix_ms, std::move(body)};
}
} // namespace

TEST_CASE("agent device record v2 golden bytes")
{
    Bytes out;
    appendDeviceRecord(out, sampleRecord());
    const Bytes golden{0x02, 'm', '1', 0x01, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01, 'x',
                       0x04, 't', 'c', 'p', ':', 0x03, 0x04, 0x03, 0x03, 'c', 'l', 'i'};
    CHECK(out == golden);
    DeviceRecord none = sampleRecord();
    none.holder_id = 0;
    none.holder_name.clear();
    Bytes noneOut;
    appendDeviceRecord(noneOut, none);
    CHECK(noneOut.size() == golden.size() - 3);
    CHECK(noneOut[noneOut.size() - 3] == 0);
    CHECK(noneOut[noneOut.size() - 2] == 0);
    CHECK(noneOut.back() == 0);
}

TEST_CASE("agent ClientList golden bytes, bounds and refusals")
{
    CHECK(encodeClientList({}) == Bytes{0});
    std::vector<ClientRecord> clients{{0x0102, "cli", true}, {7, "", false}};
    const Bytes golden{0x02, 0x02, 0x01, 0x03, 'c', 'l', 'i', 0x01, 0x07, 0x00, 0x00, 0x00};
    CHECK(encodeClientList(clients) == golden);
    auto decoded = decodeClientList(golden);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == 2);
    CHECK((*decoded)[0].client_id == 0x0102);
    CHECK((*decoded)[0].name == "cli");
    CHECK((*decoded)[0].holder);
    CHECK((*decoded)[1].client_id == 7);
    CHECK((*decoded)[1].name.empty());
    CHECK_FALSE((*decoded)[1].holder);
    requireEveryPrefixRefused(golden, [](auto p) { return decodeClientList(p); });
    requireTrailingByteRefused(golden, [](auto p) { return decodeClientList(p); });
    auto badHolder = golden;
    badHolder.back() = 2;
    CHECK_FALSE(decodeClientList(badHolder).has_value());
    auto longer = golden;
    longer[0] = 3;
    CHECK_FALSE(decodeClientList(longer).has_value());

    std::vector<ClientRecord> many(255, ClientRecord{1, "n", false});
    CHECK(decodeClientList(encodeClientList(many))->size() == 255);
    many.push_back(ClientRecord{});
    CHECK(throwsUsage([&] { encodeClientList(many); }));
    CHECK(throwsUsage([] { encodeClientList(std::vector{ClientRecord{1, std::string(256, 'x'), false}}); }));
    CHECK(decodeClientList(encodeClientList(std::vector{ClientRecord{1, std::string(255, 'x'), true}})).has_value());
}

TEST_CASE("agent TimePing golden bytes")
{
    const Bytes golden{0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    CHECK(encodeTimePing(0x1122334455667788ULL) == golden);
    auto decoded = decodeTimePing(golden);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == 0x1122334455667788ULL);
    requireEveryPrefixRefused(golden, [](auto p) { return decodeTimePing(p); });
    requireTrailingByteRefused(golden, [](auto p) { return decodeTimePing(p); });
}

TEST_CASE("agent Lease golden bytes, bounds and refusals")
{
    LeaseEvent event;
    event.holder_id = 0x0102;
    event.holder_name = "cli";
    event.previous_id = 0x0304;
    event.reason = 1;
    const Bytes golden{0x02, 0x01, 0x03, 'c', 'l', 'i', 0x04, 0x03, 0x01};
    CHECK(encodeLease(event) == golden);
    auto decoded = decodeLease(golden);
    REQUIRE(decoded.has_value());
    CHECK(decoded->holder_id == 0x0102);
    CHECK(decoded->holder_name == "cli");
    CHECK(decoded->previous_id == 0x0304);
    CHECK(decoded->reason == 1);
    requireEveryPrefixRefused(golden, [](auto p) { return decodeLease(p); });
    requireTrailingByteRefused(golden, [](auto p) { return decodeLease(p); });

    CHECK(encodeLease(LeaseEvent{}) == Bytes{0, 0, 0, 0, 0, 0});
    event.holder_name.assign(255, 'n');
    CHECK(decodeLease(encodeLease(event)).has_value());
    event.holder_name.assign(256, 'n');
    CHECK(throwsUsage([&] { encodeLease(event); }));
}

TEST_CASE("agent ClockModel golden bytes and round trip")
{
    ClockModel model;
    model.offset_valid = true;
    model.drift_valid = false;
    model.device_reference_us = 0x0102030405060708ULL;
    model.host_reference_us = 2;
    model.span_us = 3;
    model.age_us = 4;
    model.samples = 0x0A0B0C0D;
    model.slope = 1.0;
    model.residual_us = 2.0;
    model.min_rtt_us = -1.0;
    model.max_rtt_us = 0.5;
    const Bytes golden{0x01,
                       0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
                       0x02, 0, 0, 0, 0, 0, 0, 0,
                       0x03, 0, 0, 0, 0, 0, 0, 0,
                       0x04, 0, 0, 0, 0, 0, 0, 0,
                       0x0D, 0x0C, 0x0B, 0x0A,
                       0, 0, 0, 0, 0, 0, 0xF0, 0x3F,  // 1.0
                       0, 0, 0, 0, 0, 0, 0x00, 0x40,  // 2.0
                       0, 0, 0, 0, 0, 0, 0xF0, 0xBF,  // -1.0
                       0, 0, 0, 0, 0, 0, 0xE0, 0x3F}; // 0.5
    REQUIRE(golden.size() == 69);
    CHECK(encodeClockModel(model) == golden);
    auto decoded = decodeClockModel(golden);
    REQUIRE(decoded.has_value());
    CHECK(decoded->offset_valid);
    CHECK_FALSE(decoded->drift_valid);
    CHECK(decoded->device_reference_us == model.device_reference_us);
    CHECK(decoded->samples == model.samples);
    CHECK(decoded->slope == 1.0);
    CHECK(decoded->min_rtt_us == -1.0);
    CHECK(decoded->updated_us == 0);
    CHECK(decoded->drift_ppm == 0.0);

    const ClockModel full = sampleModel();
    auto roundTrip = decodeClockModel(encodeClockModel(full));
    REQUIRE(roundTrip.has_value());
    CHECK(roundTrip->offset_valid);
    CHECK(roundTrip->drift_valid);
    CHECK(roundTrip->host_reference_us == full.host_reference_us);
    CHECK(roundTrip->span_us == full.span_us);
    CHECK(roundTrip->age_us == full.age_us);
    CHECK(roundTrip->samples == full.samples);
    CHECK(roundTrip->slope == full.slope);
    CHECK(roundTrip->residual_us == full.residual_us);
    CHECK(roundTrip->max_rtt_us == full.max_rtt_us);
    CHECK(roundTrip->drift_ppm == doctest::Approx(20.0).epsilon(1e-6));

    requireEveryPrefixRefused(golden, [](auto p) { return decodeClockModel(p); });
    requireTrailingByteRefused(golden, [](auto p) { return decodeClockModel(p); });
    for (const uint8_t flags : {uint8_t{4}, uint8_t{0x80}, uint8_t{0xFF}})
    {
        auto bad = golden;
        bad[0] = flags;
        CHECK_FALSE(decodeClockModel(bad).has_value());
    }
    ClockModel many = full;
    many.samples = static_cast<size_t>(1) << 40;
    auto saturated = decodeClockModel(encodeClockModel(many));
    REQUIRE(saturated.has_value());
    CHECK(saturated->samples == 0xFFFFFFFFu);
}

TEST_CASE("agent composeClock composes device to agent with agent to client")
{
    ClockModel d2a = sampleModel();
    d2a.updated_us = 900;
    ClockModel identity;
    identity.offset_valid = true;
    identity.drift_valid = true;
    identity.slope = 1;
    identity.age_us = 10;
    identity.span_us = 5000000;
    identity.samples = 20;
    identity.updated_us = 7000;
    auto same = composeClock(d2a, identity);
    CHECK(same.offset_valid);
    CHECK(same.drift_valid);
    for (uint64_t t = 0; t < 5000000; t += 123457)
        CHECK(std::abs(same.map(t) - d2a.map(t)) < 1.0);
    CHECK(same.slope == d2a.slope);
    CHECK(same.span_us == 3000000);
    CHECK(same.samples == 8);
    CHECK(same.age_us == 250);
    CHECK(same.updated_us == 7000 - 240);
    CHECK(same.residual_us == doctest::Approx(1.5));
    CHECK(same.min_rtt_us == doctest::Approx(400));

    ClockModel a2c;
    a2c.offset_valid = true;
    a2c.drift_valid = true;
    a2c.device_reference_us = 4000;
    a2c.host_reference_us = 5000; // +1000 us at the reference
    a2c.slope = 1.0 + 50e-6;
    a2c.residual_us = 2;
    a2c.min_rtt_us = 100;
    a2c.max_rtt_us = 200;
    a2c.span_us = 1000;
    a2c.samples = 3;
    a2c.age_us = 400;
    a2c.updated_us = 100;
    auto composed = composeClock(d2a, a2c);
    REQUIRE(composed.offset_valid);
    CHECK(composed.slope == doctest::Approx(d2a.slope * a2c.slope).epsilon(1e-15));
    CHECK(composed.drift_ppm == doctest::Approx((composed.slope - 1) * 1e6));
    CHECK(composed.residual_us == doctest::Approx(3.5));
    CHECK(composed.min_rtt_us == doctest::Approx(500));
    CHECK(composed.max_rtt_us == doctest::Approx(1100));
    CHECK(composed.span_us == 1000);
    CHECK(composed.samples == 3);
    CHECK(composed.age_us == 400);
    CHECK(composed.updated_us == 100); // the device model is the younger one: no extra age
    for (uint64_t t = 1000; t < 100000000; t += 3333333)
        CHECK(std::abs(composed.map(t) - a2c.map(static_cast<uint64_t>(std::llround(d2a.map(t))))) < 1.0);

    // Either side invalid: invalid, flags only.
    ClockModel invalid;
    auto left = composeClock(invalid, a2c);
    CHECK_FALSE(left.offset_valid);
    CHECK_FALSE(left.drift_valid);
    CHECK(left.updated_us == a2c.updated_us);
    auto right = composeClock(d2a, invalid);
    CHECK_FALSE(right.offset_valid);
    CHECK_FALSE(right.drift_valid);
    ClockModel noDrift = a2c;
    noDrift.drift_valid = false;
    auto partial = composeClock(d2a, noDrift);
    CHECK(partial.offset_valid);
    CHECK_FALSE(partial.drift_valid);
}

TEST_CASE("agent History golden chunk and round trip")
{
    const std::vector<HistoryEntry> entries{entry(4, 0x0102, {1, 2, 3, 4, 5, 6}), entry(5, 9, {})};
    auto chunks = encodeHistory(entries, 1024);
    REQUIRE(chunks.size() == 1);
    const Bytes golden{0x01, 0x02,
                       0x04, 0x02, 0x01, 0, 0, 0, 0, 0, 0, 0x06, 1, 2, 3, 4, 5, 6,
                       0x05, 0x09, 0, 0, 0, 0, 0, 0, 0, 0x00};
    CHECK(chunks[0] == golden);
    auto decoded = decodeHistory(golden);
    REQUIRE(decoded.has_value());
    CHECK(decoded->last);
    REQUIRE(decoded->entries.size() == 2);
    CHECK(decoded->entries[0].kind == 4);
    CHECK(decoded->entries[0].unix_ms == 0x0102);
    CHECK(decoded->entries[0].body == Bytes{1, 2, 3, 4, 5, 6});
    CHECK(decoded->entries[1].kind == 5);
    CHECK(decoded->entries[1].body.empty());

    requireEveryPrefixRefused(golden, [](auto p) { return decodeHistory(p); });
    requireTrailingByteRefused(golden, [](auto p) { return decodeHistory(p); });
    auto badLast = golden;
    badLast[0] = 2;
    CHECK_FALSE(decodeHistory(badLast).has_value());
    auto badCount = golden;
    badCount[1] = 3;
    CHECK_FALSE(decodeHistory(badCount).has_value());
    badCount[1] = 1;
    CHECK_FALSE(decodeHistory(badCount).has_value());
    auto badLen = golden;
    badLen[2 + 9] = 200;
    CHECK_FALSE(decodeHistory(badLen).has_value());

    // Empty history: one empty chunk with last = 1.
    auto empty = encodeHistory({}, 2);
    REQUIRE(empty.size() == 1);
    CHECK(empty[0] == Bytes{1, 0});
    auto emptyDecoded = decodeHistory(empty[0]);
    REQUIRE(emptyDecoded.has_value());
    CHECK(emptyDecoded->last);
    CHECK(emptyDecoded->entries.empty());
}

TEST_CASE("agent History chunks at a small size and refuses what cannot fit")
{
    std::vector<HistoryEntry> entries;
    for (uint8_t i = 0; i < 10; ++i)
        entries.push_back(entry(1, 100 + i, Bytes(i % 3 == 0 ? 6 : 2, i)));
    const size_t maxBody = 40;
    auto chunks = encodeHistory(entries, maxBody);
    REQUIRE(chunks.size() > 2);
    std::vector<HistoryEntry> joined;
    for (size_t i = 0; i < chunks.size(); ++i)
    {
        CHECK(chunks[i].size() <= maxBody);
        auto decoded = decodeHistory(chunks[i]);
        REQUIRE(decoded.has_value());
        CHECK(decoded->last == (i + 1 == chunks.size()));
        CHECK_FALSE(decoded->entries.empty());
        for (auto &item : decoded->entries)
            joined.push_back(std::move(item));
    }
    REQUIRE(joined.size() == entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
    {
        CHECK(joined[i].kind == entries[i].kind);
        CHECK(joined[i].unix_ms == entries[i].unix_ms);
        CHECK(joined[i].body == entries[i].body);
    }

    // The largest entry that fits exactly, and one byte less room.
    const std::vector<HistoryEntry> one{entry(1, 1, Bytes(10, 7))};
    CHECK(encodeHistory(one, 2 + 10 + 10).size() == 1);
    CHECK(throwsUsage([&] { encodeHistory(one, 2 + 10 + 10 - 1); }));
    CHECK(throwsUsage([&] { encodeHistory(std::vector{entry(1, 1, Bytes(256, 0))}, 4096); }));
    CHECK(encodeHistory(std::vector{entry(1, 1, Bytes(255, 0))}, 2 + 10 + 255).size() == 1);

    // At most 255 entries per chunk, even when more would fit.
    std::vector<HistoryEntry> many(300, entry(5, 0, {}));
    auto manyChunks = encodeHistory(many, 4096);
    REQUIRE(manyChunks.size() == 2);
    CHECK(manyChunks[0][0] == 0);
    CHECK(manyChunks[0][1] == 255);
    CHECK(manyChunks[1][0] == 1);
    CHECK(manyChunks[1][1] == 45);
}
