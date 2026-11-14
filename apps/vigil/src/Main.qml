import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
ApplicationWindow {
 id: win
 width: 640; height: 690; minimumWidth: 520; minimumHeight: 620
 visible: true; title: "Vigil"; color: "#101214"
 Material.theme: Material.Dark
 Material.accent: theme.accent
 Material.roundedScale: theme.radius
 property var s: backend.status
 property bool holding: !!s.manual || (s.tasks || []).length > 0
 function stateLabel() {
  switch(s.state) {
   case "keeping-awake": return qsTr("Keeping awake")
   case "battery-warning": return qsTr("Battery getting low")
   case "sleep-pending": return qsTr("Sleep pending")
   case "suspending": return qsTr("Going to sleep")
   case "error": return qsTr("Needs attention")
   default: return s.allow_tasks ? qsTr("Ready for tasks") : qsTr("Normal sleep")
  }
 }
 Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: backend.action("toggle") }
 Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
 Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: help.open() }
 Item {
  anchors.fill: parent
  LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
  LayoutMirroring.childrenInherit: true
  ScrollView {
   anchors.fill: parent; anchors.margins: 28
   contentWidth: availableWidth
   ColumnLayout {
    width: parent.width; spacing: 18
    RowLayout {
     Layout.fillWidth: true
     Label { text: "VIGIL"; font.pixelSize: 16; font.letterSpacing: 4; color: theme.accent }
     Item { Layout.fillWidth: true }
     Button { text: qsTr("Shortcuts"); onClicked: help.open() }
    }
    Label { text: win.stateLabel(); font.pixelSize: 32; font.bold: true; wrapMode: Text.Wrap; Layout.fillWidth: true }
    Label {
     text: s.inhibited ? qsTr("Close the lid. Your work can continue.") : qsTr("Your computer follows normal sleep behavior.")
     color: "#aeb6bf"; wrapMode: Text.Wrap; Layout.fillWidth: true
    }
    Rectangle {
     Layout.fillWidth: true; implicitHeight: 90; color: "#1c2025"; radius: theme.radius
     RowLayout {
      anchors.fill: parent; anchors.margins: 18
      ColumnLayout {
       Label { text: qsTr("Keep working"); font.pixelSize: 21 }
       Label { text: s.manual ? qsTr("Manually enabled") : qsTr("Screen locking stays enabled"); color: "#aeb6bf" }
      }
      Item { Layout.fillWidth: true }
      Switch { objectName: "holdSwitch"; checked: win.holding; onClicked: backend.action(checked?"on":"off"); Accessible.name: qsTr("Keep working") }
     }
    }
    CheckBox { text: qsTr("Allow task requests"); checked: !!s.allow_tasks; onClicked: backend.action(checked?"auto":"off") }
    RowLayout {
     Layout.fillWidth: true
     Label { text: s.external_power ? qsTr("Plugged in") : qsTr("On battery"); font.bold: true }
     Item { Layout.fillWidth: true }
     Label { text: { let lang=translator.language; return s.battery>=0 ? qsTr("%1% battery").arg(Number(s.battery).toLocaleString(Qt.locale(),'f',0)) : qsTr("No battery") } }
    }
    RowLayout {
     Label { text: qsTr("Battery reserve"); Layout.fillWidth: true }
     SpinBox { from: 5; to: 50; value: s.reserve || 15; onValueModified: backend.action("reserve",value) }
     Label { text: "%" }
    }
    Label { text: qsTr("Warn 10% above reserve. Sleep at reserve after a 60-second countdown. Reconnecting power cancels the countdown."); wrapMode: Text.Wrap; Layout.fillWidth: true; color: "#aeb6bf" }
    Label { visible: s.state === "sleep-pending"; text: { let lang=translator.language; return qsTr("Sleep in %1 seconds").arg(Number(s.countdown||0).toLocaleString(Qt.locale(),'f',0)) } color: theme.accent; font.bold: true }
    Button { visible: s.state === "sleep-pending" && s.reason !== "battery-reserve"; text: qsTr("Keep working instead"); onClicked: backend.action("cancel") }
    Label { text: qsTr("Active tasks"); font.bold: true; font.pixelSize: 18 }
    Label { visible: !(s.tasks || []).length; text: qsTr("No tasks registered"); color: "#aeb6bf" }
    Repeater {
     model: s.tasks || []
     Label { required property var modelData; text: modelData.name; textFormat: Text.PlainText; wrapMode: Text.Wrap; Layout.fillWidth: true }
    }
    Label { visible: backend.error !== ""; text: backend.error; textFormat: Text.PlainText; color: "#ffb4ab"; wrapMode: Text.Wrap; Layout.fillWidth: true }
    RowLayout {
     Button { text: qsTr("Sleep now…"); onClicked: confirmSleep.open() }
     Item { Layout.fillWidth: true }
     Label { text: qsTr("Space to toggle"); color: "#aeb6bf" }
    }
   }
  }
 }
 Dialog { id: help; width: Math.min(480, win.width - 40); title: qsTr("Keyboard shortcuts"); anchors.centerIn: parent; modal: true; standardButtons: Dialog.Ok
  Label { text: qsTr("Space — Toggle keep working\n? — Show shortcuts\nQ — Close window (holds continue)"); textFormat: Text.PlainText }
 }
 Dialog { id: confirmSleep; width: Math.min(440, win.width - 40); title: qsTr("Put the computer to sleep?"); anchors.centerIn: parent; modal: true; standardButtons: Dialog.Ok | Dialog.Cancel
  Label { text: qsTr("All Vigil holds will be released."); textFormat: Text.PlainText }
  onAccepted: backend.action("sleep")
 }
}
