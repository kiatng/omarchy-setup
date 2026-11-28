.pragma library

// Model for Workspace Map - fetches and processes window data from Hyprland
// Uses Quickshell's Hyprland service directly (imported in Panel.qml)

var _history = [];
var _historyIndex = -1;
var _navigatingTo = null;
var _navigatingUntil = 0;
var _maxHistory = 50;

function workspaceIdByName(Hyprland, wsName) {
    var name = String(wsName || "");
    if (!name) return -1;
    var workspaces = Hyprland && Hyprland.workspaces ? Hyprland.workspaces.values : [];
    for (var i = 0; i < workspaces.length; i++) {
        if (String(workspaces[i].name || "") === name) {
            return Number(workspaces[i].id) || -1;
        }
    }
    var parsed = parseInt(name, 10);
    return isFinite(parsed) && parsed > 0 ? parsed : -1;
}

function ensureInitialWorkspace(Hyprland) {
    if (_history.length > 0) return false;
    var mon = Hyprland && Hyprland.focusedMonitor ? Hyprland.focusedMonitor : null;
    var ws = mon && mon.activeWorkspace ? mon.activeWorkspace : null;
    var id = ws && ws.id ? Number(ws.id) : 0;
    if (isFinite(id) && id > 0) {
        _history = [Math.round(id)];
        _historyIndex = 0;
        return true;
    }
    return false;
}

function recordWorkspace(wsId) {
    var id = Number(wsId);
    if (!isFinite(id) || id <= 0) return false;
    id = Math.round(id);

    var now = Date.now();
    if (_navigatingTo !== null) {
        if (now <= _navigatingUntil) {
            if (id === _navigatingTo) {
                _navigatingTo = null;
                _navigatingUntil = 0;
            }
            return false;
        }
        _navigatingTo = null;
        _navigatingUntil = 0;
    }

    if (_historyIndex >= 0 && _history[_historyIndex] === id) {
        return false;
    }

    if (_historyIndex < _history.length - 1) {
        _history = _history.slice(0, _historyIndex + 1);
    }
    _history.push(id);
    if (_history.length > _maxHistory) {
        _history = _history.slice(_history.length - _maxHistory);
    }
    _historyIndex = _history.length - 1;
    return true;
}

function canGoBack() {
    return _historyIndex > 0;
}

function canGoForward() {
    return _historyIndex >= 0 && _historyIndex < _history.length - 1;
}

function stepBack() {
    if (!canGoBack()) return null;
    _historyIndex -= 1;
    var target = _history[_historyIndex];
    _navigatingTo = target;
    _navigatingUntil = Date.now() + 500;
    return target;
}

function stepForward() {
    if (!canGoForward()) return null;
    _historyIndex += 1;
    var target = _history[_historyIndex];
    _navigatingTo = target;
    _navigatingUntil = Date.now() + 500;
    return target;
}

function historySnapshot() {
    return {
        canBack: canGoBack(),
        canForward: canGoForward(),
        index: _historyIndex,
        items: _history.slice()
    };
}

function clientClassOf(client) {
    var ipc = client && client.lastIpcObject ? client.lastIpcObject : null;
    var wayland = client && client.wayland ? client.wayland : null;
    return String(
        (ipc && (ipc.initialClass || ipc.class)) ||
        (wayland && wayland.appId) ||
        (client && (client.initialClass || client.class)) ||
        ""
    );
}

function clientPidOf(client) {
    var ipc = client && client.lastIpcObject ? client.lastIpcObject : null;
    var pid = ipc && ipc.pid ? Number(ipc.pid) : 0;
    return isFinite(pid) && pid > 0 ? String(Math.round(pid)) : "";
}

function getTerminalPids(Hyprland) {
    var clients = Hyprland && Hyprland.toplevels ? Hyprland.toplevels.values : [];
    var pids = [];
    var seen = {};
    for (var i = 0; i < clients.length; i++) {
        var client = clients[i];
        var clientClass = clientClassOf(client);
        if (getAppIconName(client, clientClass) !== "terminal") continue;
        var pid = clientPidOf(client);
        if (pid && !seen[pid]) {
            seen[pid] = true;
            pids.push(pid);
        }
    }
    return pids;
}

function buildWorkspaceData(Hyprland, terminalProcs) {
    var procs = terminalProcs || {};
    var monitors = Hyprland.monitors ? Hyprland.monitors.values : [];
    var workspaces = Hyprland.workspaces ? Hyprland.workspaces.values : [];
    var clients = Hyprland.toplevels ? Hyprland.toplevels.values : [];
    var result = [];

    for (var m = 0; m < monitors.length; m++) {
        var monitor = monitors[m];
        var monitorWorkspaces = getMonitorWorkspaces(workspaces, monitor.name);

        var monitorData = {
            name: monitor.name,
            description: monitor.description || "",
            workspaces: []
        };

        for (var w = 0; w < monitorWorkspaces.length; w++) {
            var ws = monitorWorkspaces[w];
            var wsClients = getClientsForWorkspace(clients, ws.id);
            var windows = [];

            for (var c = 0; c < wsClients.length; c++) {
                var client = wsClients[c];
                var clientClass = clientClassOf(client);
                var iconName = getAppIconName(client, clientClass);
                var title = String(client.title || "");

                if (iconName === "browser") {
                    windows.push({
                        kind: "browser",
                        class: clientClass,
                        iconName: "browser",
                        primary: getAppDisplayName(client, clientClass, iconName),
                        detail: "",
                        title: title,
                        address: client.address || ""
                    });
                } else if (iconName === "terminal") {
                    var pid = clientPidOf(client);
                    var procName = pid && procs[pid] ? String(procs[pid]) : "";
                    var termParts = formatTerminalParts(title, procName);
                    windows.push({
                        kind: "terminal",
                        class: clientClass,
                        iconName: "terminal",
                        primary: termParts.primary,
                        detail: termParts.detail,
                        title: title,
                        address: client.address || ""
                    });
                } else {
                    windows.push({
                        kind: "app",
                        class: clientClass,
                        iconName: iconName,
                        primary: getAppDisplayName(client, clientClass, iconName),
                        detail: title,
                        title: title,
                        address: client.address || ""
                    });
                }
            }

            var slot = ws.id > 0 ? (((ws.id - 1) % 10) + 1) : 0;
            var shortcutKey = slot === 10 ? "0" : String(slot);

            monitorData.workspaces.push({
                id: ws.id,
                name: ws.name,
                shortcut: slot > 0 ? ("Super+" + shortcutKey) : "",
                windows: windows,
                hasWindows: windows.length > 0 || (ws.toplevels && ws.toplevels.values && ws.toplevels.values.length > 0),
                isActive: !!(monitor.activeWorkspace && ws.id === monitor.activeWorkspace.id)
            });
        }

        result.push(monitorData);
    }

    return result;
}

function getMonitorWorkspaces(workspaces, monitorName) {
    var result = [];
    for (var i = 0; i < workspaces.length; i++) {
        var ws = workspaces[i];
        var wsMonitor = ws.monitor;
        if (wsMonitor && typeof wsMonitor === "object") wsMonitor = wsMonitor.name;
        if (wsMonitor === monitorName) {
            result.push(ws);
        }
    }
    // Sort by workspace ID
    result.sort(function(a, b) { return a.id - b.id; });
    return result;
}

function getClientsForWorkspace(clients, workspaceId) {
    var result = [];
    for (var i = 0; i < clients.length; i++) {
        var client = clients[i];
        if (client.workspace && client.workspace.id === workspaceId) {
            result.push(client);
        }
    }
    return result;
}

function getAppIconName(client, resolvedClass) {
    var className = String(resolvedClass !== undefined ? resolvedClass : clientClassOf(client)).toLowerCase();
    var title = String((client && client.title) || "").toLowerCase();

    // Browser detection - show tab titles (check class first, then title patterns)
    if (className === "chromium" || className === "google-chrome" || className === "chrome" ||
        className === "firefox" || className === "brave-browser" || className === "brave" ||
        className === "vivaldi-stable" || className === "vivaldi" || className === "microsoft-edge" ||
        className === "edge" || className === "zen" || className === "helium") {
        return "browser";
    }
    // Fallback: detect browser from title patterns
    if (title.includes(" - chromium") || title.includes(" - chrome") ||
        title.includes(" - firefox") || title.includes(" - brave") ||
        title.endsWith(" - google chrome") || title.endsWith(" - mozilla firefox")) {
        return "browser";
    }

    // Terminal detection - foot, alacritty, kitty, ghostty, etc.
    if (className === "foot" || className === "alacritty" || className === "kitty" ||
        className === "com.mitchellh.ghostty" || className === "ghostty" ||
        className === "org.wezfurlong.wezterm" || className === "wezterm" ||
        className === "gnome-terminal" || className === "konsole") {
        return "terminal";
    }
    // Fallback: detect terminal from title patterns (user@host, bash, zsh, etc.)
    if (title.match(/^[\w-]+@[\w.-]+:.*/) ||
        title.match(/^(bash|zsh|fish|sh)\s*$/) ||
        title.startsWith("kiat@") || title.startsWith("root@")) {
        return "terminal";
    }

    // Code editors
    if (className === "code" || className === "code-url-handler" || className === "vscode" || className === "cursor") {
        return "code";
    }
    if (title.includes(" - visual studio code") || title.includes(" - code") ||
        title.includes(" - vscode") || title.includes(" - cursor")) {
        return "code";
    }

    // Discord
    if (className === "discord" || className === "vesktop" || title.includes("discord")) {
        return "discord";
    }

    return className;
}

function getAppDisplayName(client, resolvedClass, resolvedIcon) {
    var className = String(resolvedClass !== undefined ? resolvedClass : clientClassOf(client));
    var title = String((client && client.title) || "");
    var iconName = resolvedIcon !== undefined ? resolvedIcon : getAppIconName(client, className);

    // Browser - extract tab title
    if (iconName === "browser") {
        if (title.includes(" - ")) {
            var parts = title.split(" - ");
            parts.pop();
            return parts.join(" - ") || title;
        }
        return title || "Browser";
    }

    // Terminal - extract running command
    if (iconName === "terminal") {
        return parseTerminalTitle(title);
    }

    // Code editors
    if (iconName === "code") {
        if (title.includes(" - ")) {
            var codeParts = title.split(" - ");
            return codeParts[0];
        }
        return title || "Code";
    }

    // Discord
    if (iconName === "discord") {
        return "Discord";
    }

    return className || title || "Unknown";
}

function formatTerminalParts(title, procName) {
    var proc = String(procName || "").trim();
    var cleaned = String(title || "").trim();
    if (!proc) return { primary: parseTerminalTitle(cleaned), detail: "" };

    var lowerProc = proc.toLowerCase();
    var isShell = lowerProc === "bash" || lowerProc === "zsh" || lowerProc === "fish" || lowerProc === "sh";

    var promptMatch = /^[\w-]+@[\w.-]+:(.*)$/.exec(cleaned);
    if (promptMatch) {
        var path = String(promptMatch[1] || "").trim();
        if (path.length > 32) path = "..." + path.substring(path.length - 29);
        if (isShell) return { primary: proc, detail: path };
        return { primary: proc, detail: (path && path !== "~") ? path : "" };
    }

    if (lowerProc === "copilot") {
        cleaned = cleaned.replace(/\s*-\s*GitHub Copilot$/i, "").trim();
    }

    var escaped = proc.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    var procSuffix = new RegExp("^(.*?)\\s+-\\s+" + escaped + "(?:\\s+-\\s+.*)?$", "i");
    var suffixMatch = procSuffix.exec(cleaned);
    if (suffixMatch && suffixMatch[1]) {
        cleaned = suffixMatch[1].trim();
    }

    if (!cleaned || cleaned.toLowerCase() === lowerProc) {
        return { primary: proc, detail: "" };
    }
    return { primary: proc, detail: cleaned };
}

function formatTerminalLabel(title, procName) {
    var parts = formatTerminalParts(title, procName);
    return parts.detail ? (parts.primary + " · " + parts.detail) : parts.primary;
}

function parseTerminalTitle(title) {
    if (!title) return "Terminal";

    var cleaned = title;

    // Check for SSH patterns: user@host:path
    if (cleaned.includes("@")) {
        var atIndex = cleaned.indexOf("@");
        var afterAt = cleaned.substring(atIndex + 1);
        if (afterAt.includes(":")) {
            var colonIndex = afterAt.indexOf(":");
            var host = afterAt.substring(0, colonIndex);
            var path = afterAt.substring(colonIndex + 1);
            if (path.startsWith("~")) path = path.substring(1);
            return "SSH: " + host + path;
        }
        return "SSH: " + afterAt.split(" ")[0];
    }

    // Check for running commands (Code CLI, Herdr, Amp, etc.)
    var knownCommands = ["code", "herdr", "amp", "nvim", "vim", "lazygit", "btop", "htop", "git", "npm", "yarn", "pnpm", "cargo", "go", "python", "node", "bun", "deno", "claude", "gemini", "copilot"];
    for (var i = 0; i < knownCommands.length; i++) {
        var cmd = knownCommands[i];
        var re = new RegExp("(^|\\W)" + cmd + "(\\W|$)", "i");
        if (re.test(cleaned)) {
            return cmd.charAt(0).toUpperCase() + cmd.slice(1);
        }
    }

    // Check for bash/zsh/fish with directory: user@host:~ or user@host:/path
    if (cleaned.includes("~") || cleaned.includes("/")) {
        var parts = cleaned.split(" ");
        var lastPart = parts[parts.length - 1];
        if (lastPart.includes("/") || lastPart.startsWith("~")) {
            if (lastPart.length > 30) {
                lastPart = "..." + lastPart.substring(lastPart.length - 27);
            }
            return lastPart;
        }
    }

    if (cleaned.match(/^[\w-]+@[\w.-]+:.*$/)) {
        return cleaned;
    }

    return "Terminal";
}