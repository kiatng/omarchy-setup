#!/bin/bash
# Install Afterglow into ~/.config/omarchy/ambient, backing up what is there.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
target="$HOME/.config/omarchy/ambient"
backup="$HOME/.config/omarchy/backups/afterglow/ambient-$(date +%Y%m%d-%H%M%S)"

if [[ -d $target ]]; then
  mkdir -p "$backup"
  cp -a "$target/." "$backup/"
  echo "Backed up $target to $backup"
fi
mkdir -p "$target" "$HOME/.local/bin"
install -m 644 "$source_dir"/{screensaver,weather,location,bigtext}.py "$target/"
install -m 644 "$source_dir"/{alacritty.toml,foot.ini,ghostty.conf} "$target/"
# Keep the user's own settings; only seed them on a first install or an old format.
if ! grep -q '"reveal"' "$target/settings.json" 2>/dev/null; then
  install -m 644 "$source_dir/settings.json" "$target/settings.json"
fi
# The launcher is derived from Omarchy's (generate_launcher.py) and then pointed at
# the terminal configs beside it; an existing one is kept as it is.
[[ -x $target/launch ]] || python3 "$source_dir/generate_launcher.py" "$target/launch" "$target/screensaver.py"
rm -rf "$target/__pycache__"
cat >"$HOME/.local/bin/omarchy-ambient-status" <<EOF
#!/bin/bash
# Explain the last Afterglow screensaver run and the current lock state.
exec python3 "$target/screensaver.py" --status
EOF
chmod 755 "$HOME/.local/bin/omarchy-ambient-status"
echo "Installed. Preview with: omarchy-shell idle preview"
