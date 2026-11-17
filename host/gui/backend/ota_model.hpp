#pragma once
#include <drivelink/ota.hpp>
#include <QDateTime>
#include <QString>
#include <QVariantMap>
#include <memory>
#include <optional>

namespace drivelink::gui {
// One firmware update run by this GUI (G3 "Update firmware" dialog). The
// backend advances it; otaView() turns it into what QML draws. Pure and tested.
struct OtaJob
{
    enum class Phase {
        Writing,      // BEGIN/WRITE in progress
        Verifying,    // FINISH: SHA-256 and image validation on the device
        Rebooting,    // REBOOT requested (sent once)
        Reconnecting, // waiting for Boot or the reopened link
        Confirming,   // identity check and CONFIRM
        Confirmed,    // done: the candidate runs and is confirmed
        Unconfirmed,  // the candidate runs, deliberately or uncertainly unconfirmed
        RolledBack,   // the previous build runs again
        Aborted,      // cancelled before the reboot; the upload was aborted
        Failed        // anything else; `message` says what to do
    };
    Phase phase = Phase::Writing;
    std::shared_ptr<const OtaImage> image;
    QString fileName, deviceId;
    QString previousVersion, previousBuild; // what ran before the update
    OtaCandidate candidate;                 // valid once `started`
    bool started = false;                   // the previous slot is known
    bool confirm = true;                    // false: leave the candidate pending
    bool rollback = false;                  // the reboot was an explicit roll back
    bool rebootUncertain = false;
    bool confirmSent = false;               // identity check/CONFIRM was started (never repeated)
    uint32_t offset = 0, retries = 0;
    QDateTime rebootAt;
    QString resetReason;                    // of the boot after the reboot
    QString message;                        // the result or the failure, with recovery text
    bool cancelRequested = false;
    // GUI-thread view of the step a terminal phase stopped at.
    Phase failedAt = Phase::Writing;
    bool active() const;   // between start and a terminal phase
    bool terminal() const { return !active(); }
};
QString otaPhaseName(OtaJob::Phase);
// {visible, phase, title, fileName, size, versions, sha, steps[{label,state,detail}],
//  progress (0..1 or -1), progressLabel, note, tone, canAbort, canClose,
//  canConfirm, canRollBack, message}
QVariantMap otaView(const OtaJob &job, const QString &runningVersion);
// Short ELF/SHA text as the frames show it: 4 hex, ellipsis, 4 hex.
QString shortDigest(const protocol::Sha256Digest &);
QString byteSize(uint64_t bytes);

// The Device page's Firmware update card from OTA STATUS and the last update
// this GUI recorded for the device. `blocked` is why Choose image is off.
struct FirmwareFacts
{
    QString version, buildId;
    std::optional<OtaStatus> status;
    QString lastUpdate;       // "17 Nov 14:02 · 1.4.2 → 2.0.0", from settings
    QString updateBlocked;    // empty when an update can start
    QString actionBlocked;    // common reason for the explicit actions
    bool knownCandidate = false; // this GUI uploaded the pending/Ready image
};
QVariantMap firmwareView(const FirmwareFacts &);
} // namespace drivelink::gui
