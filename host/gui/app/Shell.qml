import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
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
    property bool modalOpen: false
    property bool quitWhenDisconnected: false
    readonly property var backend: connection
    property alias endpointField: endpoint
    property alias statusBar: status
    property alias dialog: dialog
    property alias scrim: scrim
    property alias toastCount: toasts.count

    function cancelOverlay() { drawerOpen = false; dialog.reject(); modalOpen = false }
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
        mode: !shell.backend.linked ? (shell.backend.linkLost ? DLStatusBar.LinkLost : DLStatusBar.NoDevice)
            : shell.backend.loading ? DLStatusBar.Loading
            : shell.backend.safeMode ? DLStatusBar.SafeMode
            : shell.backend.arming ? DLStatusBar.Arming
            : shell.backend.armState === 1 ? DLStatusBar.Armed
            : shell.backend.armState === 2 ? DLStatusBar.Fault : DLStatusBar.Disarmed
        deviceName: shell.backend.deviceName
        deviceId: shell.backend.deviceId
        connection: shell.backend.endpoint
        reason: shell.backend.reason
        armEnabled: shell.backend.armEnabled
        armBlockedReason: shell.backend.armBlockedReason
        pendingReboot: shell.backend.pendingReboot
        unsaved: shell.backend.unsaved
        configActionsVisible: false
        // SAVE/REVERT and the badge policy are completed in H2b.
        onDisarmRequested: shell.backend.disarm()
        onArmRequested: shell.backend.arm()
        onAckFaultRequested: shell.backend.acknowledge()
        onFaultsRequested: shell.drawerOpen = !shell.drawerOpen
        onDeviceRequested: shell.page = 0
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
            anchors.left: nav.right; anchors.right: parent.right; height: parent.height
            enabled: !shell.modalOpen
            DLBanner {
                id: banner
                width: parent.width
                visible: connection.safeMode || connection.pendingReboot
                title: connection.safeMode ? qsTr("Device is in safe mode") : qsTr("Changes require a reboot")
            }
            Column {
                anchors.centerIn: parent
                width: 720
                spacing: Theme.space24
                visible: !connection.ready
                Text {
                    text: connection.loading ? qsTr("Loading device…") : qsTr("Connect to a device")
                    font: Theme.fontTitlePage; color: Theme.textPrimary
                }
                Basic.TextField {
                    id: endpoint
                    objectName: "endpoint"
                    width: parent.width; height: Theme.sizeControl
                    text: "tcp://127.0.0.1:8765"
                    placeholderText: "serial://COM3"
                    font: Theme.fontBodyDefault; color: Theme.textPrimary
                    background: Rectangle {color: Theme.bgField; radius: Theme.radiusMd; border.color: Theme.borderDefault}
                    enabled: !connection.loading && !connection.closing
                    onAccepted: connection.connectTo(text)
                }
                DLButton {
                    text: connection.loading ? qsTr("Cancel") : qsTr("Connect")
                    enabled: !connection.closing
                    onClicked: connection.loading ? connection.disconnectDevice() : connection.connectTo(endpoint.text)
                }
                Text {
                    width: parent.width; wrapMode: Text.Wrap
                    text: connection.lastError.message || ""
                    color: Theme.stateFault; font: Theme.fontBodyDefault
                }
            }
            Column {
                anchors.fill: parent; anchors.margins: Theme.space24
                anchors.topMargin: banner.visible ? banner.height + Theme.space24 : Theme.space24
                visible: connection.ready
                spacing: Theme.space24
                Row {
                    width: parent.width
                    spacing: Theme.space24
                    Text {
                        text: [qsTr("Device"), qsTr("Layout"), qsTr("Tune"), qsTr("Monitor")][shell.page]
                        font: Theme.fontTitlePage; color: Theme.textPrimary
                    }
                    DLButton {kind: DLButton.Secondary; text: qsTr("Disconnect"); onClicked: connection.disconnectDevice()}
                }
                Text {
                    text: connection.deviceName || connection.deviceId
                    font: Theme.fontBodyDefault; color: Theme.textSecondary
                }
            }
        }
        Rectangle {
            id: drawer
            objectName: "faultsDrawer"
            anchors.right: parent.right; height: parent.height; width: 480
            visible: shell.drawerOpen
            enabled: !shell.modalOpen
            color: Theme.bgSurface; border.color: Theme.borderDefault
            Column {
                anchors.fill: parent; anchors.margins: Theme.space24; spacing: Theme.space16
                Row {
                    spacing: Theme.space24
                    Text {text: qsTr("Faults"); font: Theme.fontTitleSection; color: Theme.textPrimary}
                    DLIconButton {iconName: "x"; toolTipText: qsTr("Close faults"); onClicked: shell.drawerOpen = false}
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
        DLDialog {
            id: dialog
            objectName: "shellDialog"
            parent: body
            title: qsTr("DriveLink")
            onOpened: shell.modalOpen = true
            onClosed: shell.modalOpen = false
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
