#include "drivelink/client.hpp"
#include "drivelink/protocol/ota_image.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include <algorithm>
#include <fstream>
#include <thread>

namespace drivelink {
namespace p=protocol;
namespace {
using p::wire::Reader;using p::wire::Writer;
constexpr uint8_t kFamily=static_cast<uint8_t>(p::defs::Family::Ota);
uint8_t op(p::defs::OtaOp value) { return static_cast<uint8_t>(value); }
[[noreturn]] void malformed() { throw Error(ErrorKind::Protocol,"malformed OTA response"); }
void empty(const Reply &reply) { if (!reply.body.empty()) { malformed(); } }
void checkReply(const Reply &reply) {
    if (reply.result) {
        if (reply.detail==static_cast<uint8_t>(p::defs::Detail::OtaOffset)) {
            Reader r(reply.body.data(),reply.body.size());const auto offset=r.readU32Le();if (!r.done()) { malformed(); }
            throw Error(ErrorKind::Rejected,"OTA offset/data rejected; expected offset "+std::to_string(offset),reply.result,reply.detail);
        }
        empty(reply);throw Error(ErrorKind::Rejected,"OTA operation refused",reply.result,reply.detail);
    }
    if (reply.detail) { malformed(); }
}
uint32_t next(const Reply &reply) {
    checkReply(reply);Reader r(reply.body.data(),reply.body.size());const auto n=r.readU32Le();
    if (!r.done()) { malformed(); }return n;
}
Bytes writeBody(uint32_t offset,std::span<const uint8_t> bytes) {
    if (bytes.empty() || bytes.size()>p::kOtaChunkBytes) { throw Error(ErrorKind::Usage,"OTA chunk must be 1..512 bytes"); }
    Bytes body(6+bytes.size());Writer w(body.data(),body.size());w.appendU32Le(offset);w.appendU16Le(static_cast<uint16_t>(bytes.size()));
    for (auto b:bytes) { w.appendU8(b); }return body;
}
}
Bytes readOtaImage(const std::string &path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if (!f) { throw Error(ErrorKind::Usage,"cannot open OTA image"); }
    const auto size=f.tellg();if (size<=0 || size>p::kOtaSlotBytes) { throw Error(ErrorKind::Usage,"OTA image does not fit the 4 MiB slot"); }
    Bytes bytes(static_cast<size_t>(size));f.seekg(0);
    if (!f.read(reinterpret_cast<char *>(bytes.data()),size)) { throw Error(ErrorKind::Usage,"cannot read OTA image"); }
    p::Sha256Digest identity;
    if (!p::otaImageIdentity(bytes,identity)) { throw Error(ErrorKind::Usage,"expected a complete unsigned ESP32-S3 application .bin"); }
    return bytes;
}
Json Client::otaStatus() {
    const auto reply=m_session.request(kFamily,op(p::defs::OtaOp::Status));checkReply(reply);
    Reader r(reply.body.data(),reply.body.size());
    const auto state=r.readU8(),running=r.readU8(),selected=r.readU8(),pending=r.readU8();
    const auto size=r.readU32Le(),offset=r.readU32Le(),capacity=r.readU32Le();
    const auto chunk=r.readU16Le();const auto window=r.readU8();const auto error=r.readU32Le();const auto stage=r.readU8();
    p::Sha256Digest digest{},elf{};for (auto &b:digest) { b=r.readU8(); }for (auto &b:elf) { b=r.readU8(); }
    if (!r.done() || state>3 || running>1 || selected>1 || pending>1 || size>p::kOtaSlotBytes ||
        offset>size || capacity>p::kOtaSlotBytes || (!capacity && state!=3) || !chunk || chunk>p::kOtaChunkBytes ||
        chunk+10U>m_max_payload ||
        !window || window>p::kOtaWindow || stage>8 || (state==3 && (!stage || !error)) ||
        (state!=3 && (stage || error))) { malformed(); }
    return {{"state",state},{"running_slot",running},{"boot_slot",selected},{"pending_confirm",bool(pending)},
            {"size",size},{"offset",offset},{"capacity",capacity},{"chunk_bytes",chunk},{"window",window},
            {"backend_error",static_cast<int32_t>(error)},{"stage",stage},{"sha256",hex(digest)},{"elf_sha256",hex(elf)}};
}
uint32_t Client::otaBegin(uint32_t size,const p::Sha256Digest &digest) {
    Bytes body(36);Writer w(body.data(),body.size());w.appendU32Le(size);for (auto b:digest) { w.appendU8(b); }
    const auto offset=next(m_session.request(kFamily,op(p::defs::OtaOp::Begin),body));
    if (offset>size) { malformed(); }return offset;
}
uint32_t Client::otaWrite(uint32_t offset,std::span<const uint8_t> bytes) {
    const auto n=next(m_session.request(kFamily,op(p::defs::OtaOp::Write),writeBody(offset,bytes)));
    if (offset>UINT32_MAX-bytes.size() || n<offset+bytes.size()) { malformed(); }return n;
}
Json Client::otaFinish() { const auto r=m_session.request(kFamily,op(p::defs::OtaOp::Finish));checkReply(r);empty(r);return {{"finished",true}}; }
Json Client::otaAbort() { const auto r=m_session.request(kFamily,op(p::defs::OtaOp::Abort));checkReply(r);empty(r);return {{"aborted",true}}; }
Json Client::otaConfirm() { const auto r=m_session.request(kFamily,op(p::defs::OtaOp::Confirm));checkReply(r);empty(r);return {{"confirmed",true}}; }
Json Client::otaUpload(std::span<const uint8_t> image,uint8_t window,const std::function<bool(uint32_t,uint32_t)> &progress) {
    p::Sha256Digest identity;
    if (!window || window>p::kOtaWindow || !p::otaImageIdentity(image,identity)) { throw Error(ErrorKind::Usage,"invalid OTA image/window"); }
    const auto digest=p::sha256(image);const auto size=static_cast<uint32_t>(image.size());
    auto status=otaStatus();
    if (status["pending_confirm"]==true || (status["state"]!=0 && status["state"]!=1)) {
        throw Error(ErrorKind::Rejected,"OTA is not ready; inspect ota-status and confirm or abort explicitly");
    }
    if (status["capacity"].get<uint32_t>()<size) { throw Error(ErrorKind::Usage,"image exceeds device OTA capacity"); }
    window=std::min(window,status["window"].get<uint8_t>());
    const auto chunk_bytes=status["chunk_bytes"].get<uint16_t>();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(5);
    bool attempted=false;
    auto verify=[&](const Json &s,uint32_t lower,uint32_t upper) {
        const auto offset=s["offset"].get<uint32_t>();
        if ((s["state"]!=1 && s["state"]!=2) || s["size"]!=size || s["sha256"]!=hex(digest) || offset<lower || offset>upper) {
            throw Error(ErrorKind::Protocol,"OTA state changed during upload; outcome uncertain");
        }
        return offset;
    };
    try {
        if (status["state"]==1) { (void)verify(status,0,size); }
        attempted=true;uint32_t offset=0;
        try { offset=otaBegin(size,digest); }
        catch (const Error &e) { if (e.kind!=ErrorKind::Timeout) { throw; }status=otaStatus();offset=verify(status,0,size); }
        unsigned retries=0;
        while (offset<size) {
            if (std::chrono::steady_clock::now()>deadline) { throw Error(ErrorKind::Timeout,"OTA upload deadline exceeded"); }
            if (progress && !progress(offset,size)) { throw Error(ErrorKind::Usage,"OTA upload cancelled"); }
            std::array<Bytes,p::kOtaWindow> bodies;uint32_t end=offset;size_t count=0;
            for (;count<window && end<size;++count) {
                const auto n=std::min<size_t>(chunk_bytes,size-end);bodies[count]=writeBody(end,image.subspan(end,n));end+=static_cast<uint32_t>(n);
            }
            bool reconcile=false;
            try {
                const auto replies=m_session.otaWriteBatch({bodies.data(),count});
                for (size_t i=0;i<replies.size();++i) {
                    const auto &reply=replies[i];
                    if (reply.result==static_cast<uint8_t>(p::defs::Result::Busy) &&
                        reply.detail==static_cast<uint8_t>(p::defs::Detail::StagingBusy)) { empty(reply);reconcile=true;continue; }
                    if (reply.result==static_cast<uint8_t>(p::defs::Result::InvalidArgument) &&
                        reply.detail==static_cast<uint8_t>(p::defs::Detail::OtaOffset)) {
                        Reader r(reply.body.data(),reply.body.size());const auto expected=r.readU32Le();
                        if (!r.done() || expected<offset || expected>end) { malformed(); }reconcile=true;continue;
                    }
                    const auto n=next(reply);const auto chunk_end=std::min<uint32_t>(end,offset+static_cast<uint32_t>((i+1)*chunk_bytes));
                    if (n<chunk_end || n>end) { malformed(); }
                }
            } catch (const Error &e) { if (e.kind!=ErrorKind::Timeout) { throw; }reconcile=true; }
            const auto current=reconcile ? verify(otaStatus(),offset,end) : end;
            if (current==offset && ++retries>5) { throw Error(ErrorKind::Timeout,"OTA upload made no progress"); }
            if (current!=offset) { retries=0; }offset=current;
            if (reconcile) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        }
        if (progress && !progress(size,size)) { throw Error(ErrorKind::Usage,"OTA upload cancelled"); }
        try { (void)otaFinish(); }
        catch (const Error &e) {
            if (e.kind!=ErrorKind::Timeout) { throw; }
            status=otaStatus();(void)verify(status,size,size);if (status["state"]!=2) { throw; }
        }
        status=otaStatus();(void)verify(status,size,size);
        if (status["state"]!=2) { malformed(); }return status;
    } catch (const Error &e) {
        std::string message=e.what();
        if (attempted) {
            try { (void)otaAbort();message+="; update aborted"; }
            catch (...) { message+="; cleanup uncertain: reconnect and inspect ota-status; use ota-abort before reboot"; }
        }
        throw Error(e.kind,message,e.result,e.detail);
    }
}
}
