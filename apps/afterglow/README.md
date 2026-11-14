# Afterglow

An Omarchy logo animation followed by a readable clock and optional weather dashboard. Install through the repository's `bin/setup install`; the installed files live in `~/.config/omarchy/ambient/`.

Weather and Wi-Fi positioning default off in this portable edition. Settings are local and preserved on reinstall. `home_zone` and `home_name` default to UTC. Enable location features only after reviewing the root README's network disclosure.

Run tests with `python3 -m unittest discover -s apps/afterglow` from the repository root. All radio/location test fixtures are synthetic; no location cache or diagnostic evidence is included.
