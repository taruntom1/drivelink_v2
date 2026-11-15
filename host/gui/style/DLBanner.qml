import QtQuick
import QtQuick.Layouts

// Figma Banner: full width under the status bar while a state holds (safe
// mode, pending reboot, ROS driving, first boot). Up to three actions: the
// first and third are Secondary buttons, the second is Primary.
Rectangle {
    id: banner

    enum Tone { Info, Pending, Armed, Fault }

    property int tone: DLBanner.Info
    property string title
    property string message
    property string iconName: ["info", "rotate-ccw", "triangle-alert", "shield-alert"][tone]
    property string action1Text
    property string action2Text
    property string action3Text

    signal actionTriggered(int index)

    readonly property color accent: [Theme.accentDefault, Theme.statePending, Theme.stateArmed, Theme.stateFault][tone]
    readonly property color iconColor: [Theme.accentText, Theme.statePending, Theme.stateArmed, Theme.stateFault][tone]

    implicitWidth: 1200
    implicitHeight: 48
    color: [Theme.bgSelected, Theme.statePendingSubtle, Theme.stateArmedSubtle, Theme.stateFaultSubtle][tone]
    Accessible.role: Accessible.AlertMessage
    Accessible.name: title + ". " + message

    RowLayout {
        anchors.fill: parent
        anchors.rightMargin: Theme.space12
        spacing: Theme.space12

        Rectangle {
            Layout.fillHeight: true
            implicitWidth: 3
            color: banner.accent
        }
        DLIcon {
            name: banner.iconName
            size: Theme.sizeIconMd
            color: banner.iconColor
        }
        Text {
            text: banner.title
            font: Theme.fontBodyStrong
            color: Theme.textPrimary
        }
        Text {
            Layout.fillWidth: true
            text: banner.message
            font: Theme.fontBodyDefault
            color: Theme.textSecondary
            elide: Text.ElideRight
        }
        DLButton {
            visible: banner.action1Text.length > 0
            kind: DLButton.Secondary
            text: banner.action1Text
            onClicked: banner.actionTriggered(0)
        }
        DLButton {
            visible: banner.action2Text.length > 0
            kind: DLButton.Primary
            text: banner.action2Text
            onClicked: banner.actionTriggered(1)
        }
        DLButton {
            visible: banner.action3Text.length > 0
            kind: DLButton.Secondary
            text: banner.action3Text
            onClicked: banner.actionTriggered(2)
        }
    }
}
