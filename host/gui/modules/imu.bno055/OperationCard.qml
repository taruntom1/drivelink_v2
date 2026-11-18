import QtQuick
import DriveLink.Style
import DriveLink.Shell

// Last operation: the outcome of the last Import, Read, Export or Apply. A
// device result is never inferred from a timeout; "uncertain" says so.
Card {
    id: card
    objectName: "operationCard"

    property var panel

    readonly property var operation: panel.operation

    function stateColor(state) {
        switch (state) {
        case "ok": return Theme.stateOk
        case "running": return Theme.accentText
        case "refused": return Theme.stateFault
        case "uncertain": return Theme.stateUnsaved
        case "rebooted": return Theme.stateUnsaved
        default: return Theme.textSecondary
        }
    }
    function stateIcon(state) {
        switch (state) {
        case "ok": return "circle-check"
        case "running": return "loader-circle"
        case "refused": return "circle-x"
        case "uncertain": return "triangle-alert"
        case "rebooted": return "triangle-alert"
        default: return "info"
        }
    }

    title: qsTr("Last operation")

    Text {
        objectName: "operationEmpty"
        width: parent.width
        visible: card.operation === null
        text: qsTr("No calibration action this connection")
        font: Theme.fontBodyDefault
        color: Theme.textSecondary
        wrapMode: Text.Wrap
    }
    Item {
        width: parent.width
        height: operationText.implicitHeight
        visible: card.operation !== null
        DLIcon {
            id: stateIcon
            objectName: "operationIcon"
            y: 2
            name: card.operation ? card.stateIcon(card.operation.state) : ""
            color: card.operation ? card.stateColor(card.operation.state) : Theme.textSecondary
            spinning: card.operation !== null && card.operation.state === "running"
        }
        Text {
            id: operationText
            objectName: "operationText"
            anchors.left: stateIcon.right
            anchors.leftMargin: Theme.space8
            anchors.right: parent.right
            text: card.operation ? card.operation.title + " → " + card.operation.text : ""
            font: Theme.fontBodyDefault
            color: card.operation ? card.stateColor(card.operation.state) : Theme.textSecondary
            wrapMode: Text.Wrap
        }
    }
    Column {
        objectName: "operationSteps"
        width: parent.width
        visible: card.operation !== null && card.operation.steps.length > 0
        spacing: Theme.space4
        Repeater {
            model: card.operation ? card.operation.steps : []
            Text {
                required property var modelData
                objectName: "operationStep"
                width: parent.width
                text: modelData.title + " — " + modelData.state + (modelData.text.length > 0 ? ": " + modelData.text : "")
                font: Theme.fontCaption
                color: card.stateColor(modelData.state)
                wrapMode: Text.Wrap
            }
        }
    }
    Text {
        width: parent.width
        text: qsTr("Operation results are shown here. Import and Export stay on this PC. A device result is never inferred from a timeout.")
        font: Theme.fontCaption
        color: Theme.textTertiary
        wrapMode: Text.Wrap
    }
}
