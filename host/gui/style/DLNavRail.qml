import QtQuick

// Figma NavRail: the page items at the top, footer items (Settings) at the
// bottom. Controlled: a click emits activated(index) and the shell sets
// currentIndex. Footer items are indexed after the top items.
Rectangle {
    id: rail

    // [{ text, icon }]
    property var model: []
    property var footerModel: []
    property int currentIndex: 0

    signal activated(int index)

    implicitWidth: Theme.sizeNavrail
    implicitHeight: 640
    color: Theme.bgSurface

    Rectangle {
        anchors.right: parent.right
        width: Theme.borderWidth
        height: parent.height
        color: Theme.borderDefault
    }

    Column {
        anchors.top: parent.top
        anchors.topMargin: Theme.space8
        spacing: Theme.space4

        Repeater {
            model: rail.model

            DLNavItem {
                required property var modelData
                required property int index
                text: modelData.text
                iconName: modelData.icon
                checked: rail.currentIndex === index
                onClicked: rail.activated(index)
            }
        }
    }

    Column {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space8
        spacing: Theme.space4

        Repeater {
            model: rail.footerModel

            DLNavItem {
                required property var modelData
                required property int index
                text: modelData.text
                iconName: modelData.icon
                checked: rail.currentIndex === rail.model.length + index
                onClicked: rail.activated(rail.model.length + index)
            }
        }
    }
}
