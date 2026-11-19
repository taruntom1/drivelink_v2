import QtQuick
import QtQuick.Templates as T

// Figma LeaseChip: who may command the device, beside the device identity in
// the status bar; activating it opens Device > Clients and lease. Ownership
// colours are neutral (accent for Mine and Requesting) so the arm-state colours
// keep their meaning. A view only: it shows the variant it is given.
T.AbstractButton {
    id: control

    enum Variant { Available, Mine, Ros, Other, Requesting, Revoked }

    property int variant: DLLeaseChip.Available
    // Shown for Other and Ros in place of "other GUI" / "ROS", e.g. "bench-laptop".
    property string holderName

    readonly property string label: {
        switch (variant) {
        case DLLeaseChip.Mine: return qsTr("Lease: this GUI")
        case DLLeaseChip.Ros: return holderName.length > 0 ? qsTr("Lease: %1").arg(holderName) : qsTr("Lease: ROS")
        case DLLeaseChip.Other: return holderName.length > 0 ? qsTr("Lease: %1").arg(holderName) : qsTr("Lease: other GUI")
        case DLLeaseChip.Requesting: return qsTr("Lease: requesting…")
        case DLLeaseChip.Revoked: return qsTr("Lease: revoked")
        default: return qsTr("Lease: available")
        }
    }
    readonly property string iconName: ["lock-open", "key-round", "lock", "users", "loader-circle", "key-round"][variant]
    readonly property bool highlighted: variant === DLLeaseChip.Mine || variant === DLLeaseChip.Requesting

    text: label
    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    implicitHeight: implicitContentHeight + topPadding + bottomPadding
    padding: 0
    leftPadding: Theme.space8 + Theme.borderWidth
    rightPadding: Theme.space8 + Theme.borderWidth
    topPadding: Theme.space4 + Theme.borderWidth
    bottomPadding: Theme.space4 + Theme.borderWidth
    spacing: Theme.space8
    font: Theme.fontCaption
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: text

    contentItem: Row {
        spacing: control.spacing

        DLIcon {
            objectName: "leaseIcon"
            anchors.verticalCenter: parent.verticalCenter
            name: control.iconName
            color: control.highlighted ? Theme.accentDefault : Theme.textSecondary
            spinning: control.variant === DLLeaseChip.Requesting
        }
        Text {
            objectName: "leaseLabel"
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font: control.font
            color: control.highlighted ? Theme.accentDefault : Theme.textSecondary
        }
    }

    background: Rectangle {
        radius: Theme.radiusMd
        color: control.down ? Theme.bgPressed : control.hovered ? Theme.bgHover : Theme.bgRaised
        border.width: Theme.borderWidth
        border.color: Theme.borderDefault

        Rectangle {
            visible: control.visualFocus
            anchors.fill: parent
            anchors.margins: -Theme.borderFocusWidth
            radius: parent.radius + Theme.borderFocusWidth
            color: "transparent"
            border.width: Theme.borderFocusWidth
            border.color: Theme.borderFocus
        }
    }
}
