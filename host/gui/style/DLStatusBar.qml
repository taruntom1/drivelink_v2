import QtQuick

// Figma StatusBar: three zones on every screen. Left: the device (or "Not
// connected"). Centre: arm state, last-disarm reason, the Arm / Ack fault
// button, faults, Unsaved with Save and Revert, Pending reboot; it stays
// centred however wide the sides are. Right: the red DISARM, enabled whenever
// there is a link (also while disarmed or in Fault).
//
// A view only: it shows what it is given and emits requests. The shell binds
// it to the device model and sends the requests (H2a-2).
Rectangle {
    id: bar

    enum Mode { NoDevice, Loading, Disarmed, Arming, Armed, Fault, SafeMode, LinkLost }

    property int mode: DLStatusBar.NoDevice

    property int link: DLDeviceChip.Serial
    property string deviceName
    property string deviceId
    property string connection

    // Figma's optional Lease boolean: the lease chip beside the device
    // identity, for remote sessions. Off by default, so local screens are
    // unchanged.
    property bool leaseVisible: false
    property int leaseVariant: DLLeaseChip.Available
    property string leaseHolderName

    // Last-disarm reason, loading progress, fault code: Caption beside the chip.
    property string reason
    // Disarmed: false shows "Arm disabled" with armBlockedReason in a tooltip.
    property bool armEnabled: true
    property string armBlockedReason
    property bool ackEnabled: true
    property string ackBlockedReason

    property bool faultsVisible: false
    property string faultsText
    property bool faultsActive: false

    property bool unsaved: false
    property bool configActionsVisible: true
    property bool saveEnabled: true
    property bool revertEnabled: true
    property string saveBlockedReason
    property string revertBlockedReason
    property bool pendingReboot: false

    readonly property bool linkUp: mode !== DLStatusBar.NoDevice && mode !== DLStatusBar.LinkLost
    readonly property int chipStatus: [DLStateChip.Offline, DLStateChip.Connecting, DLStateChip.Disarmed,
                                       DLStateChip.Arming, DLStateChip.Armed, DLStateChip.Fault,
                                       DLStateChip.SafeMode, DLStateChip.Offline][mode]
    readonly property bool hasArmButton: mode === DLStatusBar.Disarmed || mode === DLStatusBar.Arming
                                         || mode === DLStatusBar.Fault || mode === DLStatusBar.SafeMode
    readonly property bool hasDevice: mode !== DLStatusBar.NoDevice
    readonly property real sideWidth: Math.max(0, (width - centre.width) / 2 - Theme.space16 - Theme.space16)

    signal armRequested()
    signal disarmRequested()
    signal ackFaultRequested()
    signal saveRequested()
    signal revertRequested()
    signal faultsRequested()
    signal deviceRequested()
    signal leaseClicked()

    implicitWidth: 1920
    implicitHeight: Theme.sizeStatusbar
    color: Theme.bgSurface

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.borderWidth
        color: Theme.borderDefault
    }

    Item {
        id: left
        objectName: "leftZone"
        anchors.left: parent.left
        anchors.leftMargin: Theme.space16
        anchors.verticalCenter: parent.verticalCenter
        width: bar.sideWidth
        height: Theme.sizeControl
        clip: true

        Row {
            anchors.verticalCenter: parent.verticalCenter
            visible: !bar.hasDevice
            spacing: Theme.space8

            DLIcon {
                anchors.verticalCenter: parent.verticalCenter
                name: "unplug"
                color: Theme.textTertiary
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Not connected")
                font: Theme.fontBodyDefault
                color: Theme.textSecondary
            }
        }

        Row {
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.hasDevice
            spacing: Theme.space8

            DLDeviceChip {
                objectName: "deviceChip"
                anchors.verticalCenter: parent.verticalCenter
                link: bar.link
                name: bar.deviceName
                deviceId: bar.deviceId
                connection: bar.connection
                onClicked: bar.deviceRequested()
            }
            DLLeaseChip {
                objectName: "leaseChip"
                anchors.verticalCenter: parent.verticalCenter
                visible: bar.leaseVisible
                variant: bar.leaseVariant
                holderName: bar.leaseHolderName
                onClicked: bar.leaseClicked()
            }
        }
    }

    Row {
        id: centre
        objectName: "centreZone"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space12

        DLStateChip {
            objectName: "stateChip"
            anchors.verticalCenter: parent.verticalCenter
            status: bar.chipStatus
            onFaultsRequested: bar.faultsRequested()
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: text.length > 0
            text: bar.reason
            font: Theme.fontCaption
            color: Theme.textSecondary
        }
        DLReasonHover {
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.hasArmButton
            reason: bar.mode === DLStatusBar.Fault ? bar.ackBlockedReason : bar.armBlockedReason

            DLArmButton {
                objectName: "armButton"
                mode: bar.mode === DLStatusBar.Arming ? DLArmButton.Arming
                    : bar.mode === DLStatusBar.Fault ? DLArmButton.AckFault : DLArmButton.Arm
                enabled: bar.mode === DLStatusBar.Fault ? bar.ackEnabled
                       : bar.mode === DLStatusBar.Disarmed ? bar.armEnabled
                       : bar.mode === DLStatusBar.Arming
                onArmRequested: bar.armRequested()
                onAckFaultRequested: bar.ackFaultRequested()
            }
        }
        DLBadge {
            objectName: "faultsBadge"
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.faultsVisible && bar.hasDevice && bar.mode !== DLStatusBar.Loading
            tone: bar.faultsActive ? DLBadge.Fault : DLBadge.Neutral
            iconName: "triangle-alert"
            text: bar.faultsText
            TapHandler {
                onTapped: bar.faultsRequested()
            }
        }
        Row {
            objectName: "unsavedGroup"
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.unsaved && bar.hasDevice
            spacing: Theme.space4

            DLBadge {
                anchors.verticalCenter: parent.verticalCenter
                tone: DLBadge.Unsaved
                text: qsTr("unsaved")
            }
            DLReasonHover {
                visible: bar.configActionsVisible
                anchors.verticalCenter: parent.verticalCenter
                reason: bar.saveBlockedReason

                DLButton {
                    objectName: "saveButton"
                    kind: DLButton.Secondary
                    iconName: "save"
                    text: qsTr("Save")
                    enabled: bar.linkUp && bar.saveEnabled
                    onClicked: bar.saveRequested()
                }
            }
            DLReasonHover {
                visible: bar.configActionsVisible
                anchors.verticalCenter: parent.verticalCenter
                reason: bar.revertBlockedReason

                DLButton {
                    objectName: "revertButton"
                    kind: DLButton.Ghost
                    text: qsTr("Revert")
                    enabled: bar.linkUp && bar.revertEnabled
                    onClicked: bar.revertRequested()
                }
            }
        }
        DLBadge {
            objectName: "pendingBadge"
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.pendingReboot && bar.hasDevice
            tone: DLBadge.Pending
            iconName: "rotate-ccw"
            text: qsTr("Reboot pending")
        }
    }

    DLArmButton {
        objectName: "disarmButton"
        anchors.right: parent.right
        anchors.rightMargin: Theme.space16
        anchors.verticalCenter: parent.verticalCenter
        mode: DLArmButton.Disarm
        enabled: bar.linkUp
        onDisarmRequested: bar.disarmRequested()
    }
}
