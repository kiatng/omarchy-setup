#!/usr/bin/python3
"""Read the latest cached Copilot quota limits."""
import glob
import json
import os
import sys

sys.dont_write_bytecode = True


def main() -> int:
    cache_dir = os.path.join(
        os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"),
        "omarchy",
        "agent-usage",
    )
    best = None
    for path in glob.glob(os.path.join(cache_dir, "copilot-limits-*.json")):
        try:
            with open(path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
            if isinstance(data, dict) and data.get("available"):
                if best is None or float(data.get("fetchedAtMs") or 0) > float(best.get("fetchedAtMs") or 0):
                    best = data
        except Exception:
            pass
    print(json.dumps(best or {}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
