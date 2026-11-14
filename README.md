# Omarchy setup

Portable, MIT-licensed desktop customizations for Omarchy: **Afterglow** (animated clock/weather screensaver), **Vigil** (task-aware keep-awake), custom clock/weather/network/notification widgets, and optional **Relay** captions and agent-usage widgets.

This is a curated setup, not a copy of a home directory. It contains no account stores, passwords, API keys, saved networks, personal locations, histories, caches, or desktop screenshots. Existing local secrets are never uploaded by these tools.

## On another computer

Install Omarchy first. These customizations target the Omarchy 4.5 / Quickshell layout; a different release may need adjustments. Monitor, keyboard-device, and other hardware configuration is intentionally left to the destination computer.

```sh
git clone https://github.com/kiatng/omarchy-setup.git
cd omarchy-setup
./bin/setup plan
# Review packages.json, then install missing dependencies reported by install.
./bin/setup install --agent-guidance
```

Run as your regular desktop user. Building packages uses `makepkg`; package installation requests administrator authentication. `install` builds Vigil, backs up and merges the desktop configuration, enables its user service, and restarts the shell. It does not toggle keep-awake, suspend, or lock the computer. A fresh Vigil session starts Off. A locked desktop may defer/refuse the shell restart; retry after unlocking.

Add `--relay` to build Relay and add its widget. Configure its provider credentials separately using the app's keyring integration. Add `--agents` to include the agent-usage widget: it reads your locally configured account credentials at runtime to query provider usage; no credentials are bundled. Superwhisper is not redistributed; install it separately if wanted.

`--agent-guidance` adds a short managed Vigil snippet and skill to Codex and Claude. Other PC controls remain under the existing `omarchy` CLI. Application source and build instructions are under `apps/`.

## Backup and restore private preferences

```sh
./bin/setup backup --agents --relay
# Prints a local backup directory under ~/.local/state/omarchy-setup/backups/
./bin/setup restore --from /path/to/that/backup
```

Backups are private, local, allowlisted copies; keep them off GitHub and transfer them through your own secure backup channel. They preserve customized widget code, the shell layout, Afterglow preferences, and Vigil’s battery reserve. Restore verifies checksums, rejects unexpected paths/symlinks, and creates a rollback backup first. When moving between usernames, it rebases old home-directory paths. It does not migrate credentials, hardware configuration, or active tasks.

On a new machine, run `install` first, then restore your private backup. Restart the shell when unlocked. Agent instructions and packaged applications are reinstalled from source, not restored from account directories. Hardware-specific app preferences should be checked on the destination.

`apply --home /tmp/example-home` writes only configuration to an alternate home and starts no services. It is useful for reviewing a restore. `plan` makes no changes. Existing layout settings and unrelated configuration keys are preserved; stock widgets with bundled replacements are switched to the custom versions.

## Defaults and privacy

- Home clock: UTC. Set `HOME_ZONE` in your installed `kiat.clock/clock.py` for your own home timezone.
- Afterglow: weather and Wi-Fi positioning start **off**. Opt in through `~/.config/omarchy/ambient/settings.json`. Weather/location features contact Omarchy GeoIP and Open-Meteo; Wi-Fi positioning additionally sends nearby access-point identifiers to BeaconDB when enabled.
- The clock's explicit Detect action uses Omarchy GeoIP. The weather widget uses its normal Omarchy/Open-Meteo location behavior. These features can disclose the destination machine's location when used.
- No blanket export of `~/.config`, `~/.codex`, or `~/.claude`; no system-wide files are overwritten.

## Maintenance

Edit the curated sources here, test, then reinstall the affected component. Do not copy new home-directory files into this repository wholesale. Private backups are not a publication source.

```sh
./bin/test
./bin/audit /path/to/gitleaks
```

The audit checks the publishable tree for credentials and unexpected/private artifact types. Review source changes before pushing; automated scanning cannot prove that arbitrary text is non-private. CI runs the portable Python tests and the publication checks. Qt tests run locally with `apps/vigil/bin/test` and `apps/relay/bin/test`.

See [THIRD_PARTY.md](THIRD_PARTY.md) for attribution. Provider marks remain their respective owners' trademarks; their inclusion does not imply endorsement.
