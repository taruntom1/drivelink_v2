#include "drivelink/agent.hpp"
#include "drivelink/protocol/wire_codec.hpp"
#include <cstddef>

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
    return strSize(record.alias) + 1 + record.device_id.size() + strSize(record.name) + strSize(record.endpoint) + 1;
}

void checkRecord(const DeviceRecord &record)
{
    checkString(record.alias, 0, kMaxString, "agent device alias too long");
    checkString(record.name, 0, kMaxString, "agent device name too long");
    checkString(record.endpoint, 0, kMaxString, "agent device endpoint too long");
}

void writeRecord(Writer &writer, const DeviceRecord &record)
{
    putString(writer, record.alias);
    writer.appendU8(record.online ? 1 : 0);
    writer.appendBytes(record.device_id.data(), record.device_id.size());
    putString(writer, record.name);
    putString(writer, record.endpoint);
    writer.appendU8(record.clients);
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
    return reader.ok();
}

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
