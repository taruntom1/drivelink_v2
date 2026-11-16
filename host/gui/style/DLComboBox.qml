import QtQuick
import QtQuick.Templates as T

// Figma ComboBox with its Menu. The popup is not modal, so the status bar
// and DISARM stay reachable while it is open.
T.ComboBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, 160)
    implicitHeight: Theme.sizeControl
    leftPadding: Theme.space12
    rightPadding: Theme.space12 + Theme.sizeIconSm + Theme.space8
    font: Theme.fontBodyDefault
    opacity: enabled ? 1 : 0.4
    hoverEnabled: true

    delegate: T.ItemDelegate {
        id: item
        required property var model
        required property int index
        width: ListView.view ? ListView.view.width : implicitWidth
        implicitHeight: Theme.sizeControl
        leftPadding: Theme.space12
        rightPadding: Theme.space12
        highlighted: control.highlightedIndex === index
        hoverEnabled: true
        contentItem: Text {
            text: item.model[control.textRole] !== undefined ? item.model[control.textRole] : item.model.modelData
            font: control.font
            color: Theme.textPrimary
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: item.down ? Theme.bgPressed : item.highlighted || item.hovered ? Theme.bgHover : "transparent"
            radius: Theme.radiusSm
        }
    }

    indicator: DLIcon {
        x: control.width - width - Theme.space12
        anchors.verticalCenter: parent.verticalCenter
        name: "chevron-down"
        color: Theme.textSecondary
    }

    contentItem: Text {
        text: control.displayText
        font: control.font
        color: Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitHeight: Theme.sizeControl
        radius: Theme.radiusMd
        color: Theme.bgField
        border.width: control.visualFocus ? Theme.borderFocusWidth : Theme.borderWidth
        border.color: control.visualFocus ? Theme.borderFocus : control.hovered ? Theme.borderStrong : Theme.borderDefault
    }

    popup: T.Popup {
        y: control.height + Theme.space4
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + topPadding + bottomPadding, 320)
        padding: Theme.space4
        modal: false
        dim: false

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
        }
        background: Rectangle {
            color: Theme.bgRaised
            radius: Theme.radiusMd
            border.width: Theme.borderWidth
            border.color: Theme.borderDefault
            DLShadow { radius: parent.radius }
        }
    }
}
