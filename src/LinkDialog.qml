import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Adds a site: a folder served by nginx, or a name that forwards to a
// server you run yourself (Vite, a Node app).
Popup {
    id: dialog
    property bool proxyMode: false
    property string folder: ""

    function openFolder(path) {
        proxyMode = false
        folder = path || ""
        nameField.text = ""
        secureBox.checked = true
        phpBox.currentIndex = 0
        open()
        if (folder === "")
            backend.pickFolder("link", qsTr("Pick a project folder"))
        folderField.forceActiveFocus()
    }

    function openProxy() {
        proxyMode = true
        proxyName.text = ""
        proxyUrl.text = "http://127.0.0.1:3000"
        proxySecure.checked = true
        open()
        proxyName.forceActiveFocus()
    }

    function accept() {
        if (proxyMode) {
            if (proxyName.text.trim() === "" || proxyUrl.text.trim() === "")
                return
            backend.proxy(proxyName.text, proxyUrl.text, proxySecure.checked)
        } else {
            if (folder.trim() === "")
                return
            backend.link(folder, nameField.text, secureBox.checked, phpBox.currentValue)
        }
        close()
    }

    readonly property string suggested: folder !== "" ? backend.suggestName(folder) : ""

    Connections {
        target: backend
        function onFolderPicked(purpose, path) {
            if (purpose === "link" && dialog.visible)
                dialog.folder = path
        }
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(620, parent.width - 48)
    modal: true
    focus: true
    padding: 24
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    Overlay.modal: Rectangle { color: theme.tint(theme.sunken, 0.75) }

    background: Rectangle {
        radius: theme.radius
        color: theme.surface
        border.color: theme.selection
    }

    contentItem: ColumnLayout {
        spacing: 16
        Keys.onReturnPressed: dialog.accept()
        Keys.onEnterPressed: dialog.accept()

        RowLayout {
            spacing: 6
            LoftButton {
                compact: true
                kind: dialog.proxyMode ? "quiet" : "primary"
                glyph: win.g.folder
                text: qsTr("Folder")
                onClicked: dialog.proxyMode = false
            }
            LoftButton {
                compact: true
                kind: dialog.proxyMode ? "primary" : "quiet"
                glyph: win.g.proxy
                text: qsTr("Proxy")
                onClicked: dialog.proxyMode = true
            }
        }

        // A folder.
        ColumnLayout {
            visible: !dialog.proxyMode
            Layout.fillWidth: true
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextField {
                    id: folderField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Project folder")
                    text: dialog.folder
                    onTextEdited: dialog.folder = text
                }
                LoftButton {
                    glyph: win.g.folder
                    text: qsTr("Choose…")
                    onClicked: backend.pickFolder("link", qsTr("Pick a project folder"))
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                TextField {
                    id: nameField
                    Layout.fillWidth: true
                    placeholderText: dialog.suggested !== "" ? qsTr("Name: %1").arg(dialog.suggested) : qsTr("Name")
                }
                Label { text: "." + backend.settings.tld; color: theme.textDim; font.pixelSize: 15 }
            }
            RowLayout {
                spacing: 12
                Label { text: "PHP"; color: theme.text }
                ComboBox {
                    id: phpBox
                    readonly property bool loftControl: true
                    Layout.preferredWidth: 220
                    textRole: "text"
                    valueRole: "value"
                    model: {
                        var model = [{ text: qsTr("Default (%1)").arg(backend.settings.php || "—"), value: "" }]
                        for (var i = 0; i < backend.php.length; ++i)
                            if (backend.php[i].installed)
                                model.push({ text: backend.php[i].line, value: backend.php[i].line })
                        return model
                    }
                }
                CheckBox {
                    id: secureBox
                    text: qsTr("HTTPS")
                    checked: true
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: 12
                color: theme.textDim
                text: qsTr("It opens at %1. Same as running loft link in that folder.")
                    .arg((secureBox.checked ? "https://" : "http://") + (nameField.text.trim() || dialog.suggested || "…") + "." + backend.settings.tld)
            }
        }

        // A proxy.
        ColumnLayout {
            visible: dialog.proxyMode
            Layout.fillWidth: true
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                TextField {
                    id: proxyName
                    Layout.fillWidth: true
                    placeholderText: qsTr("Name")
                }
                Label { text: "." + backend.settings.tld; color: theme.textDim; font.pixelSize: 15 }
            }
            TextField {
                id: proxyUrl
                Layout.fillWidth: true
                placeholderText: qsTr("Forward to")
            }
            CheckBox {
                id: proxySecure
                text: qsTr("HTTPS")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: 12
                color: theme.textDim
                text: qsTr("For servers you start yourself, like Vite or a Node app. WebSockets pass through.")
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 10
            Item { Layout.fillWidth: true }
            LoftButton { text: qsTr("Cancel"); onClicked: dialog.close() }
            LoftButton {
                kind: "primary"
                glyph: dialog.proxyMode ? win.g.proxy : win.g.link
                text: dialog.proxyMode ? qsTr("Add proxy") : qsTr("Link")
                enabled: dialog.proxyMode ? proxyName.text.trim() !== "" && proxyUrl.text.trim() !== "" : dialog.folder.trim() !== ""
                onClicked: dialog.accept()
            }
        }
    }
}
