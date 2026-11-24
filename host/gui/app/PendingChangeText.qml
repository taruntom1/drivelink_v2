import QtQuick
import DriveLink.Style

// One staged change: "subject  old → new" (adds and removes say so). A change
// with a note (safe mode) shows that instead.
Row {
    id: row
    required property var modelData
    spacing: Theme.space8
    Text {
        width: Math.min(implicitWidth, row.width * 0.45)
        elide: Text.ElideRight
        text: row.modelData.kind === "add" ? qsTr("Add %1").arg(row.modelData.subject)
            : row.modelData.kind === "remove" ? qsTr("Remove %1").arg(row.modelData.subject)
            : row.modelData.kind === "label" ? qsTr("Label") : row.modelData.subject
        font: Theme.fontBodyDefault; color: Theme.textPrimary
    }
    Text {
        width: row.width - x
        elide: Text.ElideRight
        text: row.modelData.note ? row.modelData.note
            : row.modelData.kind === "add" ? row.modelData.to
            : row.modelData.kind === "remove" ? row.modelData.from
            : (row.modelData.from ? row.modelData.from + " → " : "") + row.modelData.to
        font: Theme.fontBodyNumeric; color: Theme.statePending
    }
}
