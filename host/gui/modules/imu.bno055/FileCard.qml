import QtQuick
import DriveLink.Style
import DriveLink.Shell
import DriveLink.Modules.ImuBno055

// Calibration on this PC: the file loaded locally, the last read, and the
// four calibration buttons. Every device call is the panel's; this card only
// draws and asks.
Card {
    id: card
    objectName: "fileCard"

    property var panel

    readonly property var imported: panel.imported
    readonly property var source: imported ? imported.source : null
    readonly property var lastRead: panel.lastRead
    readonly property bool applying: panel.applying
    readonly property bool empty: imported === null && lastRead === null

    function levelsText(levels) {
        if (!levels) return "—"
        return ImuCalibration.levels.map(function(n) { return levels[n] >= 0 ? levels[n] : "—" }).join("/")
    }
    function joined(a, b) { return [a, b].filter(function(t) { return t && t.length > 0 }).join(" · ") }

    title: qsTr("Calibration on this PC")

    // A small label above its value.
    component Field: Column {
        property string label
        property string value
        property bool mono: false
        width: parent.width
        spacing: Theme.space2
        Text {
            width: parent.width
            text: parent.label
            font: Theme.fontCaption
            color: Theme.textSecondary
        }
        Text {
            objectName: "fieldValue"
            width: parent.width
            text: parent.value.length > 0 ? parent.value : "—"
            font: parent.mono ? Theme.fontMonoSmall : Theme.fontBodyDefault
            color: Theme.textPrimary
            wrapMode: parent.mono ? Text.WrapAnywhere : Text.Wrap
        }
    }

    Text {
        objectName: "fileEmpty"
        width: parent.width
        visible: card.empty && card.panel.importError.length === 0
        text: qsTr("No calibration snapshot loaded. Read the current IMU calibration or import a file from this PC.")
        font: Theme.fontBodyDefault
        color: Theme.textSecondary
        wrapMode: Text.Wrap
    }

    Text {
        objectName: "importError"
        width: parent.width
        visible: card.panel.importError.length > 0
        text: qsTr("Import refused: %1").arg(card.panel.importError)
        font: Theme.fontBodyDefault
        color: Theme.stateFault
        wrapMode: Text.Wrap
    }

    Column {
        objectName: "importedFile"
        width: parent.width
        visible: card.imported !== null
        spacing: Theme.space12

        Item {
            width: parent.width
            height: Theme.sizeControlSm
            DLBadge {
                anchors.verticalCenter: parent.verticalCenter
                tone: DLBadge.Accent
                size: DLBadge.Sm
                text: qsTr("LOCAL FILE")
            }
            DLButton {
                objectName: "discardButton"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: Theme.sizeControlSm
                kind: DLButton.Ghost
                text: qsTr("Discard")
                enabled: !card.applying
                onClicked: card.panel.discardImport()
            }
        }
        Text {
            objectName: "importedName"
            width: parent.width
            text: card.imported ? card.imported.fileName : ""
            font: Theme.fontMonoDefault
            color: Theme.textPrimary
            wrapMode: Text.WrapAnywhere
        }
        Field {
            label: qsTr("Target")
            value: card.joined(card.panel.target.instance, card.panel.target.type)
        }
        Field {
            label: qsTr("Target device")
            value: card.joined(card.panel.target.deviceName, card.panel.target.deviceId)
        }
        Field {
            label: qsTr("File source")
            value: card.source ? card.joined(card.joined(card.source.deviceName, card.source.instance), card.source.deviceId) : ""
        }
        Field {
            label: qsTr("Read")
            value: card.source ? card.joined(card.source.createdUtc, qsTr("levels %1").arg(card.levelsText(card.source.levels))) : ""
        }
        Column {
            width: parent.width
            spacing: Theme.space4
            Repeater {
                model: card.imported ? card.imported.warnings : []
                Text {
                    required property string modelData
                    objectName: "importWarning"
                    width: parent.width
                    text: modelData
                    font: Theme.fontCaption
                    color: Theme.stateUnsaved
                    wrapMode: Text.Wrap
                }
            }
            Text {
                objectName: "importValid"
                width: parent.width
                visible: card.imported !== null && card.imported.warnings.length === 0
                text: qsTr("Compatible module type · file validated")
                font: Theme.fontCaption
                color: Theme.textSecondary
                wrapMode: Text.Wrap
            }
        }
    }

    Column {
        objectName: "lastReadBlock"
        width: parent.width
        visible: card.lastRead !== null
        spacing: Theme.space8

        Text {
            objectName: "lastReadText"
            width: parent.width
            text: card.lastRead ? qsTr("Read %1 · levels %2").arg(card.lastRead.at).arg(card.levelsText(card.lastRead.read.levels)) : ""
            font: Theme.fontCaption
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }
        DLTextField {
            objectName: "exportNote"
            width: parent.width
            placeholderText: qsTr("Mounting or robot (saved in the file)")
            text: card.panel.exportNote
            onTextEdited: card.panel.exportNote = text
        }
    }

    Column {
        width: parent.width
        spacing: Theme.space8

        DLButton {
            objectName: "importButton"
            width: parent.width
            text: qsTr("Import file…")
            enabled: !card.applying
            onClicked: card.panel.openImport()
        }
        DLReasonHover {
            width: parent.width
            reason: card.panel.readReason
            DLButton {
                objectName: "readButton"
                width: parent.width
                text: qsTr("Read from IMU")
                enabled: card.panel.readReason.length === 0
                onClicked: card.panel.read()
            }
        }
        Text {
            objectName: "readReason"
            width: parent.width
            visible: card.panel.readReason.length > 0
            text: card.panel.readReason
            font: Theme.fontCaption
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }
        DLReasonHover {
            width: parent.width
            reason: qsTr("Read from the IMU first")
            DLButton {
                objectName: "exportButton"
                width: parent.width
                text: qsTr("Export file…")
                enabled: card.lastRead !== null && !card.applying
                onClicked: card.panel.openExport()
            }
        }
        DLReasonHover {
            width: parent.width
            reason: card.panel.applyReason
            DLButton {
                objectName: "applyButton"
                width: parent.width
                kind: DLButton.Primary
                text: qsTr("Apply to IMU…")
                enabled: card.panel.applyReason.length === 0
                onClicked: card.panel.requestApply()
            }
        }
        Text {
            objectName: "applyReason"
            width: parent.width
            visible: card.imported !== null && card.panel.applyReason.length > 0
            text: card.panel.applyReason
            font: Theme.fontCaption
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }
        Text {
            width: parent.width
            text: qsTr("Apply changes the sensor calibration. Review the target before sending.")
            font: Theme.fontCaption
            color: Theme.textTertiary
            wrapMode: Text.Wrap
        }
    }
}
