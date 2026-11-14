import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every shortcut, shown with ?.
Rectangle {
    id: root
    signal closed()
    color: "#cc0e0e10"

    MouseArea { anchors.fill: parent; onClicked: root.closed() }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 560)
        height: grid.implicitHeight + 88
        radius: theme.radius
        color: "#1b1b1f"
        border.color: "#2c2c32"

        MouseArea { anchors.fill: parent } // clicks inside don't close it

        Label {
            id: heading
            x: 28; y: 22
            text: qsTr("Keyboard shortcuts")
            font.pixelSize: 20
            font.bold: true
        }

        GridLayout {
            id: grid
            anchors.top: heading.bottom
            anchors.topMargin: 16
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 28
            anchors.rightMargin: 28
            columns: 2
            columnSpacing: 24
            rowSpacing: 9

            Repeater {
                model: [
                    ["Space", qsTr("Start or pause listening")],
                    ["M", qsTr("Choose the microphone")],
                    ["S", qsTr("Settings: provider, model, key")],
                    ["Ctrl + / Ctrl −", qsTr("Larger or smaller text")],
                    ["F", qsTr("Full screen")],
                    ["End", qsTr("Jump to the latest caption")],
                    ["Ctrl+Shift+C", qsTr("Copy all captions")],
                    ["Ctrl+L", qsTr("Clear the captions")],
                    ["?", qsTr("Show or hide this list")],
                    ["Q", qsTr("Quit (stops listening)")]
                ]
                delegate: RowLayout {
                    required property var modelData
                    Layout.columnSpan: 2
                    spacing: 18
                    Rectangle {
                        Layout.preferredWidth: 140
                        Layout.preferredHeight: 28
                        radius: Math.min(4, theme.radius)
                        color: "#26262b"
                        Label {
                            anchors.centerIn: parent
                            text: modelData[0]
                            textFormat: Text.PlainText
                            font.family: "monospace"
                            font.pixelSize: 13
                            color: theme.accent
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData[1]
                        textFormat: Text.PlainText
                        font.pixelSize: 15
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }
}
