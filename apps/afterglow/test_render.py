from datetime import datetime, timedelta
import re
import unittest
from unittest.mock import patch
from zoneinfo import ZoneInfo

import bigtext
import screensaver

CONFIG = dict(screensaver.DEFAULTS)
NOW = datetime(2026, 11, 12, 14, 5, 30, tzinfo=ZoneInfo("Europe/Paris"))


def snapshot(**changes):
    base = {
        "city": "Paris", "country": "FR", "country_name": "France", "zone": "Europe/Paris",
        "location_source": "elsewhen", "location_note": "your Elsewhen city in France", "stale": False,
        "source": "open-meteo", "observed_at": "2026-11-12T14:00:00",
        "current": {"temperature": 29.1, "feels": 33.2, "humidity": 73, "wind": 13.3, "code": 95, "is_day": True},
        "today": {"min": 24.1, "max": 29.2, "rain": 9.9, "chance": 100, "code": 95, "uv": 7.5, "sunrise": "05:59", "sunset": "17:49"},
        "tomorrow": {"min": 24.3, "max": 30.8, "rain": 0.1, "chance": 15, "code": 51, "uv": 6, "sunrise": "06:00", "sunset": "17:49"},
        "hourly": [{"time": f"2026-11-12T{15 + i}:00:00", "temperature": 29 - i * .4, "chance": 90 - i * 8, "code": 80} for i in range(8)],
    }
    base.update(changes)
    return base


def plain(canvas):
    return [re.sub(r"\x1b\[[0-9;]*m", "", row) for row in canvas.rows()]


def frame(width=160, height=38, data=None, **options):
    return screensaver.compose(width, height, snapshot() if data is None else data, CONFIG, now=NOW, ambient=False, **options)


class TypographyTests(unittest.TestCase):
    def test_every_glyph_is_seven_rows_of_equal_width(self):
        for char, glyph in bigtext.GLYPHS.items():
            self.assertEqual(len(glyph), 7, char)
            self.assertEqual(len({len(row) for row in glyph}), 1, char)
        shapes = [glyph for char, glyph in bigtext.GLYPHS.items() if char.isalpha()]
        self.assertEqual(len(set(shapes)), 26)  # no two letters share a shape

    def test_city_names_normalise_or_decline(self):
        self.assertEqual(bigtext.displayable("São Paulo"), "SAO PAULO")
        self.assertEqual(bigtext.displayable("Washington, D.C."), "WASHINGTON D.C.")
        self.assertIsNone(bigtext.displayable("東京"))
        self.assertIsNone(bigtext.displayable("\x1b[31m"))

    def test_rows_are_exactly_the_window_width_and_reset(self):
        canvas = frame()
        rows = plain(canvas)
        self.assertEqual(len(rows), 38)
        self.assertEqual({len(row) for row in rows}, {160})
        self.assertTrue(all(row.endswith("\x1b[0m") for row in canvas.rows()))


class DashboardTests(unittest.TestCase):
    def test_city_time_and_temperature_use_large_lettering(self):
        canvas = frame()
        text = "\n".join(plain(canvas))
        expected = bigtext.Canvas(160, 7)
        expected.big(0, 0, "PARIS", 2, lambda _f: (1, 1, 1))
        city_first_row = plain(expected)[0].rstrip()
        self.assertIn(city_first_row, text)
        self.assertGreaterEqual(sum(text.count(block) for block in "█▀▄"), 600)
        for wanted in ("F R A N C E", "T H U R S D A Y   1 2   N O V E M B E R", "THUNDERSTORM", "feels 33°",
                       "15:00", "TODAY 24° – 29°", "rain 100% · 9.9 mm", "TOMORROW 24° – 31°", "sun 05:59 → 17:49",
                       "UTC  13:05", "1h00 behind", "Location: your Elsewhen city in France"):
            self.assertIn(wanted, text)

    def test_every_size_draws_something_readable_without_error(self):
        for data in (snapshot(), snapshot(city="London"), snapshot(city="東京"), snapshot(hourly=[], tomorrow=None),
                     snapshot(current=None, today=None, hourly=[], source="unavailable", city="", zone=None), {}, None):
            for width, height in ((240, 67), (160, 40), (160, 38), (120, 30), (100, 24), (80, 24), (60, 16), (40, 10), (20, 5), (8, 3), (1, 1)):
                with self.subTest(width=width, height=height, city=(data or {}).get("city")):
                    rows = plain(screensaver.compose(width, height, data, CONFIG, t=12.3, now=NOW))
                    self.assertEqual((len(rows), {len(r) for r in rows}), (height, {width}))
                    if width >= 20 and height >= 5:
                        self.assertTrue(any(ch.isdigit() or ch in "█▀▄" for row in rows for ch in row))

    def test_long_city_drops_to_smaller_lettering_instead_of_clipping(self):
        text = "\n".join(plain(frame(data=snapshot(city="Thiruvananthapuram"))))
        small = bigtext.Canvas(160, 4)
        small.big(0, 0, "THIRUVANANTHAPURAM", 1, lambda _f: (1, 1, 1))
        self.assertIn(plain(small)[0].rstrip(), text)

    def test_saved_weather_is_labelled_and_unknown_place_is_not_invented(self):
        text = "\n".join(plain(frame(data=snapshot(stale=True, observed_at="2026-11-11T22:00:00"))))
        self.assertIn("OFFLINE · showing weather saved 2026-11-11 22:00", text)
        self.assertIn("SAVED 24°", text)
        nowhere = "\n".join(plain(frame(data=snapshot(current=None, today=None, hourly=[], city="", zone=None, country_name="", source="unavailable"))))
        self.assertIn("weather unavailable", nowhere)
        self.assertIn("L O C A L   T I M E", nowhere)
        self.assertNotIn("S R I", nowhere)

    def test_lock_time_is_announced(self):
        lock_at = NOW + timedelta(seconds=150)
        text = "\n".join(plain(frame(lock_at=lock_at)))
        self.assertIn("Locks at 14:08, then the display sleeps", text)
        self.assertIn("Any key or the mouse returns", "\n".join(plain(frame())))

    def test_hostile_text_cannot_reach_the_terminal(self):
        rows = frame(data=snapshot(location_note="x\x1b]52;c;AAAA\x07y", country_name="A\x9bB")).rows()
        body = "".join(re.sub(r"\x1b\[[0-9;]*m", "", row) for row in rows)
        self.assertNotIn("\x1b", body)
        self.assertNotIn("\x07", body)
        self.assertNotIn("\x9b", body)


class InputTests(unittest.TestCase):
    screen = [None] * 20  # compositor unavailable: fall back to what the terminal reports

    def feed(self, *chunks):
        import os
        read, write = os.pipe()
        terminal = screensaver.Terminal.__new__(screensaver.Terminal)
        terminal.fd, terminal.pointer, terminal.anchor = read, None, None
        terminal.started = screensaver.time.monotonic() - 5
        results = []
        with patch.object(screensaver, "log"), patch.object(screensaver, "cursor_position", side_effect=self.screen):
            for chunk in chunks:
                os.write(write, chunk)
                results.append(terminal.pressed(.2))
        os.close(read); os.close(write)
        return results

    def test_stationary_pointer_reports_do_not_dismiss_but_travel_clicks_and_keys_do(self):
        still = b"\x1b[MCw="
        self.assertEqual(self.feed(still, still, still + still), [False, False, False])
        self.assertEqual(self.feed(still, b"\x1b[MCy>"), [False, True])      # pointer travelled
        self.assertEqual(self.feed(still, b"\x1b[M w="), [False, True])      # button press
        self.assertEqual(self.feed(b"k"), [True])
        self.assertEqual(self.feed(b"\x1b[A"), [True])                       # arrow key

    def test_window_sliding_under_a_resting_pointer_is_not_the_user(self):
        self.screen = [(245, 563)] * 20   # the pointer never moves on screen
        self.assertEqual(self.feed(b"\x1b[MCw=", b"\x1b[MCw4", b"\x1b[MCw("), [False, False, False])
        self.screen = [(245, 563), (300, 500)]
        self.assertEqual(self.feed(b"\x1b[MCw=", b"\x1b[MCy4"), [False, True])

    def test_closed_terminal_stops_the_display_instead_of_spinning(self):
        import os
        read, write = os.pipe()
        terminal = screensaver.Terminal.__new__(screensaver.Terminal)
        terminal.fd, terminal.pointer, terminal.anchor, terminal.started = read, None, None, 0
        os.close(write)
        screensaver.STOP.clear()
        try:
            self.assertFalse(terminal.pressed(.1))
            self.assertTrue(screensaver.STOP.is_set())
        finally:
            screensaver.STOP.clear()
            screensaver.END_REASON = None
            os.close(read)

    def test_input_in_the_first_moments_is_ignored(self):
        import os
        read, write = os.pipe()
        terminal = screensaver.Terminal.__new__(screensaver.Terminal)
        terminal.fd, terminal.pointer, terminal.anchor, terminal.started = read, None, None, screensaver.time.monotonic()
        os.write(write, b"k")
        self.assertFalse(terminal.pressed(.1))
        os.close(read); os.close(write)


class ResilienceTests(unittest.TestCase):
    def test_drawing_failure_is_logged_and_replaced_by_plain_clock(self):
        class Fake:
            def __init__(self): self.written, self.polls = [], 0
            def size(self): return (80, 24)
            def write(self, data): self.written.append(data)
            def pressed(self, _timeout):
                self.polls += 1
                if self.polls >= 2: screensaver.STOP.set()
                return False
        fake = Fake()
        screensaver.STOP.clear()
        try:
            with patch.object(screensaver, "compose", side_effect=ZeroDivisionError("boom")), \
                 patch.object(screensaver, "log") as log, patch.object(screensaver, "heartbeat") as beat:
                screensaver.live(fake, CONFIG, None, 0)
        finally:
            screensaver.STOP.clear()
        self.assertEqual(log.call_args_list[0].args[0], "render-error")
        self.assertIn("Afterglow hit a drawing error: ZeroDivisionError: boom", "".join(fake.written))
        self.assertEqual(beat.call_args.args[0], "safe-mode")

    def test_effect_that_fails_to_start_or_exits_badly_does_not_block(self):
        class Quiet:
            def pressed(self, _timeout): return False
            def write(self, _data): pass
        real = screensaver.subprocess.Popen
        with patch.object(screensaver, "log"):
            with patch.object(screensaver.subprocess, "Popen", side_effect=FileNotFoundError("ttfx")):
                self.assertEqual(screensaver.run_effect(Quiet(), "/dev/null", "beams", 5), "failed")
            with patch.object(screensaver.subprocess, "Popen", lambda *a, **k: real(["false"])):
                self.assertEqual(screensaver.run_effect(Quiet(), "/dev/null", "beams", 5), "failed")
            with patch.object(screensaver.subprocess, "Popen", lambda *a, **k: real(["sleep", "30"])):
                self.assertEqual(screensaver.run_effect(Quiet(), "/dev/null", "beams", .3), "watchdog")

    def test_settings_fall_back_to_defaults_when_invalid(self):
        with patch.object(screensaver.Path, "read_text", return_value='{"home_zone": "Not/AZone"}'):
            self.assertEqual(screensaver.settings(), screensaver.DEFAULTS)
        with patch.object(screensaver.Path, "read_text", return_value='{"max_effect_seconds": 9999, "logo": "yes", "junk": 1}'):
            config = screensaver.settings()
        self.assertEqual((config["max_effect_seconds"], config["logo"]), (120, False))
        self.assertNotIn("junk", config)


if __name__ == "__main__":
    unittest.main()
