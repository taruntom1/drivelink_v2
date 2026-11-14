#include "doctest.h"
#include "drivelink/protocol/time_codec.hpp"
#include <array>
#include <limits>
using namespace drivelink::protocol;
TEST_CASE("time v1 exact codec and malformed boundaries") {
    std::array<uint8_t,32> bytes{};uint16_t id=8;
    REQUIRE(time_wire::request(bytes.data(),bytes.size(),0xabcd)==3);
    CHECK(bytes[0]==1);CHECK(bytes[1]==0xcd);CHECK(bytes[2]==0xab);
    CHECK(time_wire::decodeRequest(bytes.data(),3,id));CHECK(id==0xabcd);
    for(size_t n=0;n<bytes.size();++n) { if(n!=3){CHECK_FALSE(time_wire::decodeRequest(bytes.data(),n,id));} }
    CHECK_FALSE(time_wire::decodeRequest(nullptr,3,id));bytes[0]=2;
    CHECK_FALSE(time_wire::decodeRequest(bytes.data(),3,id));CHECK(id==0xabcd);
    CHECK(time_wire::request(bytes.data(),2,1)==0);
    time_wire::Response input{65535,0x0102030405060708ULL,std::numeric_limits<uint64_t>::max()},output;
    REQUIRE(time_wire::response(bytes.data(),bytes.size(),input)==19);
    CHECK(bytes[3]==8);CHECK(bytes[10]==1);
    REQUIRE(time_wire::decodeResponse(bytes.data(),19,output));
    CHECK(output.id==input.id);CHECK(output.rx_us==input.rx_us);CHECK(output.tx_us==input.tx_us);
    for(size_t n=0;n<bytes.size();++n) {if(n!=19){CHECK_FALSE(time_wire::decodeResponse(bytes.data(),n,output));}}
    CHECK(time_wire::response(bytes.data(),18,input)==0);
}
TEST_CASE("TX stamp validates a whole frame and preserves other fields with a fresh CRC") {
    std::array<uint8_t,19> payload{};time_wire::response(payload.data(),payload.size(),{41,1234,0});
    std::array<uint8_t,28> frame{};REQUIRE(encodeFrame(frame.data(),frame.size(),defs::msg::kTimeResponse,payload.data(),19)==28);
    const auto original=frame;
    for(size_t n=0;n<frame.size();++n) {CHECK_FALSE(time_wire::stampFrame(frame.data(),n,99));CHECK(frame==original);}
    REQUIRE(time_wire::stampFrame(frame.data(),frame.size(),987654321));
    CHECK(validateCrc(frame.data()+3,19));time_wire::Response decoded;
    REQUIRE(time_wire::decodeResponse(frame.data()+7,19,decoded));
    CHECK(decoded.id==41);CHECK(decoded.rx_us==1234);CHECK(decoded.tx_us==987654321);
    REQUIRE(time_wire::stampFrame(frame.data(),frame.size(),1000000000));CHECK(validateCrc(frame.data()+3,19));
    for(size_t i=0;i<frame.size();++i) {auto bad=original;bad[i]^=1;const auto before=bad;
        CHECK_FALSE(time_wire::stampFrame(bad.data(),bad.size(),1));CHECK(bad==before);}
    encodeFrame(frame.data(),frame.size(),defs::msg::kCoreResponse,payload.data(),19);
    CHECK_FALSE(time_wire::stampFrame(frame.data(),frame.size(),1));
}
