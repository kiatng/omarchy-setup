#!/usr/bin/env python3
"""Resolve foreground process names for terminal PIDs via /proc."""

import json
import os
import sys

WRAPPERS = {"env", "node", "python", "python3", "bun", "deno"}
MAX_PIDS = 64


def read_stat(pid_str):
    with open(f"/proc/{pid_str}/stat", "r", encoding="utf-8", errors="replace") as f:
        stat = f.read()
    rp = stat.rfind(")")
    if rp == -1:
        return None
    rest = stat[rp + 2 :].split()
    if len(rest) < 6:
        return None
    return int(rest[1]), int(rest[2]), int(rest[4]), int(rest[5])


def process_name(pid):
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            raw = f.read()
        args = [a.decode("utf-8", "replace") for a in raw.split(b"\x00") if a]
    except OSError:
        args = []

    name = ""
    if args:
        base = os.path.basename(args[0])
        if base in WRAPPERS and len(args) > 1 and not args[1].startswith("-"):
            base = os.path.basename(args[1])
        name = base

    if not name:
        try:
            with open(f"/proc/{pid}/comm", "r", encoding="utf-8", errors="replace") as f:
                name = f.read().strip()
        except OSError:
            pass

    if name.endswith(".exe"):
        name = name[:-4]
    return name[:64]


def resolve_foreground(wanted_pids):
    children = {}
    pgid_members = {}

    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        try:
            parsed = read_stat(entry.name)
            if not parsed:
                continue
            ppid, pgid, tty_nr, tpgid = parsed
            pid = int(entry.name)
            if ppid in wanted_pids and tty_nr != 0 and tpgid > 0:
                children.setdefault(ppid, []).append((pid, tpgid))
            if pgid > 0:
                pgid_members.setdefault(pgid, []).append(pid)
        except OSError:
            continue

    result = {}
    for term_pid in wanted_pids:
        cands = children.get(term_pid)
        if not cands:
            continue
        child_pid, tpgid = cands[0]
        target = tpgid if os.path.exists(f"/proc/{tpgid}") else None
        if target is None:
            members = pgid_members.get(tpgid, [])
            target = members[0] if members else child_pid
        name = process_name(target)
        if name:
            result[str(term_pid)] = name
    return result


def main():
    wanted = set()
    for arg in sys.argv[1 : 1 + MAX_PIDS]:
        if arg.isdigit():
            wanted.add(int(arg))
    if not wanted:
        print("{}")
        return
    print(json.dumps(resolve_foreground(wanted)))


if __name__ == "__main__":
    main()
