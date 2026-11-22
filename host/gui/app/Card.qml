import QtQuick
import DriveLink.Style

// A G3 card: surface frame with an optional title row and actions.
Rectangle {
    id: card

    property string title
    property alias actions: actionRow.data
    default property alias content: body.data
    property real contentSpacing: Theme.space16

    implicitHeight: column.implicitHeight + 2 * Theme.space24
    color: Theme.bgSurface
    radius: Theme.radiusLg
    border.width: Theme.borderWidth
    border.color: Theme.borderDefault

    Column {
        id: column
        x: Theme.space24; y: Theme.space24
        width: card.width - 2 * Theme.space24
        spacing: Theme.space16

        Item {
            width: parent.width
            height: Math.max(titleText.implicitHeight, actionRow.implicitHeight)
            visible: card.title.length > 0 || actionRow.children.length > 0
            Text {
                id: titleText
                anchors.verticalCenter: parent.verticalCenter
                text: card.title
                font: Theme.fontTitleSection
                color: Theme.textPrimary
            }
            Row {
                id: actionRow
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space8
            }
        }
        Column {
            id: body
            width: parent.width
            spacing: card.contentSpacing
        }
    }
}
