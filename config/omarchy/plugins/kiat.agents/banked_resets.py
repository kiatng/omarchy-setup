#!/usr/bin/python3
"""Read banked reset grants; never redeem them. Cache only counts and expiries."""
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import runpy
import sys
import time
import urllib.request

sys.dont_write_bytecode = True


def timestamp(value):
    try:
        return dt.datetime.fromisoformat(str(value).replace('Z', '+00:00')).timestamp()
    except (ValueError, TypeError, OverflowError):
        return 0


def claude_counts(payload, now):
    block = payload.get('cedar_ember')
    if not isinstance(block, dict) or not isinstance(block.get('grants'), list):
        raise ValueError('Reset grants unavailable')
    groups = {'weekly': [], 'fable': []}
    for grant in block['grants'][:100]:
        if not isinstance(grant, dict):
            continue
        count = grant.get('resets_left')
        end = timestamp(grant.get('ends_at'))
        start = timestamp(grant.get('starts_at'))
        if type(count) is not int or not 0 < count <= 100 or end <= now or start > now or grant.get('paused'):
            continue
        clears = grant.get('clears') or []
        group = 'weekly' if 'seven_day' in clears else 'fable' if 'seven_day_overage_included' in clears else None
        if group:
            groups[group].append((count, end))
    return {key: {'available': sum(c for c, _ in grants),
                  'expiresAt': min((end for _, end in grants), default=0)} for key, grants in groups.items()}


def codex_counts(payload, now):
    block = payload.get('rateLimitResetCredits')
    if not isinstance(block, dict) or not isinstance(block.get('credits'), list):
        raise ValueError('Reset credits unavailable')
    credits = [c for c in block['credits'][:100] if isinstance(c, dict) and c.get('status') == 'available'
               and (not c.get('expiresAt') or float(c['expiresAt']) > now)]
    return {'weekly': {'available': len(credits), 'expiresAt': min((float(c['expiresAt']) for c in credits if c.get('expiresAt')), default=0)}}


def main():
    now = time.time()
    cache_path = Path(os.environ.get('XDG_CACHE_HOME', Path.home()/'.cache'))/'omarchy/agent-banked-resets.json'
    try:
        cache = json.loads(cache_path.read_text())
    except (OSError, ValueError):
        cache = {}
    output = {}
    for provider in sys.argv[1:]:
        if provider not in ('claude', 'codex'):
            continue
        module = runpy.run_path('/usr/bin/omarchy-agent-usage-' + provider)
        accounts = module['registered_accounts'](provider)
        if not accounts:
            accounts = [{'id': ':primary', 'primary': True, 'active': True}]
        result = {'accounts': {}}
        for account in accounts[:16]:
            home = Path(account.get('home') or Path.home()/('.'+provider)).expanduser()
            try:
                # Never reuse a balance after this home's sign-in changes.
                auth_file = home/('.credentials.json' if provider == 'claude' else 'auth.json')
                identity = hashlib.sha256(auth_file.read_bytes()).hexdigest()
                key = provider + ':' + str(home) + ':' + identity
                stored = cache.get(key, {})
                if now - stored.get('checkedAt', 0) < 60:
                    value = stored['value']
                elif provider == 'claude':
                    token, *_ = module['oauth_login'](home)
                    request = urllib.request.Request(module['USAGE_ENDPOINT']+'?cedar_ember=1', headers={
                        'Authorization': 'Bearer '+token, 'anthropic-beta': 'oauth-2025-04-20',
                        'User-Agent': 'claude-cli/2.1.322 (external, cli)', 'Accept': 'application/json'})
                    value = claude_counts(module['open_json'](request, timeout=10), now)
                else:
                    fetch = module['fetch_codex_rpc']
                    captured = {}
                    original = fetch.__globals__['reset_credits']
                    def capture(payload):
                        captured.update(codex_counts(payload, now))
                        return original(payload)
                    fetch.__globals__['reset_credits'] = capture
                    response = fetch(home, account.get('primary', False))
                    if not response.get('ok') or not captured:
                        raise ValueError('Reset credits unavailable')
                    value = captured
                cache[key] = {'checkedAt': stored.get('checkedAt', now) if now - stored.get('checkedAt', 0) < 60 else now, 'value': value}
            except Exception:
                value = {'unavailable': True}
            result['accounts'][str(account['id'])] = value
            if account.get('active'):
                result['current'] = value
        if 'current' not in result and len(accounts) == 1:
            result['current'] = result['accounts'][str(accounts[0]['id'])]
        output[provider] = result
    cache = {k: v for k, v in cache.items() if now - v.get('checkedAt', 0) < 86400}
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    temp = cache_path.with_suffix('.tmp')
    temp.write_text(json.dumps(cache))
    temp.replace(cache_path)
    print(json.dumps(output))


if __name__ == '__main__':
    main()
