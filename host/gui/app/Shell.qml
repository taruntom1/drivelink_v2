import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Dialogs
import DriveLink.Style

Basic.ApplicationWindow {
    id: shell
    width: 1920; height: 1080
    minimumWidth: 1280; minimumHeight: 720
    visible: true
    title: qsTr("DriveLink")
    color: Theme.bgApp
    property int page: 0
    property bool drawerOpen: false
    property bool armPopoverOpen: false
    property string armRefusal
    property int layoutLoop: -1
    property bool quitWhenDisconnected: false
    readonly property var backend: connection
    // The page under the status bar is blocked by a dialog or an overlay;
    // DISARM in the status bar stays live above every one of them.
    readonly property bool overlayOpen: linkLostOverlay.visible || reconnectOverlay.visible || noticeOverlay.visible || otaDialog.visible
    readonly property bool modalOpen: dialog.visible || renameDialog.visible || resetDialog.visible || overlayOpen
    property alias endpointField: connectPage.tcpField
    property alias statusBar: status
    property alias dialog: dialog
    property alias scrim: scrim
    property alias toastCount: toasts.count
    property alias otaDialog: otaDialog
    // Opens the firmware image picker (G3 "Choose image…").
    function chooseFirmware() { if (connection.firmware.updateBlocked === "") imagePicker.open() }

    function cancelOverlay() {
        drawerOpen = false; armPopoverOpen = false
        dialog.reject(); renameDialog.reject(); resetDialog.reject()
        // Esc closes the image choice; a running update is never aborted by Esc.
        if (otaDialog.choosing) otaDialog.cancel()
    }
    function notify(message, tone) {
        if (toasts.count >= 4) toasts.remove(0)
        toasts.append({"message": message, "modelTone": tone})
    }
    onClosing: function(event) {
        if (connection.linked || connection.loading || connection.closing) {
            event.accepted = false
            quitWhenDisconnected = true
            connection.disconnectDevice()
        }
    }
    Connections {
        target: connection
        function onNotification(text, tone) { shell.notify(text, tone) }
        function onDisconnected() { if (shell.quitWhenDisconnected) Qt.quit() }
        function onArmRefused(reason) { shell.armRefusal = reason; shell.armPopoverOpen = true }
    }
    Connections {
        target: safetyKeys
        function onEscapeRequested() { shell.cancelOverlay() }
        function onPageRequested(page) { if (connection.ready && !shell.modalOpen) shell.page = page }
    }
    DLStatusBar {
        id: status
        objectName: "statusBar"
        anchors.top: parent.top; width: parent.width
        z: 20
        mode: !shell.backend.linked ? (shell.backend.linkLost ? DLStatusBar.LinkLost
                                      : shell.backend.reconnecting ? DLStatusBar.Loading : DLStatusBar.NoDevice)
            : shell.backend.loading ? DLStatusBar.Loading
            : shell.backend.safeMode ? DLStatusBar.SafeMode
            : shell.backend.arming ? DLStatusBar.Arming
            : shell.backend.armState === 1 ? DLStatusBar.Armed
            : shell.backend.armState === 2 ? DLStatusBar.Fault : DLStatusBar.Disarmed
        deviceName: shell.backend.deviceName
        deviceId: shell.backend.deviceId
        connection: shell.backend.endpoint
        // While an update runs the centre names its step (G3 frames).
        reason: shell.backend.ota.active === true ? qsTr("Firmware update · %1").arg(shell.backend.ota.phase)
                                                  : shell.backend.reason
        armEnabled: shell.backend.armEnabled
        armBlockedReason: shell.backend.armBlockedReason
        ackEnabled: shell.backend.ackEnabled
        ackBlockedReason: shell.backend.ackBlockedReason
        pendingReboot: shell.backend.pendingReboot
        unsaved: shell.backend.unsaved
        configActionsVisible: true
        saveEnabled: !shell.backend.actionBlocked.save
        saveBlockedReason: shell.backend.actionBlocked.save || ""
        revertEnabled: !shell.backend.actionBlocked.revert
        revertBlockedReason: shell.backend.actionBlocked.revert || ""
        // Badge rule (H2b): active faults only; history stays in the drawer.
        faultsVisible: shell.backend.activeFaults > 0
        faultsActive: true
        faultsText: shell.backend.activeFaults === 1 ? qsTr("1 active fault")
                  : qsTr("%1 active faults").arg(shell.backend.activeFaults)
        onDisarmRequested: shell.backend.disarm()
        onArmRequested: shell.backend.arm()
        onAckFaultRequested: shell.backend.acknowledge()
        onSaveRequested: shell.backend.save()
        onRevertRequested: shell.backend.revert()
        onFaultsRequested: shell.drawerOpen = !shell.drawerOpen
        onDeviceRequested: if (shell.backend.ready) shell.page = 0
    }
    Item {
        id: body
        anchors.top: status.bottom; anchors.bottom: parent.bottom; width: parent.width
        DLNavRail {
            id: nav
            height: parent.height
            enabled: connection.ready && !connection.closing && !shell.modalOpen
            currentIndex: shell.page
            model: [{text: qsTr("Device"), icon: "cpu"}, {text: qsTr("Layout"), icon: "workflow"},
                    {text: qsTr("Tune"), icon: "sliders-horizontal"}, {text: qsTr("Monitor"), icon: "chart-line"}]
            onActivated: function(index) { shell.page = index }
        }
        Item {
            id: content
            anchors.left: nav.right; anchors.right: parent.right; height: parent.height
            enabled: !shell.modalOpen
            DLBanner {
                id: banner
                objectName: "shellBanner"
                width: parent.width
                visible: connection.ready && (connection.safeMode || connection.pendingReboot)
                tone: connection.safeMode ? DLBanner.Fault : DLBanner.Pending
                title: connection.safeMode ? qsTr("Device is in safe mode") : qsTr("Changes require a reboot")
                message: connection.safeMode
                    ? qsTr("It booted after repeated crashes: no loop runs and ARM is unavailable. Params and the layout can still be edited.")
                    : connection.unsaved ? qsTr("Save first, or the changes are lost at the reboot.") : ""
                action2Text: connection.safeMode ? qsTr("Exit safe mode")
                           : connection.unsaved ? qsTr("Save and reboot") : qsTr("Reboot")
                onActionTriggered: function(index) {
                    if (connection.safeMode) connection.exitSafeMode()
                    else if (connection.unsaved) connection.saveAndReboot()
                    else connection.reboot()
                }
            }
            ConnectPage {
                id: connectPage
                anchors.fill: parent
                backend: shell.backend
                visible: (connection.loading || !connection.linked) && !connection.linkLost && !connection.reconnecting
            }
            // A Boot in place reloads the model on the same link.
            DLCheckItem {
                objectName: "reloading"
                anchors.centerIn: parent
                width: 420
                visible: connection.linked && !connection.loading && !connection.ready && !connection.closing
                status: DLCheckItem.Active
                label: qsTr("Reloading the device model")
                detail: qsTr("The device rebooted; DISARM stays available.")
            }
            Item {
                anchors.fill: parent
                anchors.topMargin: banner.visible ? banner.height : 0
                visible: connection.ready
                DevicePage {
                    anchors.fill: parent
                    visible: shell.page === 0
                    backend: shell.backend
                    onRenameRequested: renameDialog.open()
                    onFactoryResetRequested: resetDialog.open()
                    onFaultsRequested: shell.drawerOpen = true
                    onLoopRequested: function(loopId) { shell.layoutLoop = loopId; shell.page = 1 }
                    onUpdateRequested: shell.chooseFirmware()
                }
                Column {
                    anchors.fill: parent; anchors.margins: Theme.space24
                    visible: shell.page !== 0
                    spacing: Theme.space24
                    Text {
                        text: [qsTr("Device"), qsTr("Layout"), qsTr("Tune"), qsTr("Monitor")][shell.page]
                        font: Theme.fontTitlePage; color: Theme.textPrimary
                    }
                    Text {
                        text: shell.page === 1 && shell.layoutLoop >= 0
                            ? qsTr("Loop #%1 is selected; the Layout screen follows in the next step.").arg(shell.layoutLoop)
                            : qsTr("This screen follows in a later step.")
                        font: Theme.fontBodyDefault; color: Theme.textSecondary
                    }
                }
            }
        }
        FaultsDrawer {
            id: drawer
            anchors.right: parent.right; height: parent.height; width: 480
            visible: shell.drawerOpen && (connection.ready || connection.linkLost)
            enabled: !shell.modalOpen
            backend: shell.backend
            onCloseRequested: shell.drawerOpen = false
        }
        // G3 "ARM refused": under the status bar's Arm button, with a fix
        // link per failed check.
        Rectangle {
            id: armPopover
            objectName: "armPopover"
            anchors.right: parent.right; anchors.rightMargin: Theme.space24
            y: Theme.space8; z: 5
            width: 440
            height: popoverColumn.implicitHeight + 2 * Theme.space16
            visible: shell.armPopoverOpen && connection.ready && !shell.modalOpen
            color: Theme.bgRaised
            radius: Theme.radiusLg
            border.width: Theme.borderWidth
            border.color: Theme.stateFault
            DLShadow { radius: parent.radius }
            Column {
                id: popoverColumn
                x: Theme.space16; y: Theme.space16
                width: parent.width - 2 * Theme.space16
                spacing: Theme.space12
                Item {
                    width: parent.width; height: Theme.sizeControlSm
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("ARM refused")
                        font: Theme.fontBodyStrong; color: Theme.stateFault
                    }
                    DLIconButton {
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        size: DLIconButton.Sm; iconName: "x"; toolTipText: qsTr("Close")
                        onClicked: shell.armPopoverOpen = false
                    }
                }
                Text {
                    width: parent.width; wrapMode: Text.Wrap
                    text: shell.armRefusal
                    font: Theme.fontBodyDefault; color: Theme.textPrimary
                }
                Repeater {
                    model: connection.armChecks.filter(c => c.state === "fail")
                    DLCheckItem {
                        required property var modelData
                        width: parent.width
                        status: DLCheckItem.Fail
                        label: modelData.label
                        detail: modelData.detail
                    }
                }
                Row {
                    spacing: Theme.space8
                    DLButton {
                        text: qsTr("Open faults")
                        onClicked: { shell.armPopoverOpen = false; shell.drawerOpen = true }
                    }
                    DLButton {
                        text: qsTr("Device page")
                        onClicked: { shell.armPopoverOpen = false; shell.page = 0 }
                    }
                }
            }
        }
        Rectangle {
            id: scrim
            objectName: "pageScrim"
            anchors.fill: parent
            color: Theme.bgScrim
            visible: shell.modalOpen
            MouseArea {anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: function(wheel) {wheel.accepted = true}}
        }
        // Flow 11: link lost over the last known state, with Retry/Disconnect.
        Card {
            id: linkLostOverlay
            objectName: "linkLostOverlay"
            anchors.centerIn: parent
            width: 520
            visible: connection.linkLost && !connection.linked && !connection.loading && !connection.reconnecting && !otaDialog.visible
            title: qsTr("Link lost")
            Text {
                width: parent.width; wrapMode: Text.Wrap
                text: qsTr("%1 stopped answering on %2. The device disarms itself when its command watchdog expires; nothing is sent until you reconnect.")
                    .arg(connection.deviceName || connection.deviceId || qsTr("The device")).arg(connection.endpoint)
                font: Theme.fontBodyDefault; color: Theme.textSecondary
            }
            Text {
                width: parent.width; wrapMode: Text.Wrap
                visible: text.length > 0
                text: connection.lastError.text || ""
                font: Theme.fontCaption; color: Theme.stateFault
            }
            Row {
                spacing: Theme.space8
                DLButton { objectName: "retryButton"; kind: DLButton.Primary; text: qsTr("Retry"); iconName: "refresh-cw"; onClicked: connection.retry() }
                DLButton { objectName: "lostDisconnect"; text: qsTr("Disconnect"); onClicked: connection.disconnectDevice() }
            }
        }
        Card {
            id: reconnectOverlay
            objectName: "reconnectOverlay"
            anchors.centerIn: parent
            width: 520
            visible: connection.reconnecting && !otaDialog.visible
            title: qsTr("Device is rebooting")
            DLCheckItem {
                width: parent.width
                status: DLCheckItem.Active
                label: qsTr("Reconnecting to %1").arg(connection.endpoint)
                detail: qsTr("The reboot request is not repeated; only the link is reopened.")
            }
            DLButton { text: qsTr("Stop and disconnect"); onClicked: connection.disconnectDevice() }
        }
        // Flow 11: device rebooted and model reloaded, with what was lost.
        Card {
            id: noticeOverlay
            objectName: "rebootNotice"
            anchors.centerIn: parent
            width: 560
            // After an update the dialog reports first; the notice follows it.
            visible: connection.ready && connection.rebootNotice.visible === true && !otaDialog.visible
            title: connection.rebootNotice.expected ? qsTr("Device rebooted") : qsTr("Device rebooted unexpectedly")
            InfoRow { label: qsTr("Requested"); value: connection.rebootNotice.cause || qsTr("No") }
            InfoRow { label: qsTr("Reset reason"); value: connection.rebootNotice.reason || "" }
            Text {
                width: parent.width; wrapMode: Text.Wrap
                text: (connection.rebootNotice.lost || []).length > 0
                    ? qsTr("These unsaved changes were lost:") : qsTr("No unsaved changes were lost. The device model was reloaded.")
                font: Theme.fontBodyDefault; color: Theme.textSecondary
            }
            Repeater {
                model: connection.rebootNotice.lost || []
                Text {
                    required property string modelData
                    width: parent.width; elide: Text.ElideRight
                    text: "•  " + modelData
                    font: Theme.fontBodyDefault; color: Theme.textPrimary
                }
            }
            DLButton { objectName: "noticeOk"; kind: DLButton.Primary; text: qsTr("OK"); onClicked: connection.dismissRebootNotice() }
        }
        OtaDialog {
            id: otaDialog
            anchors.centerIn: parent
            backend: shell.backend
            onChooseAnother: imagePicker.open()
        }
        // Non-modal and offered only while disarmed: an update needs a
        // disarmed device and blocks ARM, so DISARM stays the only live
        // safety action and remains reachable in the status bar.
        FileDialog {
            id: imagePicker
            title: qsTr("Choose a firmware image")
            fileMode: FileDialog.OpenFile
            modality: Qt.NonModal
            nameFilters: [qsTr("ESP32-S3 application (*.bin)"), qsTr("All files (*)")]
            onAccepted: otaDialog.choice = connection.inspectImage(selectedFile.toString())
        }
        DLDialog {
            id: dialog
            objectName: "shellDialog"
            parent: body
            title: qsTr("DriveLink")
        }
        DLDialog {
            id: renameDialog
            objectName: "renameDialog"
            parent: body
            title: qsTr("Rename device")
            text: qsTr("Up to 16 bytes of UTF-8, without control characters. Names need not be unique; an empty name shows the device id. The name applies now; Save keeps it after a reboot.")
            acceptText: qsTr("Rename")
            readonly property string problem: connection.nameProblem(nameField.text)
            acceptEnabled: problem.length === 0 && nameField.text !== connection.deviceName
            onAboutToShow: { nameField.text = connection.deviceName; nameField.selectAll(); nameField.forceActiveFocus() }
            onAccepted: connection.rename(nameField.text)
            extra: [
                DLTextField {
                    id: nameField
                    objectName: "nameField"
                    width: parent ? parent.width : 400
                    placeholderText: connection.deviceId
                    error: renameDialog.problem.length > 0
                    onAccepted: if (renameDialog.acceptEnabled) renameDialog.accept()
                },
                Text {
                    width: parent ? parent.width : 400
                    text: renameDialog.problem.length > 0 ? renameDialog.problem
                        : qsTr("%1 of 16 bytes").arg(encodeURIComponent(nameField.text).replace(/%[0-9A-F]{2}/g, "x").length)
                    font: Theme.fontCaption
                    color: renameDialog.problem.length > 0 ? Theme.stateFault : Theme.textSecondary
                }
            ]
        }
        DLDialog {
            id: resetDialog
            objectName: "resetDialog"
            parent: body
            destructive: true
            title: qsTr("Factory reset")
            text: qsTr("Erase the saved layout, params and name of %1, then reboot it with the factory layout. This cannot be undone.")
                .arg(connection.deviceName || connection.deviceId)
            acceptText: qsTr("Erase and reboot")
            confirmationText: qsTr("I understand the saved configuration is erased")
            acceptEnabled: confirmed
            onAboutToShow: confirmed = false
            onAccepted: connection.factoryReset()
        }
    }
    ListModel {id: toasts}
    Column {
        anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.margins: Theme.space24; spacing: Theme.space8; z: 30
        Repeater {
            model: toasts
            DLToast {
                objectName: "shellToast"
                required property int index
                required property string message
                required property int modelTone
                text: message
                tone: modelTone
                onDismissed: toasts.remove(index)
            }
        }
    }
}
