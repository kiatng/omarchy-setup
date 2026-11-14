import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons as Commons
import qs.Ui

// Relay's button at the end of the bar. It lights up while Relay is
// listening, so a live microphone is never a surprise. Left click opens
// Relay; middle click pauses it (it never starts listening from the bar).
BarWidget {
  id: root
  moduleName: "kiat.relay"
  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  readonly property string statePath: Quickshell.env("XDG_RUNTIME_DIR") + "/relay/state"
  property string relayState: ""
  readonly property bool live: relayState === "live"

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  // Relay writes "live" or "paused" and removes the file when it closes.
  FileView {
    id: stateFile
    path: root.statePath
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: root.relayState = String(text() || "").trim()
    onLoadFailed: root.relayState = ""
  }

  // A file that doesn't exist yet can't be watched; look again now and then.
  Timer {
    interval: 3000
    repeat: true
    running: true
    onTriggered: stateFile.reload()
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󰗊"
    active: root.live
    tooltipText: root.live ? strings.t("live") : strings.t("open")
    onPressed: function(b) {
      if (!root.bar) return
      if (b === Qt.MiddleButton) root.bar.run("relay pause")
      else if (b === Qt.LeftButton) root.bar.run("relay open")
    }
  }
}
