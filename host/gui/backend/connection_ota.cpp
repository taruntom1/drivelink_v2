// Firmware update (OTA) for LocalConnection: the G3 "Update firmware" dialog
// and the Device page's Firmware card. The library does the transfer and the
// identity check (drivelink/ota.hpp); this file keeps the GUI's rules:
// REBOOT is sent once and never repeated (the reboot expectation and its
// reconnect window are the ones every reboot uses), CONFIRM follows only the
// full identity check of the candidate this GUI uploaded, and nothing is
// confirmed or rolled back without that or an explicit user action.
#include "connection_internal.hpp"
#include <drivelink/protocol/ota_image.hpp>
#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QUrl>
#include <thread>

namespace drivelink::gui {
using Phase=OtaJob::Phase;
namespace {
QString when(const QDateTime &time) { return time.toLocalTime().toString(QStringLiteral("d MMM HH:mm")); }
}

void Connection::copyToClipboard(const QString &text) const {
    if(auto *clipboard=QGuiApplication::clipboard()) {clipboard->setText(text);}
}
QVariantMap LocalConnection::ota() const {
    if(!m_ota) {return {};}
    return otaView(*m_ota,m_snapshot?QString::fromStdString(m_snapshot->info.firmware_version):QString());
}
QVariantMap LocalConnection::firmware() const {
    FirmwareFacts facts;
    if(m_snapshot) {
        facts.version=QString::fromStdString(m_snapshot->info.firmware_version);
        facts.buildId=QString::fromStdString(m_snapshot->info.build_id);
    }
    facts.status=m_otaStatus;facts.lastUpdate=m_recent.lastUpdate(deviceId());
    facts.updateBlocked=updateBlocked();facts.actionBlocked=firmwareActionBlocked();
    facts.knownCandidate=m_candidates.contains(deviceId());
    return firmwareView(facts);
}
QString LocalConnection::firmwareActionBlocked() const {
    if(const auto common=commonBlock();!common.isEmpty()) {return common;}
    // An update reboots the device: plant identification writes its mode back first.
    if((m_tuneRun && m_tuneRun->identify) || m_modeRestore) {return tr("Wait for plant identification to write the mode back");}
    if(armState()==1 || m_arming) {return tr("Not allowed while armed; disarm first");}
    if(m_expectReboot) {return tr("The device is rebooting");}
    // OTA is exclusive through an agent.
    if(const auto why=exclusiveBlocked();!why.isEmpty()) {return why;}
    if(m_otaStatusPending || !m_otaStatus) {return tr("Reading the update state");}
    return {};
}
QString LocalConnection::updateBlocked() const {
    if(const auto why=firmwareActionBlocked();!why.isEmpty()) {return why;}
    if(m_otaStatus->pending_confirm) {return tr("Confirm or roll back the running build first");}
    if(m_otaStatus->state!=0) {return tr("Abort the unfinished update first");}
    return {};
}

QVariantMap LocalConnection::inspectImage(const QString &fileOrUrl) {
    const QUrl url(fileOrUrl);
    const QString path=url.isLocalFile()?url.toLocalFile():fileOrUrl;
    m_chosenImage.reset();m_chosenFile.clear();
    try {
        auto image=std::make_shared<const OtaImage>(loadOtaImage(path.toUtf8().toStdString()));
        m_chosenImage=image;m_chosenFile=QFileInfo(path).fileName();
        const auto running=m_snapshot?QString::fromStdString(m_snapshot->info.firmware_version):QString();
        const auto build=QString::fromStdString(image->buildId());
        const bool same=m_snapshot && m_snapshot->info.build_id==image->buildId();
        QString capacity;
        if(m_otaStatus && m_otaStatus->capacity && image->bytes.size()>m_otaStatus->capacity) {
            capacity=tr("The image (%1) exceeds the device's %2 slot").arg(byteSize(image->bytes.size()),byteSize(m_otaStatus->capacity));
        }
        return {{"ok",capacity.isEmpty()},{"problem",capacity},{"fileName",m_chosenFile},{"path",path},
            {"size",byteSize(image->bytes.size())},{"version",QString::fromStdString(image->version)},
            {"project",QString::fromStdString(image->project)},
            {"built",QString::fromStdString(image->date+" "+image->time).trimmed()},
            {"buildId",build},{"sha",shortDigest(image->sha256)},{"elf",shortDigest(image->elf_sha256)},
            {"versions",tr("running %1 → image %2").arg(running,QString::fromStdString(image->version))},
            {"sameBuild",same}};
    } catch(const Error &e) {
        return {{"ok",false},{"problem",errorText(e)},{"fileName",QFileInfo(path).fileName()},{"path",path}};
    }
}

void LocalConnection::readOtaStatus() {
    auto c=m_context;if(!c || m_otaStatusPending || !ready()) {return;}
    auto session=c->getSession();if(!session) {return;}
    m_otaStatusPending=true;m_otaStatusSerial=m_connectionSerial;m_otaStatusGeneration=m_snapshot->generation;
    const QPointer<LocalConnection> self(this);
    try {
        Client(*session).async([](Client &client){return client.otaStatus();},[self,c](std::expected<OtaStatus,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_otaStatusPending=false;
            if(result) {self->m_otaStatus=*result;self->m_otaStatusFailed=false;}
            else {
                // Unknown state: reboots stay blocked (a Ready image would be
                // committed unchecked). Report once; read again in 2 s.
                self->m_otaStatus.reset();
                if(!self->m_otaStatusFailed) {self->report(result.error());}
                self->m_otaStatusFailed=true;
                const auto serial=self->m_connectionSerial;
                QTimer::singleShot(2000,self,[self,serial] {
                    if(self && self->m_connectionSerial==serial && !self->m_otaStatus) {self->m_otaStatusSerial=0;self->otaRefresh();emit self->changed();}
                });
            }
            emit self->changed();
        });
    } catch(const Error &e) {
        m_otaStatusPending=false;m_otaStatusSerial=0;
        if(!m_otaStatusFailed) {report(e);}
        m_otaStatusFailed=true;
    }
}

void LocalConnection::startUpdate(bool confirm) {
    if(m_ota && m_ota->active()) {return;}
    if(const auto blocked=updateBlocked();!blocked.isEmpty() || !m_chosenImage) {
        report(Error(ErrorKind::Usage,(blocked.isEmpty()?tr("Choose an image first"):blocked).toStdString()));return;
    }
    auto c=m_context;auto session=c->getSession();
    OtaJob job;job.image=m_chosenImage;job.fileName=m_chosenFile;job.deviceId=deviceId();job.confirm=confirm;
    job.previousVersion=QString::fromStdString(m_snapshot->info.firmware_version);
    job.previousBuild=QString::fromStdString(m_snapshot->info.build_id);
    m_ota=job;m_otaCancel=std::make_shared<std::atomic<bool>>(false);
    const auto run=++m_otaRun;const auto cancel=m_otaCancel;const auto pause=m_otaPause;
    const auto queue=m_dispatch;const QPointer<LocalConnection> self(this);const auto image=m_chosenImage;
    emit changed();
    struct Upload { OtaStatus before,after; };
    try {
        session->async<Upload>([session,c,image,cancel,pause,queue,self,run] {
            Client client(*session);Upload upload;
            upload.before=client.otaStatus();
            post(queue,[self,c,run,slot=upload.before.running_slot] {
                if(!self || !self->current(c) || self->m_otaRun!=run || !self->m_ota) {return;}
                self->m_ota->candidate={self->m_ota->image->elf_sha256,slot};self->m_ota->started=true;emit self->changed();
            });
            int lastPercent=-1;uint32_t lastRetries=0;
            upload.after=client.otaUpload(image->bytes,protocol::kOtaWindow,[&](const OtaProgress &p) {
                const int percent=p.size?int(uint64_t(p.offset)*100/p.size):100;
                if(percent!=lastPercent || p.retries!=lastRetries) {
                    lastPercent=percent;lastRetries=p.retries;
                    post(queue,[self,c,run,p] {
                        if(!self || !self->current(c) || self->m_otaRun!=run || !self->m_ota || !self->m_ota->active()) {return;}
                        self->m_ota->offset=p.offset;self->m_ota->retries=p.retries;
                        if(p.offset==p.size) {self->m_ota->phase=Phase::Verifying;}
                        emit self->changed();
                    });
                }
                // Test/screenshot hold, after this progress is shown; abort
                // and close still end it.
                while(pause->load()>=0 && percent>=pause->load() && !cancel->load() && !c->retired) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                return !cancel->load() && !c->retired;
            });
            return upload;
        },[self,c,run,cancel](std::expected<Upload,Error> result) {
            if(!self || !self->current(c) || self->m_otaRun!=run || !self->m_ota) {return;}
            auto &job=*self->m_ota;
            if(!result) {
                // The library tried ABORT on any failure or cancellation and
                // says in the message whether that cleanup is uncertain.
                self->otaStop(cancel->load()?Phase::Aborted:Phase::Failed,errorText(result.error()));
                self->readOtaStatus();return;
            }
            job.offset=result->after.size;self->m_otaStatus=result->after;
            self->m_candidates.insert(job.deviceId,job.candidate);
            if(cancel->load()) {
                // Cancelled after FINISH: nothing is booted; release the image.
                job.cancelRequested=true;self->abortPreparedUpdate();return;
            }
            self->otaReboot();
        });
    } catch(const Error &e) {otaStop(Phase::Failed,errorText(e));}
}

void LocalConnection::abortUpdate() {
    if(!m_ota || (m_ota->phase!=Phase::Writing && m_ota->phase!=Phase::Verifying)) {return;}
    m_ota->cancelRequested=true;m_otaCancel->store(true);emit changed();
}
void LocalConnection::closeUpdate() {
    if(!m_ota || m_ota->active()) {return;}
    m_ota.reset();emit changed();
}

// The single REBOOT of an update or an explicit roll back. Intent (the
// expectation) precedes sending, as for every reboot: a link that drops
// after the reply leads to a reconnect, never to a second REBOOT.
void LocalConnection::otaReboot() {
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session) {otaStop(Phase::Failed,tr("The link closed before the reboot"));return;}
    const bool rollback=m_ota && m_ota->rollback;
    m_expectReboot=true;m_rebootCause=rollback?tr("Roll back firmware update"):tr("Firmware update");
    m_otaRebootSerial=m_connectionSerial;m_otaRebootGeneration=m_snapshot->generation;
    c->savedBeforeReboot=false;
    if(m_ota) {m_ota->phase=Phase::Rebooting;m_ota->rebootAt=QDateTime::currentDateTimeUtc();m_ota->confirmSent=false;}
    m_configAction=QStringLiteral("firmwareReboot");
    emit changed();
    const QPointer<LocalConnection> self(this);const auto run=m_otaRun;
    try {
        Client(*session).async([](Client &client){return requestOtaReboot(client);},[self,c,run](std::expected<OtaReboot,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_configAction.clear();
            const bool job=self->m_ota && self->m_otaRun==run;
            if(!result) {
                // Refused or unsent: no reboot happened.
                self->clearRebootExpectation();
                if(job) {
                    self->otaStop(Phase::Failed,errorText(result.error())+
                        (self->m_ota->rollback?QString():QStringLiteral(" ")+tr("The prepared image is still waiting; abort it in the Firmware card.")));
                } else {self->report(result.error());}
                self->readOtaStatus();return;
            }
            self->m_rebootElapsed.start();
            if(*result==OtaReboot::Uncertain) {
                emit self->notification(tr("No reply to the reboot request; it is not repeated. Waiting for the device."),0);
            }
            if(job && self->m_ota->phase==Phase::Rebooting) {
                self->m_ota->rebootUncertain=*result==OtaReboot::Uncertain;self->m_ota->phase=Phase::Reconnecting;
            }
            self->otaRefresh();emit self->changed();
        });
    } catch(const Error &e) {
        m_configAction.clear();clearRebootExpectation();otaStop(Phase::Failed,errorText(e));
    }
}

// Called after every refresh: keeps OTA STATUS current per connection and
// boot, and starts the identity check once the device is back after REBOOT.
void LocalConnection::otaRefresh() {
    if(!ready()) {return;}
    const bool fresh=m_otaStatusSerial==m_connectionSerial && m_otaStatusGeneration==m_snapshot->generation;
    const bool uploading=m_ota && (m_ota->phase==Phase::Writing || m_ota->phase==Phase::Verifying);
    if(!fresh && !uploading) {m_otaStatus.reset();readOtaStatus();}
    if(!m_ota || (m_ota->phase!=Phase::Rebooting && m_ota->phase!=Phase::Reconnecting) || m_ota->confirmSent) {return;}
    // Back after the reboot: a new link, or a Boot on the same one.
    const bool back=m_connectionSerial!=m_otaRebootSerial || m_snapshot->generation!=m_otaRebootGeneration;
    if(!back) {return;}
    if(deviceId()!=m_ota->deviceId) {otaStop(Phase::Failed,tr("A different device answered after the reboot; nothing was confirmed."));return;}
    otaConfirm(false);
}

// Identity check and, for a matching candidate, at most one CONFIRM. An
// explicit request (Confirm on the card or in the dialog) is the user's.
void LocalConnection::otaConfirm(bool explicitRequest) {
    auto c=m_context;auto session=c->getSession();
    const auto id=deviceId();
    const bool known=m_candidates.contains(id);
    const bool job=m_ota && m_ota->deviceId==id && m_ota->started &&
        (m_ota->phase==Phase::Unconfirmed || (!explicitRequest && m_ota->active()));
    const OtaCandidate candidate=job?m_ota->candidate:m_candidates.value(id);
    const bool sendConfirm=explicitRequest || (job && m_ota->confirm);
    if(job) {
        m_ota->confirmSent=true;m_ota->phase=Phase::Confirming;m_ota->message.clear();
        // The notice of this boot carries its reset reason.
        // 0 is "unknown" (vdev, or no reason recorded): not a cause to name.
        const auto code=m_notice.value("reasonCode");
        m_ota->resetReason=code.isValid() && code.toInt()!=0?m_notice.value("reason").toString():QString();
    }
    if(explicitRequest) {m_configAction=QStringLiteral("confirmFirmware");}
    emit changed();
    const QPointer<LocalConnection> self(this);const auto run=m_otaRun;
    try {
        Client(*session).async([candidate,known,job,sendConfirm,explicitRequest](Client &client) {
            if(known || job) {return confirmOtaCandidate(client,candidate,sendConfirm);}
            // Without a known candidate only the user may confirm.
            if(!explicitRequest) {throw Error(ErrorKind::Usage,"No uploaded candidate to check; nothing was confirmed");}
            // A pending build this GUI did not install: the user's explicit
            // CONFIRM, without an identity to compare (like `ota-confirm`).
            OtaConfirmation out;out.info=client.info();(void)client.otaConfirm();
            out.status=client.otaStatus();out.confirmed=!out.status.pending_confirm;
            out.verdict=out.confirmed?OtaBoot::Confirmed:OtaBoot::Confirmable;
            return out;
        },[self,c,run,id,explicitRequest,job=job](std::expected<OtaConfirmation,Error> result) {
            if(!self || !self->current(c)) {return;}
            if(explicitRequest) {self->m_configAction.clear();}
            const bool owned=job && self->m_ota && self->m_otaRun==run && self->m_ota->deviceId==id;
            if(!result) {
                const auto &e=result.error();
                if(owned) {
                    if(e.kind==ErrorKind::Timeout && e.outcome_unknown && e.family==9 && e.op==5) {
                        // STATUS still shows pending after an unanswered CONFIRM; not sent again.
                        self->otaStop(Phase::Unconfirmed,tr("No reply confirmed the image and the device still reports it pending. CONFIRM was not sent again: confirm it yourself once you are sure, or roll back."));
                    } else {
                        // The identity check (or the read after CONFIRM) did not complete.
                        self->otaStop(Phase::Failed,errorText(e)+QStringLiteral(" ")+tr("Nothing more is sent automatically; check the Firmware card."));
                    }
                } else {self->report(e);}
                self->readOtaStatus();emit self->changed();return;
            }
            self->m_otaStatus=result->status;
            self->m_otaStatusSerial=self->m_connectionSerial;self->m_otaStatusGeneration=self->m_snapshot?self->m_snapshot->generation:0;
            const auto verdict=result->verdict;
            if(verdict==OtaBoot::Confirmed || verdict==OtaBoot::RolledBack) {self->m_candidates.remove(id);}
            if(!owned) {
                if(result->confirmed) {emit self->notification(tr("Firmware confirmed"),1);}
                else if(verdict!=OtaBoot::Confirmable) {self->report(Error(ErrorKind::Protocol,otaBootProblem(verdict)));}
                emit self->changed();return;
            }
            auto &j=*self->m_ota;
            const auto version=j.image?QString::fromStdString(j.image->version):QString();
            switch(verdict) {
            case OtaBoot::Confirmed:
                self->m_recent.recordUpdate(id,tr("%1 · %2 → %3").arg(when(QDateTime::currentDateTimeUtc()),j.previousVersion,version));
                self->otaStop(Phase::Confirmed,{});break;
            case OtaBoot::Confirmable:
                self->otaStop(Phase::Unconfirmed,{});break;
            case OtaBoot::RolledBack:
                self->m_recent.recordUpdate(id,tr("%1 · %2 rolled back to %3").arg(when(QDateTime::currentDateTimeUtc()),version,j.previousVersion));
                self->otaStop(Phase::RolledBack,{});break;
            case OtaBoot::NotCommitted:
            case OtaBoot::Mismatch:
                j.failedAt=Phase::Reconnecting;
                self->otaStop(Phase::Failed,QString::fromStdString(otaBootProblem(verdict))+QStringLiteral(" ")+
                    tr("Nothing was confirmed."));break;
            }
            emit self->changed();
        });
    } catch(const Error &e) {
        if(explicitRequest) {m_configAction.clear();}
        if(job) {otaStop(Phase::Failed,errorText(e));} else {report(e);}
    }
}

void LocalConnection::otaStop(Phase phase,const QString &message) {
    if(!m_ota) {if(!message.isEmpty() && phase==Phase::Failed) {report(Error(ErrorKind::Connection,message.toStdString()));}return;}
    if(m_ota->active() && phase!=Phase::Confirmed && phase!=Phase::Unconfirmed && phase!=Phase::RolledBack &&
       m_ota->failedAt==Phase::Writing) {m_ota->failedAt=m_ota->phase;}
    m_ota->phase=phase;m_ota->message=message;
    if(phase==Phase::Failed) {emit notification(tr("Firmware update failed: %1").arg(message),2);}
    emit changed();
}
// The link closed or the user disconnected while an update was under way.
void LocalConnection::otaInterrupted(const QString &why) {
    if(!m_ota || !m_ota->active()) {return;}
    m_otaCancel->store(true);
    const bool afterReboot=m_ota->phase==Phase::Rebooting || m_ota->phase==Phase::Reconnecting || m_ota->phase==Phase::Confirming;
    otaStop(Phase::Failed,why+QStringLiteral(" ")+(afterReboot
        ?tr("The new build may be running unconfirmed: a reset rolls it back. Reconnect and check the Firmware card.")
        :tr("%1 keeps running. Reconnect and check the Firmware card for an unfinished upload.").arg(m_ota->previousVersion)));
}

void LocalConnection::confirmFirmware() {
    if(const auto why=firmwareActionBlocked();!why.isEmpty() || !m_otaStatus || !m_otaStatus->pending_confirm) {
        report(Error(ErrorKind::Usage,(why.isEmpty()?tr("No unconfirmed build is running"):why).toStdString()));return;
    }
    if(m_ota && m_ota->active()) {return;}
    otaConfirm(true);
}
void LocalConnection::rollBackFirmware() {
    if(const auto why=firmwareActionBlocked();!why.isEmpty() || !m_otaStatus || !m_otaStatus->pending_confirm) {
        report(Error(ErrorKind::Usage,(why.isEmpty()?tr("No unconfirmed build is running"):why).toStdString()));return;
    }
    if(m_ota && m_ota->active()) {return;}
    if(m_ota && m_ota->deviceId==deviceId() && m_ota->started) {
        // Classify after the reboot; never confirm after a roll back.
        m_ota->rollback=true;m_ota->confirm=false;m_ota->message.clear();
    } else {
        m_ota.reset();
    }
    ++m_otaRun;otaReboot();
}
void LocalConnection::abortPreparedUpdate() {
    const bool afterCancel=m_ota && m_ota->cancelRequested && m_ota->active();
    if(!afterCancel) {
        if(const auto why=firmwareActionBlocked();!why.isEmpty()) {report(Error(ErrorKind::Usage,why.toStdString()));return;}
    }
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session) {
        if(afterCancel) {otaStop(Phase::Failed,tr("The link closed before the prepared image was released; abort it in the Firmware card before any reboot."));}
        return;
    }
    m_configAction=QStringLiteral("abortUpdate");emit changed();
    const QPointer<LocalConnection> self(this);const auto run=m_otaRun;const auto id=deviceId();
    try {
        Client(*session).async([](Client &client){(void)client.otaAbort();return client.otaStatus();},
            [self,c,run,id,afterCancel](std::expected<OtaStatus,Error> result) {
                if(!self || !self->current(c)) {return;}
                self->m_configAction.clear();
                if(result) {self->m_otaStatus=*result;self->m_candidates.remove(id);}
                if(afterCancel && self->m_ota && self->m_otaRun==run) {
                    self->otaStop(result?Phase::Aborted:Phase::Failed,result?tr("The prepared image was released."):
                        errorText(result.error())+QStringLiteral(" ")+tr("Abort it in the Firmware card before any reboot."));
                } else if(result) {emit self->notification(tr("Prepared update aborted"),1);}
                else {self->report(result.error());self->readOtaStatus();}
                emit self->changed();
            });
    } catch(const Error &e) {m_configAction.clear();report(e);}
}
} // namespace drivelink::gui
