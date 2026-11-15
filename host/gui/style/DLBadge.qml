import QtQuick
import QtQuick.Templates as T

// Figma Badge: a short label in one of eight tones, Md (24 px) or Sm (20 px),
// with an optional 12 px icon.
T.Control {
    id: control

    enum Tone { Neutral, Accent, Armed, Fault, Ok, Pending, Unsaved, Locked }
    enum Size { Md, Sm }

    property int tone: DLBadge.Neutral
    property int size: DLBadge.Md
    property string text
    property string iconName

    readonly property var fills: [Theme.bgSecondary, Theme.bgSelected, Theme.stateArmedSubtle,
                                  Theme.stateFaultSubtle, Theme.stateOkSubtle, Theme.statePendingSubtle,
                                  Theme.bgSecondary, Theme.bgSecondary]
    readonly property var foregrounds: [Theme.textSecondary, Theme.accentText, Theme.stateArmed,
                                        Theme.stateFault, Theme.stateOk, Theme.statePending,
                                        Theme.stateUnsaved, Theme.stateLocked]
    readonly property color foreground: foregrounds[tone]

    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    implicitHeight: size === DLBadge.Md ? 24 : 20
    padding: 0
    leftPadding: size === DLBadge.Md ? Theme.space8 : Theme.space4
    rightPadding: leftPadding
    spacing: Theme.space4
    font: Theme.fontLabelDefault
    Accessible.role: Accessible.StaticText
    Accessible.name: text

    contentItem: Row {
        spacing: control.spacing

        DLIcon {
            anchors.verticalCenter: parent.verticalCenter
            name: control.iconName
            visible: name.length > 0
            size: 12
            color: control.foreground
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font: control.font
            color: control.foreground
        }
    }

    background: Rectangle {
        radius: Theme.radiusSm
        color: control.fills[control.tone]
    }
}
