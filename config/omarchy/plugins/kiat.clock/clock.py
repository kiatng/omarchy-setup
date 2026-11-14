#!/usr/bin/env python3
"""Read the clock; explicitly requested detection uses Omarchy's GeoIP service."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime, timezone
from zoneinfo import ZoneInfo

HOME_ZONE = "UTC"
CACHE = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache"))) / "omarchy/clock-timezone.json"


def valid_zone(value):
    if not isinstance(value, str) or len(value) > 100 or not re.fullmatch(r"[A-Za-z0-9_+.-]+(?:/[A-Za-z0-9_+.-]+)+", value):
        raise ValueError("Invalid timezone")
    ZoneInfo(value)
    return value


def city_label(value):
    if not isinstance(value, str):
        return "Local"
    value = value.strip()
    if not value or len(value) > 64 or any(not c.isprintable() or c in "<>" for c in value):
        return "Local"
    return value


def read_location():
    try:
        data = json.loads(CACHE.read_text())
        return valid_zone(data["timezone"]), city_label(data.get("city"))
    except (OSError, ValueError, KeyError):
        return None, "Local"


def detect_zone():
    result = subprocess.run(
        ["curl", "-fsS", "--connect-timeout", "3", "--max-time", "6",
         "--max-filesize", "16384", "https://geo.omarchy.org/"],
        capture_output=True, timeout=8, check=True,
    )
    if len(result.stdout) > 16384:
        raise ValueError("Location response too large")
    data = json.loads(result.stdout)
    zone = valid_zone(data["timezone"])
    city = city_label(data.get("name"))
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    temporary = CACHE.with_suffix(".tmp")
    temporary.write_text(json.dumps({"timezone": zone, "city": city}) + "\n")
    temporary.replace(CACHE)
    return zone, city


def clock_state(zone, now=None, city="Local"):
    now = now or datetime.now(timezone.utc)
    return {
        "localZone": zone or "",
        "localCity": city_label(city),
        "homeTime": now.astimezone(ZoneInfo(HOME_ZONE)).strftime("%H:%M"),
        "localTime": now.astimezone(ZoneInfo(zone)).strftime("%H:%M") if zone else "",
        "showLocal": bool(zone and ZoneInfo(zone) != ZoneInfo(HOME_ZONE)),
    }


def main():
    zone, city = read_location()
    if "--detect" in sys.argv:
        try:
            zone, city = detect_zone()
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
            print("Timezone detection failed; retaining last location: " + str(type(error).__name__), file=sys.stderr)
    print(json.dumps(clock_state(zone, city=city)))


if __name__ == "__main__":
    main()
