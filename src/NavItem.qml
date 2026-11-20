import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One section in the rail down the left: an icon over its name, with a
// count badge when something there wants attention.
Item {
    id: item
    property string glyph: ""
    property string label: ""
    property int index: 0
    property int badge: 0
    readonly property bool current: win.page === index
    readonly property bool loftControl: true

    activeFocusOnTab: true
    implicitWidth: 84
    implicitHeight: 64
    Keys.onReturnPressed: win.page = index
    Keys.onSpacePressed: win.page = index

    Rectangle {
        anchors.fill: parent
        anchors.margins: 6
        radius: theme.radius
        color: item.current ? theme.selection : hover.hovered ? theme.surface : "transparent"
        border.width: item.activeFocus ? 2 : 0
        border.color: theme.accent

        Rectangle {
            visible: item.current
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 3
            height: parent.height - 16
            color: theme.accent
        }

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 2
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: item.glyph
                font.pixelSize: 22
                color: item.current ? theme.accent : theme.text
            }
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: item.label
                font.pixelSize: 11
                font.weight: item.current ? Font.DemiBold : Font.Normal
                color: item.current ? theme.textBright : theme.textDim
            }
        }

        Rectangle {
            visible: item.badge > 0
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 6
            width: Math.max(18, badgeText.implicitWidth + 8)
            height: 18
            radius: 9
            color: theme.accent
            Label {
                id: badgeText
                anchors.centerIn: parent
                text: item.badge
                font.pixelSize: 10
                font.weight: Font.Bold
                color: theme.accentForeground
            }
        }

        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: { win.page = item.index; item.forceActiveFocus() } }
    }
}
