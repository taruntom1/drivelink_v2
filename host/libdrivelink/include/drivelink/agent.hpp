#pragma once
// Agent protocol v1 (docs/protocol/agent.md): the messages between a host
// client and drivelink-agent. Pure codecs shared by the agent and its clients;
// payloads only (framing is protocol/framing.hpp). Decoders validate exact
// lengths and string bounds and return Error{ErrorKind::Protocol} otherwise.
#include "error.hpp"
#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace drivelink::agent
{
using Bytes = std::vector<uint8_t>;
inline constexpr uint8_t kVersion = 1;
inline constexpr uint16_t kDefaultPort = 47620;
inline constexpr size_t kMaxToken = 128, kMaxDevice = 64, kMaxClientName = 32, kMaxString = 255;

// agent_command (0x30): version | seq | op | body
struct Command
{
    uint8_t seq = 0, op = 0;
    Bytes body;
};
// agent_response (0x70): version | seq | op | result | detail | body
struct Response
{
    uint8_t seq = 0, op = 0, result = 0, detail = 0;
    Bytes body;
};
// agent_event (0xF0): version | event | body
struct Event
{
    uint8_t event = 0;
    Bytes body;
};
Bytes encodeCommand(const Command &command);
Bytes encodeResponse(const Response &response);
Bytes encodeEvent(const Event &event);
// Decoders refuse a version byte other than kVersion, except decodeResponse,
// which returns the response with `version` set so a client can read a version
// refusal from any agent (see VersionRefusal).
struct DecodedResponse : Response
{
    uint8_t version = kVersion;
};
std::expected<Command, Error> decodeCommand(std::span<const uint8_t> payload);
std::expected<DecodedResponse, Error> decodeResponse(std::span<const uint8_t> payload);
std::expected<Event, Error> decodeEvent(std::span<const uint8_t> payload);

// Hello (op 1) request body: core_version u8 | token str8 | device str8 | client_name str8
struct Hello
{
    uint8_t core_version = 0;
    std::string token, device, client_name;
};
Bytes encodeHello(const Hello &hello);           // throws Error{Usage} on a bound violation
std::expected<Hello, Error> decodeHello(std::span<const uint8_t> body);

// Device record: alias str8 | online u8 | device_id[6] | name str8 | endpoint str8 | clients u8
struct DeviceRecord
{
    std::string alias;
    bool online = false;
    std::array<uint8_t, 6> device_id{};
    std::string name, endpoint;
    uint8_t clients = 0;
};
void appendDeviceRecord(Bytes &out, const DeviceRecord &record); // throws Error{Usage} if a string > 255
// Hello accepted body: client_id u16 | kind u8 (0 lobby, 1 device) | record (device only)
struct HelloAccepted
{
    uint16_t client_id = 0;
    bool lobby = true;
    DeviceRecord device; // meaningful when !lobby
};
Bytes encodeHelloAccepted(const HelloAccepted &accepted);
std::expected<HelloAccepted, Error> decodeHelloAccepted(std::span<const uint8_t> body);
// ListDevices answer: count u8 | count * record
Bytes encodeDeviceList(std::span<const DeviceRecord> devices); // throws Error{Usage} if > 255
std::expected<std::vector<DeviceRecord>, Error> decodeDeviceList(std::span<const uint8_t> body);
// AgentVersion refusal body: agent_version u8 | core_version u8
struct VersionRefusal
{
    uint8_t agent_version = 0, core_version = 0;
};
Bytes encodeVersionRefusal(const VersionRefusal &refusal);
std::expected<VersionRefusal, Error> decodeVersionRefusal(std::span<const uint8_t> body);
// Closing event body: reason u8 (AgentCloseReason)
Bytes encodeClosing(uint8_t reason);
std::expected<uint8_t, Error> decodeClosing(std::span<const uint8_t> body);

// Device ids are written as 12 lowercase hex digits (GET_INFO order).
std::string formatDeviceId(const std::array<uint8_t, 6> &id);
bool parseDeviceId(std::string_view text, std::array<uint8_t, 6> &id); // exactly 12 hex digits, either case
// Constant-time equality for the token (length is not secret).
bool tokenEquals(std::string_view a, std::string_view b);
} // namespace drivelink::agent
