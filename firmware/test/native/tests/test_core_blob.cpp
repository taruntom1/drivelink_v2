// core: the NVS blob codec and its CRC-32, and the namespace names.

#include "doctest.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/config_blob.hpp"
#include "core/persistence.hpp"
#include "drivelink/protocol/wire_codec.hpp"

namespace {

/// Recompute the trailing CRC after a deliberate edit, so only the edit is wrong.
void fixCrc(std::vector<uint8_t> &blob)
{
    const size_t body = blob.size() - 4U;
    drivelink::protocol::wire::Writer w(blob.data() + body, 4);
    w.appendU32Le(core::crc32(blob.data(), body));
}

core::LayoutRecord rec(uint8_t type, uint8_t inst, uint8_t loop, const char *label)
{
    core::LayoutRecord r{};
    r.type_id = type;
    r.instance = inst;
    r.loop = loop;
    std::memcpy(r.label, label, std::min(std::strlen(label), core::kLabelBytes));
    return r;
}

} // namespace

TEST_CASE("CRC-32: the IEEE check value")
{
    const char *check = "123456789";
    CHECK(core::crc32(reinterpret_cast<const uint8_t *>(check), 9) == 0xCBF43926U);
    CHECK(core::crc32(nullptr, 0) == 0U);
}

TEST_CASE("Instance blob: round trip, exact size and layout")
{
    const std::vector<core::ParamPair> pairs{{0x0001, 0xDEADBEEF}, {0x0020, 0x3FC00000}, {0xFFFF, 0}};
    const std::vector<uint8_t> blob = core::encodeInstanceBlob(7, pairs);
    REQUIRE(blob.size() == core::instanceBlobSize(3));
    CHECK(blob.size() == 1 + 4 + 2 + 18 + 4);
    CHECK(blob[0] == core::kBlobFormat);
    CHECK(blob[1] == 7); // gen, little-endian
    CHECK(blob[5] == 3); // count
    CHECK(blob[7] == 0x01);
    CHECK(blob[9] == 0xEF);

    uint32_t gen = 0;
    std::vector<core::ParamPair> out;
    REQUIRE(core::decodeInstanceBlob(blob, gen, out));
    CHECK(gen == 7);
    REQUIRE(out.size() == 3);
    CHECK(out[1].id == 0x0020);
    CHECK(out[1].value == 0x3FC00000);
    CHECK(out[2].id == 0xFFFF);

    const std::vector<uint8_t> empty = core::encodeInstanceBlob(1, {});
    REQUIRE(core::decodeInstanceBlob(empty, gen, out));
    CHECK(out.empty());
    CHECK(gen == 1);
}

TEST_CASE("Instance blob: a bad CRC, truncation, a wrong count or format are rejected")
{
    const std::vector<core::ParamPair> pairs{{1, 2}, {3, 4}};
    const std::vector<uint8_t> good = core::encodeInstanceBlob(9, pairs);
    uint32_t gen = 0;
    std::vector<core::ParamPair> out;

    std::vector<uint8_t> b = good;
    b[8] ^= 0x01U;
    CHECK_FALSE(core::decodeInstanceBlob(b, gen, out));
    CHECK(out.empty());

    b = good;
    b.pop_back();
    CHECK_FALSE(core::decodeInstanceBlob(b, gen, out));
    b = good;
    b.resize(b.size() / 2);
    CHECK_FALSE(core::decodeInstanceBlob(b, gen, out));
    CHECK_FALSE(core::decodeInstanceBlob({}, gen, out));

    b = good;
    b[5] = 3; // count 3, but two pairs
    fixCrc(b);
    CHECK_FALSE(core::decodeInstanceBlob(b, gen, out));
    b[5] = 1; // count 1: bytes left over
    fixCrc(b);
    CHECK_FALSE(core::decodeInstanceBlob(b, gen, out));

    b = good;
    b[0] = 2; // a future format
    fixCrc(b);
    CHECK_FALSE(core::decodeInstanceBlob(b, gen, out));

    b = good;
    b.push_back(0); // trailing garbage
    CHECK_FALSE(core::decodeInstanceBlob(b, gen, out));
    CHECK(gen == 0); // untouched by every failure
}

TEST_CASE("Layout blob: round trip, labels padded to 16 bytes")
{
    const std::vector<core::LayoutRecord> recs{rec(0x20, 0, 0, "left_wheel"), rec(0x21, 3, 1, "sixteen_chars___"),
                                               rec(0x05, 254, 0, "x")};
    const std::vector<uint8_t> blob = core::encodeLayoutBlob(0x01020304, recs);
    REQUIRE(blob.size() == core::layoutBlobSize(3));
    CHECK(blob.size() == 1 + 4 + 1 + 3 * 19 + 4);
    CHECK(blob[1] == 0x04);
    CHECK(blob[5] == 3);
    CHECK(blob[6] == 0x20);
    CHECK(blob[9] == 'l');
    CHECK(blob[9 + 10] == 0); // padding after "left_wheel"

    uint32_t gen = 0;
    std::vector<core::LayoutRecord> out;
    REQUIRE(core::decodeLayoutBlob(blob, gen, out));
    CHECK(gen == 0x01020304);
    REQUIRE(out.size() == 3);
    CHECK(std::strcmp(out[0].label, "left_wheel") == 0);
    CHECK(std::strcmp(out[1].label, "sixteen_chars___") == 0);
    CHECK(out[1].loop == 1);
    CHECK(out[2].instance == 254);

    const std::vector<uint8_t> empty = core::encodeLayoutBlob(5, {});
    REQUIRE(core::decodeLayoutBlob(empty, gen, out));
    CHECK(out.empty());
}

TEST_CASE("Layout blob: a bad CRC, truncation, a wrong count or format are rejected")
{
    const std::vector<core::LayoutRecord> recs{rec(1, 0, 0, "a"), rec(2, 0, 1, "b")};
    const std::vector<uint8_t> good = core::encodeLayoutBlob(3, recs);
    uint32_t gen = 0;
    std::vector<core::LayoutRecord> out;

    std::vector<uint8_t> b = good;
    b[10] ^= 0x40U;
    CHECK_FALSE(core::decodeLayoutBlob(b, gen, out));
    b = good;
    b.resize(b.size() - 19);
    CHECK_FALSE(core::decodeLayoutBlob(b, gen, out));
    b = good;
    b[5] = 3;
    fixCrc(b);
    CHECK_FALSE(core::decodeLayoutBlob(b, gen, out));
    b = good;
    b[0] = 0;
    fixCrc(b);
    CHECK_FALSE(core::decodeLayoutBlob(b, gen, out));
    // An instance blob is not a layout blob.
    CHECK_FALSE(core::decodeLayoutBlob(core::encodeInstanceBlob(3, {}), gen, out));
}

TEST_CASE("Namespaces: t<type>i<inst> in lower-case hex")
{
    CHECK(std::strcmp(core::persist::instanceNamespace(0x00, 0).text, "t00i00") == 0);
    CHECK(std::strcmp(core::persist::instanceNamespace(0xAB, 0x0F).text, "tabi0f") == 0);
    uint8_t t = 0;
    uint8_t i = 0;
    CHECK(core::persist::parseInstanceNamespace("t2ci01", t, i));
    CHECK(t == 0x2C);
    CHECK(i == 0x01);
    CHECK_FALSE(core::persist::parseInstanceNamespace("core", t, i));
    CHECK_FALSE(core::persist::parseInstanceNamespace("t2Ci01", t, i));
    CHECK_FALSE(core::persist::parseInstanceNamespace("t2ci011", t, i));
    CHECK_FALSE(core::persist::parseInstanceNamespace("x2ci01", t, i));
    CHECK_FALSE(core::persist::parseInstanceNamespace(nullptr, t, i));
}
