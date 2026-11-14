#include "core/ota_service.hpp"
#include "core/core.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"
#include <algorithm>

namespace core {
namespace p=drivelink::protocol;
Status OtaService::disarmed() const {
    return m_core.params().armed() ? Status::fail(Result::Armed,Detail::LockedWhileArmed) : Status::success();
}
Status OtaService::fail(core_os::Err err,Stage stage) {
    m_state=State::Error;m_error=err;m_stage=stage;m_gate.store(true,std::memory_order_release);
    core_os::reportDiag(p::defs::diag::codes::core::kOtaFailed,err,static_cast<uint32_t>(stage),m_offset);
    return Status::fail(Result::Fault,Detail::OtaBackend);
}
void OtaService::gate() { m_gate.store(m_state!=State::Idle || m_boot.pending || m_boot.running!=m_boot.selected,std::memory_order_release); }
void OtaService::initialize() { (void)refresh(); }
Status OtaService::refresh() {
    const auto err=m_ota.info(m_boot);if (err!=core_os::kOk) { return fail(err,Stage::Info); }
    if (m_state==State::Idle && m_boot.selected!=m_boot.running) { m_state=State::Ready; }
    gate();return {};
}
Status OtaService::begin(uint32_t size,const p::Sha256Digest &digest) {
    if (auto st=disarmed();!st.ok()) { return st; }
    if (auto st=refresh();!st.ok()) { return st; }
    if (m_boot.pending) { return Status::fail(Result::Busy,Detail::OtaPendingConfirm); }
    if (m_state==State::Receiving && size==m_size && digest==m_digest) { return {}; }
    if (m_state!=State::Idle) { return Status::fail(Result::Busy,Detail::OtaActive); }
    if (!size || size>m_boot.capacity) { return Status::fail(Result::InvalidArgument,Detail::Capacity); }
    const auto capture=m_core.capture().snapshot().state;
    if (capture==p::defs::CaptureState::Armed || capture==p::defs::CaptureState::Triggered || m_core.testgen().running()) {
        return Status::fail(Result::Busy,Detail::WrongState);
    }
    m_gate.store(true,std::memory_order_release);m_size=size;m_offset=0;m_digest=digest;m_hash={};
    const auto err=m_ota.begin(size);if (err!=core_os::kOk) { return fail(err,Stage::Begin); }
    m_state=State::Receiving;m_error=0;m_stage=Stage::None;return {};
}
Status OtaService::write(uint32_t offset,std::span<const uint8_t> bytes) {
    if (auto st=disarmed();!st.ok()) { return st; }
    if (m_state!=State::Receiving) { return Status::fail(Result::InvalidArgument,Detail::WrongState); }
    if (bytes.empty() || bytes.size()>p::kOtaChunkBytes) { return Status::fail(Result::InvalidArgument,Detail::BadValue); }
    if (offset>m_size || bytes.size()>m_size-offset || offset>m_offset ||
        (offset<m_offset && bytes.size()>m_offset-offset)) { return Status::fail(Result::InvalidArgument,Detail::OtaOffset); }
    if (offset<m_offset) {
        auto scratch=std::span(m_scratch).first(bytes.size());
        const auto err=m_ota.read(offset,scratch);if (err!=core_os::kOk) { return fail(err,Stage::Read); }
        if (!std::equal(scratch.begin(),scratch.end(),bytes.begin())) { return Status::fail(Result::InvalidArgument,Detail::OtaOffset); }
        return {};
    }
    const auto err=m_ota.write(bytes);if (err!=core_os::kOk) { return fail(err,Stage::Write); }
    m_hash.update(bytes);m_offset+=static_cast<uint32_t>(bytes.size());return {};
}
Status OtaService::finish() {
    if (auto st=disarmed();!st.ok()) { return st; }
    if (m_state==State::Ready) { return {}; }
    if (m_state!=State::Receiving || m_offset!=m_size) { return Status::fail(Result::InvalidArgument,Detail::WrongState); }
    if (m_hash.final()!=m_digest) { return Status::fail(Result::InvalidArgument,Detail::OtaHash); }
    auto err=m_ota.finish();if (err!=core_os::kOk) { return fail(err,Stage::Finish); }
    err=m_ota.select();if (err!=core_os::kOk) { return fail(err,Stage::Select); }
    m_state=State::Ready;return refresh();
}
Status OtaService::abort() {
    if (auto st=disarmed();!st.ok()) { return st; }
    core_os::OtaBootInfo info;
    auto err=m_ota.info(info);if (err!=core_os::kOk) { return fail(err,Stage::Info); }
    if (info.pending) { return Status::fail(Result::Busy,Detail::OtaPendingConfirm); }
    err=m_ota.abort();if (err!=core_os::kOk) { return fail(err,Stage::Abort); }
    m_state=State::Idle;m_size=0;m_offset=0;m_digest={};m_error=0;m_stage=Stage::None;return refresh();
}
Status OtaService::confirm() {
    if (auto st=disarmed();!st.ok()) { return st; }
    if (m_state!=State::Idle && !(m_state==State::Error && (m_stage==Stage::Confirm || !m_size))) {
        return Status::fail(Result::Busy,Detail::OtaActive);
    }
    const auto err=m_ota.confirm();if (err!=core_os::kOk) { return fail(err,Stage::Confirm); }
    m_state=State::Idle;m_error=0;m_stage=Stage::None;return refresh();
}
}
