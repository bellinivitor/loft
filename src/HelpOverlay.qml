import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Every shortcut, one press of ? away.
Rectangle {
    id: overlay
    signal close()
    color: theme.tint(theme.sunken, 0.85)

    TapHandler { onTapped: overlay.close() }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(540, parent.width - 48)
        height: list.implicitHeight + 48
        radius: theme.radius
        color: theme.surface
        border.color: theme.selection

        ColumnLayout {
            id: list
            anchors.fill: parent
            anchors.margins: 24
            spacing: 7

            Label {
                text: qsTr("Shortcuts")
                font.pixelSize: 17
                font.weight: Font.Bold
                color: theme.textBright
                Layout.bottomMargin: 8
            }

            Repeater {
                model: [
                    ["1  2  3  4", qsTr("Sites, PHP, Node, Settings")],
                    ["Tab  ⇧Tab", qsTr("Move between the list and every control")],
                    ["↑↓  J K", qsTr("Select a site")],
                    ["N", qsTr("Link a folder")],
                    ["X", qsTr("Add a proxy")],
                    ["Space  O", qsTr("Open the site in the browser")],
                    ["S", qsTr("HTTPS on or off")],
                    ["C", qsTr("Copy the address")],
                    ["F", qsTr("Open the folder")],
                    ["D", qsTr("Unlink or remove")],
                    ["R", qsTr("Refresh")],
                    ["?", qsTr("Show shortcuts")],
                    ["Q", qsTr("Quit")]
                ]
                delegate: RowLayout {
                    Layout.fillWidth: true
                    spacing: 16
                    Label {
                        text: modelData[0]
                        Layout.preferredWidth: 110
                        font.weight: Font.DemiBold
                        color: theme.accent
                    }
                    Label {
                        text: modelData[1]
                        Layout.fillWidth: true
                        color: theme.text
                    }
                }
            }
        }
    }
}
