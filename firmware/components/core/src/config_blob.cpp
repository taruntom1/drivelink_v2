// The NVS blob codec (core/config_blob.hpp), on the protocol's bounds-checked
// little-endian Reader / Writer.

#include "core/config_blob.hpp"

#include <cstring>

#include "drivelink/protocol/wire_codec.hpp"

namespace core {

namespace {

namespace wire = drivelink::protocol::wire;

constexpr size_t kCrcBytes = 4;

/// The length of `s`, at most `max`.
size_t boundedLen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0') {
        ++n;
    }
    return n;
}

/// Append the CRC of everything written so far.
void appendCrc(std::vector<uint8_t> &out, wire::Writer &w)
{
    w.appendU32Le(crc32(out.data(), w.size()));
}

/// The CRC check: the blob ends with the CRC of the bytes before it.
bool crcOk(std::span<const uint8_t> blob)
{
    if (blob.size() < kCrcBytes) {
        return false;
    }
    const size_t body = blob.size() - kCrcBytes;
    wire::Reader r(blob.data() + body, kCrcBytes);
    return r.readU32Le() == crc32(blob.data(), body);
}

} // namespace

std::vector<uint8_t> encodeInstanceBlob(uint32_t gen, std::span<const ParamPair> pairs)
{
    if (pairs.size() > kMaxInstancePairs) {
        return {};
    }
    std::vector<uint8_t> out(instanceBlobSize(pairs.size()));
    wire::Writer w(out.data(), out.size());
    w.appendU8(kBlobFormat);
    w.appendU32Le(gen);
    w.appendU16Le(static_cast<uint16_t>(pairs.size()));
    for (const ParamPair &p : pairs) {
        w.appendU16Le(p.id);
        w.appendU32Le(p.value);
    }
    appendCrc(out, w);
    return w.ok() ? out : std::vector<uint8_t>{};
}

bool decodeInstanceBlob(std::span<const uint8_t> blob, uint32_t &gen, std::vector<ParamPair> &pairs)
{
    pairs.clear();
    if (!crcOk(blob)) {
        return false;
    }
    wire::Reader r(blob.data(), blob.size() - kCrcBytes);
    const uint8_t fmt = r.readU8();
    const uint32_t g = r.readU32Le();
    const uint16_t count = r.readU16Le();
    if (!r.ok() || fmt != kBlobFormat || blob.size() != instanceBlobSize(count)) {
        return false;
    }
    pairs.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t id = r.readU16Le();
        const uint32_t value = r.readU32Le();
        pairs.push_back(ParamPair{id, value});
    }
    if (!r.done()) {
        pairs.clear();
        return false;
    }
    gen = g;
    return true;
}

std::vector<uint8_t> encodeLayoutBlob(uint32_t gen, std::span<const LayoutRecord> records, std::string_view name)
{
    if (records.size() > kMaxLayoutRecords || !drivelink::protocol::validDeviceName(name)) {
        return {};
    }
    std::vector<uint8_t> out(layoutBlobSize(records.size(), name.size()));
    wire::Writer w(out.data(), out.size());
    w.appendU8(kLayoutBlobFormat);
    w.appendU32Le(gen);
    w.appendU8(static_cast<uint8_t>(records.size()));
    for (const LayoutRecord &rec : records) {
        w.appendU8(rec.type_id);
        w.appendU8(rec.instance);
        w.appendU8(rec.loop);
        uint8_t label[kLabelBytes] = {};
        std::memcpy(label, rec.label, boundedLen(rec.label, kLabelBytes));
        w.appendBytes(label, kLabelBytes);
    }
    (void)drivelink::protocol::writeDeviceName(w, name);
    appendCrc(out, w);
    return w.ok() ? out : std::vector<uint8_t>{};
}

bool decodeLayoutBlob(std::span<const uint8_t> blob, uint32_t &gen, std::vector<LayoutRecord> &records,
                      std::array<char, drivelink::protocol::kDeviceNameBytes + 1> *name)
{
    records.clear();
    if (!crcOk(blob)) {
        return false;
    }
    wire::Reader r(blob.data(), blob.size() - kCrcBytes);
    const uint8_t fmt = r.readU8();
    const uint32_t g = r.readU32Le();
    const uint8_t count = r.readU8();
    const bool legacy = fmt == 2;
    const size_t minimum = layoutBlobSize(count) - (legacy ? 1U : 0U);
    if (!r.ok() || (!legacy && fmt != kLayoutBlobFormat) || blob.size() < minimum ||
        (legacy && blob.size() != minimum)) {
        return false;
    }
    records.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        LayoutRecord rec{};
        rec.type_id = r.readU8();
        rec.instance = r.readU8();
        rec.loop = r.readU8();
        for (size_t k = 0; k < kLabelBytes; ++k) {
            rec.label[k] = static_cast<char>(r.readU8());
        }
        rec.label[kLabelBytes] = '\0';
        records.push_back(rec);
    }
    std::array<char, drivelink::protocol::kDeviceNameBytes + 1> loaded_name{};
    if ((!legacy && !drivelink::protocol::readDeviceName(r, loaded_name)) || !r.done()) {
        records.clear();
        return false;
    }
    gen = g;
    if (name) { *name = loaded_name; }
    return true;
}

} // namespace core
