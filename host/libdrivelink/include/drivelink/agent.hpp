#pragma once
// Agent protocol v2 (docs/protocol/agent.md): the messages between a host
// client and drivelink-agent. Pure codecs shared by the agent and its clients;
// payloads only (framing is protocol/framing.hpp). Decoders validate exact
// lengths and string bounds and return Error{ErrorKind::Protocol} otherwise.
#include "error.hpp"
#include "time.hpp"
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
inline constexpr uint8_t kVersion = 2;
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

// Device record: alias str8 | online u8 | device_id[6] | name str8 | endpoint str8 | clients u8 |
//                holder_id u16 | holder_name str8   (holder_id 0: nobody holds the lease)
struct DeviceRecord
{
    std::string alias;
    bool online = false;
    std::array<uint8_t, 6> device_id{};
    std::string name, endpoint;
    uint8_t clients = 0;
    uint16_t holder_id = 0;
    std::string holder_name;
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

// ClientList answer: count u8 | count * (client_id u16 | name str8 | holder u8)
struct ClientRecord
{
    uint16_t client_id = 0;
    std::string name;
    bool holder = false;
};
Bytes encodeClientList(std::span<const ClientRecord> clients); // throws Error{Usage} if > 255 or a string > 255
std::expected<std::vector<ClientRecord>, Error> decodeClientList(std::span<const uint8_t> body);
// TimePing answer: agent_us u64 (the agent's monotonic clock when it read the ping)
Bytes encodeTimePing(uint64_t agent_us);
std::expected<uint64_t, Error> decodeTimePing(std::span<const uint8_t> body);

// Lease event body (also a Lease history entry's body):
// holder_id u16 | holder_name str8 | previous_id u16 | reason u8 (LeaseChange)
struct LeaseEvent
{
    uint16_t holder_id = 0; // 0: nobody holds it now
    std::string holder_name;
    uint16_t previous_id = 0; // 0: nobody held it before
    uint8_t reason = 0;
};
Bytes encodeLease(const LeaseEvent &event); // throws Error{Usage} if the name > 255
std::expected<LeaseEvent, Error> decodeLease(std::span<const uint8_t> body);

// ClockModel event body: the agent's device-to-agent model, host domain the
// agent's monotonic clock (69 bytes):
// flags u8 (bit 0 offset_valid, bit 1 drift_valid, others 0) | device_reference_us u64 |
// host_reference_us u64 | span_us u64 | age_us u64 | samples u32 | slope f64 |
// residual_us f64 | min_rtt_us f64 | max_rtt_us f64   (f64: IEEE 754 binary64, little-endian)
// Decoding sets updated_us to 0 and drift_ppm from the slope; samples saturate at u32.
Bytes encodeClockModel(const ClockModel &model);
std::expected<ClockModel, Error> decodeClockModel(std::span<const uint8_t> body);
// Composes device->agent with agent->client into device->client. Valid only when
// both are; slopes multiply, residuals and RTTs add, span and samples are the
// smaller, ages the larger; updated_us is the client's (agent_to_client.updated_us)
// minus the extra age of the device model.
ClockModel composeClock(const ClockModel &device_to_agent, const ClockModel &agent_to_client);

// History entry: kind u8 (HistoryKind) | unix_ms u64 | len u8 | body[len]
//   DiagEvent: the diag_event payload as the device sent it
//   Arm:       state u8 | last_disarm_reason u8 (from the status stream)
//   Lease:     a Lease event body
//   Online:    device_id[6]
//   Offline:   empty
struct HistoryEntry
{
    uint8_t kind = 0;
    uint64_t unix_ms = 0;
    Bytes body; // at most 255 bytes
};
// History event body: last u8 (1 on the final chunk) | count u8 | count * entry
struct HistoryChunk
{
    bool last = true;
    std::vector<HistoryEntry> entries;
};
// Splits entries (oldest first) into History event bodies of at most
// max_body bytes each; always at least one chunk (an empty history is one
// empty chunk with last = 1). Throws Error{Usage} if an entry body > 255 or one
// entry cannot fit max_body.
std::vector<Bytes> encodeHistory(std::span<const HistoryEntry> entries, size_t max_body);
std::expected<HistoryChunk, Error> decodeHistory(std::span<const uint8_t> body);

// Device ids are written as 12 lowercase hex digits (GET_INFO order).
std::string formatDeviceId(const std::array<uint8_t, 6> &id);
bool parseDeviceId(std::string_view text, std::array<uint8_t, 6> &id); // exactly 12 hex digits, either case
// Constant-time equality for the token (length is not secret).
bool tokenEquals(std::string_view a, std::string_view b);
} // namespace drivelink::agent
