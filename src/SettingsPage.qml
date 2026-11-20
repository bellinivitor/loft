import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Defaults, parked folders, updates, and whether every piece of Loft works.
ScrollView {
    id: page
    contentWidth: availableWidth
    clip: true

    function installed(rows) {
        var model = []
        for (var i = 0; i < rows.length; ++i)
            if (rows[i].installed)
                model.push({ text: rows[i].line + "  ·  " + rows[i].installed, value: rows[i].line })
        return model
    }

    function indexOf(model, value) {
        for (var i = 0; i < model.length; ++i)
            if (model[i].value === value)
                return i
        return -1
    }

    component Section: ColumnLayout {
        property string title: ""
        property string glyph: ""
        default property alias content: body.data
        Layout.fillWidth: true
        spacing: 12
        RowLayout {
            spacing: 10
            Label { text: glyph; font.pixelSize: 18; color: theme.accent }
            Label { text: title; font.pixelSize: 16; font.weight: Font.Bold; color: theme.textBright }
        }
        ColumnLayout {
            id: body
            Layout.fillWidth: true
            spacing: 10
        }
    }

    component Note: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: 12
        color: theme.textDim
    }

    ColumnLayout {
        width: page.availableWidth
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 28
            spacing: 30

            Section {
                title: qsTr("Defaults")
                glyph: win.g.star
                Note {
                    text: qsTr("Used by php, composer, node and npm in your terminal, and by every site you haven't isolated.")
                }
                GridLayout {
                    columns: 2
                    columnSpacing: 18
                    rowSpacing: 10
                    Label { text: "PHP"; color: theme.text; font.weight: Font.DemiBold }
                    ComboBox {
                        id: phpDefault
                        readonly property bool loftControl: true
                        Layout.preferredWidth: 240
                        textRole: "text"
                        valueRole: "value"
                        model: page.installed(backend.php)
                        enabled: model.length > 0
                        displayText: model.length > 0 ? currentText : qsTr("None installed")
                        currentIndex: page.indexOf(model, backend.settings.php)
                        onActivated: backend.makeDefault("php", currentValue)
                    }
                    Label { text: "Node"; color: theme.text; font.weight: Font.DemiBold }
                    ComboBox {
                        readonly property bool loftControl: true
                        Layout.preferredWidth: 240
                        textRole: "text"
                        valueRole: "value"
                        model: page.installed(backend.node)
                        enabled: model.length > 0
                        displayText: model.length > 0 ? currentText : qsTr("None installed")
                        currentIndex: page.indexOf(model, backend.settings.node)
                        onActivated: backend.makeDefault("node", currentValue)
                    }
                }
            }

            Section {
                title: qsTr("Parked folders")
                glyph: win.g.folder
                Note {
                    text: qsTr("Every folder inside a parked folder is a site, named after it: ~/Sites/blog opens at blog.%1. New folders show up on their own.").arg(backend.settings.tld)
                }
                Repeater {
                    model: backend.settings.parked
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 10
                        Label { text: win.g.folder; color: theme.textDim; font.pixelSize: 16 }
                        Label { Layout.fillWidth: true; text: modelData.shortPath; color: theme.text; elide: Text.ElideMiddle }
                        LoftButton {
                            compact: true
                            kind: "quiet"
                            glyph: win.g.trash
                            text: qsTr("Stop parking")
                            onClicked: backend.forget(modelData.path)
                        }
                    }
                }
                LoftButton {
                    glyph: win.g.plus
                    text: qsTr("Park a folder")
                    onClicked: backend.pickFolder("park", qsTr("Every folder inside becomes a site"))
                }
            }

            Section {
                title: qsTr("Updates")
                glyph: win.g.update
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Note {
                        text: qsTr("Check for new PHP and Node releases every day, with a notification when one is out.")
                    }
                    Switch {
                        readonly property bool loftControl: true
                        checked: backend.settings.checkUpdates
                        onToggled: backend.setCheckUpdates(checked)
                    }
                }
                RowLayout {
                    spacing: 12
                    LoftButton {
                        glyph: win.g.refresh
                        text: backend.catalogLoading ? qsTr("Checking…") : qsTr("Check now")
                        enabled: !backend.catalogLoading
                        onClicked: backend.checkUpdates()
                    }
                    Note {
                        text: backend.catalogFetched
                            ? qsTr("Last checked %1").arg(new Date(backend.catalogFetched).toLocaleString(Qt.locale(translator.language), Locale.ShortFormat))
                            : qsTr("Not checked yet")
                    }
                }
            }

            Section {
                title: qsTr("PHP build")
                glyph: win.g.php
                Note {
                    text: qsTr("Which extension set new PHP installs come with. Installed versions keep theirs until you update or reinstall them.")
                }
                ComboBox {
                    readonly property bool loftControl: true
                    Layout.preferredWidth: 560
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { text: qsTr("bulk: intl, opcache, sodium, imagick, redis, swoole…"), value: "bulk" },
                        { text: qsTr("common: lean, with pdo_sqlite and pdo_pgsql built in"), value: "common" }
                    ]
                    currentIndex: backend.settings.phpBuild === "common" ? 1 : 0
                    onActivated: backend.setPhpBuild(currentValue)
                }
            }

            Section {
                title: qsTr("Health")
                glyph: win.g.shield
                Repeater {
                    model: backend.checks
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 12
                        Label {
                            text: modelData.ok ? win.g.check : win.g.cross
                            font.pixelSize: 16
                            color: modelData.ok ? theme.green : modelData.id === "node" ? theme.yellow : theme.red
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Label { text: modelData.label; color: theme.text }
                            Label {
                                Layout.fillWidth: true
                                text: modelData.detail
                                font.pixelSize: 11
                                color: theme.textDim
                                elide: Text.ElideMiddle
                            }
                        }
                    }
                }
                Note {
                    visible: backend.checks.length === 0
                    text: backend.checking ? qsTr("Checking…") : ""
                }
                RowLayout {
                    spacing: 10
                    LoftButton {
                        kind: backend.setupNeeded ? "primary" : "ghost"
                        glyph: win.g.wrench
                        text: backend.settingUp ? qsTr("Setting up…") : qsTr("Run setup")
                        enabled: !backend.settingUp
                        onClicked: backend.runSetup()
                    }
                    LoftButton {
                        glyph: win.g.refresh
                        text: qsTr("Check again")
                        enabled: !backend.checking
                        onClicked: backend.runDoctor()
                    }
                    LoftButton {
                        glyph: win.g.restart
                        text: qsTr("Restart services")
                        onClicked: backend.restartServices()
                    }
                }
                Note {
                    text: qsTr("Settings live in %1; each PHP's php.ini is there too.").arg(backend.settings.configDir)
                }
            }
        }
    }
}
