import QtQuick
import QtQuick.Templates as T

// Figma NavItem: icon over label, 72 × 64. `checked` marks the current page;
// the rail sets it (the item never toggles itself).
T.AbstractButton {
    id: control

    property string iconName

    implicitWidth: Theme.sizeNavrail
    implicitHeight: 64
    padding: 0
    spacing: Theme.space4
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    font: Theme.fontLabelDefault
    opacity: enabled ? 1 : 0.4
    Accessible.role: Accessible.PageTab
    Accessible.name: text

    contentItem: Item {
        Column {
            anchors.centerIn: parent
            spacing: control.spacing

            DLIcon {
                anchors.horizontalCenter: parent.horizontalCenter
                name: control.iconName
                size: Theme.sizeIconMd
                color: control.checked ? Theme.accentText : Theme.textSecondary
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: control.text
                font: control.font
                color: control.checked ? Theme.textPrimary : Theme.textSecondary
            }
        }
    }

    background: Rectangle {
        color: control.checked ? Theme.bgSelected : control.enabled && control.hovered ? Theme.bgHover : "transparent"

        Rectangle {
            objectName: "indicator"
            visible: control.checked
            anchors.verticalCenter: parent.verticalCenter
            width: 3
            height: 40
            color: Theme.accentDefault
        }
        Rectangle {
            visible: control.visualFocus
            anchors.fill: parent
            anchors.margins: Theme.borderFocusWidth
            color: "transparent"
            border.width: Theme.borderFocusWidth
            border.color: Theme.borderFocus
        }
    }
}
