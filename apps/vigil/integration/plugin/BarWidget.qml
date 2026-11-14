import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons as Commons
import qs.Ui
BarWidget {
 id: root
 moduleName: "kiat.vigil"
 implicitWidth: button.implicitWidth
 implicitHeight: button.implicitHeight
 property var state: ({})
 property bool loaded: false
 property double now: Date.now()
 readonly property bool available: loaded && now - Number(state.updated_ms || 0) < 7000
 Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }
 FileView {
  id: statusFile
  path: Quickshell.env("XDG_RUNTIME_DIR") + "/vigil/status.json"
  watchChanges: true; printErrors: false
  onFileChanged: reload()
  onLoaded: { try { root.state=JSON.parse(text()); root.loaded=true } catch(e) {root.loaded=false} }
  onLoadFailed: root.loaded=false
 }
 Timer { interval: 3000; running: true; repeat: true; onTriggered: { root.now=Date.now(); statusFile.reload() } }
 BarIconButton {
  id: button; anchors.fill: parent; bar: root.bar
  text: !root.available || root.state.error ? "!" : root.state.state === "sleep-pending" ? "󰒲" : ""
  active: root.available && root.state.inhibited === true
  useActiveColor: true
  tooltipText: !root.available ? strings.t("unavailable") : root.state.error ? strings.t("error") : root.state.state === "sleep-pending" ? strings.t("pending") : root.state.inhibited ? strings.t("active") : root.state.allow_tasks ? strings.t("ready") : strings.t("off")
  onPressed: function(b) {
   if (!root.bar) return
   if (b === Qt.LeftButton) root.bar.run("vigil toggle")
   else if (b === Qt.RightButton) root.bar.run("vigil open")
  }
 }
}
