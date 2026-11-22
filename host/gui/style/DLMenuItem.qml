import QtQuick
import QtQuick.Templates as T

// Figma MenuItem: default, hover, disabled; optional second line (`detail`,
// Caption), used for the type and loop of an entry or why it is disabled.
T.MenuItem {
    id: control

    property string detail
    property string iconName

    implicitWidth: Math.max(200, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: detail.length > 0 ? 44 : Theme.sizeControl
    leftPadding: Theme.space12
    rightPadding: Theme.space12
    hoverEnabled: true
    font: Theme.fontBodyDefault

    contentItem: Row {
        spacing: Theme.space8
        DLIcon {
            anchors.verticalCenter: parent.verticalCenter
            visible: control.iconName.length > 0
            name: control.iconName
            color: Theme.textSecondary
        }
        Column {
            anchors.verticalCenter: parent.verticalCenter
            Text {
                text: control.text
                font: control.font
                color: control.enabled ? Theme.textPrimary : Theme.textDisabled
            }
            Text {
                visible: control.detail.length > 0
                text: control.detail
                font: Theme.fontCaption
                color: Theme.textSecondary
            }
        }
    }
    background: Rectangle {
        radius: Theme.radiusSm
        color: control.down ? Theme.bgPressed : control.highlighted || control.hovered ? Theme.bgHover : "transparent"
    }
}
