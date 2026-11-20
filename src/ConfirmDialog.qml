import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asks before anything that removes something.
Popup {
    id: dialog
    property string title: ""
    property string message: ""
    property string confirmText: ""
    property var action: null

    function ask(title, message, confirmText, action) {
        dialog.title = title
        dialog.message = message
        dialog.confirmText = confirmText
        dialog.action = action
        open()
        cancelButton.forceActiveFocus()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(460, parent.width - 48)
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
        spacing: 14
        Label {
            text: dialog.title
            font.pixelSize: 17
            font.weight: Font.Bold
            color: theme.textBright
        }
        Label {
            Layout.fillWidth: true
            text: dialog.message
            wrapMode: Text.Wrap
            color: theme.text
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 10
            Item { Layout.fillWidth: true }
            LoftButton { id: cancelButton; text: qsTr("Cancel"); onClicked: dialog.close() }
            LoftButton {
                kind: "danger"
                glyph: win.g.trash
                text: dialog.confirmText
                onClicked: {
                    dialog.close()
                    if (dialog.action)
                        dialog.action()
                }
            }
        }
    }
}
