import QtQuick
import Quickshell
import Quickshell.Hyprland
import qs.Commons
import qs.Ui
import "Model.js" as Model

BarWidget {
    id: root
    moduleName: "kiat.workspace-map"

    readonly property var hostWindow: root.QsWindow.window
    readonly property string screenName: hostWindow && hostWindow.screen ? hostWindow.screen.name : ""
    readonly property string instanceId: "ws-map-" + (screenName || "unknown")
    readonly property bool pointerHovered: pointerHover.hovered
    readonly property bool opened: panelLoader.item ? panelLoader.item.opened === true : false
    property bool pendingOpen: false
    property bool canGoBack: false
    property bool canGoForward: false

    function syncHistory() {
        var snap = Model.historySnapshot()
        canGoBack = snap.canBack
        canGoForward = snap.canForward
        if (panelLoader.item && panelLoader.item.syncHistory) {
            panelLoader.item.syncHistory()
        }
    }

    function goBack() {
        var target = Model.stepBack()
        root.broadcast("syncHistory")
        if (target !== null) {
            Hyprland.dispatch('hl.dsp.focus({ workspace = "' + target + '" })')
        }
    }

    function goForward() {
        var target = Model.stepForward()
        root.broadcast("syncHistory")
        if (target !== null) {
            Hyprland.dispatch('hl.dsp.focus({ workspace = "' + target + '" })')
        }
    }

    Component.onCompleted: {
        Model.ensureInitialWorkspace(Hyprland)
        syncHistory()
    }

    property int pendingHistoryWsId: -1

    Timer {
        id: historyDebounce
        interval: 60
        onTriggered: {
            if (root.pendingHistoryWsId > 0) {
                Model.recordWorkspace(root.pendingHistoryWsId)
                root.pendingHistoryWsId = -1
                root.broadcast("syncHistory")
            }
        }
    }

    Connections {
        target: Hyprland
        function onRawEvent(event) {
            if (!event) return
            var name = String(event.name || "")
            if (name === "workspacev2") {
                var parts = String(event.data || "").split(",")
                var wsId = parseInt(parts[0], 10)
                if (isFinite(wsId) && wsId > 0) {
                    root.pendingHistoryWsId = wsId
                    historyDebounce.restart()
                }
            } else if (name === "focusedmonv2") {
                var monParts = String(event.data || "").split(",")
                var wsName = monParts[1] ? String(monParts[1]) : ""
                var wsId2 = Model.workspaceIdByName(Hyprland, wsName)
                if (wsId2 > 0) {
                    root.pendingHistoryWsId = wsId2
                    historyDebounce.restart()
                }
            }
        }
    }

    function resolveClickTarget() {
        if (pointerHover.hovered) return root
        var items = root.bar && typeof root.bar.moduleWidgets === "function"
            ? root.bar.moduleWidgets(root.moduleName) : [root]
        for (var i = 0; i < items.length; i++) {
            if (items[i] && items[i].pointerHovered) return items[i]
        }
        return root
    }

    function resolveIpcTarget() {
        var items = root.bar && typeof root.bar.moduleWidgets === "function"
            ? root.bar.moduleWidgets(root.moduleName) : [root]
        for (var i = 0; i < items.length; i++) {
            if (items[i] && items[i].opened) return items[i]
        }
        var focused = Hyprland.focusedMonitor ? String(Hyprland.focusedMonitor.name || "") : ""
        if (focused) {
            for (var j = 0; j < items.length; j++) {
                if (items[j] && items[j].screenName === focused) return items[j]
            }
        }
        return root
    }

    function refresh() {
        if (panelLoader.item && panelLoader.item.refresh) panelLoader.item.refresh()
    }

    function open() {
        if (panelLoader.item) {
            panelLoader.item.open()
            pendingOpen = false
        } else {
            pendingOpen = true
        }
    }

    function close() {
        if (panelLoader.item) {
            panelLoader.item.close()
        }
        pendingOpen = false
    }

    function togglePanel() {
        if (panelLoader.item) {
            if (root.opened) panelLoader.item.close()
            else panelLoader.item.open()
        } else {
            pendingOpen = !root.opened
        }
    }

    readonly property real openPanelIndicatorWidth: Math.max(Style.space(10), Math.round(Style.bar.iconSlot * 0.55))
    readonly property real openPanelIndicatorHeight: Math.max(Style.space(10), Math.round(Style.bar.iconSlot * 0.55))

    readonly property bool popoutSwitchClosing: panelLoader.item ? panelLoader.item.popoutSwitchClosing === true : false

    function closeForPopoutSwitch() {
        if (panelLoader.item) panelLoader.item.closeForPopoutSwitch()
    }

    function injectPanel() {
        var target = panelLoader.item
        if (!target) return
        if ("bar" in target) target.bar = root.bar
        if ("settings" in target) target.settings = root.settings
        if ("anchorItem" in target) target.anchorItem = button
        if ("hostWidget" in target) target.hostWidget = root
    }

    implicitWidth: button.implicitWidth
    implicitHeight: button.implicitHeight

    onBarChanged: injectPanel()
    onSettingsChanged: injectPanel()

    HoverHandler {
        id: pointerHover
    }

    Loader {
        id: panelLoader
        active: true
        visible: false
        source: Qt.resolvedUrl("Panel.qml")
        onLoaded: {
            root.injectPanel()
            Qt.callLater(root.injectPanel)
            if (root.pendingOpen) {
                panelLoader.item.open()
                root.pendingOpen = false
            }
        }
    }

    ShellIpc {
        target: "kiat.workspace-map"

        function refresh(): void { root.broadcast("refresh") }
        function open(): void { root.resolveIpcTarget().open() }
        function close(): void { root.resolveIpcTarget().close() }
        function show(): void { root.resolveIpcTarget().open() }
        function hide(): void { root.resolveIpcTarget().close() }
        function toggle(): void { root.resolveIpcTarget().togglePanel() }
        function back(): void { root.goBack() }
        function forward(): void { root.goForward() }
    }

    BarIconButton {
        id: button
        anchors.fill: parent
        bar: root.bar
        text: "\ue900"
        fontFamily: "omarchy"
        tooltipText: "Workspace Map"
        pressFeedbackButtons: Qt.LeftButton

        onPressed: function(mouseButton) {
            if (mouseButton === Qt.LeftButton) {
                root.resolveClickTarget().togglePanel()
            }
        }
    }
}