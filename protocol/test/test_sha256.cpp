#include "doctest.h"
#include "drivelink/protocol/sha256.hpp"
#include "ota_fixture.hpp"
#include <string>

namespace p=drivelink::protocol;
namespace {
std::string hex(const p::Sha256Digest &digest) {
    constexpr char digits[]="0123456789abcdef";std::string out;
    for (auto b:digest) { out+=digits[b>>4];out+=digits[b&15]; }return out;
}
}
TEST_CASE("SHA256 known answers and split updates") {
    CHECK(hex(p::sha256({}))=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string abc="abc";const auto bytes=std::span(reinterpret_cast<const uint8_t *>(abc.data()),abc.size());
    CHECK(hex(p::sha256(bytes))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string long_text="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    p::Sha256 h;
    for (auto c:long_text) { const auto b=static_cast<uint8_t>(c);h.update({&b,1}); }
    CHECK(hex(h.final())=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(h.final()==h.final());
    h={};const uint8_t a='a';for (unsigned i=0;i<1000000;++i) { h.update({&a,1}); }
    CHECK(hex(h.final())=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    for (size_t n: {size_t(55),size_t(56),size_t(63),size_t(64),size_t(65),size_t(1000)}) {
        const std::vector<uint8_t> data(n,0x73);h={};h.update(std::span(data).first(n/2));h.update(std::span(data).subspan(n/2));
        CHECK(h.final()==p::sha256(data));
    }
}
TEST_CASE("OTA image preflight validates structural bounds checksums and identity") {
    auto image=otaFixture();p::Sha256Digest identity{};
    REQUIRE(p::otaImageIdentity(image,identity));CHECK(identity[0]==1);CHECK(identity[31]==32);
    for (size_t n: {size_t(0),size_t(23),size_t(287),image.size()-1}) { CHECK_FALSE(p::otaImageIdentity(std::span(image).first(n),identity)); }
    for (size_t index: {size_t(0),size_t(1),size_t(12),size_t(23),size_t(28),size_t(32),size_t(300),image.size()-1}) {
        auto bad=image;bad[index]^=0xff;CHECK_FALSE(p::otaImageIdentity(bad,identity));
    }
    image.push_back(0);CHECK_FALSE(p::otaImageIdentity(image,identity));
}
