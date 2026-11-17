import QtQuick
import QtQuick.Templates as T

// Figma Switch: On / Off × default, hover, disabled; an optional label.
T.Switch {
    id: control

    implicitWidth: implicitIndicatorWidth + (text.length > 0 ? spacing + implicitContentWidth : 0) + leftPadding + rightPadding
    implicitHeight: Theme.sizeControl
    padding: 0
    spacing: Theme.space8
    font: Theme.fontBodyDefault
    opacity: enabled ? 1 : 0.4
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    indicator: Rectangle {
        implicitWidth: 32
        implicitHeight: 18
        x: control.leftPadding
        y: (control.height - height) / 2
        radius: height / 2
        color: control.checked ? (control.hovered ? Theme.accentHover : Theme.accentDefault)
                               : (control.hovered ? Theme.bgSecondaryHover : Theme.bgSecondary)
        border.width: control.checked ? 0 : Theme.borderWidth
        border.color: control.visualFocus ? Theme.borderFocus : Theme.borderStrong

        Rectangle {
            width: 14; height: 14; radius: 7
            y: 2
            x: control.checked ? parent.width - width - 2 : 2
            color: control.checked ? Theme.textOnAccent : Theme.textSecondary
        }
    }
    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        visible: control.text.length > 0
        text: control.text
        font: control.font
        color: Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
    }
}
