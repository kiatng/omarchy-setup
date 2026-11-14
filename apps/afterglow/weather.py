"""Small, bounded Open-Meteo client for the ambient screensaver.

fetch_snapshot() performs blocking I/O: call it from the UI's worker thread.
Temperatures are Celsius, wind km/h, rain mm. Where we are comes from
location.resolve(); no location is guessed here.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
from datetime import datetime, timezone
from urllib.parse import urlencode

import location
from location import number as _number, zone as _zone

WEATHER_URL = "https://api.open-meteo.com/v1/forecast"
MAX_BYTES = location.MAX_BYTES
CACHE_VERSION = 2
SOURCES = {"ip", "ip-unverified", "wifi-known", "wifi-position", "last-known", "elsewhen", "country"}


def _json_request(url):
    return location.json_request(url)


def _timestamp(value):
    if not isinstance(value, str) or len(value) > 40:
        raise ValueError("Invalid timestamp")
    return datetime.fromisoformat(value)


def _where(value):
    """Validate a resolved location, including its provenance labels."""
    geo = location.place(value)
    source = value.get("source", "ip")
    if source not in SOURCES:
        raise ValueError("Unknown location source")
    geo["source"] = source
    geo["note"] = location.text(value["note"], 200) if value.get("note") else ""
    return geo


def _optional(function):
    try:
        return function()
    except (ValueError, KeyError, TypeError, IndexError):
        return None


def _snapshot(geo, data, fetched_at):
    """Validate provider data again when loading it from the local cache."""
    geo = _where(geo)
    fetched = _timestamp(fetched_at)
    if fetched.tzinfo is None:
        raise ValueError("Timestamp must include offset")
    if not isinstance(data, dict):
        raise ValueError("Invalid weather")
    weather_zone = _zone(data.get("timezone"))
    current = data["current"]
    daily = data["daily"]
    observed = _timestamp(current["time"])
    index = daily["time"].index(observed.date().isoformat())

    def code_of(value):
        value = _number(value, 0, 99)
        if int(value) != value:
            raise ValueError("Invalid condition")
        return int(value)

    def day(at):
        return {
            "date": _timestamp(daily["time"][at]).date().isoformat(),
            "min": _number(daily["temperature_2m_min"][at], -100, 70),
            "max": _number(daily["temperature_2m_max"][at], -100, 70),
            "rain": _number(daily["rain_sum"][at], 0, 3000),
            "chance": _number(daily["precipitation_probability_max"][at], 0, 100),
            "code": _optional(lambda: code_of(daily["weather_code"][at])),
            "uv": _optional(lambda: _number(daily["uv_index_max"][at], 0, 30)),
            "sunrise": _optional(lambda: _timestamp(daily["sunrise"][at]).strftime("%H:%M")),
            "sunset": _optional(lambda: _timestamp(daily["sunset"][at]).strftime("%H:%M")),
        }

    is_day = _number(current["is_day"], 0, 1)
    if int(is_day) != is_day:
        raise ValueError("Invalid condition")
    snapshot = {
        "city": geo["name"], "country": geo["country"],
        "country_name": location.country_name(geo["country"]),
        # The clock follows the place; a place found by radio alone has no
        # zone of its own, so the forecast grid's zone stands in.
        "zone": geo["timezone"] or weather_zone, "weather_zone": weather_zone,
        "latitude": geo["latitude"], "longitude": geo["longitude"],
        "location_key": [geo["latitude"], geo["longitude"]],
        "location_source": geo["source"], "location_note": geo["note"],
        "current": {
            "temperature": _number(current["temperature_2m"], -100, 70),
            "feels": _number(current["apparent_temperature"], -120, 90),
            "humidity": _number(current["relative_humidity_2m"], 0, 100),
            "wind": _number(current["wind_speed_10m"], 0, 500),
            "code": code_of(current["weather_code"]), "is_day": bool(is_day),
        },
        "today": day(index), "tomorrow": _optional(lambda: day(index + 1)),
        "hourly": [], "fetched_at": fetched.isoformat(),
        "observed_at": observed.isoformat(), "source": "open-meteo", "stale": False,
    }
    if snapshot["today"]["min"] > snapshot["today"]["max"]:
        raise ValueError("Invalid temperature range")
    # Hourly data is supplementary; missing or malformed rows do not discard
    # an otherwise useful current/today response.
    hourly = data.get("hourly", {})
    if isinstance(hourly, dict) and isinstance(hourly.get("time"), list):
        for i, when in enumerate(hourly["time"][:72]):
            try:
                stamp = _timestamp(when)
                if stamp <= observed:
                    continue
                snapshot["hourly"].append({
                    "time": stamp.isoformat(),
                    "temperature": _number(hourly["temperature_2m"][i], -100, 70),
                    "chance": _number(hourly["precipitation_probability"][i], 0, 100),
                    "code": _optional(lambda: code_of(hourly["weather_code"][i])),
                })
                if len(snapshot["hourly"]) == 12:
                    break
            except (ValueError, KeyError, TypeError, IndexError):
                continue
    return snapshot


def _default_cache():
    return Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache"))) / "omarchy/ambient-weather.json"


CACHE_PATH = _default_cache()


def _read_cache(path):
    with path.open("rb") as handle:
        raw = handle.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise ValueError("Oversized cache")
    value = json.loads(raw)
    if value["version"] != CACHE_VERSION:
        raise ValueError("Unknown cache version")
    result = _snapshot(value["geo"], value["weather"], value["fetched_at"])
    result["stale"] = True
    return result


def _write_cache(path, value):
    raw = json.dumps(value, ensure_ascii=True, allow_nan=False)
    if len(raw.encode()) > MAX_BYTES:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, prefix=".ambient-weather-", delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(raw)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def read_cached(cache_path=None):
    """Read a validated stale snapshot without network access, or return None."""
    try:
        return _read_cache(Path(cache_path) if cache_path is not None else CACHE_PATH)
    except (OSError, ValueError, KeyError, TypeError, IndexError):
        return None


UNAVAILABLE = {"city": "", "country": "", "country_name": "", "zone": None, "current": None,
               "today": None, "tomorrow": None, "hourly": [], "fetched_at": None,
               "source": "unavailable", "location_source": "unavailable", "location_note": "", "stale": True}


def fetch_snapshot(cache_path=None, locate=None):
    """Return a complete fresh snapshot, stale cached snapshot, or unavailable.

    Each call re-detects location. Failure at either step retains the whole
    previous snapshot, never a new city's name attached to old conditions.
    cache_path overrides all cache reads/writes (useful for isolated tests).
    """
    path = Path(cache_path) if cache_path is not None else CACHE_PATH
    geo = None
    try:
        geo = _where((locate or location.resolve)())
        query = urlencode({
            "latitude": geo["latitude"], "longitude": geo["longitude"],
            "current": "temperature_2m,apparent_temperature,relative_humidity_2m,wind_speed_10m,weather_code,is_day",
            "daily": "temperature_2m_min,temperature_2m_max,rain_sum,precipitation_probability_max,weather_code,uv_index_max,sunrise,sunset",
            "hourly": "temperature_2m,precipitation_probability,weather_code",
            "timezone": "auto", "forecast_days": 2,
            "temperature_unit": "celsius", "wind_speed_unit": "kmh", "precipitation_unit": "mm",
        })
        data = _json_request(WEATHER_URL + "?" + query)
        fetched_at = datetime.now(timezone.utc).isoformat()
        snapshot = _snapshot(geo, data, fetched_at)
    except (OSError, ValueError, KeyError, TypeError, IndexError, AttributeError, subprocess.SubprocessError):
        try:
            return _read_cache(path)
        except (OSError, ValueError, KeyError, TypeError, IndexError):
            if geo is None:
                return dict(UNAVAILABLE)
            # Nothing saved, but radio evidence can still place us: show the
            # city and its clock, with no weather attached.
            return dict(UNAVAILABLE, city=geo["name"], country=geo["country"],
                        country_name=location.country_name(geo["country"]), zone=geo["timezone"],
                        location_source=geo["source"], location_note=geo["note"])
    try:
        _write_cache(path, {"version": CACHE_VERSION, "geo": geo, "weather": data, "fetched_at": fetched_at})
    except (OSError, ValueError):
        pass  # A read-only cache must not hide a successful live response.
    return snapshot
