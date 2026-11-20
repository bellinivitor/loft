import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The sites down the side, the selected one beside them: where it lives,
// whether it's served over HTTPS, and which PHP and Node it runs.
Item {
    id: page
    readonly property var site: win.selected

    function focusList() {
        var index = win.selectedIndex()
        var item = list.itemAt(index)
        if (item)
            item.forceActiveFocus(Qt.TabFocusReason)
    }

    // Version menus: the default first, then each installed version.
    function versionModel(rows, fallback) {
        var model = [{ text: qsTr("Default (%1)").arg(fallback || "—"), value: "" }]
        for (var i = 0; i < rows.length; ++i)
            if (rows[i].installed)
                model.push({ text: rows[i].line + "  ·  " + rows[i].installed, value: rows[i].line })
        return model
    }

    function indexOfValue(model, value) {
        for (var i = 0; i < model.length; ++i)
            if (model[i].value === value)
                return i
        return 0
    }

    // No sites yet: say what a site is and offer the two ways to make one.
    ColumnLayout {
        anchors.centerIn: parent
        visible: backend.sites.length === 0
        spacing: 14
        width: Math.min(460, parent.width - 48)

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: win.g.sites
            font.pixelSize: 54
            color: theme.accent
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("No sites yet")
            font.pixelSize: 20
            font.weight: Font.Bold
            color: theme.textBright
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: theme.textDim
            text: qsTr("Link a project folder and it opens at folder-name.%1, served by nginx with the PHP and Node you pick. In a terminal: cd my-project && loft link").arg(backend.settings.tld)
        }
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 6
            spacing: 10
            LoftButton { kind: "primary"; glyph: win.g.link; text: qsTr("Link a folder"); onClicked: linkDialog.openFolder() }
            LoftButton { glyph: win.g.folder; text: qsTr("Park a folder"); onClicked: backend.pickFolder("park", qsTr("Every folder inside becomes a site")) }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0
        visible: backend.sites.length > 0

        // The list.
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 340
            color: theme.sunken
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: theme.selection }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8

                Label {
                    Layout.leftMargin: 6
                    Layout.topMargin: 4
                    text: qsTr("%n site(s)", "", backend.sites.length)
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    color: theme.textDim
                }

                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true
                    Column {
                        width: parent.width
                        spacing: 2
                        Repeater {
                            id: list
                            model: backend.sites
                            delegate: SiteRow {
                                width: parent.width
                                site: modelData
                            }
                        }
                    }
                }
            }
        }

        // The selected site.
        ScrollView {
            id: detailScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: detailScroll.availableWidth
                spacing: 0
                visible: page.site !== null

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: 28
                    spacing: 22

                    // Name, address, and the actions that work on it as a whole.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Label {
                                Layout.fillWidth: true
                                text: page.site ? page.site.host : ""
                                font.pixelSize: 24
                                font.weight: Font.Bold
                                color: theme.textBright
                                elide: Text.ElideRight
                            }
                            Label {
                                text: page.site ? page.site.url : ""
                                color: urlHover.hovered ? theme.accent : theme.blue
                                font.underline: urlHover.hovered
                                HoverHandler { id: urlHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: backend.openUrl(page.site.url) }
                            }
                        }
                        LoftButton {
                            id: openButton
                            kind: "primary"
                            glyph: win.g.open
                            text: qsTr("Open")
                            onClicked: backend.openUrl(page.site.url)
                        }
                        LoftButton {
                            glyph: win.g.copy
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Copy the address (C)")
                            onClicked: win.copy(page.site.url, qsTr("address"))
                        }
                        LoftButton {
                            visible: page.site !== null && page.site.kind !== "proxy"
                            glyph: win.g.folder
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Open the folder (F)")
                            onClicked: backend.openFolder(page.site.path)
                        }
                    }

                    // Something's off: say what and how to fix it.
                    Rectangle {
                        Layout.fillWidth: true
                        visible: page.site !== null && (page.site.missing || (!page.site.phpInstalled && page.site.kind !== "static" && page.site.kind !== "proxy"))
                        implicitHeight: problemRow.implicitHeight + 24
                        radius: theme.radius
                        color: theme.tint(theme.red, 0.12)
                        border.color: theme.tint(theme.red, 0.5)
                        RowLayout {
                            id: problemRow
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 12
                            Label { text: win.g.alert; font.pixelSize: 18; color: theme.red }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                color: theme.textBright
                                text: !page.site ? ""
                                    : page.site.missing ? qsTr("The folder %1 is gone. Unlink the site, or put the folder back.").arg(page.site.shortPath)
                                    : page.site.phpVersion === "" ? qsTr("No PHP is installed, so this site can't run yet.")
                                    : qsTr("This site runs PHP %1, which isn't installed.").arg(page.site.phpVersion)
                            }
                            LoftButton {
                                visible: page.site !== null && !page.site.missing
                                kind: "primary"
                                compact: true
                                glyph: win.g.download
                                text: qsTr("Install PHP %1").arg(page.site && page.site.phpVersion ? page.site.phpVersion : "8.5")
                                onClicked: { backend.install("php", page.site.phpVersion || "8.5"); win.page = 1 }
                            }
                        }
                    }

                    // What it is.
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 24
                        rowSpacing: 10

                        Label { text: qsTr("Folder"); color: theme.textDim; visible: page.site && page.site.kind !== "proxy" }
                        Label {
                            Layout.fillWidth: true
                            visible: page.site && page.site.kind !== "proxy"
                            text: page.site ? page.site.shortPath : ""
                            color: theme.text
                            elide: Text.ElideMiddle
                        }
                        Label { text: qsTr("Forwards to"); color: theme.textDim; visible: page.site && page.site.kind === "proxy" }
                        Label {
                            Layout.fillWidth: true
                            visible: page.site && page.site.kind === "proxy"
                            text: page.site ? page.site.proxy : ""
                            color: theme.text
                        }
                        Label { text: qsTr("Kind"); color: theme.textDim }
                        Label {
                            color: theme.text
                            text: !page.site ? ""
                                : page.site.kind === "laravel" ? qsTr("Laravel (serves public/)")
                                : page.site.kind === "php" ? qsTr("PHP")
                                : page.site.kind === "proxy" ? qsTr("Proxy to a server you run")
                                : qsTr("Static files")
                        }
                        Label { text: qsTr("Comes from"); color: theme.textDim }
                        Label {
                            Layout.fillWidth: true
                            color: theme.text
                            wrapMode: Text.Wrap
                            text: !page.site ? ""
                                : page.site.source === "parked" ? qsTr("The parked folder %1").arg(page.site.parkedIn)
                                : page.site.source === "proxy" ? qsTr("loft proxy")
                                : qsTr("loft link")
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: theme.selection }

                    // HTTPS.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14
                        Label { text: page.site && page.site.secure ? win.g.lock : win.g.unlock; font.pixelSize: 22; color: page.site && page.site.secure ? theme.green : theme.textDim }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label { text: qsTr("HTTPS"); font.weight: Font.DemiBold; color: theme.textBright }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                font.pixelSize: 12
                                color: theme.textDim
                                text: page.site && page.site.secure
                                    ? qsTr("Served with a certificate from Loft's own authority, which your browsers trust. http:// redirects here.")
                                    : qsTr("Plain HTTP. Turn it on for a certificate your browsers trust (S).")
                            }
                        }
                        Switch {
                            id: secureSwitch
                            readonly property bool loftControl: true
                            checked: page.site ? page.site.secure : false
                            onToggled: backend.setSecure(page.site.name, checked)
                        }
                    }

                    // PHP and Node.
                    GridLayout {
                        Layout.fillWidth: true
                        visible: page.site !== null && page.site.kind !== "proxy"
                        columns: 3
                        columnSpacing: 14
                        rowSpacing: 12

                        Label { text: win.g.php; font.pixelSize: 22; color: theme.magenta; visible: page.site && page.site.kind !== "static" }
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: page.site && page.site.kind !== "static"
                            spacing: 2
                            Label { text: qsTr("PHP"); font.weight: Font.DemiBold; color: theme.textBright }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                font.pixelSize: 12
                                color: theme.textDim
                                text: page.site && page.site.php
                                    ? qsTr("Isolated: the site and the php command in its folder use %1.").arg(page.site.php)
                                    : qsTr("Follows the default. Pick a version to isolate this site.")
                            }
                        }
                        ComboBox {
                            id: phpCombo
                            readonly property bool loftControl: true
                            visible: page.site && page.site.kind !== "static"
                            Layout.preferredWidth: 200
                            textRole: "text"
                            valueRole: "value"
                            model: page.versionModel(backend.php, backend.settings.php)
                            currentIndex: page.site ? page.indexOfValue(model, page.site.php) : 0
                            onActivated: backend.isolate(page.site.name, "php", currentValue)
                        }

                        Label { text: win.g.node; font.pixelSize: 22; color: theme.green }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label { text: qsTr("Node"); font.weight: Font.DemiBold; color: theme.textBright }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                font.pixelSize: 12
                                color: theme.textDim
                                text: page.site && page.site.node
                                    ? qsTr("Isolated: node, npm and npx in its folder use %1.").arg(page.site.node)
                                    : qsTr("Follows the default, or the project's .nvmrc when that version is installed.")
                            }
                        }
                        ComboBox {
                            readonly property bool loftControl: true
                            Layout.preferredWidth: 200
                            textRole: "text"
                            valueRole: "value"
                            model: page.versionModel(backend.node, backend.settings.node)
                            currentIndex: page.site ? page.indexOfValue(model, page.site.node) : 0
                            onActivated: backend.isolate(page.site.name, "node", currentValue)
                        }
                    }

                    // nginx: Loft's generated config, or your own copy of it.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14
                        Label { text: win.g.cog; font.pixelSize: 22; color: page.site && page.site.customNginx ? theme.yellow : theme.cyan }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label { text: qsTr("nginx config"); font.weight: Font.DemiBold; color: theme.textBright }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                font.pixelSize: 12
                                color: theme.textDim
                                text: page.site && page.site.customNginx
                                    ? qsTr("Yours, in %1. nginx reloads when you save it; HTTPS and PHP changes made here don't touch it until you go back to Loft's.").arg(page.site.nginxFile)
                                    : qsTr("Generated by Loft. Editing makes it yours: nginx reloads each time you save.")
                            }
                        }
                        LoftButton {
                            visible: page.site !== null && page.site.customNginx
                            compact: true
                            kind: "quiet"
                            glyph: win.g.restart
                            text: qsTr("Back to Loft's")
                            onClicked: backend.resetNginx(page.site.name)
                        }
                        LoftButton {
                            compact: true
                            glyph: win.g.edit
                            text: qsTr("Edit")
                            onClicked: backend.editNginx(page.site.name)
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: theme.selection }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Label {
                            Layout.fillWidth: true
                            visible: page.site !== null && page.site.source === "parked"
                            wrapMode: Text.Wrap
                            font.pixelSize: 12
                            color: theme.textDim
                            text: qsTr("Parked sites come and go with their folders. To drop this one, move the folder out of %1.").arg(page.site ? page.site.parkedIn : "")
                        }
                        Item { Layout.fillWidth: true; visible: page.site !== null && page.site.source !== "parked" }
                        LoftButton {
                            visible: page.site !== null && page.site.source !== "parked"
                            kind: "danger"
                            glyph: win.g.trash
                            text: page.site && page.site.kind === "proxy" ? qsTr("Remove proxy") : qsTr("Unlink")
                            onClicked: win.confirmRemoveSite(page.site)
                        }
                    }
                }
            }
        }
    }
}
