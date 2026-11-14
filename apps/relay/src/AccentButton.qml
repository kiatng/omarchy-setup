import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// A filled button in the theme accent, with readable text on it. Drawn here
// rather than left to the style, so it looks the same with every theme.
Button {
    id: control
    property bool quiet: false   // a neutral fill, for the button while live
    Material.roundedScale: theme.radius
    Material.foreground: quiet ? "white" : theme.accentForeground
    background: Rectangle {
        implicitWidth: 96
        implicitHeight: 40
        radius: Math.min(theme.radius, height / 2)
        opacity: control.enabled ? 1 : 0.4
        color: {
            const base = control.quiet ? Qt.color("#2c2c31") : Qt.color(theme.accent)
            return control.down ? Qt.darker(base, 1.15) : control.hovered ? Qt.lighter(base, 1.08) : base
        }
    }
}
