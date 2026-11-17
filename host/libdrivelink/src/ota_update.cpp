#include "drivelink/ota.hpp"
#include "drivelink/protocol/ota_image.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include <algorithm>
#include <thread>

namespace drivelink {
namespace p=protocol;
namespace {
// esp_app_desc_t follows the first segment header: version at 16, project 48,
// time 80, date 96, IDF version 112, ELF SHA-256 144 (all from its start).
constexpr size_t kAppDesc=32;
std::string descText(const Bytes &bytes,size_t offset,size_t length) {
    std::string text;
    for (size_t i=0;i<length;++i) {
        const auto c=bytes[kAppDesc+offset+i];
        if (!c) { break; }
        text.push_back(c<0x20 || c==0x7f ? '?' : static_cast<char>(c));
    }
    return text;
}
bool sameImage(const OtaCandidate &candidate,const OtaStatus &status) {
    return status.elf_sha256==candidate.elf_sha256 && status.running_slot!=candidate.previous_slot &&
           status.running_slot==status.boot_slot && status.state==0;
}
}
std::string OtaImage::buildId() const { return hex(elf_sha256).substr(0,16); }
OtaImage describeOtaImage(Bytes bytes) {
    OtaImage image;
    if (!p::otaImageIdentity(bytes,image.elf_sha256)) {
        throw Error(ErrorKind::Usage,"expected a complete unsigned ESP32-S3 application .bin");
    }
    image.sha256=p::sha256(bytes);
    image.version=descText(bytes,16,32);image.project=descText(bytes,48,32);
    image.time=descText(bytes,80,16);image.date=descText(bytes,96,16);image.idf_version=descText(bytes,112,32);
    image.bytes=std::move(bytes);
    return image;
}
OtaImage loadOtaImage(const std::string &utf8_path) { return describeOtaImage(readOtaImage(utf8_path)); }

OtaBoot classifyOtaBoot(const OtaCandidate &candidate,const DeviceInfo &info,const OtaStatus &status) {
    // REBOOT was accepted but the old slot still runs with the image prepared
    // or failed: the commit (boot selection) did not complete.
    if (status.running_slot==candidate.previous_slot && (status.state==2 || status.state==3)) { return OtaBoot::NotCommitted; }
    if (sameImage(candidate,status) && info.build_id==hex(candidate.elf_sha256).substr(0,16)) {
        return status.pending_confirm ? OtaBoot::Confirmable : OtaBoot::Confirmed;
    }
    // The bootloader went back to the previous, confirmed slot.
    if (status.running_slot==candidate.previous_slot && status.running_slot==status.boot_slot && status.state==0 &&
        !status.pending_confirm && status.elf_sha256!=candidate.elf_sha256) {
        return OtaBoot::RolledBack;
    }
    return OtaBoot::Mismatch;
}
std::string otaBootProblem(OtaBoot verdict) {
    switch (verdict) {
    case OtaBoot::NotCommitted:
        return "OTA reboot was accepted but commit did not complete; inspect ota-status, abort if boot_slot equals running_slot, otherwise reset for rollback";
    case OtaBoot::RolledBack:
    case OtaBoot::Mismatch:
        return "running image/slot does not match uploaded candidate; not confirmed";
    case OtaBoot::Confirmable:
    case OtaBoot::Confirmed:
        break;
    }
    return {};
}
OtaReboot requestOtaReboot(Client &client) {
    try { client.system(static_cast<uint8_t>(p::defs::SystemOp::Reboot));return OtaReboot::Accepted; }
    catch (const Error &e) {
        if (e.kind!=ErrorKind::Timeout && e.kind!=ErrorKind::Connection && !e.boot_changed) { throw; }
        return OtaReboot::Uncertain;
    }
}
OtaConfirmation confirmOtaCandidate(Client &client,const OtaCandidate &candidate,bool confirm,
                                    const std::function<bool()> &cancelled) {
    OtaConfirmation out;
    out.info=client.info();out.status=client.otaStatus();
    out.verdict=classifyOtaBoot(candidate,out.info,out.status);
    if (out.verdict!=OtaBoot::Confirmable && out.verdict!=OtaBoot::Confirmed) { return out; }
    if (confirm && out.status.pending_confirm) {
        if (cancelled && cancelled()) { throw Error(ErrorKind::Usage,"candidate left unconfirmed"); }
        try { (void)client.otaConfirm(); }
        catch (const Error &e) { if (e.kind!=ErrorKind::Timeout) { throw; } }
        // CONFIRM is idempotent; STATUS settles a lost reply without resending.
        out.status=client.otaStatus();
        if (out.status.pending_confirm) { throw Error(ErrorKind::Timeout,"confirmation uncertain; inspect ota-status and use ota-confirm"); }
        if (!sameImage(candidate,out.status)) { throw Error(ErrorKind::Protocol,"running image changed during confirmation; inspect ota-status"); }
        out.verdict=OtaBoot::Confirmed;
    }
    out.confirmed=!out.status.pending_confirm;
    return out;
}
OtaUpdateResult otaUpdate(const std::string &endpoint,const SessionOptions &options,const OtaImage &image,
                          const OtaUpdateOptions &update,const std::function<bool(const OtaProgress &)> &progress,
                          const std::function<bool()> &cancelled) {
    auto stop=[&] { return cancelled && cancelled(); };
    auto session=std::make_unique<Session>(endpoint,options);
    auto client=std::make_unique<Client>(*session);client->info();
    const auto before=client->otaStatus();
    auto state=client->otaUpload(image.bytes,update.window,progress);
    if (update.upload_only) { return {state,false,false}; }
    if (stop()) { client->otaAbort();throw Error(ErrorKind::Usage,"OTA cancelled before reboot; update aborted"); }
    // Sent once. A lost reply or link is not a reason to send it again: a
    // second reset of an unconfirmed candidate is itself a rollback request.
    (void)requestOtaReboot(*client);
    client.reset();session->close();session.reset();
    const OtaCandidate candidate{image.elf_sha256,before.running_slot};
    const auto deadline=std::chrono::steady_clock::now()+update.reconnect_window;
    std::string failure="device did not reconnect";
    while (std::chrono::steady_clock::now()<deadline) {
        if (stop()) { throw Error(ErrorKind::Usage,"OTA cancelled after reboot; candidate left unconfirmed; inspect ota-status"); }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        try {
            auto reconnect_options=options;
            reconnect_options.timeout=std::min(options.timeout,std::chrono::milliseconds(1500));
            session=std::make_unique<Session>(endpoint,reconnect_options);client=std::make_unique<Client>(*session);
            const auto result=confirmOtaCandidate(*client,candidate,update.confirm,cancelled);
            if (const auto problem=otaBootProblem(result.verdict);!problem.empty()) { throw Error(ErrorKind::Protocol,problem); }
            return {result.status,true,result.confirmed};
        } catch (const Error &e) {
            if (e.kind!=ErrorKind::Connection && e.kind!=ErrorKind::Timeout && !e.boot_changed) { throw; }
            failure=e.what();client.reset();if (session) { session->close();session.reset(); }
        }
    }
    throw Error(ErrorKind::Timeout,"OTA reconnect/confirmation failed: "+failure+"; inspect ota-status; confirmation remains uncertain");
}
} // namespace drivelink
