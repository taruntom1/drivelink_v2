// Agent discovery: pure packet building and parsing (docs/protocol/agent.md,
// "Discovery"). Names and records are decoded with mjansson/mdns; packets we
// send are built here (no name compression) so they are testable byte for byte.
#include "drivelink/discovery.hpp"
#include <mdns.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <utility>

namespace drivelink::discovery
{
namespace
{
constexpr uint16_t kTypeA = 1, kTypePtr = 12, kTypeTxt = 16, kTypeSrv = 33, kTypeAny = 255;
constexpr uint16_t kClassIn = 1, kCacheFlush = 0x8000, kUnicastBit = 0x8000;
constexpr uint32_t kLegacyTtl = 10; // RFC 6762 6.7
constexpr size_t kNameCapacity = 512;

std::string lower(std::string_view text)
{
    std::string out(text);
    for (auto &c : out) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return out;
}

// Lower case without one trailing dot: the form names are compared in.
std::string canonical(std::string_view name)
{
    if (!name.empty() && name.back() == '.') { name.remove_suffix(1); }
    return lower(name);
}

void put16(std::vector<uint8_t> &out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put32(std::vector<uint8_t> &out, uint32_t v)
{
    put16(out, static_cast<uint16_t>(v >> 16));
    put16(out, static_cast<uint16_t>(v));
}

// A domain name as uncompressed labels. The instance label is passed apart
// because it may contain any byte but '.' (instanceError keeps it that way).
void putName(std::vector<uint8_t> &out, std::string_view name)
{
    if (!name.empty() && name.back() == '.') { name.remove_suffix(1); }
    while (!name.empty())
    {
        const auto dot = name.find('.');
        const auto label = name.substr(0, dot);
        out.push_back(static_cast<uint8_t>(std::min<size_t>(label.size(), 63)));
        out.insert(out.end(), label.begin(), label.begin() + static_cast<ptrdiff_t>(std::min<size_t>(label.size(), 63)));
        if (dot == std::string_view::npos) { break; }
        name.remove_prefix(dot + 1);
    }
    out.push_back(0);
}

std::string serviceType() { return std::string(kServiceType.substr(0, kServiceType.size() - 1)); } // no trailing dot

std::string instanceName(const Advert &advert) { return advert.instance + "." + serviceType(); }
std::string hostName(const Advert &advert) { return advert.host + ".local"; }

struct Record
{
    std::string name;
    uint16_t type = 0;
    bool unique = false;
    uint32_t ttl = 0;
    std::vector<uint8_t> data;
};

Record ptrRecord(std::string owner, std::string_view target)
{
    Record r{std::move(owner), kTypePtr, false, kServiceTtl, {}};
    putName(r.data, target);
    return r;
}

Record srvRecord(const Advert &advert)
{
    Record r{instanceName(advert), kTypeSrv, true, kHostTtl, {}};
    put16(r.data, 0); // priority
    put16(r.data, 0); // weight
    put16(r.data, advert.port);
    putName(r.data, hostName(advert));
    return r;
}

Record txtRecord(const Advert &advert)
{
    Record r{instanceName(advert), kTypeTxt, true, kServiceTtl, {}};
    for (const auto &entry : {std::string("txtvers=") + std::to_string(kTxtVersion),
                              std::string("proto=") + std::to_string(advert.proto),
                              std::string("version=") + advert.version})
    {
        const auto size = std::min<size_t>(entry.size(), 255);
        r.data.push_back(static_cast<uint8_t>(size));
        r.data.insert(r.data.end(), entry.begin(), entry.begin() + static_cast<ptrdiff_t>(size));
    }
    return r;
}

Record aRecord(const Advert &advert)
{
    Record r{hostName(advert), kTypeA, true, kHostTtl, {}};
    put32(r.data, advert.address);
    return r;
}

// Instance label from a full instance name, if it is one of ours.
std::optional<std::string> instanceLabel(std::string_view full)
{
    const auto suffix = "." + serviceType();
    const auto name = full.ends_with('.') ? full.substr(0, full.size() - 1) : full;
    if (name.size() <= suffix.size()) { return std::nullopt; }
    if (canonical(name.substr(name.size() - suffix.size())) != suffix) { return std::nullopt; }
    return std::string(name.substr(0, name.size() - suffix.size()));
}

std::string extractName(std::span<const uint8_t> packet, size_t offset)
{
    std::array<char, kNameCapacity> buffer{};
    const auto s = mdns_string_extract(packet.data(), packet.size(), &offset, buffer.data(), buffer.size());
    return std::string(s.str ? s.str : "", s.length);
}

uint16_t read16(std::span<const uint8_t> p, size_t at) { return static_cast<uint16_t>((p[at] << 8) | p[at + 1]); }
} // namespace

std::string formatIpv4(uint32_t address)
{
    return std::to_string(address >> 24) + "." + std::to_string((address >> 16) & 0xFF) + "." +
           std::to_string((address >> 8) & 0xFF) + "." + std::to_string(address & 0xFF);
}

std::optional<uint32_t> parseIpv4(std::string_view text)
{
    uint32_t out = 0;
    for (int part = 0; part < 4; ++part)
    {
        const auto end = part < 3 ? text.find('.') : text.size();
        if (end == std::string_view::npos || end == 0 || end > 3) { return std::nullopt; }
        const auto digits = text.substr(0, end);
        if (digits.size() > 1 && digits[0] == '0') { return std::nullopt; }
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (ec != std::errc() || ptr != digits.data() + digits.size() || value > 255) { return std::nullopt; }
        out = (out << 8) | value;
        text.remove_prefix(part < 3 ? end + 1 : end);
    }
    return out;
}

std::string FoundAgent::address() const
{
    if (port == 0) { return {}; }
    std::string ip;
    if (!from.empty() && std::find(addresses.begin(), addresses.end(), from) != addresses.end()) { ip = from; }
    else if (!addresses.empty()) { ip = addresses.front(); }
    else { ip = from; }
    return ip.empty() ? std::string() : ip + ":" + std::to_string(port);
}

std::string FoundAgent::endpoint() const
{
    const auto a = address();
    return a.empty() ? std::string() : "tcp://" + a;
}

std::string instanceError(std::string_view instance)
{
    if (instance.empty() || instance.size() > 63) { return "must be 1..63 bytes"; }
    for (size_t i = 0; i < instance.size();)
    {
        const auto c = static_cast<unsigned char>(instance[i]);
        if (c == '.') { return "must not contain '.'"; }
        if (c < 0x20 || c == 0x7F) { return "must not contain control characters"; }
        size_t extra = 0;
        uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else { return "must be valid UTF-8"; }
        if (i + extra >= instance.size()) { return "must be valid UTF-8"; }
        for (size_t k = 1; k <= extra; ++k)
        {
            const auto cc = static_cast<unsigned char>(instance[i + k]);
            if ((cc & 0xC0) != 0x80) { return "must be valid UTF-8"; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        const uint32_t min[] = {0, 0x80, 0x800, 0x10000};
        if (cp < min[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { return "must be valid UTF-8"; }
        if (cp >= 0x80 && cp <= 0x9F) { return "must not contain control characters"; }
        i += extra + 1;
    }
    return {};
}

std::string hostLabel(std::string_view hostname)
{
    const auto first = hostname.substr(0, hostname.find('.'));
    std::string label = "drivelink-";
    for (const char raw : first)
    {
        const auto c = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
        label.push_back(((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') ? c : '-');
    }
    if (label.size() > 63) { label.resize(63); }
    while (!label.empty() && label.back() == '-') { label.pop_back(); }
    return label;
}

Answer match(const Advert &advert, uint16_t rtype, std::string_view name)
{
    const auto n = canonical(name);
    const bool any = rtype == kTypeAny;
    if (n == canonical(kServicesQuery) && (rtype == kTypePtr || any)) { return Answer::Services; }
    if (n == serviceType() && (rtype == kTypePtr || any)) { return Answer::Service; }
    if (n == lower(instanceName(advert)))
    {
        if (rtype == kTypeSrv) { return Answer::Srv; }
        if (rtype == kTypeTxt) { return Answer::Txt; }
        if (any) { return Answer::Service; }
    }
    if (n == lower(hostName(advert)) && (rtype == kTypeA || any)) { return Answer::A; }
    return Answer::None;
}

std::vector<uint8_t> buildResponse(const Advert &advert, Answer answer, bool goodbye, uint16_t query_id,
                                   const Question *legacy)
{
    std::vector<Record> answers, additional;
    switch (answer)
    {
    case Answer::None: return {};
    case Answer::Services: answers.push_back(ptrRecord(std::string("_services._dns-sd._udp.local"), serviceType())); break;
    case Answer::Service:
        answers.push_back(ptrRecord(serviceType(), instanceName(advert)));
        additional = {srvRecord(advert), txtRecord(advert), aRecord(advert)};
        break;
    case Answer::Srv:
        answers.push_back(srvRecord(advert));
        additional.push_back(aRecord(advert));
        break;
    case Answer::Txt: answers.push_back(txtRecord(advert)); break;
    case Answer::A: answers.push_back(aRecord(advert)); break;
    }
    std::vector<uint8_t> out;
    put16(out, legacy ? query_id : 0);
    put16(out, 0x8400); // response, authoritative
    put16(out, legacy ? 1 : 0);
    put16(out, static_cast<uint16_t>(answers.size()));
    put16(out, 0);
    put16(out, static_cast<uint16_t>(additional.size()));
    if (legacy)
    {
        putName(out, legacy->name);
        put16(out, legacy->rtype);
        put16(out, static_cast<uint16_t>(legacy->rclass & ~kUnicastBit));
    }
    for (const auto *section : {&answers, &additional})
    {
        for (const auto &r : *section)
        {
            putName(out, r.name);
            put16(out, r.type);
            put16(out, static_cast<uint16_t>(kClassIn | (r.unique && !legacy ? kCacheFlush : 0)));
            put32(out, goodbye ? 0 : legacy ? std::min(r.ttl, kLegacyTtl) : r.ttl);
            put16(out, static_cast<uint16_t>(r.data.size()));
            out.insert(out.end(), r.data.begin(), r.data.end());
        }
    }
    return out;
}

std::vector<uint8_t> buildQuery(std::string_view name, uint16_t rtype, bool unicast)
{
    std::vector<uint8_t> out;
    put16(out, 0);
    put16(out, 0);
    put16(out, 1);
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    putName(out, name);
    put16(out, rtype);
    put16(out, static_cast<uint16_t>(kClassIn | (unicast ? kUnicastBit : 0)));
    return out;
}

std::optional<ParsedQuery> parseQuery(std::span<const uint8_t> packet)
{
    if (packet.size() < 12) { return std::nullopt; }
    ParsedQuery q;
    q.id = read16(packet, 0);
    q.flags = read16(packet, 2);
    const auto count = read16(packet, 4);
    size_t offset = 12;
    for (uint16_t i = 0; i < count; ++i)
    {
        const auto start = offset;
        if (!mdns_string_skip(packet.data(), packet.size(), &offset) || offset + 4 > packet.size()) { break; }
        ParsedQuestion question{extractName(packet, start), read16(packet, offset), read16(packet, offset + 2)};
        offset += 4;
        q.questions.push_back(std::move(question));
    }
    return q;
}

void Collector::add(std::span<const uint8_t> packet, uint32_t from)
{
    if (packet.size() < 12) { return; }
    const auto flags = read16(packet, 2);
    if ((flags & 0x8000) == 0) { return; } // a query, not a response
    const uint16_t questions = read16(packet, 4);
    const size_t records = size_t{read16(packet, 6)} + read16(packet, 8) + read16(packet, 10);
    size_t offset = 12;
    for (uint16_t i = 0; i < questions; ++i)
    {
        if (!mdns_string_skip(packet.data(), packet.size(), &offset) || offset + 4 > packet.size()) { return; }
        offset += 4;
    }
    struct Context
    {
        Collector *self;
        uint32_t from;
    } context{this, from};
    const auto callback = [](int, const struct sockaddr *, size_t, mdns_entry_type_t, uint16_t, uint16_t rtype,
                             uint16_t, uint32_t ttl, const void *data, size_t size, size_t name_offset, size_t,
                             size_t record_offset, size_t record_length, void *user) -> int {
        auto &ctx = *static_cast<Context *>(user);
        auto &c = *ctx.self;
        const std::span<const uint8_t> p(static_cast<const uint8_t *>(data), size);
        const auto name = canonical(extractName(p, name_offset));
        std::array<char, kNameCapacity> buffer{};
        if (rtype == kTypePtr)
        {
            if (name != serviceType()) { return 0; }
            const auto s = mdns_record_parse_ptr(data, size, record_offset, record_length, buffer.data(), buffer.size());
            const auto target = canonical(std::string_view(s.str ? s.str : "", s.length));
            if (!instanceLabel(target)) { return 0; }
            const auto it = std::find_if(c.m_instances.begin(), c.m_instances.end(),
                                         [&](const auto &e) { return canonical(e.first) == target; });
            if (ttl == 0)
            {
                if (it != c.m_instances.end()) { c.m_instances.erase(it); }
            }
            else if (it == c.m_instances.end())
            {
                c.m_instances.emplace_back(std::string(s.str ? s.str : "", s.length), ctx.from);
            }
        }
        else if (rtype == kTypeSrv)
        {
            const auto srv = mdns_record_parse_srv(data, size, record_offset, record_length, buffer.data(), buffer.size());
            std::erase_if(c.m_srv, [&](const Srv &e) { return e.name == name; });
            if (ttl != 0 && srv.name.length > 0)
            {
                c.m_srv.push_back({name, canonical(std::string_view(srv.name.str, srv.name.length)), srv.port});
            }
        }
        else if (rtype == kTypeTxt)
        {
            std::array<mdns_record_txt_t, 16> entries{};
            const auto n = mdns_record_parse_txt(data, size, record_offset, record_length, entries.data(), entries.size());
            std::erase_if(c.m_txt, [&](const Txt &e) { return e.name == name; });
            if (ttl != 0)
            {
                Txt txt{name, {}};
                for (size_t i = 0; i < n; ++i)
                {
                    txt.entries.emplace_back(lower(std::string_view(entries[i].key.str, entries[i].key.length)),
                                             entries[i].value.str ? std::string(entries[i].value.str, entries[i].value.length)
                                                                  : std::string());
                }
                c.m_txt.push_back(std::move(txt));
            }
        }
        else if (rtype == kTypeA && record_length == 4)
        {
            const uint32_t address = (uint32_t{p[record_offset]} << 24) | (uint32_t{p[record_offset + 1]} << 16) |
                                     (uint32_t{p[record_offset + 2]} << 8) | p[record_offset + 3];
            const auto it = std::find_if(c.m_a.begin(), c.m_a.end(),
                                         [&](const A &e) { return e.name == name && e.address == address; });
            if (ttl == 0)
            {
                if (it != c.m_a.end()) { c.m_a.erase(it); }
            }
            else if (it == c.m_a.end())
            {
                c.m_a.push_back({name, address});
            }
        }
        return 0;
    };
    // The answer, authority and additional sections are parsed as one run:
    // every record is treated alike.
    mdns_records_parse(0, nullptr, 0, packet.data(), packet.size(), &offset, MDNS_ENTRYTYPE_ANSWER, 0, records,
                       callback, &context);
}

std::vector<FoundAgent> Collector::agents() const
{
    std::vector<FoundAgent> out;
    for (const auto &[full, from] : m_instances)
    {
        const auto key = canonical(full);
        const auto srv = std::find_if(m_srv.begin(), m_srv.end(), [&](const Srv &s) { return s.name == key; });
        if (srv == m_srv.end() || srv->port == 0) { continue; }
        FoundAgent agent;
        agent.instance = *instanceLabel(full);
        agent.host = srv->target + ".";
        agent.port = srv->port;
        agent.from = from ? formatIpv4(from) : std::string();
        for (const auto &a : m_a)
        {
            if (a.name == srv->target) { agent.addresses.push_back(formatIpv4(a.address)); }
        }
        const auto txt = std::find_if(m_txt.begin(), m_txt.end(), [&](const Txt &t) { return t.name == key; });
        if (txt != m_txt.end())
        {
            for (const auto &[k, v] : txt->entries)
            {
                if (k == "proto")
                {
                    unsigned value = 0;
                    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), value);
                    if (ec == std::errc() && ptr == v.data() + v.size() && !v.empty()) { agent.proto = value; }
                }
                else if (k == "version") { agent.version = v; }
            }
        }
        if (agent.address().empty()) { continue; }
        out.push_back(std::move(agent));
    }
    std::sort(out.begin(), out.end(), [](const FoundAgent &a, const FoundAgent &b) {
        return std::pair(a.instance, a.address()) < std::pair(b.instance, b.address());
    });
    return out;
}
} // namespace drivelink::discovery
