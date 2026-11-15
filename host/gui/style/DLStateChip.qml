import QtQuick
import QtQuick.Templates as T

// Figma StateChip: connection plus arm state, in the status bar centre.
// Fault and Safe mode are clickable and ask for the faults drawer.
T.AbstractButton {
    id: control

    enum Status { Offline, Connecting, Disarmed, Arming, Armed, Fault, SafeMode }

    property int status: DLStateChip.Offline

    signal faultsRequested()

    readonly property bool opensFaults: status === DLStateChip.Fault || status === DLStateChip.SafeMode
    readonly property var labels: [qsTr("Offline"), qsTr("Connecting…"), qsTr("Disarmed"),
                                   qsTr("Arming…"), qsTr("ARMED"), qsTr("Fault"), qsTr("Safe mode")]
    readonly property var icons: ["unplug", "loader-circle", "power", "loader-circle", "zap",
                                  "triangle-alert", "shield-alert"]
    readonly property var iconColors: [Theme.textTertiary, Theme.textSecondary, Theme.stateDisarmed,
                                       Theme.stateArmed, Theme.textOnArmed, Theme.stateFault, Theme.stateFault]
    readonly property var textColors: [Theme.textTertiary, Theme.textSecondary, Theme.textSecondary,
                                       Theme.stateArmed, Theme.textOnArmed, Theme.stateFault, Theme.stateFault]
    readonly property var fills: [Theme.bgSecondary, Theme.bgSecondary, Theme.stateDisarmedSubtle,
                                  Theme.stateArmedSubtle, Theme.stateArmed, Theme.stateFaultSubtle,
                                  Theme.stateFaultSubtle]
    readonly property var borders: [null, null, null, Theme.stateArmed, null, Theme.stateFault, Theme.stateFault]

    text: labels[status]
    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    implicitHeight: Theme.sizeControl
    padding: 0
    leftPadding: Theme.space12
    rightPadding: Theme.space12
    spacing: Theme.space8
    font: Theme.fontBodyStrong
    hoverEnabled: opensFaults
    focusPolicy: opensFaults ? Qt.StrongFocus : Qt.NoFocus
    Accessible.name: text
    onClicked: if (opensFaults) faultsRequested()

    contentItem: Row {
        spacing: control.spacing

        DLIcon {
            anchors.verticalCenter: parent.verticalCenter
            name: control.icons[control.status]
            color: control.iconColors[control.status]
            spinning: control.status === DLStateChip.Connecting || control.status === DLStateChip.Arming
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font: control.font
            color: control.textColors[control.status]
        }
    }

    background: Rectangle {
        radius: height / 2
        color: control.fills[control.status]
        border.width: control.borders[control.status] ? Theme.borderWidth : 0
        border.color: control.borders[control.status] || "transparent"

        Rectangle {
            visible: control.visualFocus
            anchors.fill: parent
            anchors.margins: -Theme.borderFocusWidth
            radius: height / 2
            color: "transparent"
            border.width: Theme.borderFocusWidth
            border.color: Theme.borderFocus
        }
    }
}
