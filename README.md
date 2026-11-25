# Omarchy setup

Kiat’s portable desktop customizations for Omarchy: apps, bar widgets, and background services, with one installer and local backup/restore tools. MIT licensed.

This repository contains curated source and neutral defaults. Personal preferences travel separately in private backups; passwords, accounts, and hardware configuration are excluded. Targets the Omarchy 4.5 / Quickshell layout.

## Customizations

| Component | What it does | Install |
| --- | --- | --- |
| [Afterglow (formerly Colombo)](docs/components.md#afterglow) | Animated logo, large clock, and optional weather screensaver. | Default |
| [Vigil](docs/components.md#vigil) | One keep-awake switch for manual and AI tasks, with battery-aware sleep policy. | Default |
| [Local and Home Clock](docs/components.md#local-and-home-clock) | Local/home times and a calendar popup. | Default |
| [My Weather](docs/components.md#my-weather) | Bar weather pill and detailed forecast panel. | Default |
| [My Network](docs/components.md#my-network) | Wi-Fi connections, connection diagnostics, and captive-portal sign-in helper. | Default |
| [My Notifications](docs/components.md#my-notifications) | Grouped notification stacks with inline actions, code and message copy, history, and Do Not Disturb. | Default |
| [My Idle](docs/components.md#my-idle) | Screensaver, lock, and display-wake handling integrated with Vigil. | Default |
| [My Agents](docs/components.md#my-agents) | AI account usage as per-window gas tanks: segmented level, pace needle, prepaid balances, and available reset credits. | `--agents` |
| [Relay](docs/components.md#relay) | Live translated meeting captions with a desktop app and bar widget. | `--relay` |

## Setup tools

| Tool | Purpose |
| --- | --- |
| [Installer and layout integration](docs/install.md) | Build apps, install components, and merge the bar layout. |
| [Private backup and restore](docs/backup-restore.md) | Move allowlisted preferences between computers, with checksum checks and rollback backups. |
| [Agent guidance](AGENTS.md) | Repository workflow; `--agent-guidance` also installs minimal Vigil instructions for Codex and Claude. |

## Get started

Install Omarchy first, then run as your regular desktop user:

```sh
git clone https://github.com/kiatng/omarchy-setup.git
cd omarchy-setup
./bin/setup plan --agents --relay
./bin/setup install --agents --relay --agent-guidance
```

Omit `--agents` or `--relay` if unwanted. Install reports missing dependencies; see [installation](docs/install.md) for details and side effects. Configure provider accounts separately. Superwhisper and machine-specific Hyprland settings are not bundled.

## Guides

[Use components](docs/components.md) · [Install](docs/install.md) · [Back up / restore](docs/backup-restore.md) · [Maintain / extend](docs/maintenance.md) · [Privacy](docs/privacy.md)

Agents: start with [AGENTS.md](AGENTS.md), then read only the relevant guide. The same guides serve human operators.

See [THIRD_PARTY.md](THIRD_PARTY.md) for upstream attribution and provider trademarks.
