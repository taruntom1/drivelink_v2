import QtQuick
import QtQuick.Templates as T

// Figma Tooltip: Caption text on a raised surface, 500 ms delay, above or to
// the right of its parent and wrapped at 320 px.
T.ToolTip {
    id: control

    property bool beside: false
    x: !parent ? 0 : beside ? parent.width + Theme.space8 : (parent.width - implicitWidth) / 2
    y: beside && parent ? (parent.height - implicitHeight) / 2 : -implicitHeight - Theme.space4
    implicitWidth: Math.min(320, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: implicitContentHeight + topPadding + bottomPadding
    topPadding: Theme.space4
    bottomPadding: Theme.space4
    leftPadding: Theme.space8
    rightPadding: Theme.space8
    delay: 500
    font: Theme.fontCaption
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent

    contentItem: Text {
        text: control.text
        font: control.font
        color: Theme.textPrimary
        wrapMode: Text.Wrap
        lineHeightMode: Text.FixedHeight
        lineHeight: Theme.lineCaption
    }

    background: Rectangle {
        color: Theme.bgRaised
        radius: Theme.radiusSm
        border.width: Theme.borderWidth
        border.color: Theme.borderStrong

        DLShadow {
            radius: parent.radius
        }
    }
}
