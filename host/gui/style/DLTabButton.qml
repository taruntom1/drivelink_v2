import QtQuick
import QtQuick.Templates as T

// Figma TabButton: default, hover, selected (accent underline).
T.TabButton {
    id: control

    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    implicitHeight: Theme.sizeControl
    leftPadding: Theme.space8
    rightPadding: Theme.space8
    font: Theme.fontBodyStrong
    hoverEnabled: true

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.checked || control.hovered ? Theme.textPrimary : Theme.textSecondary
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Item {
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width; height: 2
            color: control.checked ? Theme.accentDefault : "transparent"
        }
    }
}
