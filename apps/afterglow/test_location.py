import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import location

LONDON = {"name": "London", "latitude": 51.5, "longitude": -0.12, "country": "GB", "timezone": "Europe/London"}
PARIS = {"name": "Paris", "latitude": 48.85, "longitude": 2.35, "country": "FR", "timezone": "Europe/Paris"}
SCAN = """BSS 02:00:00:00:00:01(on wlan0) -- associated
\tfreq: 2462.0
\tsignal: -35.00 dBm
\tlast seen: 4601 ms ago
\tSSID: phone
\tCountry: FR\tEnvironment: bogus
BSS 00:00:5e:00:53:01(on wlan0)
\tfreq: 2412.0
\tsignal: -52.00 dBm
\tlast seen: 800 ms ago
\tSSID: cafe \x1b[31m
BSS 00:00:5e:00:53:02(on wlan0)
\tsignal: -70.00 dBm
\tlast seen: 900 ms ago
\tCountry: FR\tEnvironment: Indoor/Outdoor
BSS 00:00:5e:00:53:03(on wlan0)
\tsignal: garbage
\tlast seen: 999999 ms ago
\tCountry: US\tEnvironment: Indoor
"""


def evidence(country="FR", metered=True, bssids=("00:00:5e:00:53:01", "00:00:5e:00:53:02")):
    aps = [{"bssid": b, "associated": False, "signal": -60, "frequency": 2412, "country": country, "age_ms": 500} for b in bssids]
    return {"aps": aps, "country": country, "metered": metered, "interface": "wlan0"}


class LocationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.places = Path(self.directory.name) / "places.json"
        self.config = Path(self.directory.name) / "shell.json"
        self.config.write_text(json.dumps({"bar": {"layout": {"centerEnd": [{"id": "omarchy.elsewhen", "cities": [
            {"name": "Paris", "zone": "Europe/Paris", "lat": 48.85, "lon": 2.35},
            {"name": "London", "zone": "Europe/London"},
            {"name": "<b>Bad</b>", "zone": "../etc/passwd", "lat": 1, "lon": 1}]}]}}}))

    def resolve(self, ip, seen, **options):
        effect = [ip] if isinstance(ip, dict) else OSError("offline")
        with patch.object(location, "json_request", side_effect=effect) as request:
            result = location.resolve(places_path=self.places, config_path=self.config, evidence=seen,
                                      wifi_positioning=options.get("wifi_positioning", False))
        self.requests = request.call_count
        return result

    def test_scan_parsing_votes_on_fresh_beacons_only(self):
        aps = location.parse_scan(SCAN)
        self.assertEqual(len(aps), 4)
        self.assertEqual(aps[0]["signal"], -35)
        self.assertTrue(aps[0]["associated"])
        self.assertIsNone(aps[3]["signal"])
        self.assertEqual(location.beacon_country(aps), "FR")  # the stale US beacon has no vote
        self.assertIsNone(location.beacon_country(aps[1:2]))

    def test_roaming_hotspot_contradiction_uses_radio_country_not_ip_city(self):
        result = self.resolve(LONDON, evidence("FR"))
        self.assertEqual((result["name"], result["country"], result["timezone"]), ("Paris", "FR", "Europe/Paris"))
        self.assertEqual(result["source"], "elsewhen")
        self.assertIn("IP says London, GB; nearby Wi-Fi says FR", result["note"])
        self.assertFalse(self.places.exists())  # an untrusted reading is never remembered

    def test_trusted_wifi_is_remembered_then_recognised_on_hotspot(self):
        first = self.resolve(PARIS, evidence("FR", metered=False))
        self.assertEqual(first["source"], "ip")
        later = self.resolve(LONDON, evidence("FR", bssids=("00:00:5e:00:53:02", "11:22:33:44:55:66")))
        self.assertEqual((later["name"], later["source"]), ("Paris", "wifi-known"))
        moved = self.resolve(LONDON, evidence("FR", bssids=("11:22:33:44:55:66",)))
        self.assertEqual((moved["name"], moved["source"]), ("Paris", "last-known"))

    def test_agreeing_country_trusts_ip_even_on_hotspot_without_remembering(self):
        result = self.resolve(LONDON, evidence("GB"))
        self.assertEqual((result["name"], result["source"]), ("London", "ip"))
        self.assertFalse(self.places.exists())

    def test_unknown_country_falls_back_to_tz_table_city(self):
        result = self.resolve(LONDON, evidence("JP"))
        self.assertEqual((result["name"], result["country"], result["source"]), ("Tokyo", "JP", "country"))

    def test_hotspot_without_beacons_marks_ip_unverified(self):
        result = self.resolve(LONDON, evidence(None))
        self.assertEqual(result["source"], "ip-unverified")

    def test_offline_recognises_known_wifi_and_otherwise_admits_ignorance(self):
        self.resolve(PARIS, evidence("FR", metered=False))
        self.assertEqual(self.resolve(None, evidence(None, bssids=("00:00:5e:00:53:01",)))["source"], "wifi-known")
        self.places.unlink()
        self.assertIsNone(self.resolve(None, evidence(None)))

    def test_randomised_and_hotspot_addresses_are_not_landmarks_or_sent(self):
        seen = evidence("FR", bssids=("02:00:00:00:00:01", "00:00:5e:00:53:01", "00:00:5e:00:53:02"))
        self.assertEqual([ap["bssid"] for ap in location.landmarks(seen)], ["00:00:5e:00:53:01", "00:00:5e:00:53:02"])
        with patch.object(location, "json_request", return_value={"location": {"lat": 48.85, "lng": 2.35}, "accuracy": 80}) as request:
            fix = location.wifi_position(seen)
        self.assertEqual(fix, {"latitude": 48.85, "longitude": 2.35})
        url, body = request.call_args.args
        self.assertEqual(url, location.BEACONDB_URL)
        self.assertFalse(body["considerIp"])
        self.assertNotIn("02:00:00:00:00:01", json.dumps(body))

    def test_wifi_positioning_names_the_nearest_known_city(self):
        with patch.object(location, "json_request", side_effect=[LONDON, {"location": {"lat": 48.85, "lng": 2.35}, "accuracy": 80}]):
            result = location.resolve(places_path=self.places, config_path=self.config, evidence=evidence("FR"))
        self.assertEqual((result["name"], result["source"], result["latitude"]), ("Paris", "wifi-position", 48.85))

    def test_wifi_positioning_can_be_disabled_and_needs_two_landmarks(self):
        self.resolve(LONDON, evidence("FR"), wifi_positioning=False)
        self.assertEqual(self.requests, 1)
        with patch.object(location, "json_request") as request:
            self.assertIsNone(location.wifi_position(evidence("FR", bssids=("00:00:5e:00:53:01",))))
        request.assert_not_called()

    def test_corrupt_places_file_is_ignored(self):
        for raw in ("garbage", json.dumps({"places": [{"name": "X"}, 7]}), "x" * (location.MAX_BYTES + 1)):
            self.places.write_text(raw)
            self.assertEqual(location.load_places(self.places), [])

    def test_malformed_elsewhen_entries_are_skipped(self):
        cities = location.elsewhen_cities(self.config)
        self.assertEqual([c["name"] for c in cities], ["Paris"])
        self.assertEqual(location.elsewhen_cities(Path(self.directory.name) / "missing.json"), [])


if __name__ == "__main__":
    unittest.main()
