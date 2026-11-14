import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The captions, newest at the bottom and largest. Scrolling up to reread
// stops the view following new captions until End or the button below.
Item {
    id: root

    property bool following: true

    function follow() {
        following = true
        list.positionViewAtEnd()
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.leftMargin: 40
        anchors.rightMargin: 40
        topMargin: 24
        bottomMargin: 28
        spacing: 22
        clip: true
        model: backend.captions
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        onCountChanged: if (root.following) Qt.callLater(list.positionViewAtEnd)
        onHeightChanged: if (root.following) Qt.callLater(list.positionViewAtEnd)
        // A streamed caption grows as words arrive.
        onContentHeightChanged: if (root.following) Qt.callLater(list.positionViewAtEnd)
        onMovementEnded: root.following = atYEnd
        onFlickEnded: root.following = atYEnd

        delegate: Item {
            id: row
            required property int index
            required property string text
            required property string language
            required property date time
            required property int kind
            readonly property bool latest: index === list.count - 1
            readonly property bool caption: kind === 0
            width: ListView.view.width
            height: column.implicitHeight

            Column {
                id: column
                width: parent.width
                spacing: 4

                Label {
                    visible: row.caption
                    textFormat: Text.PlainText
                    font.pixelSize: 13
                    font.letterSpacing: 0.4
                    color: row.latest ? theme.accent : "#6e6e76"
                    text: {
                        translator.language
                        const clock = row.time.toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
                        return row.language !== "" ? qsTr("%1 · from %2").arg(clock).arg(row.language) : clock
                    }
                }
                Label {
                    width: parent.width
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    text: row.text
                    lineHeight: 1.15
                    font.pixelSize: row.caption
                        ? (row.latest ? backend.fontSize : Math.round(backend.fontSize * 0.72))
                        : 15
                    font.italic: !row.caption
                    color: row.kind === 2 ? "#ff8a8a"
                         : row.kind === 1 ? "#8a8a92"
                         : row.latest ? "#ffffff" : "#b4b4bc"
                }
            }
        }
    }

    // Nothing yet: say what to do and what Relay does with the audio.
    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 620)
        spacing: 14
        visible: list.count === 0

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.pixelSize: 30
            color: "#d8d8de"
            text: backend.live ? qsTr("Listening… English captions appear here as people speak.")
                               : qsTr("Press Space to start listening")
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.pixelSize: 15
            color: "#8a8a92"
            text: qsTr("Relay detects the spoken language and translates it into English. It uses the microphone only while live, and keeps no recordings or transcripts after you close it.")
        }
    }

    AccentButton {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        visible: !root.following && list.count > 0
        text: qsTr("Jump to latest (End)")
        focusPolicy: Qt.NoFocus
        onClicked: root.follow()
    }
}
