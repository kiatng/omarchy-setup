# Install on another computer

Run commands from the repository root.

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


See [components](components.md) for usage and [privacy](privacy.md) before enabling network features.
