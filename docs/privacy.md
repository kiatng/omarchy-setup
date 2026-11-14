# Privacy boundaries

Public source contains no account stores, passwords, API keys, saved networks, personal locations, histories, caches, or desktop screenshots. Runtime data and private backups stay outside this repository.

- Home clock: UTC. Set `HOME_ZONE` in your installed `kiat.clock/clock.py` for your own home timezone.
- Afterglow: weather and Wi-Fi positioning start **off**. Opt in through `~/.config/omarchy/ambient/settings.json`. Weather/location features contact Omarchy GeoIP and Open-Meteo; Wi-Fi positioning additionally sends nearby access-point identifiers to BeaconDB when enabled.
- The clock's explicit Detect action uses Omarchy GeoIP. The weather widget uses its normal Omarchy/Open-Meteo location behavior. These features can disclose the destination machine's location when used.
- No blanket export of `~/.config`, `~/.codex`, or `~/.claude`; no system-wide files are overwritten.


My Agents uses locally configured credentials to query provider usage. Optional synced aggregation writes usage snapshots to a user-selected folder; leave it off unless intentionally configured. Never commit those snapshots. Relay cloud providers receive captured audio when enabled; configure keys through its keyring integration. No recordings, transcripts, or model weights are bundled.
