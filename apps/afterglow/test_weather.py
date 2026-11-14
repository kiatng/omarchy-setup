import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import location
import weather

GEO = {"name": "Test City", "latitude": 48.85, "longitude": 2.35, "country": "FR", "timezone": "Europe/Paris",
       "source": "ip", "note": "network location"}
DATA = {
    "timezone": "Europe/Paris",
    "current": {"time": "2026-11-11T12:15", "temperature_2m": 29,
                "apparent_temperature": 32, "relative_humidity_2m": 80,
                "wind_speed_10m": 10, "weather_code": 3, "is_day": 1},
    "daily": {"time": ["2026-11-11", "2026-11-12"], "temperature_2m_min": [24, 25],
              "temperature_2m_max": [31, 30], "rain_sum": [2, 0], "precipitation_probability_max": [60, 10],
              "weather_code": [61, 2], "uv_index_max": [7.5, 8], "sunrise": ["2026-11-11T05:59", "2026-11-12T06:00"],
              "sunset": ["2026-11-11T17:49", "2026-11-12T17:49"]},
    "hourly": {"time": ["2026-11-11T12:00", "2026-11-11T13:00"],
               "temperature_2m": [29, 30], "precipitation_probability": [40, 60], "weather_code": [3, 61]},
}


class WeatherTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.cache = Path(self.directory.name) / "weather.json"

    def fresh(self, geo=None, data=None):
        with patch.object(weather, "_json_request", return_value=data or DATA):
            return weather.fetch_snapshot(self.cache, locate=lambda: geo or GEO)

    def test_live_data_cached_as_complete_location_and_time(self):
        result = self.fresh()
        self.assertFalse(result["stale"])
        self.assertEqual(result["zone"], "Europe/Paris")
        self.assertEqual(result["country_name"], "France")
        self.assertEqual(result["current"]["temperature"], 29)
        self.assertEqual(result["hourly"][0]["temperature"], 30)
        self.assertEqual(result["hourly"][0]["code"], 61)
        self.assertEqual(result["today"]["sunrise"], "05:59")
        self.assertEqual(result["tomorrow"]["max"], 30)
        with patch.object(weather, "_json_request", side_effect=AssertionError("Network forbidden")):
            cached = weather.read_cached(self.cache)
        self.assertEqual(cached["fetched_at"], result["fetched_at"])
        self.assertTrue(cached["stale"])
        self.assertEqual(cached["city"], "Test City")

    def test_location_provenance_is_kept_for_display(self):
        note = "your Elsewhen city in France · IP says London, GB; nearby Wi-Fi says FR"
        result = self.fresh(dict(GEO, source="elsewhen", note=note))
        self.assertEqual(result["location_source"], "elsewhen")
        self.assertEqual(result["location_note"], note)
        self.assertEqual(weather.read_cached(self.cache)["location_note"], note)

    def test_radio_fix_without_zone_uses_forecast_zone_for_clock(self):
        result = self.fresh(dict(GEO, timezone=None, source="wifi-position"))
        self.assertEqual(result["zone"], "Europe/Paris")

    def test_new_location_weather_failure_keeps_old_city_and_conditions(self):
        old = self.fresh()
        elsewhere = dict(GEO, name="Elsewhere", latitude=40, longitude=-74, timezone="America/New_York", country="US")
        with patch.object(weather, "_json_request", side_effect=subprocess.TimeoutExpired("curl", 10)):
            result = weather.fetch_snapshot(self.cache, locate=lambda: elsewhere)
        self.assertEqual(result["city"], old["city"])
        self.assertEqual(result["zone"], old["zone"])
        self.assertEqual(result["current"], old["current"])
        self.assertTrue(result["stale"])

    def test_invalid_or_missing_location_never_fetches_weather(self):
        bad = [dict(GEO, **change) for change in (
            {"latitude": float("nan")}, {"latitude": 10 ** 400}, {"latitude": True}, {"longitude": 181},
            {"timezone": "../../etc/passwd"}, {"country": "LKA"}, {"name": "x" * 81}, {"source": "made-up"})] + [None]
        for geo in bad:
            with self.subTest(geo=geo), patch.object(weather, "_json_request") as request:
                result = weather.fetch_snapshot(self.cache, locate=lambda: geo)
                self.assertEqual(result["source"], "unavailable")
                request.assert_not_called()

    def test_terminal_controls_removed_and_place_zone_used_for_clock(self):
        result = self.fresh(dict(GEO, name="Test\x1b\n‮ City"), dict(DATA, timezone="UTC"))
        self.assertEqual(result["city"], "Test City")
        self.assertEqual(result["zone"], "Europe/Paris")
        self.assertEqual(result["weather_zone"], "UTC")

    def test_bad_current_does_not_replace_good_cache(self):
        self.fresh()
        broken = copy.deepcopy(DATA)
        broken["current"]["temperature_2m"] = "<script>"
        result = self.fresh(dict(GEO, name="New City"), broken)
        self.assertEqual(result["city"], "Test City")
        self.assertTrue(result["stale"])

    def test_missing_extras_do_not_discard_core_weather(self):
        plain = copy.deepcopy(DATA)
        for key in ("weather_code", "uv_index_max", "sunrise", "sunset"):
            del plain["daily"][key]
        del plain["hourly"]["weather_code"]
        result = self.fresh(data=plain)
        self.assertFalse(result["stale"])
        self.assertIsNone(result["today"]["uv"])
        self.assertIsNone(result["hourly"][0]["code"])

    def test_offline_corrupt_or_missing_cache_does_not_invent_city(self):
        for raw in ("garbage", json.dumps({"version": 2}), json.dumps({"version": 1}), "x" * (weather.MAX_BYTES + 1)):
            self.cache.write_text(raw)
            result = weather.fetch_snapshot(self.cache, locate=lambda: None)
            self.assertEqual(result["source"], "unavailable")
            self.assertIsNone(result["zone"])
            self.assertIsNone(weather.read_cached(self.cache))
        self.cache.unlink()
        self.assertIsNone(weather.read_cached(self.cache))

    def test_known_place_without_weather_or_cache_shows_city_and_no_conditions(self):
        with patch.object(weather, "_json_request", side_effect=OSError("offline")):
            result = weather.fetch_snapshot(self.cache, locate=lambda: dict(GEO, source="elsewhen"))
        self.assertEqual((result["city"], result["zone"], result["source"]), ("Test City", "Europe/Paris", "unavailable"))
        self.assertIsNone(result["current"])
        self.assertTrue(result["stale"])

    def test_readonly_cache_does_not_hide_live_weather(self):
        with patch.object(weather, "_write_cache", side_effect=PermissionError):
            result = self.fresh()
        self.assertFalse(result["stale"])

    def test_request_is_fixed_https_bounded_and_not_shell(self):
        with patch.object(location.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, b"{}")) as run:
            self.assertEqual(weather._json_request(weather.WEATHER_URL), {})
        args, options = run.call_args
        self.assertEqual(args[0][-1], weather.WEATHER_URL)
        self.assertIn("--max-filesize", args[0])
        self.assertIn("--max-time", args[0])
        self.assertEqual(args[0][args[0].index("--proto") + 1], "=https")
        self.assertEqual(options["timeout"], 10)
        self.assertNotIn("shell", options)


if __name__ == "__main__":
    unittest.main()
