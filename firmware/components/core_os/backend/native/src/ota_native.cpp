#include "core_os/native_ota.hpp"
#include "core_os/native_file.hpp"
#include "drivelink/protocol/ota_image.hpp"
#include "drivelink/protocol/wire_codec.hpp"
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace core_os {
namespace p=drivelink::protocol;
namespace {
std::filesystem::path directory;
drivelink::protocol::Sha256Digest baseline_elf{};
std::string baseline_version;
constexpr size_t kMetaBytes=4+4+2*(32+33)+32;
}
struct Ota::Impl {
    struct Slot { p::Sha256Digest elf{};std::array<char,33> version{}; };
    std::array<Slot,2> slots{};
    uint8_t running=0,selected=0,previous=0;
    bool pending=false,valid=true,open=false,finished=false;
    std::vector<uint8_t> image;
    std::filesystem::path path;
    std::string fail;
    bool failed(const char *name) { if (fail==name) { fail.clear();return true; }return false; }
    bool save() {
        if (path.empty()) { return true; }
        std::array<uint8_t,kMetaBytes> bytes{};
        p::wire::Writer w(bytes.data(),bytes.size());w.appendU32Le(0x3141544f);
        w.appendU8(running);w.appendU8(selected);w.appendU8(previous);w.appendU8(pending);
        for (const auto &slot:slots) {
            for (auto b:slot.elf) { w.appendU8(b); }
            for (auto b:slot.version) { w.appendU8(static_cast<uint8_t>(b)); }
        }
        const auto digest=p::sha256(std::span(bytes).first(kMetaBytes-32));
        std::copy(digest.begin(),digest.end(),bytes.end()-32);
        return native::writeFile(path,bytes)==kOk;
    }
    void boot() {
        open=false;finished=false;image.clear();
        if (pending) { running=previous;selected=previous;pending=false; }
        else if (selected!=running) { previous=running;running=selected;pending=true; }
        valid=valid && save();
    }
};
Ota::Ota() : m_impl(std::make_unique<Impl>()) {
    auto &d=*m_impl;
    constexpr uint8_t baseline[]{'n','a','t','i','v','e'};
    d.slots[0].elf=p::sha256(baseline);std::copy_n("0.1.0-vdev",11,d.slots[0].version.data());
    if (!baseline_version.empty()) {
        d.slots[0].elf=baseline_elf;
        d.slots[0].version.fill(0);
        std::copy_n(baseline_version.data(),std::min<size_t>(32,baseline_version.size()),d.slots[0].version.data());
    }
    if (!directory.empty()) {
        d.path=directory/"ota.bin";
        std::error_code ec;const bool exists=std::filesystem::exists(d.path,ec);
        if (ec) { d.valid=false;return; }
        if (exists) {
            std::ifstream f(d.path,std::ios::binary);
            std::array<uint8_t,kMetaBytes> bytes{};
            if (!f.read(reinterpret_cast<char *>(bytes.data()),bytes.size()) || f.peek()!=EOF) { d.valid=false;return; }
            const auto digest=p::sha256(std::span(bytes).first(kMetaBytes-32));
            p::wire::Reader r(bytes.data(),bytes.size());
            if (r.readU32Le()!=0x3141544f || !std::equal(digest.begin(),digest.end(),bytes.end()-32)) { d.valid=false;return; }
            d.running=r.readU8();d.selected=r.readU8();d.previous=r.readU8();const auto pending=r.readU8();
            if (d.running>1 || d.selected>1 || d.previous>1 || pending>1) { d.valid=false;return; }d.pending=pending;
            for (auto &slot:d.slots) {
                for (auto &b:slot.elf) { b=r.readU8(); }
                for (auto &b:slot.version) { b=static_cast<char>(r.readU8()); }slot.version[32]=0;
            }
        }
    }
    d.boot();
}
Ota::~Ota() = default;
Err Ota::info(OtaBootInfo &out) {
    auto &d=*m_impl;if (!d.valid || d.failed("info")) { return -1; }
    out={d.running,d.selected,d.pending,p::kOtaSlotBytes,d.slots[d.running].elf,d.slots[d.running].version};return kOk;
}
Err Ota::begin(uint32_t size) {
    auto &d=*m_impl;if (!d.valid || d.open || d.pending || !size || size>p::kOtaSlotBytes || d.failed("begin")) { return -1; }
    d.image.clear();d.image.reserve(size);d.open=true;d.finished=false;return kOk;
}
Err Ota::write(std::span<const uint8_t> bytes) {
    auto &d=*m_impl;if (!d.open || bytes.size()>d.image.capacity()-d.image.size() || d.failed("write")) { return -1; }
    d.image.insert(d.image.end(),bytes.begin(),bytes.end());return kOk;
}
Err Ota::read(uint32_t offset,std::span<uint8_t> bytes) {
    auto &d=*m_impl;if (offset>d.image.size() || bytes.size()>d.image.size()-offset || d.failed("read")) { return -1; }
    std::copy_n(d.image.data()+offset,bytes.size(),bytes.data());return kOk;
}
Err Ota::finish() {
    auto &d=*m_impl;if (!d.open) { return -1; }d.open=false;
    p::Sha256Digest identity;
    if (d.failed("finish") || !p::otaImageIdentity(d.image,identity)) { return -1; }
    auto &slot=d.slots[1-d.running];slot.elf=identity;
    std::copy_n(d.image.data()+32+16,32,slot.version.data());slot.version[32]=0;
    d.finished=true;return kOk;
}
Err Ota::select() {
    auto &d=*m_impl;if (!d.valid || !d.finished || d.failed("select")) { return -1; }
    d.selected=1-d.running;if (!d.save()) { d.valid=false;return -1; }return kOk;
}
Err Ota::abort() {
    auto &d=*m_impl;if (!d.valid || d.pending || d.failed("abort")) { return -1; }
    d.open=false;d.finished=false;d.selected=d.running;
    if (!d.save()) { d.valid=false;return -1; }return kOk;
}
Err Ota::confirm() {
    auto &d=*m_impl;if (!d.valid || d.failed("confirm")) { return -1; }
    d.pending=false;d.previous=d.running;
    if (!d.save()) { d.valid=false;return -1; }return kOk;
}
namespace native {
void setOtaDirectory(const std::filesystem::path &path) { directory=path; }
void setOtaBaseline(const drivelink::protocol::Sha256Digest &elf,const char *version) { baseline_elf=elf;baseline_version=version; }
void failOtaOperation(Ota &ota,const char *operation) { ota.m_impl->fail=operation; }
void rebootOta(Ota &ota) { ota.m_impl->boot(); }
}
}
