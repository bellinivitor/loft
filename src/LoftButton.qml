import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every button in Loft: an optional Nerd Font icon, a label, and a ring in
// the accent when it has keyboard focus, so Tab always shows where it is.
//   kind: "primary" fills with the accent, "ghost" sits on a surface,
//         "danger" is tinted red, "quiet" only shows on hover.
Button {
    id: control
    property string kind: "ghost"
    property string glyph: ""
    property bool compact: false
    // Lets the window's single-key shortcuts leave Space and Enter to it.
    readonly property bool loftControl: true

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    font.family: theme.font
    font.pixelSize: compact ? 12 : 13
    font.weight: kind === "primary" ? Font.DemiBold : Font.Medium
    topPadding: 0
    bottomPadding: 0
    // Material keeps a margin around its buttons for the ripple; these sit
    // flush with the other controls instead.
    topInset: 0
    bottomInset: 0
    leftInset: 0
    rightInset: 0
    leftPadding: text === "" ? 0 : compact ? 10 : 14
    rightPadding: text === "" ? 0 : compact ? 10 : 14
    implicitHeight: compact ? 28 : 36
    implicitWidth: text === "" ? implicitHeight : implicitContentWidth + leftPadding + rightPadding

    readonly property color ink: !enabled ? theme.textDim
        : kind === "primary" ? theme.accentForeground
        : kind === "danger" ? theme.red
        : hovered || visualFocus ? theme.textBright : theme.text

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        RowLayout {
            id: row
            anchors.centerIn: parent
            spacing: 8
            Label {
                visible: control.glyph !== ""
                text: control.glyph
                font.family: theme.font
                font.pixelSize: control.compact ? 14 : 16
                color: control.ink
            }
            Label {
                visible: control.text !== ""
                text: control.text
                font: control.font
                color: control.ink
            }
        }
    }

    background: Rectangle {
        radius: theme.radius
        color: {
            if (control.kind === "primary")
                return !control.enabled ? theme.selection
                     : control.down ? Qt.darker(theme.accent, 1.15)
                     : control.hovered ? Qt.lighter(theme.accent, 1.08) : theme.accent
            if (control.kind === "danger")
                return theme.tint(theme.red, control.down ? 0.3 : control.hovered ? 0.2 : 0.1)
            if (control.kind === "quiet")
                return control.down ? theme.selection : control.hovered ? theme.surface : "transparent"
            return control.down ? theme.border : control.hovered ? theme.selection : theme.surface
        }
        border.width: control.kind === "ghost" ? 1 : 0
        border.color: theme.selection

        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: theme.radius > 0 ? theme.radius + 3 : 0
            color: "transparent"
            border.width: 2
            border.color: theme.accent
            visible: control.visualFocus
        }
    }
}
