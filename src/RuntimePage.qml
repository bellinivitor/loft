import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every PHP (or Node) line: installed or not, the default or not, how many
// sites use it, and whether a newer patch release is out.
ScrollView {
    id: page
    property string tool: "php"
    readonly property bool php: tool === "php"
    readonly property var rows: php ? backend.php : backend.node
    contentWidth: availableWidth
    clip: true

    ColumnLayout {
        width: page.availableWidth
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 28
            spacing: 18

            RowLayout {
                Layout.fillWidth: true
                spacing: 14
                Label {
                    text: page.php ? win.g.php : win.g.node
                    font.pixelSize: 30
                    color: page.php ? theme.magenta : theme.green
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: page.php ? "PHP" : "Node.js"
                        font.pixelSize: 22
                        font.weight: Font.Bold
                        color: theme.textBright
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: theme.textDim
                        text: page.php
                            ? qsTr("Static builds from static-php.dev (%1 extension set), each with its own PHP-FPM. The default is what php runs in your terminal outside isolated sites.").arg(backend.settings.phpBuild)
                            : qsTr("Official builds from nodejs.org, checked against their published checksums. The default is what node, npm and npx run outside isolated sites.")
                    }
                }
                LoftButton {
                    glyph: win.g.refresh
                    text: backend.catalogLoading ? qsTr("Checking…") : qsTr("Check for updates")
                    enabled: !backend.catalogLoading
                    onClicked: backend.checkUpdates()
                }
            }

            Label {
                visible: page.rows.length === 0
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: theme.textDim
                text: backend.catalogLoading ? qsTr("Fetching the list of versions…")
                                             : qsTr("No versions to show. Check your connection and try Check for updates.")
            }

            Repeater {
                model: page.rows
                delegate: Rectangle {
                    id: card
                    required property var modelData
                    readonly property var row: modelData
                    readonly property bool installed: row.installed !== ""
                    Layout.fillWidth: true
                    implicitHeight: cardRow.implicitHeight + 28
                    radius: theme.radius
                    color: row.isDefault ? theme.tint(theme.accent, 0.08) : theme.surface
                    border.color: row.isDefault ? theme.tint(theme.accent, 0.6) : theme.selection

                    RowLayout {
                        id: cardRow
                        anchors.fill: parent
                        anchors.leftMargin: 18
                        anchors.rightMargin: 14
                        anchors.topMargin: 14
                        anchors.bottomMargin: 14
                        spacing: 16

                        Label {
                            text: card.row.line
                            font.pixelSize: 22
                            font.weight: Font.Bold
                            color: card.installed ? theme.textBright : theme.textDim
                            Layout.preferredWidth: 58
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            RowLayout {
                                spacing: 8
                                Label {
                                    text: card.installed ? card.row.installed : qsTr("Not installed")
                                    font.weight: Font.DemiBold
                                    color: card.installed ? theme.text : theme.textDim
                                }
                                Rectangle {
                                    visible: card.row.isDefault
                                    implicitWidth: defaultLabel.implicitWidth + 12
                                    implicitHeight: 20
                                    radius: Math.min(4, theme.radius)
                                    color: theme.accent
                                    Label {
                                        id: defaultLabel
                                        anchors.centerIn: parent
                                        text: qsTr("Default")
                                        font.pixelSize: 11
                                        font.weight: Font.Bold
                                        color: theme.accentForeground
                                    }
                                }
                                Rectangle {
                                    visible: card.row.lts !== ""
                                    implicitWidth: ltsLabel.implicitWidth + 12
                                    implicitHeight: 20
                                    radius: Math.min(4, theme.radius)
                                    color: theme.tint(theme.green, 0.15)
                                    Label {
                                        id: ltsLabel
                                        anchors.centerIn: parent
                                        text: "LTS " + card.row.lts
                                        font.pixelSize: 11
                                        color: theme.green
                                    }
                                }
                                Rectangle {
                                    visible: card.row.update
                                    implicitWidth: updateLabel.implicitWidth + 12
                                    implicitHeight: 20
                                    radius: Math.min(4, theme.radius)
                                    color: theme.tint(theme.yellow, 0.18)
                                    Label {
                                        id: updateLabel
                                        anchors.centerIn: parent
                                        text: qsTr("%1 available").arg(card.row.latest)
                                        font.pixelSize: 11
                                        color: theme.yellow
                                    }
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                font.pixelSize: 12
                                color: theme.textDim
                                elide: Text.ElideRight
                                text: card.row.busy ? qsTr("Downloading and checking…")
                                    : card.installed && card.row.sites === 0 ? qsTr("No site uses it")
                                    : card.installed ? qsTr("Used by %n site(s)", "", card.row.sites)
                                    : card.row.latest ? qsTr("%1 ready to install").arg(card.row.latest) : ""
                            }
                            ProgressBar {
                                visible: card.row.busy
                                Layout.fillWidth: true
                                Layout.maximumWidth: 360
                                from: 0
                                to: 1
                                value: card.row.progress
                                indeterminate: card.row.progress <= 0
                            }
                        }

                        LoftButton {
                            visible: !card.installed && !card.row.busy
                            kind: "primary"
                            glyph: win.g.download
                            text: qsTr("Install")
                            onClicked: backend.install(page.tool, card.row.line)
                        }
                        LoftButton {
                            visible: card.row.update && !card.row.busy
                            kind: "primary"
                            glyph: win.g.update
                            text: qsTr("Update")
                            onClicked: backend.install(page.tool, card.row.line)
                        }
                        LoftButton {
                            visible: card.installed && !card.row.isDefault && !card.row.busy
                            glyph: win.g.star
                            text: qsTr("Make default")
                            onClicked: backend.makeDefault(page.tool, card.row.line)
                        }
                        LoftButton {
                            visible: card.installed && !card.row.busy
                            glyph: win.g.folder
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Open the binary's folder")
                            onClicked: backend.openFolder(card.row.path)
                        }
                        LoftButton {
                            visible: page.php && card.installed && !card.row.busy
                            glyph: win.g.file
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Edit php.ini")
                            onClicked: backend.openPhpIni(card.row.line)
                        }
                        LoftButton {
                            visible: page.php && card.installed && !card.row.busy
                            glyph: win.g.restart
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Restart PHP-FPM (after editing php.ini)")
                            onClicked: backend.restartPhp(card.row.line)
                        }
                        LoftButton {
                            visible: card.installed && !card.row.busy
                            kind: "quiet"
                            glyph: win.g.trash
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Remove")
                            onClicked: win.confirmUninstall(page.tool, card.row)
                        }
                    }
                }
            }
        }
    }
}
