import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1100
    height: 720
    minimumWidth: 720
    minimumHeight: 460
    visible: true
    title: backend.live ? qsTr("Relay — Live") : "Relay"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius
    Material.background: "#0e0e10"

    property bool helpVisible: false
    readonly property color dim: "#8a8a92"
    readonly property color faint: "#5c5c64"
    readonly property color surface: "#17171a"
    readonly property color danger: "#ff6b6b"
    // Keys typed into a text field are the field's, not shortcuts.
    readonly property bool typing: activeFocusItem !== null && activeFocusItem.selectedText !== undefined

    function number(value, decimals) {
        translator.language // re-format on a language switch
        return Number(value).toLocaleString(Qt.locale(), 'f', decimals)
    }

    function closeOverlays() {
        if (helpVisible) helpVisible = false
        else if (settings.opened) settings.close()
        else if (win.visibility === Window.FullScreen) win.showNormal()
    }

    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.toggle() }
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Qt.quit() }
    Shortcut { sequences: [StandardKey.Quit]; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: settings.opened ? settings.close() : settings.open() }
    Shortcut { sequence: "Ctrl+,"; context: Qt.ApplicationShortcut; onActivated: settings.opened ? settings.close() : settings.open() }
    Shortcut { sequence: "M"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: { microphone.forceActiveFocus(); microphone.popup.open() } }
    Shortcut { sequence: "Ctrl+L"; context: Qt.ApplicationShortcut; onActivated: backend.clearCaptions() }
    Shortcut { sequence: "Ctrl+Shift+C"; context: Qt.ApplicationShortcut; onActivated: backend.copyCaptions() }
    Shortcut { sequences: ["Ctrl+=", "Ctrl++"]; context: Qt.ApplicationShortcut; onActivated: backend.fontSize += 4 }
    Shortcut { sequence: "Ctrl+-"; context: Qt.ApplicationShortcut; onActivated: backend.fontSize -= 4 }
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; onActivated: backend.fontSize = 40 }
    Shortcut { sequences: ["F", "F11"]; context: Qt.ApplicationShortcut; enabled: !win.typing
        onActivated: win.visibility === Window.FullScreen ? win.showNormal() : win.showFullScreen() }
    Shortcut { sequence: "End"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: captionList.follow() }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: win.closeOverlays() }

    Connections {
        target: backend
        function onCopied() { toast.show(qsTr("Captions copied")) }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Controls
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 68
                color: win.surface

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 12
                    spacing: 14

                    AccentButton {
                        id: startButton
                        text: backend.live ? qsTr("Pause") : qsTr("Start")
                        icon.source: backend.live ? "qrc:/icons/pause.svg" : "qrc:/icons/mic.svg"
                        font.pixelSize: 16
                        font.bold: true
                        Layout.preferredHeight: 48
                        Layout.minimumWidth: 128
                        // The accent invites starting; listening shows a quiet button.
                        quiet: backend.live
                        icon.color: backend.live ? "white" : theme.accentForeground
                        focusPolicy: Qt.NoFocus
                        onClicked: backend.toggle()
                    }

                    ColumnLayout {
                        spacing: 4
                        Layout.preferredWidth: 220
                        Layout.minimumWidth: 150
                        Layout.maximumWidth: 240
                        RowLayout {
                            spacing: 8
                            Rectangle {
                                width: 10; height: 10; radius: 5
                                color: backend.live ? (backend.hearing ? theme.accent : "#4cd964") : win.faint
                            }
                            Label {
                                textFormat: Text.PlainText
                                font.pixelSize: 15
                                text: !backend.live ? qsTr("Paused — not listening")
                                      : backend.connecting ? qsTr("Live — connecting…")
                                      : backend.hearing ? qsTr("Live — hearing speech")
                                      : qsTr("Live — listening")
                                color: backend.live ? "white" : win.dim
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                        // Input level, so a muted or wrong microphone shows at once.
                        Rectangle {
                            Layout.fillWidth: true
                            height: 4
                            radius: Math.min(2, theme.radius)
                            color: "#2a2a2f"
                            Rectangle {
                                height: parent.height
                                radius: parent.radius
                                width: parent.width * Math.min(1, Math.sqrt(backend.level) * 2.2)
                                color: backend.hearing ? theme.accent : "#4cd964"
                                visible: backend.live
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }

                    ComboBox {
                        id: microphone
                        Layout.fillWidth: true
                        Layout.preferredWidth: 340
                        Layout.minimumWidth: 150
                        Layout.maximumWidth: 380
                        model: backend.devices
                        textRole: "name"
                        valueRole: "id"
                        displayText: win.width < 980 ? currentText : qsTr("Microphone: %1").arg(currentText)
                        Material.roundedScale: theme.radius
                        Component.onCompleted: currentIndex = Math.max(0, indexOfValue(backend.deviceId))
                        onActivated: backend.deviceId = currentValue
                        Connections {
                            target: backend
                            function onDevicesChanged() { microphone.currentIndex = Math.max(0, microphone.indexOfValue(backend.deviceId)) }
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Microphone (M)")
                        ToolTip.delay: 600
                    }

                    Button {
                        flat: true
                        text: backend.providerName + " · " + backend.modelName
                        icon.source: "qrc:/icons/settings.svg"
                        // Narrow windows keep the icon; the footer still names the model.
                        display: win.width < 980 ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
                        Layout.maximumWidth: 320
                        Material.roundedScale: theme.radius
                        focusPolicy: Qt.NoFocus
                        onClicked: settings.open()
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Provider and settings (S)")
                        ToolTip.delay: 600
                    }

                    ToolButton {
                        text: "?"
                        font.pixelSize: 18
                        focusPolicy: Qt.NoFocus
                        onClicked: win.helpVisible = !win.helpVisible
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Keyboard shortcuts (?)")
                        ToolTip.delay: 600
                    }
                }
            }

            // What's wrong, in words, with the way to fix it.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? problemRow.implicitHeight + 20 : 0
                visible: backend.problem !== ""
                color: "#3a1d1f"

                RowLayout {
                    id: problemRow
                    anchors.fill: parent
                    anchors.leftMargin: 18
                    anchors.rightMargin: 12
                    spacing: 12
                    Label {
                        text: "!"
                        font.pixelSize: 20
                        font.bold: true
                        color: win.danger
                    }
                    Label {
                        textFormat: Text.PlainText
                        text: backend.problem
                        wrapMode: Text.Wrap
                        font.pixelSize: 15
                        color: "#ffd9d9"
                        Layout.fillWidth: true
                    }
                    Button {
                        visible: backend.problemAction === "key" || backend.problemAction === "model"
                        text: backend.problemAction === "key" ? qsTr("Add key") : qsTr("Choose model")
                        flat: true
                        Material.foreground: "white"
                        focusPolicy: Qt.NoFocus
                        onClicked: settings.open()
                    }
                    Button {
                        visible: backend.problemAction === "device"
                        text: qsTr("Choose microphone")
                        flat: true
                        Material.foreground: "white"
                        focusPolicy: Qt.NoFocus
                        onClicked: microphone.popup.open()
                    }
                    ToolButton {
                        text: "✕"
                        focusPolicy: Qt.NoFocus
                        onClicked: backend.dismissProblem()
                    }
                }
            }

            CaptionList {
                id: captionList
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            // Where the captions come from, how far behind they run, and
            // what they cost so far.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                color: win.surface

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: 18
                    Label {
                        textFormat: Text.PlainText
                        color: win.dim
                        font.pixelSize: 13
                        text: backend.providerName + " · " + backend.modelName
                        elide: Text.ElideRight
                        Layout.maximumWidth: 320
                    }
                    Label {
                        textFormat: Text.PlainText
                        color: win.dim
                        font.pixelSize: 13
                        visible: backend.lastLatencyMs > 0
                        text: qsTr("Last caption took %1 s").arg(win.number(backend.lastLatencyMs / 1000, 1))
                    }
                    Label {
                        textFormat: Text.PlainText
                        color: backend.pending > 2 ? theme.accent : win.dim
                        font.pixelSize: 13
                        visible: backend.pending > 0
                        text: qsTr("%n waiting", "", backend.pending)
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        textFormat: Text.PlainText
                        color: win.dim
                        font.pixelSize: 13
                        visible: backend.audioSeconds > 0
                        text: qsTr("%1 min of speech sent").arg(win.number(backend.audioSeconds / 60, 1))
                              + (backend.sessionCost > 0 ? "  ·  ≈ US$" + win.number(backend.sessionCost, 3) : "")
                    }
                }
            }
        }
    }

    SettingsPanel {
        id: settings
        height: win.height
        width: Math.min(540, win.width * 0.92)
    }

    ShortcutHelp {
        anchors.fill: parent
        visible: win.helpVisible
        onClosed: win.helpVisible = false
    }

    // A brief confirmation, centered over the captions.
    Rectangle {
        id: toast
        property string message: ""
        function show(text) { message = text; opacity = 1; toastTimer.restart() }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 60
        width: toastLabel.implicitWidth + 32
        height: 40
        radius: Math.min(8, theme.radius)
        color: "#2a2a2f"
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        Label { id: toastLabel; anchors.centerIn: parent; text: toast.message; textFormat: Text.PlainText }
        Timer { id: toastTimer; interval: 1600; onTriggered: toast.opacity = 0 }
    }
}
