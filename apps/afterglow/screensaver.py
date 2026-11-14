#!/usr/bin/env python3
"""Omarchy Afterglow: the Omarchy logo animation, then city, time and weather.

Three phases: `logo` (one of Omarchy's terminal effects, run to its natural
end), `reveal` (the dashboard itself animated in by another effect), and
`live` (a calm, always-readable display). Every run is journalled so that a
dark screen can be traced to a renderer fault or to the normal lock.
"""
import argparse
from datetime import datetime, timedelta
import json
import math
import os
from pathlib import Path
import random
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import termios
import threading
import time
import traceback
import tty
from zoneinfo import ZoneInfo

from bigtext import Canvas, EIGHTHS, big_width, displayable, gradient, rgb
import location
import weather

ROOT = Path(__file__).resolve().parent
STATE = location._state_dir() / 'ambient'
EVENTS, HEARTBEAT = STATE / 'events.log', STATE / 'heartbeat.json'
STOP = threading.Event()
FETCHED = threading.Event()
SNAPSHOT = None
DISMISSED = False
SETTLE = 2.5  # seconds for the window to finish sliding in under the pointer
END_REASON = None
DEFAULTS = {'logo': True, 'reveal': True, 'max_effect_seconds': 45,
            'skip_effects': ['swarm', 'matrix'], 'weather_enabled': False,
            'wifi_positioning': False, 'refresh_seconds': 900,
            'home_zone': 'UTC', 'home_name': 'UTC'}
# Effects that keep the dashboard's own colours and settle within a few seconds.
REVEALS = ['slide', 'wipe', 'expand', 'middleout', 'slice', 'scattered', 'randomsequence',
           'synthgrid', 'waves', 'rain', 'errorcorrect', 'pour', 'decrypt']
BACKGROUND = '#08101f'
INK, SOFT, DIM, FAINT = rgb('#eef2ff'), rgb('#a9b4d6'), rgb('#6f7ba3'), rgb('#2d3757')
MINT, SKY, AMBER, CORAL, VIOLET = rgb('#8ef0c4'), rgb('#67d8ff'), rgb('#ffd166'), rgb('#ff8fa3'), rgb('#bfa6ff')
CITY_STOPS = [CORAL, AMBER, MINT, SKY, VIOLET]
HEAT_STOPS = [(-5, rgb('#9cc9ff')), (10, rgb('#7ee8d6')), (20, rgb('#a8e97f')),
              (26, AMBER), (31, rgb('#ffab5e')), (37, rgb('#ff6b6b'))]
CONDITIONS = {0: ('Clear', 'clear'), 1: ('Mostly clear', 'clear'), 2: ('Partly cloudy', 'partly'),
              3: ('Overcast', 'cloud'), 45: ('Fog', 'fog'), 48: ('Icy fog', 'fog'),
              51: ('Light drizzle', 'rain'), 53: ('Drizzle', 'rain'), 55: ('Heavy drizzle', 'rain'),
              56: ('Freezing drizzle', 'rain'), 57: ('Freezing drizzle', 'rain'),
              61: ('Light rain', 'rain'), 63: ('Rain', 'rain'), 65: ('Heavy rain', 'rain'),
              66: ('Freezing rain', 'rain'), 67: ('Freezing rain', 'rain'),
              71: ('Light snow', 'snow'), 73: ('Snow', 'snow'), 75: ('Heavy snow', 'snow'),
              77: ('Snow grains', 'snow'), 80: ('Light showers', 'rain'), 81: ('Showers', 'rain'),
              82: ('Heavy showers', 'rain'), 85: ('Snow showers', 'snow'), 86: ('Snow showers', 'snow'),
              95: ('Thunderstorm', 'storm'), 96: ('Thunderstorm, hail', 'storm'), 99: ('Thunderstorm, hail', 'storm')}


# ------------------------------------------------------------------ journal
def log(event, **details):
    """Append one line to the run journal. Never raises."""
    try:
        STATE.mkdir(parents=True, exist_ok=True)
        with EVENTS.open('a') as handle:
            handle.write(json.dumps({'at': datetime.now().astimezone().isoformat(timespec='seconds'),
                                     'pid': os.getpid(), 'event': event, **details}) + '\n')
    except OSError:
        pass


def trim_journal(keep=400):
    try:
        lines = EVENTS.read_text().splitlines()
        if len(lines) > keep * 2:
            EVENTS.write_text('\n'.join(lines[-keep:]) + '\n')
    except OSError:
        pass


def heartbeat(phase, frames, size):
    try:
        HEARTBEAT.write_text(json.dumps({'pid': os.getpid(), 'phase': phase, 'frames': frames,
                                         'columns': size[0], 'rows': size[1], 'at': time.time()}))
    except OSError:
        pass


def dismiss(reason='input'):
    global DISMISSED, END_REASON
    DISMISSED = True
    END_REASON = END_REASON or 'dismissed by ' + reason
    STOP.set()


# --------------------------------------------------------------- compositor
def cursor_hidden(hidden):
    value = 'true' if hidden else 'false'
    try:
        result = subprocess.run(['hyprctl', 'eval', 'hl.config({ cursor = { invisible = '+value+' } })'],
                                capture_output=True, timeout=2)
        if result.returncode:
            subprocess.run(['hyprctl', 'keyword', 'cursor:invisible', value], capture_output=True, timeout=2)
    except (OSError, subprocess.SubprocessError):
        pass


def close_saver_windows():
    """Close only compositor-confirmed screensaver terminals, not matching shells."""
    try:
        result = subprocess.run(['hyprctl', 'clients', '-j'], capture_output=True, timeout=2, check=True)
        clients = json.loads(result.stdout)
        if not isinstance(clients, list):
            return
        for client in clients:
            if not isinstance(client, dict):
                continue
            pid = client.get('pid')
            if client.get('class') == 'org.omarchy.screensaver' and type(pid) is int and pid > 1:
                try:
                    os.kill(pid, signal.SIGTERM)
                except OSError:
                    pass
    except (OSError, ValueError, TypeError, subprocess.SubprocessError):
        pass


def shell_json(*command):
    try:
        result = subprocess.run(['omarchy-shell', *command], capture_output=True, timeout=3, check=True)
        return json.loads(result.stdout)
    except (OSError, ValueError, subprocess.SubprocessError):
        return None


def session_locked():
    return shell_json('lock', 'isLocked') is True


def lock_deadline():
    """When the idle service will lock, if this run belongs to an idle cycle."""
    status = shell_json('idle', 'status')
    try:
        if status['inIdleCycle'] and status['timers']['lock'] and status['lock'] > 0:
            return datetime.now().astimezone() + timedelta(seconds=max(0, status['lockDelay']))
    except (KeyError, TypeError):
        pass
    return None


def settings():
    out = dict(DEFAULTS)
    try:
        data = json.loads((ROOT/'settings.json').read_text())
        if not isinstance(data, dict):
            return out
        out.update({key: value for key, value in data.items() if key in out})
        out['max_effect_seconds'] = max(5, min(120, float(out['max_effect_seconds'])))
        out['refresh_seconds'] = max(300, min(3600, float(out['refresh_seconds'])))
        for key in ('logo', 'reveal', 'weather_enabled', 'wifi_positioning'):
            out[key] = out[key] is True
        out['skip_effects'] = [str(name) for name in out['skip_effects']][:40]
        ZoneInfo(out['home_zone'])
        out['home_name'] = str(out['home_name'])[:40]
    except (OSError, ValueError, TypeError, KeyError):
        return dict(DEFAULTS)
    return out


# ------------------------------------------------------------------ workers
def weather_worker(config):
    global SNAPSHOT
    locate = lambda: location.resolve(wifi_positioning=config['wifi_positioning'])
    while not STOP.is_set():
        fresh = weather.fetch_snapshot(locate=locate)
        log('weather', city=fresh.get('city'), source=fresh.get('location_source'),
            stale=fresh.get('stale'), note=fresh.get('location_note'))
        SNAPSHOT = fresh
        FETCHED.set()
        STOP.wait(60 if fresh.get('stale') else config['refresh_seconds'])


def cursor_position():
    """The pointer in screen coordinates, which window animations cannot shift."""
    try:
        result = subprocess.run(['hyprctl', 'cursorpos', '-j'], capture_output=True, timeout=1, check=True)
        point = json.loads(result.stdout)
        return point['x'], point['y']
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError):
        return None


def mouse_worker():
    # Hidden compositor cursors can suppress terminal mouse reports. Sample
    # position only while this display exists, as a fallback to mouse events.
    if STOP.wait(SETTLE):
        return
    previous = cursor_position()
    while not STOP.wait(.75):
        current = cursor_position()
        if previous is not None and current is not None and current != previous:
            dismiss('mouse movement')
            return
        previous = current


def focus_worker():
    """Use compositor events rather than continuously spawning hyprctl."""
    runtime = os.getenv('XDG_RUNTIME_DIR', '')
    instance = os.getenv('HYPRLAND_INSTANCE_SIGNATURE', '')
    path = f'{runtime}/hypr/{instance}/.socket2.sock'
    try:
        with socket.socket(socket.AF_UNIX) as stream:
            stream.connect(path)
            stream.settimeout(.5)
            pending = b''
            while not STOP.is_set():
                try:
                    chunk = stream.recv(8192)
                except socket.timeout:
                    continue
                if not chunk:
                    return
                pending += chunk
                lines = pending.split(b'\n')
                pending = lines.pop()[-8192:]
                for line in lines:
                    if line.startswith(b'activewindow>>'):
                        app = line.split(b'>>', 1)[1].split(b',', 1)[0]
                        if app != b'org.omarchy.screensaver':
                            dismiss('focus moving to another window')
    except OSError:
        pass


# ----------------------------------------------------------------- drawing
def condition(code):
    return CONDITIONS.get(code, ('Rain', 'rain'))


def heat(value):
    low = HEAT_STOPS[0]
    for high in HEAT_STOPS[1:]:
        if value <= high[0]:
            span = high[0] - low[0]
            return gradient([low[1], high[1]], (value - low[0]) / span if span else 0)
        low = high
    return HEAT_STOPS[-1][1]


def clock_tones(now, is_day):
    if 5 <= now.hour < 8:
        return [rgb('#ffd3a8'), rgb('#ffa1b8')]
    if 17 <= now.hour < 20:
        return [rgb('#ffc078'), rgb('#ff8aa8')]
    if is_day if is_day is not None else 8 <= now.hour < 17:
        return [rgb('#fff1c2'), rgb('#ffffff')]
    return [rgb('#cdbfff'), rgb('#9cc4ff')]


def draw_icon(canvas, x, y, kind, is_day, t):
    """A 24x8-cell weather picture drawn with half-cell pixels."""
    def dot(dx, dy, color):
        canvas.pixel(x + dx, y * 2 + dy, color)

    def disc(cx, cy, radius, color, cut=None):
        for dy in range(int(cy - radius) - 1, int(cy + radius) + 2):
            for dx in range(int(cx - radius) - 1, int(cx + radius) + 2):
                if (dx - cx) ** 2 + (dy - cy) ** 2 <= radius * radius + .4:
                    if cut and (dx - cut[0]) ** 2 + (dy - cut[1]) ** 2 <= cut[2] ** 2:
                        continue
                    dot(dx, dy, color)

    def cloud(cx, cy, color):
        disc(cx - 4, cy + 1, 2.6, color)
        disc(cx, cy - 1, 3.6, color)
        disc(cx + 5, cy + 1, 2.6, color)
        for dy in range(cy + 1, cy + 4):
            for dx in range(cx - 7, cx + 8):
                dot(dx, dy, color)

    def sky(cx, cy, radius):
        if is_day:
            disc(cx, cy, radius, AMBER)
            reach = radius + 2 + int(t) % 2
            for angle in range(8):
                a = angle * math.pi / 4
                dot(round(cx + math.cos(a) * reach), round(cy + math.sin(a) * reach), rgb('#ffe9a8'))
        else:
            disc(cx, cy, radius, rgb('#dcd6ff'), cut=(cx + radius * .55, cy - radius * .35, radius * .85))
            for i, (sx, sy) in enumerate(((3, 3), (20, 2), (18, 12), (5, 13))):
                if (int(t * 1.5) + i) % 4:
                    dot(sx, sy, VIOLET)

    grey, dark = rgb('#cfd8f0'), rgb('#8d99bd')
    if kind == 'clear':
        sky(12, 8, 4)
    elif kind == 'partly':
        sky(8, 5, 3)
        cloud(13, 10, grey)
    elif kind == 'cloud':
        cloud(15, 5, dark)
        cloud(11, 9, grey)
    elif kind == 'fog':
        for i, row in enumerate((3, 6, 9, 12)):
            shift = round(math.sin(t / 3 + i) * 2)
            for dx in range(3 + shift, 21 + shift):
                if (dx + i * 3) % 7:
                    dot(dx, row, grey if i % 2 else dark)
    else:
        cloud(12, 5, dark if kind == 'storm' else grey)
        step = int(t * (2 if kind == 'snow' else 5))
        for i, column in enumerate((6, 10, 14, 18)):
            fall = (step + i * 2) % 6
            if kind == 'snow':
                dot(column + (fall % 2), 10 + fall, INK)
            else:
                dot(column - fall // 2, 10 + fall, SKY)
                dot(column - (fall + 1) // 2, 11 + fall, SKY)
        if kind == 'storm':
            bolt = rgb('#fff3a0') if int(t * 2) % 5 == 0 else AMBER
            for dx, dy in ((13, 9), (12, 10), (11, 11), (12, 11), (13, 11), (12, 12), (11, 13)):
                dot(dx, dy, bolt)


def particles(canvas, kind, is_day, t):
    """Sparse, slow background motion in whatever space the content leaves."""
    width, height = canvas.width, canvas.height
    count = max(6, width * height // 260)
    for i in range(1, count + 1):
        seed = i * 7919
        if kind in ('rain', 'storm'):
            x = (seed * 31 - int(t * 3)) % width
            y = int(seed * 17 + t * (5 + i % 4)) % height
            mark, color = '╱', gradient([FAINT, rgb('#3f6f9c')], (i % 5) / 4)
        elif kind == 'snow':
            x = int(seed * 31 + math.sin(t / 2 + i) * 2) % width
            y = int(seed * 17 + t * (1 + i % 3) / 2) % height
            mark, color = '•' if i % 3 else '*', gradient([FAINT, SOFT], (i % 5) / 4)
        else:
            x = int(seed * 31 + t * (0.4 if is_day else 0.15) * (1 + i % 3)) % width
            y = (seed * 17) % height
            glow = (math.sin(t / 2.5 + i * 1.7) + 1) / 2
            warm = rgb('#6a5a3c') if is_day else rgb('#5b5f9c')
            mark, color = ('·' if i % 4 else '+'), gradient([FAINT, warm], glow)
        if canvas.empty(x, y):
            canvas.text(x, y, mark, color)


def spans(canvas, x, y, pieces):
    """Draw (text, colour[, bold]) pieces left to right; returns the end column."""
    for piece in pieces:
        canvas.text(x, y, piece[0], piece[1], len(piece) > 2 and piece[2])
        x += len(piece[0])
    return x


def spans_width(pieces):
    return sum(len(piece[0]) for piece in pieces)


def compose(width, height, snapshot, config, t=0.0, now=None, lock_at=None, ambient=True):
    """Build one dashboard frame. Pure: all state comes in as arguments."""
    canvas = Canvas(width, height)
    snapshot = snapshot or {}
    zone = snapshot.get('zone')
    now = now or (datetime.now(ZoneInfo(zone)) if zone else datetime.now().astimezone())
    current = snapshot.get('current')
    today = snapshot.get('today')
    label, kind = condition(current['code']) if current else ('', 'clear')
    is_day = current['is_day'] if current else None
    city = str(snapshot.get('city') or '')
    title = displayable(city) if city else None
    hero = title or (None if city else now.strftime('%A').upper())
    clock = now.strftime('%H:%M')
    degrees = f"{round(current['temperature'])}°" if current else '--°'  # round() avoids "-0"

    # Choose the layout that keeps the lettering largest: big time and
    # temperature first, then the city, then the forecast band and spacing.
    rows_of = lambda scale: math.ceil(7 * scale / 2) if scale else 1
    hours = [h for h in snapshot.get('hourly') or [] if 'time' in h]
    band_full = 8 if current and hours and width >= 86 else 0
    city_scales = [s for s in (2, 1) if hero and big_width(hero, s) <= width - 6] + [0]
    main_scales = [s for s in (3, 2, 1) if big_width(clock, s) + big_width(degrees, s) + 2 * s + 2 <= width - 4] + [0]
    best = None
    for main_scale in main_scales:
        for city_scale in city_scales:
            for band_rows in {band_full, 0}:
                for gap, footer_rows in ((2, 3), (1, 3), (1, 2), (0, 2), (0, 1)):
                    used = (1 + rows_of(city_scale) + 2 + gap + rows_of(main_scale) + 2
                            + (gap + band_rows if band_rows else 0) + gap + footer_rows)
                    score = main_scale * 100 + city_scale * 60 + (50 if band_rows else 0) + footer_rows * 5 + gap
                    if used <= height and (best is None or score > best[0]):
                        best = (score, main_scale, city_scale, band_rows, gap, footer_rows, used)
    _, main_scale, city_scale, band_rows, gap, footer_rows, used = best or (0, 0, 0, 0, 0, 1, height)
    # A larger window earns larger lettering, but only with full spacing left over.
    for bigger_main, bigger_city in ((4, 3), (4, 2), (3, 3)):
        roomy = (1 + rows_of(bigger_city) + 4 + rows_of(bigger_main) + 2 + (2 + band_full if band_full else 0) + 5)
        if (best and roomy <= height and hero and big_width(hero, bigger_city) <= width - 6
                and big_width(clock, bigger_main) + big_width(degrees, bigger_main) + 2 * bigger_main + 2 <= width - 4
                and (bigger_main, bigger_city) > (main_scale, city_scale)):
            main_scale, city_scale, band_rows, gap, footer_rows, used = bigger_main, bigger_city, band_full, 2, 3, roomy
            break
    city_rows, main_rows = rows_of(city_scale), rows_of(main_scale)
    y = max(0, (height - used) // 2) + (1 if best else 0)

    # City (or the weekday while no place is known).
    drift = (t / 90) % 1
    city_color = lambda f: gradient(CITY_STOPS + CITY_STOPS[:1], (f * .8 + drift) % 1)
    if city_scale:
        canvas.big((width - big_width(hero, city_scale)) // 2, y, hero, city_scale, city_color)
    else:
        canvas.center(y, (city or now.strftime('%A')).upper(), CORAL, True)
    y += city_rows + 1
    where = snapshot.get('country_name') or ('LOCAL TIME' if not city else '')
    date = now.strftime('%A %d %B').upper()
    for line in ('  ·  '.join(' '.join(part) for part in (where.upper(), date) if part),
                 ' · '.join(part for part in (where.upper(), date) if part), date, now.strftime('%a %d %b').upper()):
        if len(line) <= width - 2:
            canvas.center(y, line, SOFT)
            break
    y += 1 + gap

    # Time and temperature, equally large.
    tones = clock_tones(now, is_day)
    warmth = heat(current['temperature']) if current else DIM
    if main_scale:
        clock_w, temp_w = big_width(clock, main_scale), big_width(degrees, main_scale)
        between = min(5 * main_scale, width - 6 - clock_w - temp_w)
        left = (width - (clock_w + between + temp_w)) // 2
        shown = clock if now.second % 2 == 0 or t == 0 else clock.replace(':', ' ')
        canvas.big(left, y, shown, main_scale, lambda f: gradient(tones, f))
        temp_x = left + clock_w + between
        canvas.big(temp_x, y, degrees, main_scale, lambda f: gradient([warmth, gradient([warmth, INK], .35)], f))
        canvas.text(temp_x + temp_w + 1, y, 'C', warmth, True)
        y += main_rows + 1
        filled = round(now.second / 59 * (clock_w - 1))
        canvas.text(left, y, '━' * filled, gradient(tones, .5))
        canvas.text(left + filled, y, '─' * (clock_w - filled), FAINT)
        caption_x, caption_w = temp_x, temp_w + 2
    else:
        canvas.center(y, f'{clock}   {degrees}C', INK, True)
        y += main_rows + 1
        caption_x, caption_w = 0, width
    if current:
        caption = [(label.upper(), warmth, True), ('  feels ', DIM), (f"{current['feels']:.0f}°", INK, True)]
    elif snapshot.get('source') == 'unavailable':
        caption = [('weather unavailable · retrying', DIM)]
    else:
        caption = [('finding local weather…' if config['weather_enabled'] else 'weather is switched off', DIM)]
    spans(canvas, max(0, caption_x + (caption_w - spans_width(caption)) // 2), y, caption)
    y += 1

    # Weather band: picture, the next hours, today and tomorrow.
    if band_rows:
        y += gap
        shown_hours = hours[:max(3, min(10, (width - 44) // 9))]
        strip_w = 6 + 9 * len(shown_hours)
        x = (width - (24 + 4 + strip_w)) // 2
        draw_icon(canvas, x, y, kind, is_day if is_day is not None else True, t)
        x += 28
        temps = [h['temperature'] for h in shown_hours]
        low, high = min(temps), max(temps)
        canvas.text(x, y, 'NEXT', DIM)
        canvas.text(x, y + 3, 'TEMP', DIM)
        canvas.text(x, y + 4, 'RAIN', DIM)
        for i, hour in enumerate(shown_hours):
            column = x + 6 + i * 9
            stamp = datetime.fromisoformat(hour['time'])
            canvas.text(column, y, stamp.strftime('%H:%M'), SOFT)
            level = 3 + round((hour['temperature'] - low) / (high - low) * 13) if high > low else 8
            tone = heat(hour['temperature'])
            canvas.text(column + 1, y + 1, EIGHTHS[max(0, level - 8)] * 3, tone)
            canvas.text(column + 1, y + 2, EIGHTHS[min(8, level)] * 3, tone)
            canvas.text(column + 1, y + 3, f"{hour['temperature']:.0f}°", tone, True)
            wet = hour['chance'] / 100
            canvas.text(column + 1, y + 4, f"{hour['chance']:.0f}%", gradient([DIM, SKY], wet), wet >= .5)
        if today:
            saved = str(snapshot.get('observed_at', ''))[:10] != now.date().isoformat()
            line = [('SAVED ' if saved else 'TODAY ', AMBER if saved else DIM), (f"{today['min']:.0f}°", heat(today['min']), True),
                    (' – ', DIM), (f"{today['max']:.0f}°", heat(today['max']), True),
                    ('   rain ', DIM), (f"{today['chance']:.0f}%", SKY, True), (f" · {today['rain']:.1f} mm", SOFT),
                    ('   wind ', DIM), (f"{current['wind']:.0f} km/h", INK),
                    ('   humidity ', DIM), (f"{current['humidity']:.0f}%", INK)]
            if today.get('uv') is not None and spans_width(line) + 10 <= strip_w + 20:
                line += [('   UV ', DIM), (f"{today['uv']:.0f}", AMBER if today['uv'] >= 6 else INK)]
            if today.get('sunrise') and today.get('sunset') and x + spans_width(line) + 22 <= width - 2:
                line += [('   sun ', DIM), (f"{today['sunrise']} → {today['sunset']}", rgb('#ffd3a8'))]
            spans(canvas, x, y + 6, line)
        tomorrow = snapshot.get('tomorrow')
        if tomorrow:
            line = [('TOMORROW ', DIM), (f"{tomorrow['min']:.0f}°", heat(tomorrow['min']), True), (' – ', DIM),
                    (f"{tomorrow['max']:.0f}°", heat(tomorrow['max']), True),
                    ('   rain ', DIM), (f"{tomorrow['chance']:.0f}%", SKY, True)]
            if tomorrow.get('code') is not None:
                line += [('   ' + condition(tomorrow['code'])[0].lower(), SOFT)]
            spans(canvas, x, y + 7, line)
        y += band_rows

    # Footer: home, where the location came from, and what happens next.
    y = max(y + gap, height - footer_rows - 1)
    home = datetime.now(ZoneInfo(config['home_zone'])) if t else now.astimezone(ZoneInfo(config['home_zone']))
    minutes = round((home.utcoffset() - now.utcoffset()).total_seconds() / 60)
    ahead = 'same time' if not minutes else f"{abs(minutes) // 60}h{abs(minutes) % 60:02d} {'ahead' if minutes > 0 else 'behind'}"
    line = [('HOME  ', DIM), (config['home_name'].upper() + '  ', MINT), (home.strftime('%H:%M'), MINT, True),
            (home.strftime('  %a') + '  ·  ' + ahead, DIM)]
    spans(canvas, (width - spans_width(line)) // 2, y, line)
    if footer_rows >= 2:
        note = snapshot.get('location_note') or ''
        saved_at = str(snapshot.get('observed_at', ''))[:16].replace('T', ' ')
        if current and snapshot.get('pending'):
            status, tone = f'Updating location and weather · showing {saved_at}', DIM
        elif current and snapshot.get('stale'):
            status, tone = f'OFFLINE · showing weather saved {saved_at}', AMBER
        elif current:
            status, tone = ('Location: ' + note if note else 'Live weather') + ' · Open-Meteo', DIM
        elif city and note:
            status, tone = 'Location: ' + note + ' · no weather yet', DIM
        else:
            status, tone = 'System clock' + ('' if config['weather_enabled'] else ' · weather off'), DIM
        canvas.center(y + 1, status[:width - 2], tone)
    if footer_rows >= 3:
        if lock_at:
            left = (lock_at - datetime.now().astimezone()).total_seconds() if t else 60
            when = f'Locking in {max(0, int(left))} s' if left < 30 else 'Locks at ' + lock_at.astimezone(now.tzinfo).strftime('%H:%M')
            hint = when + ', then the display sleeps  ·  any key or the mouse returns'
        else:
            hint = 'Any key or the mouse returns'
        canvas.center(y + 2, hint[:width - 2], AMBER if lock_at and t and left < 30 else FAINT if not lock_at else DIM)
    if ambient:
        particles(canvas, kind if current else 'clear', is_day if is_day is not None else 8 <= now.hour < 18, t)
    return canvas


def safe_rows(width, height, error):
    """Last-resort frame: plain text that cannot fail on layout."""
    rows = [' ' * width for _ in range(height)]
    lines = [datetime.now().strftime('%H:%M'), datetime.now().strftime('%A %d %B'), '',
             'Afterglow hit a drawing error: ' + error, 'Details: omarchy-ambient-status  (press any key to return)']
    for i, line in enumerate(lines):
        row = height // 2 - 2 + i
        if 0 <= row < height:
            rows[row] = line[:width].center(width)
    return rows


# ----------------------------------------------------------------- terminal
class Terminal:
    def __init__(self):
        self.fd = sys.stdin.fileno()
        self.saved = None
        self.pointer = self.anchor = None
        self.started = time.monotonic()

    def __enter__(self):
        try:
            self.saved = termios.tcgetattr(self.fd)
            tty.setcbreak(self.fd)
        except (termios.error, OSError):
            self.saved = None
        self.write(f'\033]11;{BACKGROUND}\007\033[?25l\033[?7l\033[2J\033[H\033[?1003h')
        return self

    def __exit__(self, *_):
        self.write('\033[?1003l\033[?7h\033[0m\033[?25h')
        if self.saved is not None:
            try:
                termios.tcsetattr(self.fd, termios.TCSADRAIN, self.saved)
            except (termios.error, OSError):
                pass

    @staticmethod
    def write(data):
        try:
            sys.stdout.write(data)
            sys.stdout.flush()
        except (OSError, ValueError):
            pass

    @staticmethod
    def size():
        size = shutil.get_terminal_size((80, 24))
        return size.columns, size.lines

    def wait_for_resize(self, limit=2.0):
        # Terminals open at 80x24 and resize once the compositor has sized the
        # window; an effect started earlier draws a tiny canvas in the corner.
        deadline = time.monotonic() + limit
        while self.size() == (80, 24) and time.monotonic() < deadline and not STOP.is_set():
            time.sleep(.02)
        return self.size()

    def pressed(self, timeout):
        """True if the user typed or moved the mouse in this window."""
        try:
            if not select.select([self.fd], [], [], timeout)[0]:
                return False
            data = os.read(self.fd, 256)
        except (OSError, ValueError):
            STOP.wait(timeout)
            return False
        if data and time.monotonic() - self.started < .6:
            return False
        # A window sliding into place reports the pointer "moving" across it
        # although the pointer is at rest. Keys and clicks count at once; motion
        # counts only when the pointer has also moved on the screen itself.
        rest, moved, travelled = data, False, False
        while rest.startswith(b'\033[M') and len(rest) >= 6:
            button, point, rest = rest[3] - 32, (rest[4], rest[5]), rest[6:]
            if not button & 32:
                moved = True
            elif time.monotonic() - self.started < SETTLE:
                continue
            elif self.pointer is None:
                self.pointer, self.anchor = point, cursor_position()
            elif abs(point[0] - self.pointer[0]) + abs(point[1] - self.pointer[1]) >= 2:
                travelled = True
        if travelled and not moved:
            now_at = cursor_position()
            moved = now_at is None or self.anchor is None or now_at != self.anchor
        if not data:
            # End of input: the terminal is gone. Stop instead of spinning.
            global END_REASON
            END_REASON = END_REASON or 'terminal closed'
            STOP.set()
            return False
        if not rest and not moved:
            return False
        log('input', kind='mouse' if moved and not rest else 'key', data=data[:24].hex(),
            after=round(time.monotonic() - self.started, 2))
        return True


def effects_available():
    try:
        text = subprocess.run(['ttfx', '--help'], capture_output=True, timeout=3, check=True).stdout.decode()
        block = text.split('Commands:', 1)[1].split('Options:', 1)[0]
        return [line.split()[0] for line in block.splitlines() if line.strip() and line.split()[0] != 'help']
    except (OSError, IndexError, subprocess.SubprocessError):
        return []


def run_effect(terminal, source, effect, limit, extra=(), anchor='c'):
    """Run one ttfx effect to its natural end. Returns how it finished."""
    command = ['ttfx', '-i', str(source), '--frame-rate', '120', '--canvas-width', '0', '--canvas-height', '0',
               '--reuse-canvas', '--anchor-canvas', anchor, '--anchor-text', anchor, '--no-eol', '--no-restore-cursor',
               '--terminal-background-color', BACKGROUND.lstrip('#'), *extra, effect]
    began = time.monotonic()
    try:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as error:
        log('effect', effect=effect, result='could not start', error=str(error))
        return 'failed'
    result = None
    while result is None:
        if terminal.pressed(.05):
            dismiss('keyboard or mouse')
        if STOP.is_set():
            result = 'interrupted'
        elif process.poll() is not None:
            result = 'completed' if process.returncode == 0 else 'failed'
        elif time.monotonic() - began > limit:
            result = 'watchdog'
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    log('effect', effect=effect, result=result, seconds=round(time.monotonic() - began, 1),
        exit=process.returncode)
    terminal.write('\033[0m\033[?25l\033[?7l\033[?1003h')
    return result


def paint(terminal, rows, previous):
    # A full repaint clears inside the synchronized update, so the handover
    # from the reveal never shows an empty frame.
    out = ['\033[?2026h' + ('\033[2J' if previous is None else '')]
    for index, row in enumerate(rows):
        if previous is None or index >= len(previous) or previous[index] != row:
            out.append(f'\033[{index + 1};1H{row}')
    out.append('\033[?2026l')
    if len(out) > 2:
        terminal.write(''.join(out))


def live(terminal, config, lock_at, preview_seconds):
    began = time.monotonic()
    previous, size, frames, failed, beat = None, None, 0, None, 0
    while not STOP.is_set():
        if preview_seconds and time.monotonic() - began >= preview_seconds:
            return 'preview finished'
        now_size = terminal.size()
        if now_size != size:
            size, previous = now_size, None
        try:
            if failed:
                rows = safe_rows(size[0], size[1], failed)
            else:
                rows = compose(size[0], size[1], SNAPSHOT, config, time.monotonic() - began + .01, lock_at=lock_at).rows()
        except Exception as error:  # A drawing bug must not leave an empty screen.
            failed = f'{type(error).__name__}: {error}'
            log('render-error', error=failed, trace=traceback.format_exc()[-1500:], columns=size[0], rows=size[1])
            continue
        paint(terminal, rows, previous)
        previous = rows
        frames += 1
        if time.monotonic() - beat > 5:
            beat = time.monotonic()
            heartbeat('live' if not failed else 'safe-mode', frames, size)
        if terminal.pressed(.15):
            dismiss('keyboard or mouse')
    return None


def run(args, config):
    global END_REASON
    with Terminal() as terminal:
        size = terminal.wait_for_resize()
        log('start', columns=size[0], rows=size[1], term=os.getenv('TERM'), preview=bool(args.preview_seconds))
        heartbeat('starting', 0, size)
        cursor_hidden(True)
        lock_at = lock_deadline()
        known = effects_available()
        logo = Path.home()/'.config/omarchy/branding/screensaver.txt'
        if config['logo'] and not args.no_splash and known and logo.exists() and not STOP.is_set():
            heartbeat('logo', 0, size)
            choices = [name for name in known if name not in config['skip_effects']] or known
            if run_effect(terminal, logo, random.choice(choices), config['max_effect_seconds']) == 'completed':
                # Hold the finished logo briefly, and a little longer if the
                # first location and weather answer is still on its way.
                held = time.monotonic()
                while not STOP.is_set() and (time.monotonic() - held < 1.2 or (
                        config['weather_enabled'] and not FETCHED.is_set() and time.monotonic() - held < 6)):
                    if terminal.pressed(.1):
                        dismiss('keyboard or mouse')
        if config['reveal'] and not args.no_splash and known and not STOP.is_set():
            heartbeat('reveal', 0, size)
            size = terminal.size()
            sheet = compose(size[0], size[1], SNAPSHOT, config, lock_at=lock_at, ambient=False)
            # ttfx drops blank leading lines, which would lift the whole sheet a
            # row above where the live display draws it. Pin the top-left cell
            # with a mark in the background colour.
            if sheet.empty(0, 0):
                sheet.text(0, 0, '.', rgb(BACKGROUND))
            poster = sheet.rows()
            with tempfile.NamedTemporaryFile('w', suffix='.txt', prefix='afterglow-', delete=False) as handle:
                handle.write('\n'.join(poster))
            try:
                terminal.write('\033[2J\033[H')
                choices = [name for name in REVEALS if name in known and name not in config['skip_effects']]
                if choices:
                    run_effect(terminal, handle.name, random.choice(choices), config['max_effect_seconds'],
                               ('--existing-color-handling', 'always'), anchor='nw')
            finally:
                Path(handle.name).unlink(missing_ok=True)
        if not STOP.is_set():
            log('live')
            END_REASON = live(terminal, config, lock_at, args.preview_seconds) or END_REASON


def status():
    """Explain the most recent run and the current idle/lock state."""
    try:
        events = [json.loads(line) for line in EVENTS.read_text().splitlines()[-400:]]
    except (OSError, ValueError):
        events = []
    starts = [i for i, event in enumerate(events) if event.get('event') == 'start']
    last = events[starts[-1]:] if starts else []
    print('Afterglow status')
    if not last:
        print('  No screensaver run has been recorded yet.')
    for event in last:
        detail = ', '.join(f'{k}={v}' for k, v in event.items() if k not in ('at', 'pid', 'event', 'trace') and v is not None)
        print(f"  {event['at'][11:19]}  {event['event']:<13} {detail}")
    try:
        beat = json.loads(HEARTBEAT.read_text())
        alive = Path(f"/proc/{beat['pid']}").exists()
        age = time.time() - beat['at']
        print(f"  Renderer pid {beat['pid']} is {'running' if alive else 'not running'}; last frame report "
              f"{age:.0f} s ago in phase '{beat['phase']}' after {beat['frames']} frames at {beat['columns']}x{beat['rows']}.")
    except (OSError, ValueError, KeyError):
        beat, alive = None, False
    locked = session_locked()
    idle = shell_json('idle', 'status') or {}
    try:
        monitors = json.loads(subprocess.run(['hyprctl', 'monitors', '-j'], capture_output=True, timeout=2).stdout)
        display = ', '.join(f"{m['name']} {'on' if m.get('dpmsStatus') else 'OFF'}" for m in monitors)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError):
        display = 'unknown'
    print(f"  Session locked: {locked}.  Displays: {display}.")
    print(f"  Idle service: screensaver after {idle.get('screensaver')} s, lock after {idle.get('lock')} s, "
          f"stay-awake {idle.get('stayAwake')}, last event '{idle.get('lastEvent')}'.")
    errors = [event for event in last if event['event'] in ('render-error', 'crash')]
    ended = next((event for event in reversed(last) if event['event'] == 'end'), None)
    print('Verdict:')
    if errors:
        print(f"  RENDERING FAILURE in the last run: {errors[-1].get('error')}. See {EVENTS}.")
    elif last and not ended and not alive:
        print('  The last run stopped without a clean exit (killed or crashed before it could report).')
    elif ended and ended.get('locked'):
        print('  Normal: the last run drew correctly and was ended by the system lock. The lock screen blanks the')
        print('  display about 5 s after it appears; a dark screen after that is the display sleeping, not a fault.')
    elif ended:
        print(f"  The last run drew correctly and ended: {ended.get('reason')}.")
    elif alive:
        print('  The renderer is running now.')
    if locked:
        print('  Right now the session is locked' + (' and the display is off' if 'OFF' in display else '')
              + ': a dark screen at this moment is the lock, not the screensaver.')
    return 1 if errors else 0


def main():
    global SNAPSHOT, END_REASON
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-splash', action='store_true', help='skip the logo and reveal animations')
    parser.add_argument('--preview-seconds', type=int, default=0, help='exit after this long on the dashboard')
    parser.add_argument('--status', action='store_true', help='explain the last run and the lock state')
    parser.add_argument('--render', metavar='COLSxROWS', help='print one dashboard frame and exit')
    args = parser.parse_args()
    if args.status:
        return status()
    config = settings()
    if args.render:
        columns, rows = (int(part) for part in args.render.lower().split('x'))
        print('\n'.join(compose(columns, rows, weather.read_cached() if config['weather_enabled'] else None, config).rows()))
        return 0
    trim_journal()
    if session_locked():
        log('skipped', reason='session is locked')
        return 0
    if config['weather_enabled']:
        SNAPSHOT = weather.read_cached()
        if SNAPSHOT:
            SNAPSHOT['pending'] = True
        threading.Thread(target=weather_worker, args=(config,), daemon=True).start()
    threading.Thread(target=focus_worker, daemon=True).start()
    threading.Thread(target=mouse_worker, daemon=True).start()

    def stopped(number, _frame):
        global END_REASON
        # SIGHUP/SIGTERM arrive when the lock closes this terminal.
        END_REASON = END_REASON or f'{signal.Signals(number).name} (system lock or window closed)'
        STOP.set()
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, stopped)
    code = 0
    try:
        run(args, config)
    except Exception as error:
        log('crash', error=f'{type(error).__name__}: {error}', trace=traceback.format_exc()[-1500:])
        code = 1
    finally:
        STOP.set()
        cursor_hidden(False)
        if DISMISSED:
            close_saver_windows()
        # The lock closes this terminal a moment before the session reports
        # itself locked, so give it time before recording why we ended.
        returned = DISMISSED and END_REASON and ('keyboard' in END_REASON or 'mouse' in END_REASON)
        locked = session_locked()
        for _ in range(8):
            if locked or returned or not END_REASON:
                break
            time.sleep(.3)
            locked = session_locked()
        log('end', reason=(END_REASON or 'finished') + (' · session locked' if locked else ''), locked=locked)
    return code


if __name__ == '__main__':
    sys.exit(main())
