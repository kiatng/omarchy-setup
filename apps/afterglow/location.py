"""Where is this laptop? IP location, cross-checked against nearby Wi-Fi.

IP geolocation follows the network's exit point, so a roaming phone hotspot or
a VPN reports the wrong country. Wi-Fi beacons carry a regulatory country code
(an Android hotspot advertises the country its SIM is registered in), which is
local radio evidence that routing cannot change. resolve() trusts the IP city
only while that evidence does not contradict it, and otherwise falls back to:

  1. a place remembered from a trusted network whose access points are in range
  2. Wi-Fi positioning (BeaconDB), when enabled
  3. the most recent remembered place in the beacon country
  4. a city from the user's Elsewhen list (shell.json) in that country
  5. the country's principal city from the system timezone table
  6. the IP city, marked unverified

Nothing here names a particular city. All calls block: use a worker thread.
"""
import json
import math
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import unicodedata
from zoneinfo import ZoneInfo

GEO_URL = "https://geo.omarchy.org/"
BEACONDB_URL = "https://api.beacondb.net/v1/geolocate"
MAX_BYTES = 131072
MAX_PLACES = 30
MAX_PLACE_APS = 40
FRESH_SCAN_MS = 90_000
ZONE_TAB = Path("/usr/share/zoneinfo/zone.tab")
COUNTRY_TAB = Path("/usr/share/zoneinfo/iso3166.tab")
SHELL_CONFIG = Path.home() / ".config/omarchy/shell.json"
MAC = re.compile(r"[0-9a-f]{2}(?::[0-9a-f]{2}){5}")


def _state_dir():
    return Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local/state"))) / "omarchy"


PLACES_PATH = _state_dir() / "ambient-places.json"


# ---------------------------------------------------------------- validation
def text(value, limit=80):
    if not isinstance(value, str) or len(value) > limit:
        raise ValueError("Invalid text")
    value = " ".join("".join(c for c in value if not unicodedata.category(c).startswith("C")).split())
    if not value:
        raise ValueError("Empty text")
    return value


def number(value, low, high):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("Invalid number")
    if not low <= value <= high or not math.isfinite(value):
        raise ValueError("Number outside bounds")
    return value


def zone(value):
    if not isinstance(value, str) or len(value) > 100 or not re.fullmatch(r"[A-Za-z0-9_+.-]+(?:/[A-Za-z0-9_+.-]+)*", value):
        raise ValueError("Invalid timezone")
    ZoneInfo(value)
    return value


def place(value):
    """Validate a location from any source (network, cache, or config)."""
    if not isinstance(value, dict):
        raise ValueError("Invalid location")
    country = value.get("country")
    if not isinstance(country, str) or not re.fullmatch("[A-Z]{2}", country):
        raise ValueError("Invalid country")
    timezone = value.get("timezone")
    return {
        "name": text(value.get("name")), "country": country,
        "latitude": number(value.get("latitude"), -90, 90),
        "longitude": number(value.get("longitude"), -180, 180),
        "timezone": zone(timezone) if timezone is not None else None,
    }


def json_request(url, body=None):
    command = ["curl", "-q", "-fsS", "--proto", "=https", "--connect-timeout", "3",
               "--max-time", "8", "--max-filesize", str(MAX_BYTES), "-A", "ambient-screensaver"]
    if body is not None:
        command += ["-H", "Content-Type: application/json", "--data-binary", json.dumps(body)]
    result = subprocess.run(command + [url], capture_output=True, timeout=10, check=True)
    if len(result.stdout) > MAX_BYTES:
        raise ValueError("Oversized response")
    value = json.loads(result.stdout)
    if not isinstance(value, dict):
        raise ValueError("Expected object")
    return value


def distance_km(a, b):
    lat1, lon1, lat2, lon2 = map(math.radians, (a["latitude"], a["longitude"], b["latitude"], b["longitude"]))
    h = math.sin((lat2 - lat1) / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin((lon2 - lon1) / 2) ** 2
    return 12742 * math.asin(min(1, math.sqrt(h)))


# ------------------------------------------------------------ Wi-Fi evidence
def _run(command, timeout=6):
    return subprocess.run(command, capture_output=True, timeout=timeout, check=True).stdout.decode("utf-8", "replace")


def wireless_interface():
    try:
        for entry in sorted(Path("/sys/class/net").iterdir()):
            if (entry / "wireless").exists() and re.fullmatch(r"[A-Za-z0-9_.-]{1,15}", entry.name):
                return entry.name
    except OSError:
        pass
    return None


def parse_scan(dump):
    """Parse `iw dev <if> scan dump` into access points."""
    aps, current = [], None
    for line in dump.splitlines():
        head = re.match(r"BSS ([0-9a-f:]{17})\(on [^)]*\)(.*)", line)
        if head:
            current = {"bssid": head.group(1), "associated": "associated" in head.group(2),
                       "signal": None, "frequency": None, "country": None, "age_ms": None}
            aps.append(current)
            continue
        if current is None:
            continue
        line = line.strip()
        try:
            if line.startswith("signal:"):
                current["signal"] = max(-120, min(0, round(float(line.split()[1]))))
            elif line.startswith("freq:"):
                current["frequency"] = int(float(line.split()[1]))
            elif line.startswith("last seen:") and line.endswith("ms ago"):
                current["age_ms"] = int(line.split()[2])
            elif line.startswith("Country:"):
                code = line.split()[1]
                if re.fullmatch("[A-Z]{2}", code):
                    current["country"] = code
        except (ValueError, IndexError):
            continue
    return [ap for ap in aps if MAC.fullmatch(ap["bssid"])][:200]


def _mobile(ap):
    # Locally administered addresses are randomised (phone hotspots, guest
    # radios): useless as a landmark and nobody's business upstream.
    return ap["bssid"][1] in "26ae"


def beacon_country(aps):
    """Most-advertised country code among access points heard recently."""
    votes = {}
    for ap in aps:
        if ap["country"] and (ap["associated"] or (ap["age_ms"] is not None and ap["age_ms"] <= FRESH_SCAN_MS)):
            votes[ap["country"]] = votes.get(ap["country"], 0) + (2 if ap["associated"] else 1)
    if not votes:
        return None
    best = max(votes.values())
    winners = sorted(code for code, count in votes.items() if count == best)
    return winners[0] if len(winners) == 1 else None


def wifi_evidence(rescan=True):
    """Nearby access points, their consensus country, and whether we are tethered."""
    evidence = {"aps": [], "country": None, "metered": False, "interface": wireless_interface()}
    iface = evidence["interface"]
    if not iface:
        return evidence
    try:
        aps = parse_scan(_run(["iw", "dev", iface, "scan", "dump"]))
        others = [ap["age_ms"] for ap in aps if not ap["associated"] and ap["age_ms"] is not None]
        if rescan and (not others or min(others) > FRESH_SCAN_MS):
            subprocess.run(["nmcli", "device", "wifi", "rescan", "ifname", iface], capture_output=True, timeout=6)
            time.sleep(4)
            aps = parse_scan(_run(["iw", "dev", iface, "scan", "dump"]))
        evidence["aps"] = aps
        evidence["country"] = beacon_country(aps)
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        metered = _run(["nmcli", "-t", "-f", "GENERAL.METERED", "device", "show", iface])
        evidence["metered"] = metered.split(":", 1)[-1].strip().startswith("yes")
    except (OSError, subprocess.SubprocessError):
        pass
    return evidence


def landmarks(evidence):
    """Fixed access points in range, strongest first."""
    fresh = [ap for ap in evidence["aps"] if not _mobile(ap) and not (ap["associated"] and evidence["metered"])
             and (ap["associated"] or (ap["age_ms"] is not None and ap["age_ms"] <= FRESH_SCAN_MS))]
    return sorted(fresh, key=lambda ap: ap["signal"] if ap["signal"] is not None else -200, reverse=True)


# ------------------------------------------------------------- places store
def load_places(path=None):
    path = Path(path) if path is not None else PLACES_PATH
    try:
        with path.open("rb") as handle:
            raw = handle.read(MAX_BYTES + 1)
        value = json.loads(raw) if len(raw) <= MAX_BYTES else None
        places = []
        for item in value["places"][:MAX_PLACES]:
            try:
                entry = place(item)
                entry["bssids"] = [b for b in item.get("bssids", []) if isinstance(b, str) and MAC.fullmatch(b)][:MAX_PLACE_APS]
                entry["seen_at"] = number(item.get("seen_at"), 0, 4e9)
                places.append(entry)
            except (ValueError, TypeError, AttributeError):
                continue
        return places
    except (OSError, ValueError, KeyError, TypeError):
        return []


def remember_place(geo, evidence, path=None, now=None):
    """Record a trusted location with the fixed access points around it."""
    path = Path(path) if path is not None else PLACES_PATH
    now = time.time() if now is None else now
    places = load_places(path)
    seen = [ap["bssid"] for ap in landmarks(evidence)]
    match = next((p for p in places if p["country"] == geo["country"] and distance_km(p, geo) <= 15), None)
    if match:
        places.remove(match)
        seen += [b for b in match["bssids"] if b not in seen]
    places.insert(0, dict(geo, bssids=seen[:MAX_PLACE_APS], seen_at=now))
    raw = json.dumps({"version": 1, "places": places[:MAX_PLACES]}, ensure_ascii=True, allow_nan=False)
    temporary = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, prefix=".ambient-places-", delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(raw)
        temporary.replace(path)
    except OSError:
        pass
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


# ------------------------------------------------------- offline references
def country_name(code):
    try:
        for line in COUNTRY_TAB.read_text().splitlines():
            if line.startswith(code + "\t"):
                return text(line.split("\t", 1)[1], 60)
    except (OSError, ValueError):
        pass
    return code


def _zone_rows():
    try:
        for line in ZONE_TAB.read_text().splitlines():
            if line.startswith("#"):
                continue
            parts = line.split("\t")
            match = re.fullmatch(r"([+-])(\d\d)(\d\d)(\d\d)?([+-])(\d\d\d)(\d\d)(\d\d)?", parts[1]) if len(parts) >= 3 else None
            if not match:
                continue
            g = match.groups()
            latitude = (int(g[1]) + int(g[2]) / 60 + int(g[3] or 0) / 3600) * (-1 if g[0] == "-" else 1)
            longitude = (int(g[5]) + int(g[6]) / 60 + int(g[7] or 0) / 3600) * (-1 if g[4] == "-" else 1)
            yield parts[0], parts[2], round(latitude, 4), round(longitude, 4)
    except OSError:
        return


def zone_country(name):
    return next((code for code, zone_name, _, _ in _zone_rows() if zone_name == name), None)


def principal_city(code):
    """The first timezone city the tz database lists for a country."""
    for country, zone_name, latitude, longitude in _zone_rows():
        if country == code:
            try:
                return place({"name": zone_name.rsplit("/", 1)[-1].replace("_", " "), "country": code,
                              "latitude": latitude, "longitude": longitude, "timezone": zone_name})
            except ValueError:
                continue
    return None


def elsewhen_cities(config_path=None):
    """Cities with coordinates from the user's own Elsewhen bar widget."""
    found = []
    try:
        config = json.loads((Path(config_path) if config_path is not None else SHELL_CONFIG).read_text())
        for widgets in config["bar"]["layout"].values():
            for widget in widgets:
                if not isinstance(widget, dict) or not isinstance(widget.get("cities"), list):
                    continue
                for city in widget["cities"][:40]:
                    try:
                        code = zone_country(zone(city["zone"]))
                        found.append(place({"name": city["name"], "country": code, "latitude": city["lat"],
                                            "longitude": city["lon"], "timezone": city["zone"]}))
                    except (ValueError, KeyError, TypeError):
                        continue
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        pass
    return found


# ------------------------------------------------------------------ resolve
def wifi_position(evidence):
    """Ask BeaconDB where these fixed access points are. Returns lat/lon or None."""
    aps = landmarks(evidence)[:12]
    if len(aps) < 2:
        return None
    body = {"considerIp": False, "wifiAccessPoints": [
        {"macAddress": ap["bssid"], **({"signalStrength": ap["signal"]} if ap["signal"] is not None else {})} for ap in aps]}
    try:
        answer = json_request(BEACONDB_URL, body)
        if number(answer.get("accuracy", 0), 0, 1e7) > 20000:
            return None
        return {"latitude": number(answer["location"]["lat"], -90, 90),
                "longitude": number(answer["location"]["lng"], -180, 180)}
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError):
        return None


def _tag(location, source, note, ip=None):
    return dict(location, source=source, note=note, ip_city=ip["name"] if ip else None,
                ip_country=ip["country"] if ip else None)


def resolve(wifi_positioning=True, places_path=None, config_path=None, evidence=None):
    """Best available location as a validated place plus `source` and `note`.

    Returns None only when no source can say anything (for example offline at
    an unfamiliar spot with no country beacons).
    """
    evidence = wifi_evidence() if evidence is None else evidence
    try:
        ip = place(json_request(GEO_URL))
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError):
        ip = None
    beacons = evidence["country"]
    tethered = evidence["metered"]
    contradicted = bool(ip and beacons and ip["country"] != beacons)

    if ip and not contradicted and not (tethered and beacons is None):
        if not tethered:
            remember_place(ip, evidence, places_path)
        return _tag(ip, "ip", "nearby Wi-Fi agrees" if beacons else "network location", ip)

    why = (f"IP says {ip['name']}, {ip['country']}; nearby Wi-Fi says {beacons}" if contradicted
           else "phone hotspot, IP location unconfirmed" if ip else "no network location")
    places = load_places(places_path)
    candidates = [p for p in places if beacons is None or p["country"] == beacons]
    here = {ap["bssid"] for ap in landmarks(evidence)}
    overlap = max(candidates, key=lambda p: len(here & set(p["bssids"])), default=None)
    if overlap and here & set(overlap["bssids"]):
        return _tag(place(overlap), "wifi-known", "recognised nearby Wi-Fi · " + why, ip)

    if wifi_positioning and ip is not None:
        fix = wifi_position(evidence)
        if fix:
            known = [c for c in places + elsewhen_cities(config_path) if distance_km(c, fix) <= 40]
            nearest = min(known, key=lambda c: distance_km(c, fix), default=None)
            country = beacons or (nearest["country"] if nearest else None)
            if country:
                name = nearest["name"] if nearest else (principal_city(country) or {}).get("name", country_name(country))
                located = place({"name": name, "country": country, "timezone": nearest["timezone"] if nearest else None, **fix})
                return _tag(located, "wifi-position", "Wi-Fi positioning · " + why, ip)

    if beacons:
        if candidates:
            return _tag(place(max(candidates, key=lambda p: p["seen_at"])), "last-known",
                        "last trusted place in " + country_name(beacons) + " · " + why, ip)
        listed = next((c for c in elsewhen_cities(config_path) if c["country"] == beacons), None)
        if listed:
            return _tag(listed, "elsewhen", "your Elsewhen city in " + country_name(beacons) + " · " + why, ip)
        capital = principal_city(beacons)
        if capital:
            return _tag(capital, "country", "approximate, " + country_name(beacons) + " · " + why, ip)
    if ip:
        return _tag(ip, "ip-unverified", why, ip)
    return None


if __name__ == "__main__":
    import sys
    seen = wifi_evidence()
    print(json.dumps({"location": resolve(evidence=seen), "beacon_country": seen["country"],
                      "metered": seen["metered"], "access_points": len(seen["aps"]),
                      "landmarks": len(landmarks(seen))}, indent=2), file=sys.stdout)
