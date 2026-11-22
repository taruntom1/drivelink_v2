import QtQuick
import DriveLink.Style

// G6 lease dialogs of a remote connection: take (with the explicit "safe to
// stop" confirmation), release, the held live edit and the revoked notice.
// The shell gives this item the page area (anchors.fill) so the dialogs sit
// where its other DLDialogs sit. They are not Qt modals: the shell's scrim
// stays under the status bar, so DISARM is live above them. The item itself
// takes no mouse input; only an open dialog does.
Item {
    id: root
    objectName: "leaseDialogs"

    property var backend
    readonly property bool anyOpen: takeDialog.visible || releaseDialog.visible || liveEditDialog.visible || revokedDialog.visible
    readonly property var lease: backend && backend.lease ? backend.lease : ({})
    readonly property var liveEdit: backend && backend.liveEdit ? backend.liveEdit : ({})
    readonly property bool hasLiveEdit: liveEdit.key !== undefined

    signal viewClientsRequested()

    // Take: asks only when another client holds the lease (G6 "Take from ROS");
    // an unheld lease is taken at once. Taking never arms.
    function requestTake() {
        if (lease.holderId && !lease.mine) takeDialog.open()
        else backend.takeLease()
    }
    function requestRelease() { releaseDialog.open() }
    // Esc and the shell's overlay cancel: every dialog leaves as its Cancel
    // would (a held live edit is dropped unsent).
    function closeAll() {
        takeDialog.reject(); releaseDialog.reject(); liveEditDialog.reject(); revokedDialog.reject()
    }

    function holderLabel(source) {
        // lease has holderName/holderKindText; liveEdit names the holder as holder.
        const name = source.holderName || source.holder || source.holderKindText || qsTr("another client")
        return name
    }

    // Each held edit has its own serial: a new one asks afresh, even when it
    // follows the last answer at once.
    readonly property string liveEditSerial: liveEdit.serial || ""
    // Opened on the next turn: an edit held again while Apply runs must not
    // meet the close that ends the Apply.
    onLiveEditSerialChanged: {
        if (hasLiveEdit) Qt.callLater(askLiveEdit)
        else liveEditDialog.close()
    }
    function askLiveEdit() {
        if (!hasLiveEdit || (liveEditDialog.visible && liveEditDialog.shownSerial === liveEditSerial)) return
        liveEditDialog.shownSerial = liveEditSerial
        liveEditDialog.confirmed = false
        liveEditDialog.open()
    }
    Component.onCompleted: if (hasLiveEdit) { liveEditDialog.shownSerial = liveEditSerial; liveEditDialog.open() }

    // The take question names one holder: when the lease changes hands while
    // it is open, it closes unanswered (the operator asks again).
    readonly property int holderId: lease.holderId || 0
    onHolderIdChanged: if (takeDialog.visible && holderId !== takeDialog.askedHolder) takeDialog.reject()

    Connections {
        target: root.backend
        function onLeaseRevoked(by) {
            const at = root.lease.revokedAt
            revokedDialog.by = root.lease.revokedBy || by || ""
            revokedDialog.at = at && !isNaN(at.getTime()) ? Qt.formatTime(at, "HH:mm:ss") : Qt.formatTime(new Date(), "HH:mm:ss")
            revokedDialog.open()
        }
    }

    DLDialog {
        id: takeDialog
        objectName: "takeLeaseDialog"
        readonly property string holder: root.holderLabel(root.lease)
        title: root.lease.ros ? qsTr("Take the lease from ROS?") : qsTr("Take the lease from %1?").arg(holder)
        text: root.lease.ros
            ? qsTr("ROS (%1) holds the control lease and may be driving the device. Taking it disarms the device first and stops ROS control. ROS does not get the lease back by itself.").arg(holder)
            : qsTr("%1 (%2) holds the control lease. Taking it disarms the device first and ends their control.")
                .arg(holder).arg(root.lease.holderKindText || "")
        confirmationText: qsTr("It is safe to stop %1 now").arg(holder)
        acceptText: qsTr("Take lease")
        // The holder this question was asked about.
        property int askedHolder: 0
        acceptEnabled: confirmed
        onAboutToShow: { confirmed = false; askedHolder = root.holderId }
        onAccepted: if (root.holderId === askedHolder) root.backend.takeLease()
    }

    DLDialog {
        id: releaseDialog
        objectName: "releaseLeaseDialog"
        title: qsTr("Release the control lease?")
        text: qsTr("Releasing the lease disarms the device. It does not restart ROS or any other client; one of them has to take the lease again.")
        acceptText: qsTr("Release lease")
        onAccepted: root.backend.releaseLease()
    }

    // A live param edit held while another client drives (G6 "Confirm live edit").
    DLDialog {
        id: liveEditDialog
        objectName: "liveEditDialog"
        property string shownSerial
        readonly property string holder: root.holderLabel(root.liveEdit)
        title: qsTr("Apply live edit?")
        text: root.liveEdit.ros
            ? qsTr("ROS (%1) holds the control lease and may be driving the device. This edit changes a running parameter now:").arg(holder)
            : qsTr("%1 holds the control lease and may be driving the device. This edit changes a running parameter now:").arg(holder)
        confirmationText: qsTr("Don't ask again for this connection")
        acceptText: qsTr("Apply live")
        rejectText: qsTr("Cancel")
        onAccepted: if (root.liveEditSerial === shownSerial) root.backend.applyLiveEdit(confirmed)
        // Cancel and Esc (a Dialog rejects on Esc) end here; the edit leaves
        // with them. Only the edit this dialog asked about: a newer one stays.
        onRejected: if (root.hasLiveEdit && root.liveEditSerial === shownSerial) root.backend.cancelLiveEdit()
        extra: Text {
            objectName: "liveEditWhat"
            width: parent ? parent.width : 400
            wrapMode: Text.Wrap
            text: root.liveEdit.what || ""
            font: Theme.fontMonoDefault
            color: Theme.textPrimary
        }
    }

    DLDialog {
        id: revokedDialog
        objectName: "leaseRevokedDialog"
        property string by
        property string at
        // Either button ends here; "View clients" also asks the shell for the card.
        property bool viewClients: false
        title: qsTr("Control lease revoked")
        text: (by.length > 0 ? qsTr("%1 took the control lease at %2.").arg(by).arg(at) : qsTr("Another client took the control lease at %1.").arg(at))
            + " " + qsTr("The device was disarmed. Test signals and auto re-run are stopped. Live telemetry and saved runs remain.")
        acceptText: qsTr("View clients")
        rejectText: qsTr("Keep observing")
        onAboutToShow: viewClients = false
        onAccepted: viewClients = true
        onClosed: {
            root.backend.acknowledgeRevoked()
            // After the dialog is gone, so the shell does not see a modal open.
            if (viewClients) Qt.callLater(root.viewClientsRequested)
        }
    }
}
