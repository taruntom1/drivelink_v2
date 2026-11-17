import QtQuick
import QtQuick.Templates as T

// Figma Menu: a raised list of DLMenuItems. Not modal, so the status bar and
// DISARM stay reachable while it is open.
T.Menu {
    id: control

    implicitWidth: 240
    implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
    padding: Theme.space4
    modal: false
    dim: false

    delegate: DLMenuItem {}
    contentItem: ListView {
        implicitHeight: contentHeight
        model: control.contentModel
        interactive: false
        clip: true
        currentIndex: control.currentIndex
    }
    background: Rectangle {
        color: Theme.bgRaised
        radius: Theme.radiusMd
        border.width: Theme.borderWidth
        border.color: Theme.borderDefault
        DLShadow { radius: parent.radius }
    }
}
