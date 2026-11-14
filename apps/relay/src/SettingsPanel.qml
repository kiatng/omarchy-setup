import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Provider, model, key, and caption settings, sliding in from the side.
Drawer {
    id: root
    edge: Qt.application.layoutDirection === Qt.RightToLeft ? Qt.LeftEdge : Qt.RightEdge
    Material.roundedScale: theme.radius
    background: Rectangle { color: "#151518" }

    readonly property color muted: "#8a8a92"
    readonly property color warn: "#ffb86b"

    onOpened: {
        for (let i = 0; i < cards.count; ++i)
            if (cards.itemAt(i).selected)
                cards.itemAt(i).focusKey()
    }

    Flickable {
        anchors.fill: parent
        contentHeight: content.implicitHeight + 48
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: content
            x: 24
            y: 24
            width: parent.width - 48
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Layout.bottomMargin: 6
                Label {
                    text: qsTr("Settings")
                    font.pixelSize: 24
                    font.bold: true
                    Layout.fillWidth: true
                }
                ToolButton {
                    text: "✕"
                    font.pixelSize: 16
                    focusPolicy: Qt.NoFocus
                    onClicked: root.close()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Close (Esc)")
                    ToolTip.delay: 600
                }
            }

            Label { text: qsTr("Translation service"); font.pixelSize: 13; font.bold: true; color: root.muted; font.capitalization: Font.AllUppercase }
            Label {
                Layout.fillWidth: true
                Layout.bottomMargin: 2
                wrapMode: Text.Wrap
                color: root.muted
                font.pixelSize: 13
                text: qsTr("Each service keeps its own key, so you can switch between them freely.")
            }
            Repeater {
                id: cards
                model: backend.providers
                delegate: ProviderCard {
                    required property var modelData
                    provider: modelData
                }
            }

            Label { text: qsTr("Captions"); font.pixelSize: 13; font.bold: true; color: root.muted; font.capitalization: Font.AllUppercase; Layout.topMargin: 18 }
            Label {
                visible: !backend.streaming
                textFormat: Text.PlainText
                font.pixelSize: 15
                text: qsTr("Longest stretch per caption: %1 s").arg(backend.chunkSeconds)
            }
            Slider {
                visible: !backend.streaming
                Layout.fillWidth: true
                from: 4; to: 15; stepSize: 1
                value: backend.chunkSeconds
                onMoved: backend.chunkSeconds = value
            }
            Label {
                visible: backend.streaming
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: root.muted
                font.pixelSize: 13
                text: qsTr("%1 translates as people speak, so each caption grows word by word and ends where the speaker pauses.").arg(backend.providerName)
            }
            Label {
                visible: !backend.streaming
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: root.muted
                font.pixelSize: 13
                text: qsTr("Captions are cut at pauses in speech. Shorter stretches appear sooner; longer ones give the translator more context. Groq bills at least 10 seconds per request, so very short stretches cost more there.")
            }
            Label {
                textFormat: Text.PlainText
                font.pixelSize: 15
                Layout.topMargin: 6
                text: qsTr("Text size: %1 px").arg(backend.fontSize)
            }
            Slider {
                Layout.fillWidth: true
                from: 20; to: 96; stepSize: 2
                value: backend.fontSize
                onMoved: backend.fontSize = value
            }

            Label { text: qsTr("Privacy"); font.pixelSize: 13; font.bold: true; color: root.muted; font.capitalization: Font.AllUppercase; Layout.topMargin: 14 }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#c8c8ce"
                font.pixelSize: 13
                text: backend.streaming
                      ? qsTr("The microphone is open only while Relay is live, and all of that time its audio streams to %1, pauses included. Captions stay in memory and are gone when Relay closes.").arg(backend.providerName)
                      : qsTr("The microphone is open only while Relay is live. Silence is never sent. Cloud services receive the speech they translate; Voxtype keeps it on this laptop. Captions stay in memory and are gone when Relay closes.")
            }
        }
    }
}
