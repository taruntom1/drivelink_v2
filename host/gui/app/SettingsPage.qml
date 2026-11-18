import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import DriveLink.Style

// H2g Settings: Appearance (Dark / Light / Follow system) and About (version,
// Qt, compiler, licence, third-party notices with Copy). Inline, no popups.
Flickable {
    id: page
    objectName: "settingsPage"

    // The context property of main.cpp, read once the page is built.
    property var settings: appSettings
    signal closeRequested()
    signal copied(string what)

    readonly property bool wide: width >= 1500
    // The notices fill the About card down to the window's bottom edge.
    readonly property real noticesHeight: Math.max(360, height - 2 * Theme.space24 - header.height - Theme.space24
                                                   - 2 * Theme.space24 - aboutFacts.height - noticesHead.height
                                                   - licenceBlock.height - 4 * Theme.space16 - Theme.space20)

    contentHeight: column.implicitHeight + 2 * Theme.space24
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    Column {
        id: column
        x: Theme.space24; y: Theme.space24
        width: page.width - 2 * Theme.space24
        spacing: Theme.space24

        Item {
            id: header
            width: parent.width; height: Theme.sizeControl
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Settings")
                font: Theme.fontTitlePage; color: Theme.textPrimary
            }
            DLIconButton {
                objectName: "settingsClose"
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                iconName: "x"; toolTipText: qsTr("Close")
                onClicked: page.closeRequested()
            }
        }

        GridLayout {
            width: parent.width
            columns: page.wide ? 2 : 1
            columnSpacing: Theme.space24
            rowSpacing: Theme.space24

            Card {
                objectName: "appearanceCard"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                title: qsTr("Appearance")
                Text {
                    text: qsTr("Theme")
                    font: Theme.fontBodyStrong; color: Theme.textPrimary
                }
                Row {
                    spacing: Theme.space4
                    Repeater {
                        model: [{value: "dark", name: "themeDark", text: qsTr("Dark")},
                                {value: "light", name: "themeLight", text: qsTr("Light")},
                                {value: "system", name: "themeSystem", text: qsTr("Follow system")}]
                        delegate: DLTabButton {
                            required property var modelData
                            objectName: modelData.name
                            text: modelData.text
                            checkable: false
                            checked: page.settings.theme === modelData.value
                            onClicked: page.settings.theme = modelData.value
                        }
                    }
                }
                Text {
                    objectName: "themeHint"
                    width: parent.width; wrapMode: Text.Wrap
                    text: page.settings.theme === "system"
                        ? qsTr("Follows the operating system's app mode (now: %1).").arg(page.settings.dark ? qsTr("Dark") : qsTr("Light"))
                        : qsTr("Saved on this computer.")
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
            }

            Card {
                objectName: "aboutCard"
                Layout.fillWidth: true
                Layout.preferredWidth: page.wide ? 2 : 1
                Layout.alignment: Qt.AlignTop
                title: qsTr("About")
                Column {
                    id: aboutFacts
                    width: parent.width
                    spacing: Theme.space8
                    Item {
                        width: parent.width; height: Theme.sizeControlSm
                        Text {
                            id: versionLabel
                            anchors.verticalCenter: parent.verticalCenter
                            width: 180
                            text: qsTr("DriveLink")
                            font: Theme.fontBodyDefault; color: Theme.textSecondary
                        }
                        TextEdit {
                            objectName: "versionText"
                            anchors.left: versionLabel.right; anchors.verticalCenter: parent.verticalCenter
                            readOnly: true
                            selectByMouse: true
                            text: page.settings.version
                            textFormat: TextEdit.PlainText
                            font: Theme.fontMonoDefault; color: Theme.textPrimary
                        }
                        DLIconButton {
                            objectName: "copyVersion"
                            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                            size: DLIconButton.Sm
                            iconName: "copy"; toolTipText: qsTr("Copy the version")
                            onClicked: { page.settings.copyText(page.settings.version); page.copied(qsTr("Version")) }
                        }
                    }
                    InfoRow { label: qsTr("Qt"); value: page.settings.qtVersion; mono: true }
                    InfoRow { label: qsTr("Compiler"); value: page.settings.compiler; mono: true }
                    InfoRow { label: qsTr("Licence"); value: qsTr("Proprietary, all rights reserved") }
                }
                Text {
                    id: licenceBlock
                    objectName: "licenceText"
                    width: parent.width; wrapMode: Text.Wrap
                    text: page.settings.licence.trim()
                    textFormat: Text.PlainText
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
                Item {
                    id: noticesHead
                    width: parent.width; height: Theme.sizeControl
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Third-party notices (each component keeps its own licence)")
                        font: Theme.fontBodyDefault; color: Theme.textPrimary
                    }
                    DLButton {
                        objectName: "copyNotices"
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Copy notices"); iconName: "copy"
                        onClicked: { page.settings.copyText(page.settings.notices); page.copied(qsTr("Notices")) }
                    }
                }
                Rectangle {
                    objectName: "noticesFrame"
                    width: parent.width; height: page.noticesHeight
                    color: Theme.bgApp
                    radius: Theme.radiusMd
                    border.width: Theme.borderWidth
                    border.color: Theme.borderDefault
                    Flickable {
                        id: noticesFlick
                        objectName: "noticesFlick"
                        anchors.fill: parent; anchors.margins: Theme.space12
                        contentWidth: width; contentHeight: notices.contentHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        T.ScrollBar.vertical: T.ScrollBar {
                            contentItem: Rectangle {
                                implicitWidth: Theme.space8
                                radius: Theme.radiusMd
                                color: Theme.textTertiary
                                opacity: parent.pressed ? 1 : 0.6
                            }
                        }
                        // Plain text: the file is long and must never be read as markup.
                        TextEdit {
                            id: notices
                            objectName: "noticesView"
                            width: noticesFlick.width - Theme.space16
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            text: page.settings.notices
                            textFormat: TextEdit.PlainText
                            font: Theme.fontMonoDefault; color: Theme.textPrimary
                        }
                    }
                }
            }
        }
    }
}
