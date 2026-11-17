#include "ota_model.hpp"
#include <drivelink/protocol/core_codec.hpp>
#include <QCoreApplication>
#include <QVariantList>

namespace drivelink::gui {
namespace {
QString tr(const char *text) { return QCoreApplication::translate("Ota", text); }
using Phase=OtaJob::Phase;
// Rows of the dialog (G3 frames), in order.
enum Step { Disarmed, Write, Verify, Reboot, Reconnect, Confirm, StepCount };
int stepOf(Phase phase) {
    switch (phase) {
    case Phase::Writing: return Write;
    case Phase::Verifying: return Verify;
    case Phase::Rebooting: return Reboot;
    case Phase::Reconnecting: return Reconnect;
    case Phase::Confirming: return Confirm;
    default: return StepCount;
    }
}
QVariantMap step(const QString &label,const char *state,const QString &detail={}) {
    return {{"label",label},{"state",QString::fromLatin1(state)},{"detail",detail}};
}
QString slotName(int slot) { return QStringLiteral("ota_%1").arg(slot); }
QString stageName(uint8_t stage) {
    static const char *names[]={"none","info","begin","write","read","finish","select","abort","confirm"};
    return stage<9?QString::fromLatin1(names[stage]):QString::number(stage);
}
}

bool OtaJob::active() const {
    switch (phase) {
    case Phase::Writing: case Phase::Verifying: case Phase::Rebooting: case Phase::Reconnecting: case Phase::Confirming:
        return true;
    default:
        return false;
    }
}
QString otaPhaseName(Phase phase) {
    static const char *names[]={"writing","verifying","rebooting","reconnecting","confirming","confirmed",
                                "unconfirmed","rolledBack","aborted","failed"};
    return QString::fromLatin1(names[int(phase)]);
}
QString shortDigest(const protocol::Sha256Digest &digest) {
    const auto text=QString::fromStdString(hex(digest));
    return text.left(4)+QStringLiteral("…")+text.right(4);
}
QString byteSize(uint64_t bytes) {
    if (bytes>=1024*1024) { return QStringLiteral("%1 MB").arg(double(bytes)/(1024.0*1024.0),0,'f',2); }
    if (bytes>=1024) { return QStringLiteral("%1 KB").arg((bytes+512)/1024); }
    return QStringLiteral("%1 B").arg(bytes);
}

QVariantMap otaView(const OtaJob &job,const QString &runningVersion) {
    const auto size=job.image?uint32_t(job.image->bytes.size()):0U;
    const auto version=job.image?QString::fromStdString(job.image->version):QString();
    const auto previous=job.previousVersion;
    const QString target=job.started?slotName(1-job.candidate.previous_slot):tr("the inactive slot");
    // The row a terminal phase stopped at, and how far the update got.
    // An explicit roll back is the expected outcome: only CONFIRM stays unsent.
    const bool chosen=job.phase==Phase::RolledBack && job.rollback;
    const int current=job.active()?stepOf(job.phase):job.phase==Phase::Confirmed || job.phase==Phase::Unconfirmed?StepCount
                     :job.phase==Phase::RolledBack?int(chosen?Confirm:Reconnect):stepOf(job.failedAt);
    const bool stopped=job.phase==Phase::Failed || job.phase==Phase::Aborted || (job.phase==Phase::RolledBack && !chosen);
    auto state=[&](int row) -> const char * {
        if (row<current) { return "pass"; }
        if (row==current) { return stopped?"fail":job.active()?"active":"pending"; }
        if (job.phase==Phase::RolledBack && row==Confirm) { return chosen?"pending":"fail"; }
        return "pending";
    };
    const auto retried=job.retries==1?tr("1 chunk retried"):tr("%1 chunks retried").arg(job.retries);
    QVariantList steps;
    steps<<step(tr("Device disarmed"),"pass",job.active()?tr("ARM is refused until the update ends"):QString());
    QString writeDetail=job.phase==Phase::Writing?tr("%1 of %2").arg(byteSize(job.offset),byteSize(size)):byteSize(size);
    if (job.retries) { writeDetail+=QStringLiteral(" · ")+retried; }
    steps<<step(tr("Writing to %1").arg(target),state(Write),writeDetail);
    steps<<step(tr("Verify SHA-256"),state(Verify),
                current>Verify && job.image?tr("%1 matches").arg(shortDigest(job.image->sha256)):QString());
    QString rebootDetail;
    if (job.rebootAt.isValid()) { rebootDetail=job.rebootAt.toLocalTime().toString(QStringLiteral("HH:mm:ss")); }
    if (job.rebootUncertain) { rebootDetail+=(rebootDetail.isEmpty()?QString():QStringLiteral(" · "))+tr("no reply, not repeated"); }
    steps<<step(job.rollback?tr("Reboot to roll back"):tr("Reboot"),state(Reboot),rebootDetail);
    QString reconnectDetail;
    if (job.phase==Phase::RolledBack) {
        reconnectDetail=job.resetReason.isEmpty() || chosen?tr("%1 runs again").arg(previous)
                                                  :tr("%1 reset (%2)").arg(version,job.resetReason);
    } else if (current>Reconnect) {
        reconnectDetail=tr("%1 · core v%2").arg(version).arg(int(protocol::core_wire::kVersion));
    }
    steps<<step(tr("Reconnect and check the version"),state(Reconnect),reconnectDetail);
    QString confirmDetail;
    switch (job.phase) {
    case Phase::Confirming: confirmDetail=tr("sending CONFIRM");break;
    case Phase::Confirmed: confirmDetail=tr("confirmed");break;
    case Phase::Unconfirmed: confirmDetail=job.confirm?tr("uncertain: check and confirm"):tr("not sent: installed without confirming");break;
    case Phase::RolledBack: confirmDetail=tr("not sent · rolled back");break;
    default: confirmDetail=job.confirm?tr("automatic once %1 answers").arg(version):tr("skipped: install without confirming");break;
    }
    steps<<step(tr("Confirm the image"),job.phase==Phase::Unconfirmed?"pending":state(Confirm),confirmDetail);

    QString versions;
    switch (job.phase) {
    case Phase::Confirmed: versions=tr("running %1 · previous %2").arg(version,previous);break;
    case Phase::Unconfirmed: case Phase::Confirming: versions=tr("running %1 (not confirmed) · previous %2").arg(version,previous);break;
    case Phase::RolledBack: versions=tr("running %1 · image %2 not confirmed").arg(previous,version);break;
    default: versions=tr("running %1 → image %2").arg(runningVersion.isEmpty()?previous:runningVersion,version);break;
    }
    QString title=tr("Update firmware"),headline,note,tone=QStringLiteral("info");
    double progress=-1;QString progressLabel;
    switch (job.phase) {
    case Phase::Writing:
        progress=size?double(job.offset)/size:0;progressLabel=tr("Writing firmware");
        note=tr("Flash writes can delay the loops, so overruns may rise while writing. They stop when the write ends.");break;
    case Phase::Verifying:
        progress=1;progressLabel=tr("The device checks the SHA-256 and the image");
        note=tr("Nothing changes until the reboot; Abort still leaves %1 running.").arg(previous);break;
    case Phase::Rebooting: case Phase::Reconnecting: case Phase::Confirming:
        progress=job.phase==Phase::Confirming?0.5:0.25;
        progressLabel=job.phase==Phase::Confirming?tr("Confirming %1").arg(version):tr("Waiting for %1").arg(version);
        note=job.rollback?tr("The unconfirmed image is not booted again; the bootloader returns to the previous build.")
                         :tr("Until the device accepts CONFIRM, any reset rolls it back to %1. Stay connected for a moment.").arg(previous);
        break;
    case Phase::Confirmed:
        title=tr("Firmware updated");tone=QStringLiteral("ok");
        headline=tr("%1 is confirmed").arg(version);
        note=tr("It stays installed after resets. %1 remains in the other slot until the next update.").arg(previous);break;
    case Phase::Unconfirmed:
        title=tr("Firmware not confirmed");tone=QStringLiteral("pending");
        headline=tr("%1 runs unconfirmed").arg(version);
        note=job.message.isEmpty()?tr("ARM stays refused until it is confirmed. Any reset rolls back to %1.").arg(previous):job.message;break;
    case Phase::RolledBack:
        title=tr("Update rolled back");tone=chosen?QStringLiteral("pending"):QStringLiteral("fault");
        headline=tr("Rolled back to %1 · %2 was never confirmed.").arg(previous,version);
        note=job.rollback?tr("You rolled the update back. The saved configuration is untouched.")
                         :tr("The configuration in flash is untouched. Report the problem with the details below, or try another image.");
        break;
    case Phase::Aborted:
        title=tr("Update aborted");tone=QStringLiteral("pending");
        headline=tr("%1 keeps running").arg(previous);note=job.message;break;
    case Phase::Failed:
        title=tr("Update failed");tone=QStringLiteral("fault");
        headline=tr("Stopped at: %1").arg(steps.value(current<StepCount?current:Confirm).toMap().value("label").toString());
        note=job.message;break;
    }
    QString explanation;
    if (job.phase==Phase::RolledBack && !job.rollback) {
        explanation=tr("%1 did not stay running before it was confirmed, so the bootloader went back to %2.").arg(version,previous);
        if (!job.resetReason.isEmpty()) { explanation+=QStringLiteral(" ")+tr("Last reset: %1.").arg(job.resetReason); }
    }
    QString details;
    if (job.image) {
        details=tr("Image %1 (%2), version %3, project %4, built %5 %6, ELF %7, SHA-256 %8").arg(job.fileName,byteSize(size),version,
            QString::fromStdString(job.image->project),QString::fromStdString(job.image->date),QString::fromStdString(job.image->time),
            QString::fromStdString(hex(job.image->elf_sha256)),QString::fromStdString(hex(job.image->sha256)));
        details+=tr("\nDevice %1, previous %2 (build %3), result %4").arg(job.deviceId,previous,job.previousBuild,otaPhaseName(job.phase));
        if (!job.message.isEmpty()) { details+=QStringLiteral("\n")+job.message; }
        if (!job.resetReason.isEmpty()) { details+=tr("\nReset reason: %1").arg(job.resetReason); }
    }
    return {{"visible",true},{"phase",otaPhaseName(job.phase)},{"active",job.active()},{"title",title},
            {"headline",headline},{"explanation",explanation},{"tone",tone},
            {"fileName",job.fileName},{"size",byteSize(size)},{"versions",versions},
            {"sha",job.image?shortDigest(job.image->sha256):QString()},{"steps",steps},
            {"progress",progress},{"progressLabel",progressLabel},{"percent",progress<0?0:int(progress*100+0.5)},
            {"note",note},{"details",details},{"confirm",job.confirm},
            {"canAbort",(job.phase==Phase::Writing || job.phase==Phase::Verifying) && !job.cancelRequested},
            {"canClose",job.terminal()},
            {"canConfirm",job.phase==Phase::Unconfirmed},{"canRollBack",job.phase==Phase::Unconfirmed},
            {"canCopy",job.phase==Phase::RolledBack || job.phase==Phase::Failed}};
}

QVariantMap firmwareView(const FirmwareFacts &f) {
    QVariantMap map{{"version",f.version},{"buildId",f.buildId},{"lastUpdate",f.lastUpdate},
                    {"updateBlocked",f.updateBlocked},{"known",f.status.has_value()}};
    QString running=f.version,slotText,state,note;
    bool pending=false,canAbort=false;
    if (f.status) {
        const auto &s=*f.status;pending=s.pending_confirm;
        running+=pending?tr(" · not confirmed"):tr(" · confirmed");
        const int other=1-s.running_slot;
        slotText=tr("%1 (running) · %2").arg(slotName(s.running_slot),slotName(other));
        if (s.boot_slot!=s.running_slot) { slotText+=tr(" (boots next)"); }
        if (s.capacity) { slotText+=tr(" · %1 each").arg(byteSize(s.capacity)); }
        switch (s.state) {
        case 1: state=tr("An unfinished upload: %1 of %2").arg(byteSize(s.offset),byteSize(s.size));break;
        case 2: state=tr("A prepared update waits (%1); a reboot would install it unchecked").arg(byteSize(s.size));break;
        case 3: state=tr("The last update failed at %1 (error %2)").arg(stageName(s.stage)).arg(s.backend_error);break;
        default: break;
        }
        canAbort=s.state>=1 && s.state<=3 && s.boot_slot==s.running_slot && !pending;
        if (pending) {
            note=tr("ARM is refused until the running build is confirmed or rolled back.");
            if (!f.knownCandidate) {
                note+=QStringLiteral(" ")+tr("This window did not install it, so its identity cannot be checked: confirm only the build you meant to install.");
            }
        }
    }
    const QVariantMap more{{"running",running},{"slots",slotText},{"state",state},{"note",note},{"pending",pending},
                {"canConfirm",pending},{"canRollBack",pending},{"canAbort",canAbort},
                {"confirmBlocked",pending?f.actionBlocked:QString()},{"abortBlocked",canAbort?f.actionBlocked:QString()}};
    map.insert(more);
    return map;
}
} // namespace drivelink::gui
