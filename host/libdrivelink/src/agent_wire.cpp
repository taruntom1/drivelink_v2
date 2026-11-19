#include "drivelink/agent.hpp"
#include "drivelink/protocol/wire_codec.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>

namespace drivelink::agent
{
namespace
{
using protocol::wire::Reader;
using protocol::wire::Writer;

std::unexpected<Error> bad(const char *message)
{
    return std::unexpected(Error(ErrorKind::Protocol, message));
}

[[noreturn]] void usage(const char *message)
{
    throw Error(ErrorKind::Usage, message);
}

void checkString(const std::string &text, size_t min, size_t max, const char *what)
{
    if (text.size() < min || text.size() > max)
        usage(what);
}

size_t strSize(const std::string &text)
{
    return 1 + text.size();
}

void putString(Writer &writer, const std::string &text)
{
    writer.appendU8(static_cast<uint8_t>(text.size()));
    writer.appendBytes(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

// Reads `length u8 | bytes` with the length in min..max. `data` is the buffer the
// reader was built on, so the bytes can be copied out.
bool getString(Reader &reader, const uint8_t *data, size_t min, size_t max, std::string &out)
{
    const size_t length = reader.readU8();
    if (!reader.ok() || length < min || length > max || length > reader.remaining())
        return false;
    out.assign(reinterpret_cast<const char *>(data + reader.offset()), length);
    reader.skip(length);
    return reader.ok();
}

size_t recordSize(const DeviceRecord &record)
{
    return strSize(record.alias) + 1 + record.device_id.size() + strSize(record.name) + strSize(record.endpoint) + 1 + 2 +
           strSize(record.holder_name);
}

void checkRecord(const DeviceRecord &record)
{
    checkString(record.alias, 0, kMaxString, "agent device alias too long");
    checkString(record.name, 0, kMaxString, "agent device name too long");
    checkString(record.endpoint, 0, kMaxString, "agent device endpoint too long");
    checkString(record.holder_name, 0, kMaxString, "agent device holder name too long");
}

void writeRecord(Writer &writer, const DeviceRecord &record)
{
    putString(writer, record.alias);
    writer.appendU8(record.online ? 1 : 0);
    writer.appendBytes(record.device_id.data(), record.device_id.size());
    putString(writer, record.name);
    putString(writer, record.endpoint);
    writer.appendU8(record.clients);
    writer.appendU16Le(record.holder_id);
    putString(writer, record.holder_name);
}

bool readRecord(Reader &reader, const uint8_t *data, DeviceRecord &record)
{
    if (!getString(reader, data, 0, kMaxString, record.alias))
        return false;
    const uint8_t online = reader.readU8();
    if (!reader.ok() || online > 1)
        return false;
    record.online = online == 1;
    for (auto &byte : record.device_id)
        byte = reader.readU8();
    if (!getString(reader, data, 0, kMaxString, record.name) || !getString(reader, data, 0, kMaxString, record.endpoint))
        return false;
    record.clients = reader.readU8();
    record.holder_id = reader.readU16Le();
    return reader.ok() && getString(reader, data, 0, kMaxString, record.holder_name);
}

void putF64(Writer &writer, double value)
{
    writer.appendU64Le(std::bit_cast<uint64_t>(value));
}

double getF64(Reader &reader)
{
    return std::bit_cast<double>(reader.readU64Le());
}

constexpr size_t kClockModelSize = 69;
constexpr size_t kHistoryHeader = 2;
constexpr size_t kHistoryEntryFixed = 1 + 8 + 1;

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
} // namespace

Bytes encodeCommand(const Command &command)
{
    Bytes out(3 + command.body.size());
    Writer writer(out.data(), out.size());
    writer.appendU8(kVersion);
    writer.appendU8(command.seq);
    writer.appendU8(command.op);
    writer.appendBytes(command.body.data(), command.body.size());
    return out;
}

Bytes encodeResponse(const Response &response)
{
    Bytes out(5 + response.body.size());
    Writer writer(out.data(), out.size());
    writer.appendU8(kVersion);
    writer.appendU8(response.seq);
    writer.appendU8(response.op);
    writer.appendU8(response.result);
    writer.appendU8(response.detail);
    writer.appendBytes(response.body.data(), response.body.size());
    return out;
}

Bytes encodeEvent(const Event &event)
{
    Bytes out(2 + event.body.size());
    Writer writer(out.data(), out.size());
    writer.appendU8(kVersion);
    writer.appendU8(event.event);
    writer.appendBytes(event.body.data(), event.body.size());
    return out;
}

std::expected<Command, Error> decodeCommand(std::span<const uint8_t> payload)
{
    Reader reader(payload.data(), payload.size());
    const uint8_t version = reader.readU8();
    Command command;
    command.seq = reader.readU8();
    command.op = reader.readU8();
    if (!reader.ok())
        return bad("agent_command is shorter than its header");
    if (version != kVersion)
        return bad("agent_command has an unsupported agent protocol version");
    command.body.assign(payload.begin() + static_cast<std::ptrdiff_t>(reader.offset()), payload.end());
    return command;
}

std::expected<DecodedResponse, Error> decodeResponse(std::span<const uint8_t> payload)
{
    Reader reader(payload.data(), payload.size());
    DecodedResponse response;
    response.version = reader.readU8();
    response.seq = reader.readU8();
    response.op = reader.readU8();
    response.result = reader.readU8();
    response.detail = reader.readU8();
    if (!reader.ok())
        return bad("agent_response is shorter than its header");
    response.body.assign(payload.begin() + static_cast<std::ptrdiff_t>(reader.offset()), payload.end());
    return response;
}

std::expected<Event, Error> decodeEvent(std::span<const uint8_t> payload)
{
    Reader reader(payload.data(), payload.size());
    const uint8_t version = reader.readU8();
    Event event;
    event.event = reader.readU8();
    if (!reader.ok())
        return bad("agent_event is shorter than its header");
    if (version != kVersion)
        return bad("agent_event has an unsupported agent protocol version");
    event.body.assign(payload.begin() + static_cast<std::ptrdiff_t>(reader.offset()), payload.end());
    return event;
}

Bytes encodeHello(const Hello &hello)
{
    checkString(hello.token, 1, kMaxToken, "agent hello token must be 1..128 bytes");
    checkString(hello.device, 0, kMaxDevice, "agent hello device is longer than 64 bytes");
    checkString(hello.client_name, 0, kMaxClientName, "agent hello client name is longer than 32 bytes");
    Bytes out(1 + strSize(hello.token) + strSize(hello.device) + strSize(hello.client_name));
    Writer writer(out.data(), out.size());
    writer.appendU8(hello.core_version);
    putString(writer, hello.token);
    putString(writer, hello.device);
    putString(writer, hello.client_name);
    return out;
}

std::expected<Hello, Error> decodeHello(std::span<const uint8_t> body)
{
    Reader reader(body.data(), body.size());
    Hello hello;
    hello.core_version = reader.readU8();
    if (!reader.ok() || !getString(reader, body.data(), 1, kMaxToken, hello.token) ||
        !getString(reader, body.data(), 0, kMaxDevice, hello.device) ||
        !getString(reader, body.data(), 0, kMaxClientName, hello.client_name) || !reader.done())
        return bad("malformed agent Hello body");
    return hello;
}

void appendDeviceRecord(Bytes &out, const DeviceRecord &record)
{
    checkRecord(record);
    const size_t start = out.size();
    out.resize(start + recordSize(record));
    Writer writer(out.data() + start, out.size() - start);
    writeRecord(writer, record);
}

Bytes encodeHelloAccepted(const HelloAccepted &accepted)
{
    if (!accepted.lobby)
        checkRecord(accepted.device);
    Bytes out(3);
    Writer writer(out.data(), out.size());
    writer.appendU16Le(accepted.client_id);
    writer.appendU8(accepted.lobby ? 0 : 1);
    if (!accepted.lobby)
        appendDeviceRecord(out, accepted.device);
    return out;
}

std::expected<HelloAccepted, Error> decodeHelloAccepted(std::span<const uint8_t> body)
{
    Reader reader(body.data(), body.size());
    HelloAccepted accepted;
    accepted.client_id = reader.readU16Le();
    const uint8_t kind = reader.readU8();
    if (!reader.ok() || kind > 1)
        return bad("malformed agent Hello accepted body");
    accepted.lobby = kind == 0;
    if (!accepted.lobby && !readRecord(reader, body.data(), accepted.device))
        return bad("malformed agent device record");
    if (!reader.done())
        return bad("agent Hello accepted body has trailing bytes");
    return accepted;
}

Bytes encodeDeviceList(std::span<const DeviceRecord> devices)
{
    if (devices.size() > 255)
        usage("agent device list has more than 255 entries");
    for (const auto &device : devices)
        checkRecord(device);
    Bytes out;
    out.push_back(static_cast<uint8_t>(devices.size()));
    for (const auto &device : devices)
        appendDeviceRecord(out, device);
    return out;
}

std::expected<std::vector<DeviceRecord>, Error> decodeDeviceList(std::span<const uint8_t> body)
{
    Reader reader(body.data(), body.size());
    const uint8_t count = reader.readU8();
    if (!reader.ok())
        return bad("agent device list is empty");
    std::vector<DeviceRecord> devices;
    devices.reserve(count);
    for (uint8_t i = 0; i < count; ++i)
    {
        DeviceRecord record;
        if (!readRecord(reader, body.data(), record))
            return bad("malformed agent device record");
        devices.push_back(std::move(record));
    }
    if (!reader.done())
        return bad("agent device list has trailing bytes");
    return devices;
}

Bytes encodeVersionRefusal(const VersionRefusal &refusal)
{
    return Bytes{refusal.agent_version, refusal.core_version};
}

std::expected<VersionRefusal, Error> decodeVersionRefusal(std::span<const uint8_t> body)
{
    Reader reader(body.data(), body.size());
    VersionRefusal refusal;
    refusal.agent_version = reader.readU8();
    refusal.core_version = reader.readU8();
    if (!reader.done())
        return bad("malformed agent version refusal body");
    return refusal;
}

Bytes encodeClosing(uint8_t reason)
{
    return Bytes{reason};
}

std::expected<uint8_t, Error> decodeClosing(std::span<const uint8_t> body)
{
    Reader reader(body.data(), body.size());
    const uint8_t reason = reader.readU8();
    if (!reader.done())
        return bad("malformed agent Closing body");
    return reason;
}

Bytes encodeClientList(std::span<const ClientRecord> clients)
{
    if (clients.size() > 255)
        usage("agent client list has more than 255 entries");
    for (const auto &client : clients)
        checkString(client.name, 0, kMaxString, "agent client name too long");
    Bytes out;
    out.push_back(static_cast<uint8_t>(clients.size()));
    for (const auto &client : clients)
    {
        const size_t start = out.size();
        out.resize(start + 2 + strSize(client.name) + 1);
        Writer writer(out.data() + start, out.size() - start);
        writer.appendU16Le(client.client_id);
        putString(writer, client.name);
        writer.appendU8(client.holder ? 1 : 0);
    }
    return out;
}

std::expected<std::vector<ClientRecord>, Error> decodeClientList(std::span<const uint8_t> body)
{
    Reader reader(body.data(), body.size());
    const uint8_t count = reader.readU8();
    if (!reader.ok())
        return bad("agent client list is empty");
    std::vector<ClientRecord> clients;
    clients.reserve(count);
    for (uint8_t i = 0; i < count; ++i)
    {
        ClientRecord client;
        client.client_id = reader.readU16Le();
        if (!reader.ok() || !getString(reader, body.data(), 0, kMaxString, client.name))
            return bad("malformed agent client record");
        const uint8_t holder = reader.readU8();
        if (!reader.ok() || holder > 1)
            return bad("malformed agent client record");
        client.holder = holder == 1;
        clients.push_back(std::move(client));
    }
    if (!reader.done())
        return bad("agent client list has trailing bytes");
    return clients;
}

Bytes encodeTimePing(uint64_t agent_us)
{
    Bytes out(8);
    Writer writer(out.data(), out.size());
    writer.appendU64Le(agent_us);
    return out;
}

std::expected<uint64_t, Error> decodeTimePing(std::span<const uint8_t> body)
{
    Reader reader(body.data(), body.size());
    const uint64_t agent_us = reader.readU64Le();
    if (!reader.done())
        return bad("malformed agent TimePing body");
    return agent_us;
}

Bytes encodeLease(const LeaseEvent &event)
{
    checkString(event.holder_name, 0, kMaxString, "agent lease holder name too long");
    Bytes out(2 + strSize(event.holder_name) + 2 + 1);
    Writer writer(out.data(), out.size());
    writer.appendU16Le(event.holder_id);
    putString(writer, event.holder_name);
    writer.appendU16Le(event.previous_id);
    writer.appendU8(event.reason);
    return out;
}

std::expected<LeaseEvent, Error> decodeLease(std::span<const uint8_t> body)
{
    Reader reader(body.data(), body.size());
    LeaseEvent event;
    event.holder_id = reader.readU16Le();
    if (!reader.ok() || !getString(reader, body.data(), 0, kMaxString, event.holder_name))
        return bad("malformed agent Lease body");
    event.previous_id = reader.readU16Le();
    event.reason = reader.readU8();
    if (!reader.done())
        return bad("malformed agent Lease body");
    return event;
}

Bytes encodeClockModel(const ClockModel &model)
{
    Bytes out(kClockModelSize);
    Writer writer(out.data(), out.size());
    writer.appendU8(static_cast<uint8_t>((model.offset_valid ? 1 : 0) | (model.drift_valid ? 2 : 0)));
    writer.appendU64Le(model.device_reference_us);
    writer.appendU64Le(model.host_reference_us);
    writer.appendU64Le(model.span_us);
    writer.appendU64Le(model.age_us);
    writer.appendU32Le(static_cast<uint32_t>(std::min<size_t>(model.samples, std::numeric_limits<uint32_t>::max())));
    putF64(writer, model.slope);
    putF64(writer, model.residual_us);
    putF64(writer, model.min_rtt_us);
    putF64(writer, model.max_rtt_us);
    return out;
}

std::expected<ClockModel, Error> decodeClockModel(std::span<const uint8_t> body)
{
    if (body.size() != kClockModelSize)
        return bad("agent ClockModel body is not 69 bytes");
    Reader reader(body.data(), body.size());
    ClockModel model;
    const uint8_t flags = reader.readU8();
    if ((flags & ~uint8_t{3}) != 0)
        return bad("agent ClockModel has reserved flag bits set");
    model.offset_valid = (flags & 1) != 0;
    model.drift_valid = (flags & 2) != 0;
    model.device_reference_us = reader.readU64Le();
    model.host_reference_us = reader.readU64Le();
    model.span_us = reader.readU64Le();
    model.age_us = reader.readU64Le();
    model.samples = reader.readU32Le();
    model.slope = getF64(reader);
    model.residual_us = getF64(reader);
    model.min_rtt_us = getF64(reader);
    model.max_rtt_us = getF64(reader);
    if (!reader.done())
        return bad("malformed agent ClockModel body");
    model.updated_us = 0;
    model.drift_ppm = (model.slope - 1.0) * 1e6;
    return model;
}

ClockModel composeClock(const ClockModel &device_to_agent, const ClockModel &agent_to_client)
{
    ClockModel result;
    result.offset_valid = device_to_agent.offset_valid && agent_to_client.offset_valid;
    result.drift_valid = device_to_agent.drift_valid && agent_to_client.drift_valid;
    result.updated_us = agent_to_client.updated_us;
    if (!result.offset_valid)
        return result;
    result.device_reference_us = device_to_agent.device_reference_us;
    const double mapped = std::round(agent_to_client.map(device_to_agent.host_reference_us));
    if (!(mapped > 0.0))
        result.host_reference_us = 0;
    else if (mapped >= 9223372036854775807.0)
        result.host_reference_us = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    else
        result.host_reference_us = static_cast<uint64_t>(std::llround(mapped));
    result.slope = device_to_agent.slope * agent_to_client.slope;
    result.drift_ppm = (result.slope - 1.0) * 1e6;
    result.residual_us = device_to_agent.residual_us + agent_to_client.residual_us;
    result.min_rtt_us = device_to_agent.min_rtt_us + agent_to_client.min_rtt_us;
    result.max_rtt_us = device_to_agent.max_rtt_us + agent_to_client.max_rtt_us;
    result.span_us = std::min(device_to_agent.span_us, agent_to_client.span_us);
    result.samples = std::min(device_to_agent.samples, agent_to_client.samples);
    result.age_us = std::max(device_to_agent.age_us, agent_to_client.age_us);
    const uint64_t extra =
        device_to_agent.age_us > agent_to_client.age_us ? device_to_agent.age_us - agent_to_client.age_us : 0;
    result.updated_us = agent_to_client.updated_us >= extra ? agent_to_client.updated_us - extra : 0;
    return result;
}

std::vector<Bytes> encodeHistory(std::span<const HistoryEntry> entries, size_t max_body)
{
    for (const auto &entry : entries)
    {
        if (entry.body.size() > 255)
            usage("agent history entry body is longer than 255 bytes");
        if (kHistoryHeader + kHistoryEntryFixed + entry.body.size() > max_body)
            usage("agent history entry does not fit the event size");
    }
    if (max_body < kHistoryHeader)
        usage("agent history event size is too small");
    std::vector<Bytes> chunks;
    size_t index = 0;
    do
    {
        Bytes chunk{0, 0};
        size_t count = 0;
        while (index < entries.size() && count < 255 &&
               chunk.size() + kHistoryEntryFixed + entries[index].body.size() <= max_body)
        {
            const auto &entry = entries[index];
            const size_t start = chunk.size();
            chunk.resize(start + kHistoryEntryFixed + entry.body.size());
            Writer writer(chunk.data() + start, chunk.size() - start);
            writer.appendU8(entry.kind);
            writer.appendU64Le(entry.unix_ms);
            writer.appendU8(static_cast<uint8_t>(entry.body.size()));
            writer.appendBytes(entry.body.data(), entry.body.size());
            ++count;
            ++index;
        }
        chunk[0] = index == entries.size() ? 1 : 0;
        chunk[1] = static_cast<uint8_t>(count);
        chunks.push_back(std::move(chunk));
    } while (index < entries.size());
    return chunks;
}

std::expected<HistoryChunk, Error> decodeHistory(std::span<const uint8_t> body)
{
    Reader reader(body.data(), body.size());
    const uint8_t last = reader.readU8();
    const uint8_t count = reader.readU8();
    if (!reader.ok() || last > 1)
        return bad("malformed agent History body");
    HistoryChunk chunk;
    chunk.last = last == 1;
    chunk.entries.reserve(count);
    for (uint8_t i = 0; i < count; ++i)
    {
        HistoryEntry entry;
        entry.kind = reader.readU8();
        entry.unix_ms = reader.readU64Le();
        const size_t length = reader.readU8();
        if (!reader.ok() || length > reader.remaining())
            return bad("malformed agent History entry");
        entry.body.assign(body.begin() + static_cast<std::ptrdiff_t>(reader.offset()),
                          body.begin() + static_cast<std::ptrdiff_t>(reader.offset() + length));
        reader.skip(length);
        chunk.entries.push_back(std::move(entry));
    }
    if (!reader.done())
        return bad("agent History body has trailing bytes");
    return chunk;
}

std::string formatDeviceId(const std::array<uint8_t, 6> &id)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(12);
    for (const uint8_t byte : id)
    {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 0x0F]);
    }
    return text;
}

bool parseDeviceId(std::string_view text, std::array<uint8_t, 6> &id)
{
    if (text.size() != 12)
        return false;
    std::array<uint8_t, 6> parsed{};
    for (size_t i = 0; i < parsed.size(); ++i)
    {
        const int high = hexValue(text[2 * i]), low = hexValue(text[2 * i + 1]);
        if (high < 0 || low < 0)
            return false;
        parsed[i] = static_cast<uint8_t>(high << 4 | low);
    }
    id = parsed;
    return true;
}

bool tokenEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < a.size(); ++i)
        difference |= static_cast<uint8_t>(static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]));
    return difference == 0;
}
} // namespace drivelink::agent
