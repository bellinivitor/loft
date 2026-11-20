import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Sites, PHP, Node and Settings down a rail on the left; what's running
// across the top. Everything changes through the loft command, so the
// window and the terminal always agree.
ApplicationWindow {
    id: win
    width: 1160
    height: 760
    minimumWidth: 920
    minimumHeight: 560
    visible: true
    title: "Loft"
    color: theme.background
    font.family: theme.font
    font.pixelSize: 13

    Material.theme: theme.dark ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.background: theme.surface
    Material.foreground: theme.text
    Material.roundedScale: theme.radius

    property int page: 0
    property string selectedHost: ""
    property bool helpVisible: false
    property bool setupDismissed: false

    // Nerd Font icons (Material Design set), by what they stand for.
    readonly property var g: ({
        logo: String.fromCodePoint(0xF112B), sites: String.fromCodePoint(0xF059F),
        php: String.fromCodePoint(0xF031F), node: String.fromCodePoint(0xF0399),
        settings: String.fromCodePoint(0xF0493), lock: String.fromCodePoint(0xF033E),
        unlock: String.fromCodePoint(0xF033F), folder: String.fromCodePoint(0xF024B),
        link: String.fromCodePoint(0xF0339), open: String.fromCodePoint(0xF03CC),
        copy: String.fromCodePoint(0xF018F), proxy: String.fromCodePoint(0xF04E1),
        laravel: String.fromCodePoint(0xE73F), page: String.fromCodePoint(0xF0219),
        download: String.fromCodePoint(0xF01DA), update: String.fromCodePoint(0xF06B0),
        trash: String.fromCodePoint(0xF01B4), star: String.fromCodePoint(0xF04CE),
        file: String.fromCodePoint(0xF0214), restart: String.fromCodePoint(0xF0709),
        refresh: String.fromCodePoint(0xF0450), alert: String.fromCodePoint(0xF0026),
        check: String.fromCodePoint(0xF012C), cross: String.fromCodePoint(0xF0156),
        shield: String.fromCodePoint(0xF0565), wrench: String.fromCodePoint(0xF05B7),
        plus: String.fromCodePoint(0xF0415)
    })

    readonly property var selected: {
        var sites = backend.sites
        for (var i = 0; i < sites.length; ++i)
            if (sites[i].host === selectedHost)
                return sites[i]
        return sites.length > 0 ? sites[0] : null
    }

    readonly property bool dialogOpen: linkDialog.visible || confirmDialog.visible
    readonly property bool shortcutsOn: !dialogOpen && !helpVisible
    readonly property bool onSites: page === 0 && selected !== null
    // Space and Enter belong to a focused button, switch or menu; the
    // shortcuts only take them when the list or nothing has focus.
    readonly property bool controlFocused: activeFocusItem !== null && activeFocusItem.loftControl === true

    function selectedIndex() {
        var sites = backend.sites
        for (var i = 0; i < sites.length; ++i)
            if (selected && sites[i].host === selected.host)
                return i
        return -1
    }

    function move(step) {
        var sites = backend.sites
        if (sites.length === 0)
            return
        var index = Math.max(0, Math.min(sites.length - 1, selectedIndex() + step))
        selectedHost = sites[index].host
        sitesPage.focusList()
    }

    function copy(text, what) {
        backend.copy(text)
        toast.show(qsTr("Copied: %1").arg(what), false)
    }

    function confirmRemoveSite(site) {
        if (!site || site.source === "parked")
            return
        if (site.kind === "proxy")
            confirmDialog.ask(qsTr("Remove %1?").arg(site.host),
                              qsTr("%1 stops forwarding to %2. The server itself keeps running.").arg(site.host).arg(site.proxy),
                              qsTr("Remove proxy"), function() { backend.unproxy(site.name) })
        else
            confirmDialog.ask(qsTr("Unlink %1?").arg(site.host),
                              qsTr("%1 stops being served. The folder %2 isn't touched.").arg(site.host).arg(site.shortPath),
                              qsTr("Unlink"), function() { backend.unlink(site.name) })
    }

    function confirmUninstall(tool, row) {
        var label = tool === "php" ? "PHP" : "Node"
        confirmDialog.ask(qsTr("Remove %1 %2?").arg(label).arg(row.installed),
                          tool === "php" ? qsTr("Its binaries go; its php.ini stays, in case you install it again.")
                                         : qsTr("Packages installed globally with this version go with it."),
                          qsTr("Remove"), function() { backend.uninstall(tool, row.line) })
    }

    Connections {
        target: backend
        function onNotice(message, error) { toast.show(message, error) }
        function onFolderPicked(purpose, path) {
            if (purpose === "park")
                backend.park(path)
        }
    }

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: win.shortcutsOn; onActivated: Qt.quit() }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; enabled: win.shortcutsOn; onActivated: { backend.refresh(); backend.runDoctor() } }
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; enabled: win.shortcutsOn; onActivated: linkDialog.openFolder() }
    Shortcut { sequence: "X"; context: Qt.ApplicationShortcut; enabled: win.shortcutsOn; onActivated: linkDialog.openProxy() }
    Instantiator {
        model: 4
        delegate: Shortcut {
            sequence: String(index + 1)
            context: Qt.ApplicationShortcut
            enabled: win.shortcutsOn
            onActivated: win.page = index
        }
    }
    Shortcut { sequences: ["Down", "J"]; context: Qt.ApplicationShortcut; enabled: win.shortcutsOn && win.page === 0 && !win.controlFocused; onActivated: win.move(1) }
    Shortcut { sequences: ["Up", "K"]; context: Qt.ApplicationShortcut; enabled: win.shortcutsOn && win.page === 0 && !win.controlFocused; onActivated: win.move(-1) }
    Shortcut {
        sequences: ["Space", "O"]; context: Qt.ApplicationShortcut
        enabled: win.shortcutsOn && win.onSites && !win.controlFocused
        onActivated: backend.openUrl(win.selected.url)
    }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: win.shortcutsOn && win.onSites; onActivated: backend.setSecure(win.selected.name, !win.selected.secure) }
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; enabled: win.shortcutsOn && win.onSites; onActivated: win.copy(win.selected.url, qsTr("address")) }
    Shortcut { sequence: "F"; context: Qt.ApplicationShortcut; enabled: win.shortcutsOn && win.onSites && win.selected.kind !== "proxy"; onActivated: backend.openFolder(win.selected.path) }
    Shortcut { sequences: ["D", "Delete"]; context: Qt.ApplicationShortcut; enabled: win.shortcutsOn && win.onSites; onActivated: win.confirmRemoveSite(win.selected) }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // What's running, and the one action that's always there.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 60
                color: theme.sunken
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: theme.selection }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 22
                    anchors.rightMargin: 16
                    spacing: 14

                    Label { text: win.g.logo; font.pixelSize: 24; color: theme.accent }
                    Label { text: "loft"; font.pixelSize: 18; font.weight: Font.Bold; color: theme.textBright }

                    Rectangle {
                        Layout.leftMargin: 10
                        implicitHeight: 30
                        implicitWidth: servicesRow.implicitWidth + 24
                        radius: theme.radius
                        color: theme.surface
                        border.color: theme.selection
                        RowLayout {
                            id: servicesRow
                            anchors.centerIn: parent
                            spacing: 16
                            Repeater {
                                model: [
                                    { key: "nginx", label: "nginx" },
                                    { key: "dns", label: "DNS ." + backend.settings.tld },
                                    { key: "php", label: backend.settings.php ? "PHP " + backend.settings.php : qsTr("no PHP") }
                                ]
                                delegate: RowLayout {
                                    required property var modelData
                                    readonly property string unitState: backend.services[modelData.key] || ""
                                    spacing: 6
                                    Rectangle {
                                        width: 8
                                        height: 8
                                        radius: 4
                                        color: unitState === "active" ? theme.green
                                             : unitState === "activating" || unitState === "reloading" ? theme.yellow
                                             : unitState === "failed" ? theme.red : theme.border
                                    }
                                    Label { text: modelData.label; font.pixelSize: 12; color: theme.text }
                                }
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }

                    BusyIndicator {
                        visible: backend.working
                        running: visible
                        implicitWidth: 26
                        implicitHeight: 26
                    }
                    LoftButton {
                        visible: backend.updateCount > 0
                        kind: "ghost"
                        glyph: win.g.update
                        text: qsTr("%n update(s)", "", backend.updateCount)
                        onClicked: win.page = backend.php.some(function(row) { return row.update }) ? 1 : 2
                    }
                    LoftButton {
                        kind: "primary"
                        glyph: win.g.link
                        text: qsTr("Link folder")
                        onClicked: linkDialog.openFolder()
                    }
                }
            }

            // Setup isn't finished: say what's missing and fix it in one click.
            Rectangle {
                Layout.fillWidth: true
                visible: backend.setupNeeded && !win.setupDismissed
                implicitHeight: bannerRow.implicitHeight + 20
                color: theme.tint(theme.accent, 0.12)
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: theme.tint(theme.accent, 0.4) }
                RowLayout {
                    id: bannerRow
                    anchors.fill: parent
                    anchors.leftMargin: 22
                    anchors.rightMargin: 16
                    spacing: 12
                    Label { text: win.g.wrench; font.pixelSize: 18; color: theme.accent }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: theme.textBright
                        text: qsTr("Loft isn't fully set up yet. Setup does every missing piece and asks for your password once.")
                    }
                    LoftButton {
                        kind: "quiet"
                        compact: true
                        text: qsTr("Details")
                        onClicked: win.page = 3
                    }
                    LoftButton {
                        kind: "primary"
                        compact: true
                        text: backend.settingUp ? qsTr("Setting up…") : qsTr("Set up now")
                        enabled: !backend.settingUp
                        onClicked: backend.runSetup()
                    }
                    LoftButton { kind: "quiet"; compact: true; glyph: win.g.cross; onClicked: win.setupDismissed = true }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Rectangle {
                    Layout.fillHeight: true
                    implicitWidth: 92
                    color: theme.sunken
                    Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: theme.selection }
                    ColumnLayout {
                        anchors.top: parent.top
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.topMargin: 8
                        spacing: 2
                        NavItem { index: 0; glyph: win.g.sites; label: qsTr("Sites") }
                        NavItem { index: 1; glyph: win.g.php; label: "PHP"; badge: backend.php.filter(function(row) { return row.update }).length }
                        NavItem { index: 2; glyph: win.g.node; label: "Node"; badge: backend.node.filter(function(row) { return row.update }).length }
                        NavItem { index: 3; glyph: win.g.settings; label: qsTr("Settings"); badge: backend.setupNeeded ? 1 : 0 }
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: win.page
                    SitesPage { id: sitesPage }
                    RuntimePage { tool: "php" }
                    RuntimePage { tool: "node" }
                    SettingsPage {}
                }
            }
        }

        Rectangle {
            id: toast
            property bool error: false
            function show(message, isError) {
                toastText.text = message
                error = isError
                opacity = 1
                toastTimer.restart()
            }
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 40
            width: Math.min(parent.width - 80, toastText.implicitWidth + 36)
            height: toastText.implicitHeight + 22
            radius: theme.radius
            color: error ? theme.tint(theme.red, 0.95) : theme.selection
            border.color: error ? theme.red : theme.border
            opacity: 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 180 } }
            Label {
                id: toastText
                anchors.centerIn: parent
                width: Math.min(implicitWidth, win.width - 116)
                wrapMode: Text.Wrap
                color: toast.error ? "white" : theme.textBright
            }
            TapHandler { onTapped: toast.opacity = 0 }
            Timer { id: toastTimer; interval: toast.error ? 9000 : 2600; onTriggered: toast.opacity = 0 }
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            onClose: win.helpVisible = false
        }
    }

    LinkDialog { id: linkDialog }
    ConfirmDialog { id: confirmDialog }
}
