#!/usr/bin/env python3
"""Stage a user-owned screensaver launcher based on the installed Omarchy launcher."""
import argparse
from pathlib import Path
import shlex


def generate(source: Path, destination: Path, runner: Path) -> None:
    if not runner.is_absolute():
        raise ValueError("runner path must be absolute")
    script = source.read_text()
    token = "-e omarchy-screensaver"
    if script.count(token) != 4:
        raise ValueError("Unexpected Omarchy launcher: expected four terminal invocations")
    script = script.replace(token, "-e python3 " + shlex.quote(str(runner)))
    for original, name in [("$OMARCHY_PATH/default/alacritty/screensaver.toml", "alacritty.toml"),
                           ("$OMARCHY_PATH/default/ghostty/screensaver", "ghostty.conf"),
                           ("$OMARCHY_PATH/default/foot/screensaver.ini", "foot.ini")]:
        script = script.replace('"' + original + '"', shlex.quote(str(runner.parent / name)))
    script = script.replace("--font-size=18", "--font-size=10").replace("font_size=18", "font_size=10")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(script)
    destination.chmod(0o755)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    parser.add_argument("runner", type=Path, help="absolute path to the installed Python screensaver")
    parser.add_argument("--source", type=Path, default=Path("/usr/share/omarchy/bin/omarchy-launch-screensaver"))
    args = parser.parse_args()
    generate(args.source, args.destination, args.runner)
