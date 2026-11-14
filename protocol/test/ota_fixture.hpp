#pragma once
#include "drivelink/protocol/ota_image.hpp"
#include <vector>

// Structurally valid unsigned app for native simulation; contains no executable
// firmware and must never be flashed. Real hardware uses an IDF-built .bin.
inline std::vector<uint8_t> otaFixture(size_t data_bytes=4096,uint8_t seed=1) {
    data_bytes=std::max<size_t>(256,(data_bytes+3)&~size_t(3));
    const size_t end=(32+data_bytes+16)&~size_t(15);
    std::vector<uint8_t> bytes(end+32,0);
    bytes[0]=0xe9;bytes[1]=1;bytes[12]=9;bytes[23]=1;
    drivelink::protocol::wire::Writer w(bytes.data()+24,8);w.appendU32Le(0x3c000020);w.appendU32Le(static_cast<uint32_t>(data_bytes));
    drivelink::protocol::wire::Writer d(bytes.data()+32,256);d.appendU32Le(0xabcd5432);
    std::copy_n("native-candidate",16,bytes.data()+48);
    for (size_t i=0;i<32;++i) { bytes[32+144+i]=static_cast<uint8_t>(seed+i); }
    for (size_t i=288;i<32+data_bytes;++i) { bytes[i]=static_cast<uint8_t>(i+seed); }
    uint8_t checksum=0xef;for (size_t i=32;i<32+data_bytes;++i) { checksum^=bytes[i]; }bytes[end-1]=checksum;
    const auto digest=drivelink::protocol::sha256(std::span(bytes).first(end));
    std::copy(digest.begin(),digest.end(),bytes.begin()+end);return bytes;
}
