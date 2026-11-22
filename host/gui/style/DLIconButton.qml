import QtQuick
import QtQuick.Templates as T

// Figma IconButton: a 16 px icon in a 32 (Md) or 24 (Sm) square. It always has
// a tooltip, which is also its accessible name.
T.Button {
    id: control

    enum Size { Md, Sm }

    property int size: DLIconButton.Md
    property string iconName
    property string toolTipText
    property color iconColor: Theme.textSecondary

    readonly property real side: size === DLIconButton.Md ? Theme.sizeControl : Theme.sizeControlSm

    implicitWidth: side
    implicitHeight: side
    padding: 0
    display: T.AbstractButton.IconOnly
    opacity: enabled ? 1 : 0.4
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: toolTipText

    contentItem: Item {
        DLIcon {
            anchors.centerIn: parent
            name: control.iconName
            color: control.iconColor
        }
    }

    background: Rectangle {
        radius: Theme.radiusMd
        color: control.down ? Theme.bgPressed : control.enabled && control.hovered ? Theme.bgHover : "transparent"

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

    DLToolTip {
        visible: control.hovered && control.toolTipText.length > 0
        text: control.toolTipText
    }
}
