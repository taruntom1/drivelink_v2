import QtQuick

// Figma CheckItem (G3): Pass, Fail, Active, Pending and Warning with a label,
// an optional detail line and an optional trailing tag (right-aligned small
// text, any state). Warning is neutral (amber means Armed). Used for ARM
// checks, model loading, OTA steps and Tune's identification checks.
Item {
    id: control

    enum Status { Pass, Fail, Active, Pending, Warning }

    property int status: DLCheckItem.Pending
    property string label
    property string detail
    property string tag

    readonly property var icons: ["circle-check", "circle-x", "loader-circle", "ellipsis", "triangle-alert"]
    readonly property var colors: [Theme.stateOk, Theme.stateFault, Theme.accentText, Theme.textTertiary, Theme.textSecondary]

    implicitWidth: 320
    implicitHeight: Math.max(Theme.sizeIconMd, column.implicitHeight)

    DLIcon {
        id: icon
        y: 1
        name: control.icons[control.status]
        color: control.colors[control.status]
        size: Theme.sizeIconMd
        spinning: control.status === DLCheckItem.Active
    }
    Column {
        id: column
        anchors.left: icon.right
        anchors.leftMargin: Theme.space8
        anchors.right: tagText.visible ? tagText.left : parent.right
        anchors.rightMargin: tagText.visible ? Theme.space8 : 0
        spacing: Theme.space2
        Text {
            objectName: "label"
            width: parent.width
            text: control.label
            font: Theme.fontBodyDefault
            color: control.status === DLCheckItem.Pending ? Theme.textSecondary : Theme.textPrimary
            wrapMode: Text.Wrap
        }
        Text {
            objectName: "detail"
            width: parent.width
            visible: text.length > 0
            text: control.detail
            font: Theme.fontCaption
            color: control.status === DLCheckItem.Fail ? Theme.stateFault : Theme.textSecondary
            wrapMode: Text.Wrap
        }
    }
    Text {
        id: tagText
        objectName: "tag"
        anchors.right: parent.right
        y: 2
        visible: text.length > 0
        text: control.tag
        font: Theme.fontCaption
        color: Theme.textSecondary
    }
}
