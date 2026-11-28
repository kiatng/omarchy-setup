import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "Model.js" as Model

// Wrapper panel that embeds the Workspace Map KeyboardPanel
// This allows BarWidget to inject anchorItem/bar before the panel opens
Panel {
    id: root
    moduleName: "kiat.workspace-map"
    // IPC handled by BarWidget only
    manageIpc: false

    property var anchorItem: null
    property var hostWidget: null
    readonly property var barIdentity: hostWidget || root
    property bool canGoBack: false
    property bool canGoForward: false

    function syncHistory() {
        var snap = Model.historySnapshot()
        canGoBack = snap.canBack
        canGoForward = snap.canForward
    }

    function goBack() {
        if (hostWidget && typeof hostWidget.goBack === "function") {
            hostWidget.goBack()
        } else {
            var target = Model.stepBack()
            syncHistory()
            if (target !== null) Hyprland.dispatch('hl.dsp.focus({ workspace = "' + target + '" })')
        }
        panel.refreshData()
    }

    function goForward() {
        if (hostWidget && typeof hostWidget.goForward === "function") {
            hostWidget.goForward()
        } else {
            var target = Model.stepForward()
            syncHistory()
            if (target !== null) Hyprland.dispatch('hl.dsp.focus({ workspace = "' + target + '" })')
        }
        panel.refreshData()
    }

    function refresh() {
        syncHistory()
        panel.refreshData()
    }

    function open() {
        Model.ensureInitialWorkspace(Hyprland)
        syncHistory()
        panel.refreshData()
        root.controller.show()
        Qt.callLater(function() {
            if (root.opened) setCenterHoverRevealSuppressed(true)
        })
    }

    function close() {
        setCenterHoverRevealSuppressed(false)
        root.controller.hide()
    }

    function switchPanel(direction) {
        if (root.bar && typeof root.bar.switchPanelFrom === "function")
            return root.bar.switchPanelFrom(root.barIdentity, direction)
        return false
    }

    function setCenterHoverRevealSuppressed(value) {
        if (root.bar && typeof root.bar.setCenterHoverRevealSuppressed === "function")
            root.bar.setCenterHoverRevealSuppressed(value)
        else if (root.bar && "centerHoverRevealSuppressed" in root.bar)
            root.bar.centerHoverRevealSuppressed = value
    }

    Connections {
        target: Hyprland
        function onRawEvent(event) {
            if (!root.opened || !event) return
            var name = String(event.name || "")
            if (name === "workspacev2" || name === "moveworkspacev2" ||
                name === "openwindow" || name === "closewindow" ||
                name === "movewindowv2" || name === "windowtitlev2" ||
                name === "focusedmonv2" || name === "monitoradded" ||
                name === "monitorremoved" || name === "changeworkspaceid") {
                panel.refreshData()
            }
        }
    }

    Process {
        id: procResolver
        stdout: StdioCollector {
            waitForEnd: true
            onStreamFinished: {
                try {
                    var parsed = JSON.parse(String(text || "{}"))
                    if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
                        panel.terminalProcs = parsed
                        panel.workspaceData = Model.buildWorkspaceData(Hyprland, panel.terminalProcs)
                    }
                } catch (e) {
                }
            }
        }
        onExited: {
            if (panel.pendingProcRefresh) {
                panel.pendingProcRefresh = false
                panel.refreshTerminalProcs()
            }
        }
    }

    KeyboardPanel {
        id: panel
        anchorItem: root.anchorItem
        owner: root.barIdentity
        bar: root.bar
        open: root.opened
        centerOnBar: false
        focusTarget: panel.loadedContent
        padding: Commons.Style.space(12)
        contentWidth: panel.fittedContentWidth(Commons.Style.space(860))
        contentHeight: panel.fittedContentHeight(panel.loadedContentHeight, Commons.Style.space(700))

        readonly property int maxWindowsPerWorkspace: 8
        property var workspaceData: []
        property var terminalProcs: ({})
        property bool pendingProcRefresh: false
        readonly property string procScriptPath: decodeURIComponent(Qt.resolvedUrl("terminal_procs.py").toString().replace(/^file:\/\//, ""))

        signal workspaceSelected(int workspaceId, string monitorName)

        onWorkspaceSelected: function(workspaceId, monitorName) {
            var id = Number(workspaceId)
            root.close()
            if (isFinite(id)) {
                Hyprland.dispatch('hl.dsp.focus({ workspace = "' + Math.round(id) + '" })')
            }
        }

        function refreshData() {
            panel.workspaceData = Model.buildWorkspaceData(Hyprland, panel.terminalProcs)
            refreshTerminalProcs()
        }

        function refreshTerminalProcs() {
            var pids = Model.getTerminalPids(Hyprland)
            if (pids.length === 0) return
            if (procResolver.running) {
                pendingProcRefresh = true
                return
            }
            procResolver.command = ["python3", panel.procScriptPath].concat(pids)
            procResolver.running = true
        }

        function resolveAppName(winData) {
            var cls = winData && winData.class ? String(winData.class) : ""
            if (cls) {
                var entry = DesktopEntries.heuristicLookup(cls)
                if (entry && entry.name) return String(entry.name)
            }
            return winData && winData.primary ? String(winData.primary) : ""
        }

        function resolveAppDetail(winData, appName) {
            var title = winData && winData.title ? String(winData.title).trim() : ""
            if (!title) return ""
            var lowerApp = String(appName || "").toLowerCase()
            var lowerTitle = title.toLowerCase()
            if (lowerTitle === lowerApp) return ""
            if (/^[\w.-]+\.[a-z]{2,}(\/.*)?$/i.test(title)) return ""
            if (lowerApp === "discord") {
                title = title.replace(/^\(\d+\)\s*/, "").replace(/^Discord\s*\|\s*/i, "").trim()
            } else if (lowerApp) {
                var escaped = appName.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
                title = title.replace(new RegExp("\\s*(?:/|\\||-|—)\\s*" + escaped + "$", "i"), "").trim()
            }
            return title.toLowerCase() === lowerApp ? "" : title
        }

        function resolveAppIcon(appData) {
            var cls = appData && appData.class ? String(appData.class) : ""
            var iconName = appData && appData.iconName ? String(appData.iconName) : ""
            if (cls) {
                var entry = DesktopEntries.heuristicLookup(cls)
                if (entry && entry.icon) {
                    var entryPath = Quickshell.iconPath(entry.icon, true)
                    if (entryPath) return entryPath
                }
                var classPath = Quickshell.iconPath(cls.toLowerCase(), true)
                if (classPath) return classPath
            }
            var mapped = iconName === "browser" ? "web-browser"
                : (iconName === "terminal" ? "utilities-terminal"
                : (iconName === "code" ? "visual-studio-code" : iconName))
            if (mapped) {
                var mappedPath = Quickshell.iconPath(mapped, true)
                if (mappedPath) return mappedPath
            }
            return ""
        }

        function fallbackGlyph(iconName) {
            if (iconName === "browser") return "󰖟"
            if (iconName === "terminal") return "󰆍"
            if (iconName === "code") return "󰨞"
            if (iconName === "discord") return "󰙯"
            return "󰟢"
        }

        Component.onCompleted: {
            panel.refreshData()
        }

        content: PanelKeyCatcher {
            id: keyCatcher
            implicitHeight: contentColumn.implicitHeight
            anchors.fill: parent
            onCloseRequested: root.close()
            onTabRequested: function(direction) { root.switchPanel(direction) }
            onTextKey: function(t) {
                if (t === "[") root.goBack()
                else if (t === "]") root.goForward()
            }

            ColumnLayout {
                id: contentColumn
                anchors.fill: parent
                spacing: Commons.Style.space(8)

                readonly property color fg: Commons.Color.popups.text || Commons.Color.foreground
                readonly property color accent: Commons.Color.accent
                readonly property string fontFamily: panel.bar ? panel.bar.fontFamily : Commons.Style.font.family

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Commons.Style.space(8)

                    Text {
                        textFormat: Text.PlainText
                        text: "Workspace Map"
                        font.family: contentColumn.fontFamily
                        font.pixelSize: Commons.Style.font.subtitle
                        font.weight: Font.DemiBold
                        color: contentColumn.fg
                    }

                    Row {
                        spacing: Commons.Style.space(4)

                        PanelActionButton {
                            iconText: "󰅁"
                            fontFamily: contentColumn.fontFamily
                            fontSize: Commons.Style.font.iconLarge
                            size: Commons.Style.space(23)
                            bordered: true
                            foreground: contentColumn.fg
                            hoverColor: contentColumn.accent
                            enabled: root.canGoBack
                            opacity: enabled ? 1 : 0.35
                            tooltipText: "Back (Super + [)"
                            onClicked: root.goBack()
                        }

                        PanelActionButton {
                            iconText: "󰅂"
                            fontFamily: contentColumn.fontFamily
                            fontSize: Commons.Style.font.iconLarge
                            size: Commons.Style.space(23)
                            bordered: true
                            foreground: contentColumn.fg
                            hoverColor: contentColumn.accent
                            enabled: root.canGoForward
                            opacity: enabled ? 1 : 0.35
                            tooltipText: "Forward (Super + ])"
                            onClicked: root.goForward()
                        }
                    }

                    Item { Layout.fillWidth: true }
                }

                Flickable {
                    id: scrollArea
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    implicitHeight: workspaceGrid.implicitHeight
                    contentWidth: width
                    contentHeight: workspaceGrid.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    interactive: contentHeight > height

                    RowLayout {
                        id: workspaceGrid
                        width: scrollArea.width
                        spacing: Commons.Style.space(10)

                        Repeater {
                            model: panel.workspaceData

                            Column {
                                id: monitorCol
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                Layout.minimumWidth: 0
                                Layout.alignment: Qt.AlignTop
                                spacing: Commons.Style.space(4)

                                readonly property var monitorData: modelData
                                readonly property string monitorName: monitorData.name || ""
                                readonly property string monitorDescription: monitorData.description || ""

                                Rectangle {
                                    width: parent.width
                                    height: Commons.Style.space(26)
                                    radius: Commons.Style.cornerRadius
                                    color: Util.alpha(contentColumn.fg, 0.08)
                                    border.color: Util.alpha(contentColumn.fg, 0.14)
                                    border.width: Commons.Style.spacing.hairline

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: Commons.Style.space(8)
                                        anchors.rightMargin: Commons.Style.space(8)
                                        spacing: Commons.Style.space(6)

                                        Text {
                                            textFormat: Text.PlainText
                                            text: "󰍹"
                                            font.family: contentColumn.fontFamily
                                            font.pixelSize: Commons.Style.font.iconSmall
                                            color: contentColumn.fg
                                            opacity: 0.8
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            textFormat: Text.PlainText
                                            text: monitorCol.monitorDescription || monitorCol.monitorName
                                            font.family: contentColumn.fontFamily
                                            font.pixelSize: Commons.Style.font.bodySmall
                                            font.weight: Font.Medium
                                            color: contentColumn.fg
                                            elide: Text.ElideRight
                                        }

                                        Text {
                                            textFormat: Text.PlainText
                                            text: monitorCol.monitorName
                                            font.family: contentColumn.fontFamily
                                            font.pixelSize: Commons.Style.font.caption
                                            color: contentColumn.fg
                                            opacity: 0.45
                                        }
                                    }
                                }

                                Column {
                                    width: parent.width
                                    spacing: Commons.Style.space(3)

                                    Repeater {
                                        model: monitorCol.monitorData.workspaces

                                        MouseArea {
                                            id: wsMouseArea
                                            required property var modelData
                                            readonly property var workspaceData: modelData
                                            readonly property bool isActive: workspaceData.isActive === true
                                            readonly property bool hasWindows: workspaceData.hasWindows === true
                                            readonly property var winList: workspaceData.windows || []
                                            width: parent.width
                                            height: Math.max(Commons.Style.space(34), wsRow.implicitHeight + Commons.Style.space(10))
                                            cursorShape: Qt.PointingHandCursor
                                            hoverEnabled: true

                                            onClicked: {
                                                panel.workspaceSelected(workspaceData.id, monitorCol.monitorName)
                                            }

                                            Rectangle {
                                                anchors.fill: parent
                                                radius: Commons.Style.cornerRadius
                                                color: wsMouseArea.containsMouse
                                                    ? Util.alpha(contentColumn.fg, 0.08)
                                                    : (wsMouseArea.isActive ? Util.alpha(contentColumn.accent, 0.13) : Util.alpha(contentColumn.fg, 0.025))
                                                border.color: wsMouseArea.isActive
                                                    ? contentColumn.accent
                                                    : (wsMouseArea.containsMouse ? Util.alpha(contentColumn.fg, 0.2) : Util.alpha(contentColumn.fg, 0.09))
                                                border.width: wsMouseArea.isActive ? 1.5 : Commons.Style.spacing.hairline

                                                RowLayout {
                                                    id: wsRow
                                                    anchors.left: parent.left
                                                    anchors.right: parent.right
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    anchors.leftMargin: Commons.Style.space(8)
                                                    anchors.rightMargin: Commons.Style.space(8)
                                                    spacing: Commons.Style.space(8)

                                                    Column {
                                                        Layout.preferredWidth: Commons.Style.space(48)
                                                        Layout.alignment: Qt.AlignVCenter
                                                        spacing: Commons.Style.space(1)

                                                        Text {
                                                            width: parent.width
                                                            textFormat: Text.PlainText
                                                            text: wsMouseArea.workspaceData.name
                                                            font.family: contentColumn.fontFamily
                                                            font.pixelSize: Commons.Style.font.body
                                                            font.weight: wsMouseArea.isActive ? Font.DemiBold : Font.Medium
                                                            color: wsMouseArea.isActive ? contentColumn.accent : contentColumn.fg
                                                            opacity: wsMouseArea.isActive ? 1 : 0.85
                                                            horizontalAlignment: Text.AlignHCenter
                                                        }

                                                        Text {
                                                            visible: wsMouseArea.workspaceData.shortcut !== ""
                                                            width: parent.width
                                                            textFormat: Text.PlainText
                                                            text: wsMouseArea.workspaceData.shortcut
                                                            font.family: contentColumn.fontFamily
                                                            font.pixelSize: Math.max(9, Commons.Style.font.caption - 1)
                                                            color: contentColumn.fg
                                                            opacity: 0.42
                                                            horizontalAlignment: Text.AlignHCenter
                                                        }
                                                    }

                                                    Rectangle {
                                                        Layout.preferredWidth: 1
                                                        Layout.fillHeight: true
                                                        Layout.topMargin: Commons.Style.space(2)
                                                        Layout.bottomMargin: Commons.Style.space(2)
                                                        color: wsMouseArea.isActive
                                                            ? Util.alpha(contentColumn.accent, 0.35)
                                                            : Util.alpha(contentColumn.fg, 0.1)
                                                    }

                                                    ColumnLayout {
                                                        Layout.fillWidth: true
                                                        spacing: Commons.Style.space(2)

                                                        Repeater {
                                                            model: wsMouseArea.winList.slice(0, panel.maxWindowsPerWorkspace)

                                                            RowLayout {
                                                                id: winLine
                                                                required property var modelData
                                                                readonly property var winData: modelData
                                                                readonly property string kind: winData.kind || "app"
                                                                readonly property string resolvedIcon: kind === "app" ? panel.resolveAppIcon(winData) : ""
                                                                readonly property string primaryText: kind === "app"
                                                                    ? panel.resolveAppName(winData)
                                                                    : String(winData.primary || "")
                                                                readonly property string detailText: kind === "app"
                                                                    ? panel.resolveAppDetail(winData, primaryText)
                                                                    : String(winData.detail || "")

                                                                Layout.fillWidth: true
                                                                Layout.preferredHeight: Commons.Style.space(20)
                                                                spacing: Commons.Style.space(6)

                                                                Item {
                                                                    Layout.preferredWidth: Commons.Style.space(16)
                                                                    Layout.preferredHeight: Commons.Style.space(16)
                                                                    Layout.alignment: Qt.AlignVCenter

                                                                    Image {
                                                                        id: appIcon
                                                                        anchors.centerIn: parent
                                                                        width: Commons.Style.space(15)
                                                                        height: Commons.Style.space(15)
                                                                        fillMode: Image.PreserveAspectFit
                                                                        source: winLine.resolvedIcon
                                                                        asynchronous: true
                                                                        visible: winLine.kind === "app" && winLine.resolvedIcon !== "" && status === Image.Ready
                                                                    }

                                                                    Text {
                                                                        anchors.centerIn: parent
                                                                        visible: !appIcon.visible
                                                                        textFormat: Text.PlainText
                                                                        text: panel.fallbackGlyph(winLine.winData.iconName)
                                                                        font.family: contentColumn.fontFamily
                                                                        font.pixelSize: Commons.Style.font.iconSmall
                                                                        color: winLine.kind === "browser" ? contentColumn.accent : contentColumn.fg
                                                                        opacity: winLine.kind === "browser" ? 0.9 : 0.72
                                                                    }
                                                                }

                                                                Text {
                                                                    textFormat: Text.PlainText
                                                                    text: winLine.primaryText
                                                                    font.family: contentColumn.fontFamily
                                                                    font.pixelSize: Commons.Style.font.bodySmall
                                                                    font.weight: winLine.kind === "browser" ? Font.Normal : Font.Medium
                                                                    color: contentColumn.fg
                                                                    elide: Text.ElideRight
                                                                    Layout.fillWidth: winLine.detailText === ""
                                                                    Layout.maximumWidth: winLine.detailText === "" ? -1 : Commons.Style.space(130)
                                                                }

                                                                Text {
                                                                    visible: winLine.detailText !== ""
                                                                    Layout.fillWidth: true
                                                                    textFormat: Text.PlainText
                                                                    text: "· " + winLine.detailText
                                                                    font.family: contentColumn.fontFamily
                                                                    font.pixelSize: Commons.Style.font.bodySmall
                                                                    color: contentColumn.fg
                                                                    opacity: 0.62
                                                                    elide: Text.ElideRight
                                                                }
                                                            }
                                                        }

                                                        Text {
                                                            visible: wsMouseArea.winList.length > panel.maxWindowsPerWorkspace
                                                            textFormat: Text.PlainText
                                                            text: "+" + (wsMouseArea.winList.length - panel.maxWindowsPerWorkspace) + " more"
                                                            font.family: contentColumn.fontFamily
                                                            font.pixelSize: Commons.Style.font.caption
                                                            color: contentColumn.fg
                                                            opacity: 0.45
                                                        }

                                                        Text {
                                                            visible: !wsMouseArea.hasWindows
                                                            textFormat: Text.PlainText
                                                            text: "empty"
                                                            font.family: contentColumn.fontFamily
                                                            font.pixelSize: Commons.Style.font.caption
                                                            color: contentColumn.fg
                                                            opacity: 0.3
                                                            font.italic: true
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    Item {
                                        visible: monitorCol.monitorData.workspaces.length === 0
                                        width: parent.width
                                        height: Commons.Style.space(60)

                                        Text {
                                            anchors.centerIn: parent
                                            textFormat: Text.PlainText
                                            text: "No workspaces"
                                            font.family: contentColumn.fontFamily
                                            font.pixelSize: Commons.Style.font.caption
                                            color: contentColumn.fg
                                            opacity: 0.4
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Item {
                        anchors.fill: parent
                        visible: panel.workspaceData.length === 0

                        Column {
                            anchors.centerIn: parent
                            spacing: Commons.Style.space(8)

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                textFormat: Text.PlainText
                                text: "󰹑"
                                font.family: contentColumn.fontFamily
                                font.pixelSize: Commons.Style.font.displayLarge
                                color: contentColumn.fg
                                opacity: 0.4
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                textFormat: Text.PlainText
                                text: "No monitors found"
                                font.family: contentColumn.fontFamily
                                font.pixelSize: Commons.Style.font.body
                                color: contentColumn.fg
                                opacity: 0.6
                            }
                        }
                    }
                }
            }
        }
    }
}