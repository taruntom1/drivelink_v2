import QtQuick
import DriveLink.Style

// A label/value pair in a device card.
Item {
    id: row
    property string label
    property string value
    property bool mono: false
    property real labelWidth: 180

    width: parent ? parent.width : 400
    implicitHeight: Math.max(labelText.implicitHeight, valueText.implicitHeight)

    Text {
        id: labelText
        width: row.labelWidth
        text: row.label
        font: Theme.fontBodyDefault
        color: Theme.textSecondary
        elide: Text.ElideRight
    }
    Text {
        id: valueText
        objectName: "value"
        anchors.left: labelText.right
        anchors.right: parent.right
        text: row.value
        font: row.mono ? Theme.fontMonoDefault : Theme.fontBodyNumeric
        color: Theme.textPrimary
        elide: Text.ElideRight
    }
}
