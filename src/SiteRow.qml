import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One site in the list. It takes keyboard focus with Tab, and focusing it
// selects it, so Tab and the arrows walk the same list.
Item {
    id: row
    property var site: ({})
    readonly property bool selected: win.selectedHost === site.host
    readonly property bool isSiteRow: true

    activeFocusOnTab: true
    implicitHeight: 58
    onActiveFocusChanged: if (activeFocus) win.selectedHost = site.host
    Keys.onReturnPressed: backend.openUrl(site.url)
    Keys.onEnterPressed: backend.openUrl(site.url)

    Rectangle {
        anchors.fill: parent
        radius: theme.radius
        color: row.selected ? theme.selection : hover.hovered ? theme.surface : "transparent"
        border.width: row.activeFocus ? 2 : 0
        border.color: theme.accent

        Rectangle {
            visible: row.selected
            width: 3
            height: parent.height
            color: theme.accent
        }

        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: row.forceActiveFocus() }
        TapHandler { acceptedButtons: Qt.LeftButton; onDoubleTapped: backend.openUrl(row.site.url) }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 12
            spacing: 10

            Label {
                text: row.site.kind === "proxy" ? win.g.proxy
                    : row.site.kind === "laravel" ? win.g.laravel
                    : row.site.kind === "php" ? win.g.php : win.g.page
                font.pixelSize: 20
                color: row.site.missing ? theme.red : row.selected ? theme.accent : theme.textDim
                Layout.preferredWidth: 24
                horizontalAlignment: Text.AlignHCenter
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    Layout.fillWidth: true
                    text: row.site.host
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    color: row.selected ? theme.textBright : theme.text
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: row.site.kind === "proxy" ? "→ " + row.site.proxy : row.site.shortPath
                    font.pixelSize: 11
                    color: row.site.missing ? theme.red : theme.textDim
                    elide: Text.ElideMiddle
                }
            }

            Label {
                visible: row.site.kind !== "proxy" && row.site.kind !== "static"
                text: row.site.phpVersion || "—"
                font.pixelSize: 11
                font.weight: row.site.php ? Font.Bold : Font.Normal
                color: !row.site.phpInstalled ? theme.red : row.site.php ? theme.accent : theme.textDim
            }

            Label {
                text: row.site.secure ? win.g.lock : win.g.unlock
                font.pixelSize: 15
                color: row.site.secure ? theme.green : theme.border
            }
        }
    }
}
