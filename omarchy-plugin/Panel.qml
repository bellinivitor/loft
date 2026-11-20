import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui

// Loft in the Omarchy bar: nginx's state on the icon, and in the panel the
// start/restart/stop controls, the PHP the terminal runs, and the sites.
// Everything goes through the loft command, so the bar, the window and the
// terminal always agree.
Panel {
  id: root
  moduleName: "loft.panel"
  ipcTarget: "loft.panel"

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  property var status: null       // `loft status --json`, null until the first answer
  property bool failed: false
  property string pending: ""     // the action in flight: "start", "php:8.4"…
  property int cursor: -1         // keyboard cursor over the sites

  readonly property bool running: status !== null && status.nginx === "active"
  readonly property var sites: status ? status.sites : []
  readonly property var phpVersions: status ? status.php : []
  readonly property var defaultPhp: {
    for (var i = 0; i < phpVersions.length; ++i)
      if (phpVersions[i]["default"]) return phpVersions[i]
    return null
  }
  readonly property int maxSites: 8
  readonly property var shownSites: sites.slice(0, maxSites)
  readonly property color fg: root.bar ? root.bar.foreground : Commons.Color.foreground
  readonly property string fontFamily: root.bar ? root.bar.fontFamily : Style.font.family

  readonly property var glyph: ({
    loft: String.fromCodePoint(0xF112B), start: String.fromCodePoint(0xF040A),
    restart: String.fromCodePoint(0xF0709), stop: String.fromCodePoint(0xF04DB),
    lock: String.fromCodePoint(0xF033E), unlock: String.fromCodePoint(0xF033F),
    folder: String.fromCodePoint(0xF024B), edit: String.fromCodePoint(0xF03EB),
    proxy: String.fromCodePoint(0xF04E1), list: String.fromCodePoint(0xF0279),
    php: String.fromCodePoint(0xF031F)
  })

  readonly property string statusLine: {
    if (failed) return strings.t("notSetUp")
    if (status === null) return ""
    return (running ? strings.t("running") : strings.t("stopped"))
      + " · " + strings.tn("sites", sites.length).arg(Commons.I18n.formatNumber(sites.length))
  }

  function refresh() {
    if (!statusProc.running) statusProc.running = true
  }

  function act(name, args) {
    if (actionProc.running) return
    pending = name
    actionProc.command = ["loft"].concat(args)
    actionProc.running = true
  }

  function launch(args) {
    Quickshell.execDetached(args)
  }

  function openSite(site) {
    launch(["xdg-open", site.url])
    root.close()
  }

  function editSite(site) {
    launch(["loft", "nginx:edit", site.name, "--no-wait"])
    root.close()
  }

  function openApp(page) {
    launch(["loft", "app", page])
    root.close()
  }

  function moveCursor(step) {
    if (shownSites.length === 0) return
    cursor = cursor < 0 ? 0 : Math.max(0, Math.min(shownSites.length - 1, cursor + step))
  }

  onOpenedChanged: {
    if (opened) {
      cursor = -1
      refresh()
    }
  }

  Process {
    id: statusProc
    command: ["loft", "status", "--json"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        try {
          var parsed = JSON.parse(text)
          if (parsed && Array.isArray(parsed.sites) && Array.isArray(parsed.php)) {
            root.status = parsed
            root.failed = false
          }
        } catch (e) {
          root.failed = true
        }
      }
    }
    onExited: function(exitCode) { if (exitCode !== 0) root.failed = true }
  }

  Process {
    id: actionProc
    onExited: {
      root.pending = ""
      root.refresh()
    }
  }

  // The icon follows nginx even with the panel closed, just less often.
  Timer {
    interval: root.opened ? 2000 : 15000
    running: true
    repeat: true
    triggeredOnStart: true
    onTriggered: root.refresh()
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: root.glyph.loft
    opacity: root.running ? 1 : 0.4
    tooltipText: root.opened ? "" : "Loft · " + root.statusLine
    onPressed: function(b) {
      if (b === Qt.RightButton) root.openApp("sites")
      else root.toggle()
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(360))
    contentHeight: panel.fittedContentHeight(panel.loadedContentHeight)

    content: PanelKeyCatcher {
      anchors.fill: parent
      implicitHeight: column.implicitHeight
      onMoveRequested: function(dx, dy) { if (dy !== 0) root.moveCursor(dy) }
      onActivateRequested: if (root.cursor >= 0 && root.cursor < root.shownSites.length) root.openSite(root.shownSites[root.cursor])
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Style.space(14)

        // Loft, and whether nginx is up.
        Item {
          width: parent.width
          implicitHeight: Math.max(heroIcon.implicitHeight, heroLabels.implicitHeight)

          PlainText {
            id: heroIcon
            text: root.glyph.loft
            font.pixelSize: Style.font.display
            opacity: root.running ? 1 : 0.4
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
          }

          Column {
            id: heroLabels
            anchors.left: heroIcon.right
            anchors.leftMargin: Style.space(14)
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(2)

            PlainText {
              text: "Loft"
              font.pixelSize: Style.font.title
              font.bold: true
              width: parent.width
            }
            PlainText {
              text: root.statusLine.toUpperCase()
              opacity: 0.7
              font.pixelSize: Style.font.caption
              font.bold: true
              font.letterSpacing: 1.2
              width: parent.width
            }
          }
        }

        // nginx (with DNS and PHP-FPM, which come and go with it).
        Row {
          id: controls
          width: parent.width
          spacing: Style.space(6)
          readonly property real cellWidth: (width - spacing * 2) / 3

          Repeater {
            model: [
              { name: "start", label: strings.t("start"), enabled: !root.running },
              { name: "restart", label: strings.t("restart"), enabled: root.running },
              { name: "stop", label: strings.t("stop"), enabled: root.running }
            ]
            Button {
              required property var modelData
              width: controls.cellWidth
              iconText: root.glyph[modelData.name]
              iconSize: Style.font.title
              iconSpinning: root.pending === modelData.name && modelData.name === "restart"
              text: modelData.label
              fontSize: Style.font.bodySmall
              foreground: root.fg
              fontFamily: root.fontFamily
              bordered: true
              enabled: modelData.enabled && !root.failed && root.pending === ""
              opacity: enabled ? 1 : 0.4
              onClicked: if (enabled) root.act(modelData.name, [modelData.name])
            }
          }
        }

        PanelSeparator { foreground: root.fg }

        // The PHP `php` runs in the terminal, outside isolated sites.
        Column {
          width: parent.width
          spacing: Style.space(10)

          PanelSectionHeader {
            text: strings.t("terminalPhp")
            foreground: root.fg
            fontFamily: root.fontFamily
          }

          PlainText {
            visible: root.phpVersions.length === 0
            text: strings.t("noPhp")
            opacity: 0.6
            font.pixelSize: Style.font.bodySmall
          }

          Item {
            visible: root.phpVersions.length > 0
            width: parent.width
            implicitHeight: Math.max(phpRow.implicitHeight, binButton.implicitHeight)

            Row {
              id: phpRow
              anchors.left: parent.left
              anchors.right: binButton.left
              anchors.rightMargin: Style.space(8)
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(6)

              Repeater {
                model: root.phpVersions.slice().reverse()
                Button {
                  required property var modelData
                  text: modelData.line
                  tooltipText: "PHP " + modelData.version
                  fontSize: Style.font.bodySmall
                  foreground: root.fg
                  fontFamily: root.fontFamily
                  bordered: true
                  active: modelData["default"] === true
                  iconSpinning: root.pending === "php:" + modelData.line
                  onClicked: if (!modelData["default"]) root.act("php:" + modelData.line, ["use", modelData.line])
                }
              }
            }

            PanelActionButton {
              id: binButton
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              iconText: root.glyph.folder
              tooltipText: root.defaultPhp ? strings.t("binFolder").arg(root.defaultPhp.version) : ""
              foreground: root.fg
              fontFamily: root.fontFamily
              visible: root.defaultPhp !== null
              onClicked: {
                root.launch(["xdg-open", root.defaultPhp.path])
                root.close()
              }
            }
          }
        }

        PanelSeparator { foreground: root.fg }

        // The sites: a click opens one, the pencil edits its nginx config.
        Column {
          width: parent.width
          spacing: Style.space(4)

          PanelSectionHeader {
            text: strings.t("sitesHeader")
            foreground: root.fg
            fontFamily: root.fontFamily
          }

          PlainText {
            visible: root.sites.length === 0
            width: parent.width
            text: root.failed ? strings.t("notSetUp") : strings.t("noSites")
            opacity: 0.6
            wrapMode: Text.Wrap
            font.pixelSize: Style.font.bodySmall
          }

          Repeater {
            model: root.shownSites
            Rectangle {
              id: siteRow
              required property var modelData
              required property int index
              readonly property bool hot: rowMouse.containsMouse || root.cursor === index
              width: parent.width
              height: Math.max(Style.spacing.popupRowHeight, editButton.implicitHeight)
              radius: Style.cornerRadius
              color: hot ? Style.hoverFillFor(root.fg, Commons.Color.accent) : "transparent"

              MouseArea {
                id: rowMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onEntered: root.cursor = siteRow.index
                onClicked: root.openSite(siteRow.modelData)
              }

              PlainText {
                id: lockGlyph
                anchors.left: parent.left
                anchors.leftMargin: Style.space(8)
                anchors.verticalCenter: parent.verticalCenter
                text: siteRow.modelData.kind === "proxy" ? root.glyph.proxy
                    : siteRow.modelData.secure ? root.glyph.lock : root.glyph.unlock
                opacity: siteRow.modelData.secure ? 0.9 : 0.4
                font.pixelSize: Style.font.body
              }

              PlainText {
                anchors.left: lockGlyph.right
                anchors.leftMargin: Style.space(10)
                anchors.right: phpLabel.left
                anchors.rightMargin: Style.space(8)
                anchors.verticalCenter: parent.verticalCenter
                text: siteRow.modelData.host
                opacity: siteRow.modelData.missing ? 0.5 : 1
                elide: Text.ElideRight
                font.pixelSize: Style.font.bodySmall
              }

              PlainText {
                id: phpLabel
                anchors.right: editButton.left
                anchors.rightMargin: Style.space(6)
                anchors.verticalCenter: parent.verticalCenter
                text: siteRow.modelData.phpVersion || ""
                opacity: 0.5
                font.pixelSize: Style.font.caption
              }

              PanelActionButton {
                id: editButton
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                iconText: root.glyph.edit
                fontSize: Style.font.bodySmall
                tooltipText: siteRow.modelData.customNginx ? strings.t("customNginx") : strings.t("editNginx")
                foreground: siteRow.modelData.customNginx ? Commons.Color.accent : root.fg
                fontFamily: root.fontFamily
                opacity: siteRow.hot || siteRow.modelData.customNginx ? 1 : 0.35
                onClicked: root.editSite(siteRow.modelData)
              }
            }
          }

          PlainText {
            visible: root.sites.length > root.maxSites
            leftPadding: Style.space(8)
            text: strings.tn("more", root.sites.length - root.maxSites)
              .arg(Commons.I18n.formatNumber(root.sites.length - root.maxSites))
            opacity: 0.5
            font.pixelSize: Style.font.caption
          }
        }

        // The window, on the page that lists everything.
        Row {
          id: footer
          width: parent.width
          spacing: Style.space(6)
          readonly property real cellWidth: (width - spacing) / 2

          Button {
            width: footer.cellWidth
            iconText: root.glyph.list
            text: strings.t("openSites")
            fontSize: Style.font.bodySmall
            foreground: root.fg
            fontFamily: root.fontFamily
            bordered: true
            onClicked: root.openApp("sites")
          }
          Button {
            width: footer.cellWidth
            iconText: root.glyph.php
            text: strings.t("openPhp")
            fontSize: Style.font.bodySmall
            foreground: root.fg
            fontFamily: root.fontFamily
            bordered: true
            onClicked: root.openApp("php")
          }
        }
      }
    }
  }

  // Every label here is plain text: site names come from folder names.
  component PlainText: Text {
    textFormat: Text.PlainText
    color: root.fg
    font.family: root.fontFamily
  }
}
