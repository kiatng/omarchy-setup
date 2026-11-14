import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One translation service in Settings. Its name and whether it has a key
// show at a glance; chosen, it opens to show its models and its key.
Rectangle {
    id: card

    required property var provider            // an entry of backend.providers
    readonly property bool selected: backend.providerId === provider.id
    readonly property string keyState: backend.keyStates[provider.id] || "none"
    readonly property bool hasKey: keyState === "saved" || keyState === "session"
    property bool replacing: false             // typing a new key over one that's set
    readonly property bool asking: selected && provider.needsKey && (keyState === "missing" || replacing)

    readonly property color muted: "#8a8a92"
    readonly property color good: "#4cd964"
    readonly property color warn: "#ffb86b"

    Layout.fillWidth: true
    implicitHeight: body.implicitHeight + 32
    radius: Math.min(theme.radius, 12)
    color: selected ? "#1c1c21" : hover.hovered ? "#1a1a1e" : "#161619"
    border.width: selected ? 2 : 1
    border.color: selected ? theme.accent : "#2a2a30"
    Behavior on color { ColorAnimation { duration: 120 } }

    function focusKey() {
        if (asking)
            keyField.forceActiveFocus()
    }
    function save() {
        const id = provider.id, key = keyField.text, keep = remember.checked && backend.keyringAvailable
        keyField.text = ""
        replacing = false
        backend.setKeyFor(id, key, keep)
    }

    onSelectedChanged: {
        if (!selected) {
            replacing = false
            keyField.text = ""
        }
    }

    HoverHandler { id: hover; cursorShape: card.selected ? Qt.ArrowCursor : Qt.PointingHandCursor }
    TapHandler {
        enabled: !card.selected
        onTapped: {
            backend.providerId = card.provider.id
            Qt.callLater(card.focusKey)
        }
    }

    ColumnLayout {
        id: body
        x: 18
        y: 16
        width: parent.width - 36
        spacing: 6

        // Name, and the key at a glance.
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Rectangle {
                width: 18; height: 18; radius: 9
                color: "transparent"
                border.width: 2
                border.color: card.selected ? theme.accent : "#6e6e76"
                Rectangle {
                    anchors.centerIn: parent
                    width: 8; height: 8; radius: 4
                    color: theme.accent
                    visible: card.selected
                }
            }
            Label {
                text: card.provider.name
                font.pixelSize: 17
                font.bold: card.selected
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Rectangle {
                id: badge
                implicitWidth: badgeRow.implicitWidth + 20
                implicitHeight: 26
                radius: 13
                color: card.hasKey ? Qt.rgba(card.good.r, card.good.g, card.good.b, 0.12) : "#222227"
                RowLayout {
                    id: badgeRow
                    anchors.centerIn: parent
                    spacing: 6
                    Rectangle {
                        width: 7; height: 7; radius: 3.5
                        visible: card.keyState !== "none" && card.keyState !== "checking"
                        color: card.keyState === "saved" ? card.good
                             : card.keyState === "session" ? card.warn
                             : "#5c5c64"
                    }
                    Label {
                        font.pixelSize: 12
                        textFormat: Text.PlainText
                        color: card.hasKey ? "#d8f5dc" : card.muted
                        text: card.keyState === "saved" ? qsTr("Key saved")
                            : card.keyState === "session" ? qsTr("Key for this session")
                            : card.keyState === "checking" ? qsTr("Checking keyring…")
                            : card.keyState === "missing" ? qsTr("No key")
                            : card.provider.local ? qsTr("On this laptop") : qsTr("No key needed")
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 30
            text: card.provider.summary
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            color: card.muted
            font.pixelSize: 13
        }

        // ---- Chosen: models and key.
        ColumnLayout {
            visible: card.selected
            Layout.fillWidth: true
            Layout.leftMargin: 30
            Layout.topMargin: 8
            spacing: 6

            Label {
                text: backend.models.length > 1 ? qsTr("Model") : qsTr("Model: %1").arg(backend.modelName)
                font.pixelSize: 12
                font.bold: true
                font.capitalization: backend.models.length > 1 ? Font.AllUppercase : Font.MixedCase
                color: backend.models.length > 1 ? card.muted : "#d8d8de"
            }
            Repeater {
                model: card.selected ? backend.models : []
                delegate: ColumnLayout {
                    required property var modelData
                    readonly property bool only: backend.models.length === 1
                    Layout.fillWidth: true
                    spacing: 0
                    RadioButton {
                        visible: !parent.only
                        text: modelData.name
                        font.pixelSize: 15
                        opacity: modelData.problem === "" ? 1 : 0.6
                        checked: backend.modelId === modelData.id
                        onClicked: backend.modelId = modelData.id
                        Layout.leftMargin: -8
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: parent.only ? 0 : 40
                        visible: text !== ""
                        text: modelData.note
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        color: card.muted
                        font.pixelSize: 13
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: parent.only ? 0 : 40
                        Layout.bottomMargin: 4
                        visible: text !== ""
                        text: modelData.problem
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        color: card.warn
                        font.pixelSize: 13
                    }
                }
            }
            Button {
                visible: card.provider.local
                text: qsTr("Check for downloaded models")
                flat: true
                Layout.leftMargin: -8
                onClicked: backend.refreshModels()
            }

            // The key: set, or asked for right here.
            Rectangle {
                visible: card.provider.needsKey
                Layout.fillWidth: true
                Layout.topMargin: 8
                implicitHeight: keyBox.implicitHeight + 24
                radius: Math.min(theme.radius, 8)
                color: "#141417"
                border.width: 1
                border.color: card.asking ? "#3a3a42" : "#26262c"

                ColumnLayout {
                    id: keyBox
                    x: 14
                    y: 12
                    width: parent.width - 28
                    spacing: 8

                    Label {
                        text: qsTr("API key")
                        font.pixelSize: 12
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                        color: card.muted
                    }

                    // A key is set.
                    RowLayout {
                        visible: card.hasKey && !card.replacing
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            color: "#d8d8de"
                            text: card.keyState === "saved"
                                  ? qsTr("Saved in the desktop keyring.")
                                  : qsTr("Set for this session only; forgotten when Relay closes.")
                        }
                        Button {
                            text: qsTr("Replace")
                            flat: true
                            onClicked: {
                                card.replacing = true
                                Qt.callLater(card.focusKey)
                            }
                        }
                        Button {
                            text: qsTr("Remove")
                            flat: true
                            Material.foreground: "#ff8a8a"
                            onClicked: backend.forgetKeyFor(card.provider.id)
                        }
                    }

                    Label {
                        visible: card.keyState === "checking"
                        text: qsTr("Checking the keyring…")
                        font.pixelSize: 14
                        color: card.muted
                    }

                    // No key, or a new one: paste it here.
                    RowLayout {
                        visible: card.asking
                        Layout.fillWidth: true
                        spacing: 8
                        TextField {
                            id: keyField
                            Layout.fillWidth: true
                            echoMode: TextInput.Password
                            placeholderText: qsTr("Paste a %1 API key").arg(card.provider.name)
                            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                            Material.roundedScale: theme.radius
                            onAccepted: if (text.trim() !== "") card.save()
                        }
                        AccentButton {
                            text: qsTr("Save")
                            enabled: keyField.text.trim() !== ""
                            Layout.preferredHeight: keyField.height - 8
                            onClicked: card.save()
                        }
                    }
                    RowLayout {
                        visible: card.asking
                        Layout.fillWidth: true
                        spacing: 4
                        CheckBox {
                            id: remember
                            text: qsTr("Remember in the desktop keyring")
                            checked: backend.keyringAvailable
                            enabled: backend.keyringAvailable
                            font.pixelSize: 13
                            Layout.leftMargin: -8
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            visible: card.replacing
                            text: qsTr("Cancel")
                            flat: true
                            onClicked: {
                                card.replacing = false
                                keyField.text = ""
                            }
                        }
                    }
                    Label {
                        visible: card.asking && !backend.keyringAvailable
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: card.warn
                        font.pixelSize: 13
                        text: qsTr("secret-tool isn't installed, so keys last only until Relay closes.")
                    }
                    Label {
                        visible: card.asking && card.provider.keyUrl !== ""
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font.pixelSize: 13
                        color: card.muted
                        textFormat: Text.StyledText
                        linkColor: theme.accent
                        text: qsTr("Get a key at %1").arg("<a href=\"" + card.provider.keyUrl + "\">"
                              + card.provider.keyUrl.replace("https://", "") + "</a>")
                        onLinkActivated: link => Qt.openUrlExternally(link)
                        HoverHandler { cursorShape: parent.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor }
                    }
                }
            }
        }
    }
}
