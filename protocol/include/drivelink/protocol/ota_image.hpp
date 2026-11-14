#pragma once
#include "drivelink/protocol/sha256.hpp"
#include "drivelink/protocol/wire_codec.hpp"
#include <algorithm>

namespace drivelink::protocol {
inline constexpr uint32_t kOtaSlotBytes = 0x400000;
inline constexpr uint16_t kOtaChunkBytes = 512;
inline constexpr uint8_t kOtaWindow = 4;
// Preflight of an unsigned ESP32-S3 app binary. IDF remains the authoritative
// chip/revision/image validator. Rejects trailing bytes and invalid checksums.
// The returned ELF hash identifies a build; it is NOT the SHA of this file.
inline bool otaImageIdentity(std::span<const uint8_t> bytes, Sha256Digest &elf) {
    if (bytes.size()<288 || bytes.size()>kOtaSlotBytes) { return false; }
    wire::Reader header(bytes.data(),24);
    if (header.readU8()!=0xe9) { return false; }
    const auto segments=header.readU8();
    if (!segments || segments>16 || bytes[12]!=9 || bytes[13]!=0 || bytes[23]>1) { return false; }
    size_t pos=24;
    uint8_t checksum=0xef;
    for (size_t i=0;i<segments;++i) {
        if (bytes.size()-pos<8) { return false; }
        wire::Reader r(bytes.data()+pos,8);(void)r.readU32Le();const auto n=r.readU32Le();pos+=8;
        if (n>bytes.size()-pos || (n%4)!=0) { return false; }
        if (i==0) {
            if (n<256) { return false; }
            wire::Reader desc(bytes.data()+pos,256);
            if (desc.readU32Le()!=0xabcd5432) { return false; }
            std::copy_n(bytes.data()+pos+144,32,elf.data());
        }
        for (size_t j=0;j<n;++j) { checksum^=bytes[pos+j]; }
        pos+=n;
    }
    const auto end=(pos+16)&~size_t(15); // checksum is the last byte of a 16-byte block
    if (end>bytes.size() || bytes[end-1]!=checksum) { return false; }
    if (bytes[23]) {
        if (bytes.size()!=end+32) { return false; }
        const auto digest=sha256(bytes.first(end));
        return std::equal(digest.begin(),digest.end(),bytes.begin()+end);
    }
    return bytes.size()==end;
}
}
