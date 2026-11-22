import QtQuick
import QtQuick.Templates as T

// Figma ArmButton. The status bar shows one in the centre (Arm, Arming, Ack
// fault) and the red DISARM on the right. This control only draws the state and
// reports the request; the shell (H2a-2) sends it, and owns the application-wide
// Esc shortcut that always disarms.
//
// Mode Arm while disabled is "Arm disabled" (lock icon); Disarm while disabled
// is "Disarm disabled" (no link). Arming ignores clicks while ARM is in flight.
T.Button {
    id: control

    enum Mode { Arm, Arming, Disarm, AckFault }

    property int mode: DLArmButton.Arm

    signal armRequested()
    signal disarmRequested()
    signal ackFaultRequested()

    readonly property bool hot: enabled && hovered
    readonly property color foreground: {
        switch (mode) {
        case DLArmButton.Arm: return enabled ? Theme.stateArmed : Theme.textDisabled
        case DLArmButton.Arming: return Theme.stateArmed
        case DLArmButton.Disarm: return Theme.textOnAccent
        default: return Theme.stateFault
        }
    }
    readonly property string iconName: {
        switch (mode) {
        case DLArmButton.Arm: return enabled ? "power" : "lock"
        case DLArmButton.Arming: return "loader-circle"
        case DLArmButton.Disarm: return "octagon-x"
        default: return "circle-check"
        }
    }

    text: mode === DLArmButton.Arm ? qsTr("ARM")
        : mode === DLArmButton.Arming ? qsTr("ARMING…")
        : mode === DLArmButton.Disarm ? qsTr("DISARM") : qsTr("ACK FAULT")
    implicitWidth: 184
    implicitHeight: Theme.sizeDisarm
    padding: 0
    leftPadding: Theme.space16
    rightPadding: Theme.space16
    spacing: Theme.space8
    font: Theme.fontTitleSection
    opacity: !enabled && mode !== DLArmButton.Arm ? 0.4 : 1
    hoverEnabled: true
    // Tab reaches it; a click does not take focus, so clicking DISARM never
    // ends (and so sends) an open param edit. The shell cancels that edit.
    focusPolicy: Qt.TabFocus
    Accessible.name: mode === DLArmButton.Disarm ? text + qsTr(" (Esc)") : text

    onClicked: {
        switch (mode) {
        case DLArmButton.Arm: armRequested(); break
        case DLArmButton.Disarm: disarmRequested(); break
        case DLArmButton.AckFault: ackFaultRequested(); break
        default: break
        }
    }

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight

        Row {
            id: row
            anchors.centerIn: parent
            spacing: control.spacing

            DLIcon {
                anchors.verticalCenter: parent.verticalCenter
                name: control.iconName
                size: Theme.sizeIconMd
                color: control.foreground
                spinning: control.mode === DLArmButton.Arming
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                font: control.font
                color: control.foreground
            }
            Rectangle {
                objectName: "escHint"
                anchors.verticalCenter: parent.verticalCenter
                visible: control.mode === DLArmButton.Disarm
                implicitWidth: esc.implicitWidth + 2 * Theme.space4
                implicitHeight: 18
                radius: Theme.radiusSm
                color: "transparent"
                border.width: Theme.borderWidth
                border.color: Theme.textOnAccent

                Text {
                    id: esc
                    anchors.centerIn: parent
                    text: qsTr("Esc")
                    font: Theme.fontMonoSmall
                    color: Theme.textOnAccent
                }
            }
        }
    }

    background: Rectangle {
        radius: Theme.radiusMd
        color: {
            switch (control.mode) {
            case DLArmButton.Arm: return control.hot ? Theme.stateArmedSubtle : "transparent"
            case DLArmButton.Arming: return Theme.stateArmedSubtle
            case DLArmButton.Disarm:
                return control.down ? Theme.disarmPressed : control.hot ? Theme.disarmHover : Theme.disarmBg
            default: return Theme.stateFaultSubtle
            }
        }
        border.width: control.mode === DLArmButton.Disarm ? 0 : Theme.borderWidth
        border.color: control.mode === DLArmButton.AckFault ? Theme.stateFault
                    : control.mode === DLArmButton.Arm && !control.enabled ? Theme.borderDefault : Theme.stateArmed

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
