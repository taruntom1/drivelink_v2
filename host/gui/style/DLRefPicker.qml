import QtQuick
import QtQuick.Templates as T

// Figma RefPicker and SourcePicker: [chip] › target, with a menu of
// candidates ({value, text, detail, enabled, reason}). Entries that cannot be
// picked stay listed, disabled, with their reason. `status` draws Pending
// (violet), Rejected (fault) or Unset (dashed tone). The owner fills
// `candidates` in `aboutToOpen` and sends the picked value.
T.ComboBox {
    id: control

    enum Status { Normal, Pending, Rejected, Unset }

    property string chipText
    property string valueText
    property int status: DLRefPicker.Normal
    property var candidates: []

    signal aboutToOpen()
    signal picked(var value)

    model: candidates
    textRole: "text"
    currentIndex: -1
    displayText: valueText
    implicitWidth: 320
    implicitHeight: Theme.sizeControl
    leftPadding: Theme.space8
    rightPadding: Theme.space12 + Theme.sizeIconSm + Theme.space8
    font: Theme.fontBodyDefault
    opacity: enabled ? 1 : 0.4
    hoverEnabled: true
    onActivated: function(index) {
        const entry = candidates[index]
        currentIndex = -1
        if (entry && entry.enabled) picked(entry.value)
    }

    delegate: T.ItemDelegate {
        id: item
        required property var modelData
        required property int index
        width: ListView.view ? ListView.view.width : implicitWidth
        implicitHeight: modelData.detail || modelData.reason ? 44 : Theme.sizeControl
        leftPadding: Theme.space12
        rightPadding: Theme.space12
        enabled: modelData.enabled
        highlighted: control.highlightedIndex === index
        hoverEnabled: true
        contentItem: Column {
            Text {
                text: item.modelData.text
                font: control.font
                color: item.enabled ? Theme.textPrimary : Theme.textDisabled
                elide: Text.ElideRight
                width: parent.width
            }
            Text {
                visible: text.length > 0
                width: parent.width
                text: item.modelData.enabled ? (item.modelData.detail || "") : (item.modelData.reason || "")
                font: Theme.fontCaption
                color: item.modelData.enabled ? Theme.textSecondary : Theme.textTertiary
                elide: Text.ElideRight
            }
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

    contentItem: Row {
        spacing: Theme.space8
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            visible: control.chipText.length > 0
            width: chip.implicitWidth + 2 * Theme.space4; height: 20
            radius: Theme.radiusSm
            color: Theme.bgSecondary
            Text {
                id: chip
                anchors.centerIn: parent
                text: control.chipText
                font: Theme.fontMonoSmall
                color: Theme.textSecondary
            }
        }
        DLIcon {
            anchors.verticalCenter: parent.verticalCenter
            visible: control.chipText.length > 0
            name: "chevron-right"
            size: 12
            color: Theme.textTertiary
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: control.width - control.leftPadding - control.rightPadding - x
            text: control.displayText
            font: control.font
            color: control.status === DLRefPicker.Unset ? Theme.textTertiary : Theme.textPrimary
            elide: Text.ElideRight
        }
    }

    background: Rectangle {
        implicitHeight: Theme.sizeControl
        radius: Theme.radiusMd
        color: Theme.bgField
        border.width: control.visualFocus || control.status === DLRefPicker.Pending || control.status === DLRefPicker.Rejected
                      ? Theme.borderFocusWidth : Theme.borderWidth
        border.color: control.status === DLRefPicker.Rejected ? Theme.stateFault
                    : control.status === DLRefPicker.Pending ? Theme.statePending
                    : control.visualFocus ? Theme.borderFocus
                    : control.status === DLRefPicker.Unset ? Theme.stateUnsaved
                    : control.hovered ? Theme.borderStrong : Theme.borderDefault
    }

    popup: T.Popup {
        y: control.height + Theme.space4
        width: Math.max(control.width, 320)
        implicitHeight: Math.min(contentItem.implicitHeight + topPadding + bottomPadding, 400)
        padding: Theme.space4
        modal: false
        dim: false
        onAboutToShow: control.aboutToOpen()
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
