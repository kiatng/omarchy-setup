#!/usr/bin/env python3
"""Exercise the staged launcher with fake terminals and Hyprland."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from generate_launcher import generate


class LauncherTest(unittest.TestCase):
    def test_terminal_dispatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binaries = root / "bin"
            binaries.mkdir()
            launcher = root / "launch"
            runner = root / "a directory" / "screensaver.py"
            generate((Path(__file__).resolve().parents[2] / "tests/fixtures/omarchy-launch-screensaver"), launcher, runner)
            commands = {
                "pgrep": "exit 1",
                "omarchy-toggle-enabled": "exit 1",
                "omarchy-hyprland-monitor-focused": "echo TEST",
                "xdg-terminal-exec": 'echo "$TEST_TERMINAL"',
                "socat": "echo 'openwindow>>1,2,org.omarchy.screensaver,screen'",
                "hyprctl": '''if [[ $1 == monitors ]]; then echo '[{"name":"TEST","specialWorkspace":{"name":""}}]';
elif [[ $1 == clients ]]; then echo '[]';
elif [[ $1 == dispatch && $2 == hl.dsp.exec_cmd* ]]; then printf '%s\\n' "$2" >> "$TEST_LOG"; fi''',
            }
            for name, body in commands.items():
                path = binaries / name
                path.write_text("#!/bin/bash\n" + body + "\n")
                path.chmod(0o755)
            env = dict(os.environ, PATH=f"{binaries}:{os.environ['PATH']}",
                       OMARCHY_PATH="/usr/share/omarchy", XDG_RUNTIME_DIR=tmp,
                       HYPRLAND_INSTANCE_SIGNATURE="test", TEST_LOG=str(root / "log"))
            for terminal in ("Alacritty", "ghostty", "foot", "kitty"):
                with self.subTest(terminal=terminal):
                    (root / "log").unlink(missing_ok=True)
                    subprocess.run([str(launcher), "force"], env={**env, "TEST_TERMINAL": terminal},
                                   check=True, timeout=5)
                    dispatch = (root / "log").read_text()
                    command = dispatch.split("workspace special:screensaver-TEST] ", 1)[1].rsplit("]])", 1)[0]
                    args = shlex.split(command)
                    self.assertEqual(args[0].lower(), terminal.lower())
                    self.assertEqual(args[-3:], ["-e", "python3", str(runner)])

    def test_rejects_unexpected_launcher(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "source"
            source.write_text("-e omarchy-screensaver\n")
            with self.assertRaises(ValueError):
                generate(source, Path(tmp) / "out", Path("/tmp/runner"))


if __name__ == "__main__":
    unittest.main()
