# Component reference

Use the matching section only. Commands assume installation; source paths link into this repository. Default components install together; optional flags are listed below. Runtime preferences and account data belong on the destination computer.

## Afterglow

- **Use:** the idle screensaver displays the animation, clock, and optional weather. `omarchy-ambient-status` reports its status.
- **Configure:** `~/.config/omarchy/ambient/settings.json`; home timezone/name default to UTC, weather and Wi-Fi positioning default off. Existing settings survive reinstall.
- **Source:** [apps/afterglow](../apps/afterglow). Formerly Colombo; this bundle is the screensaver, not a complete system color theme.

## Vigil

- **Use:** the bar switch or Vigil app controls keep-awake. `vigil status --json` reports state; `vigil --help` documents CLI operations.
- **Agent tasks:** prefer `vigil run --name "Task" -- <command>` for supervised work. For multi-command leases, follow the [Vigil skill](../apps/vigil/integration/SKILL.md). Respect user-disabled requests and release only your own task.
- **Policy:** default reserve 15%, warning 10 points above reserve, 60-second reserve-sleep countdown. External power cancels that countdown. Normal screen locking remains active; other sleep inhibitors are respected. Startup/resume clear holds and disable task requests.
- **Configure:** app settings; reserve persists in `~/.config/vigil/settings.json`. `--agent-guidance` installs minimal Vigil guidance for Codex and Claude.
- **Source:** [apps/vigil](../apps/vigil), including the `kiat.vigil` bar plugin.

## Local and Home Clock

- **Use:** read local/home time in the bar; open the calendar popup. The explicit Detect action uses Omarchy GeoIP.
- **Configure:** `HOME_ZONE` in installed `~/.config/omarchy/plugins/kiat.clock/clock.py`; portable default UTC. Keep personal changes local or in a private backup.
- **Source:** [kiat.clock](../config/omarchy/plugins/kiat.clock).

## My Weather

- **Use:** open the bar weather pill for details and its weather settings form. Uses Omarchy/Open-Meteo location behavior; review [privacy](privacy.md).
- **Source:** [kiat.weather](../config/omarchy/plugins/kiat.weather). Separate from Afterglow’s optional weather display.

## My Network

- **Use:** open the network panel to select Wi-Fi, inspect connectivity, change DNS/band preferences, or open a captive portal’s sign-in page. Connection actions change the active network.
- **Source:** [kiat.network](../config/omarchy/plugins/kiat.network) and [Wi-Fi sign-in helper](../config/omarchy/wifi-signin). Saved networks and passwords are not bundled or migrated.

## My Notifications

- **Use:** receives desktop notifications and supplies popups, history, and Do Not Disturb behavior. Runs as a shell service; no separate launcher.
- **Source:** [kiat.notifications](../config/omarchy/plugins/kiat.notifications). Installer replaces the stock notification service. Notification history is not bundled.

## My Idle

- **Use:** background service for screensaver, locking, and display wake. Vigil supplies the keep-awake control; the installer removes the duplicate stock StayAwake indicator.
- **Configure:** idle timings in `~/.config/omarchy/shell.json`; existing values are preserved, fresh defaults are screensaver 150 seconds and lock 300 seconds. Do not disable locking to keep tasks running.
- **Source:** [kiat.idle](../config/omarchy/plugins/kiat.idle). Replaces the stock idle service.

## My Agents

- **Install:** `--agents`.
- **Use:** open the bar panel for provider/account usage, limit windows, pace, balances, and available reset credits where supported. It also exposes account selection and agent launching. Provider support and fields vary; missing usage is not proof of an unused quota.
- **Limit rows:** each limit window is a seven-cell gas tank, one cell per day of the window, showing the allowance left. The fill cools as it drains (green to half, amber by a quarter, then the theme's urgent colour), and unfilled cells are hollow, so an empty tank reads as empty. A white needle above the tank marks where the clock is in the window: fill level with it is on pace, right of it is under-using, left is over-using. Reset days are three letters and reset times are hh:mm, and every tank is the same length.
- **Configure:** widget settings control providers, refresh interval (default 900 seconds), and optional snapshot aggregation (default Off). Sign in locally; the widget reads local credentials at runtime. Reset-credit display reads availability; it does not spend credits.
- **Privacy:** never commit account stores or usage snapshots. Enabling aggregation writes snapshots to the configured sync folder.
- **Source:** [kiat.agents](../config/omarchy/plugins/kiat.agents).

## Relay

- **Install:** `--relay`.
- **Use:** open Relay from the launcher or its bar widget; configure microphone and provider on this computer before starting captions.
- **Configure:** provider keys through the app’s system-keyring integration. Cloud providers receive captured audio when enabled; install an offline provider separately if desired.
- **Source:** [apps/relay](../apps/relay), including its shell widget. Credentials, recordings, transcripts, and model weights are excluded.
