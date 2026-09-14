"""Tailscale-only OTA preflight/start. Never writes flash or publishes releases."""
import argparse
import hashlib
import hmac
import ipaddress
import json
from pathlib import Path
import re
import time
import urllib.request

from publish_firmware import BOARD, version_tuple


def tailnet_url(value):
    from urllib.parse import urlsplit
    u = urlsplit(value)
    try:
        address = ipaddress.IPv4Address(u.hostname)
        port = u.port
    except ValueError:
        raise ValueError('Require literal Tailscale IPv4 URL') from None
    if (u.scheme != 'http' or address not in ipaddress.ip_network('100.64.0.0/10')
            or u.username or u.password or u.path not in ('', '/') or u.query or u.fragment
            or port not in (None, 80, 8237)):
        raise ValueError('Require direct Tailscale HTTP URL; no LAN, DNS, proxy or redirect')
    return value.rstrip('/')


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        raise ValueError('Redirect refused')


class Client:
    def __init__(self):
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def request(self, url, headers=None, body=None):
        request = urllib.request.Request(url, data=body, headers=headers or {})
        with self.opener.open(request, timeout=20) as response:
            data = response.read(65537)
            if len(data) > 65536:
                raise ValueError('Response exceeds limit')
            return data.decode('utf-8')

    def get(self, url, headers=None):
        return json.loads(self.request(url, headers))


def verify_manifest(reply, token, expected, current):
    m = reply.get('manifest')
    if reply.get('available') is not True or not isinstance(m, dict):
        raise ValueError('No published release')
    if (m.get('board') != BOARD or m.get('version') != expected
            or type(m.get('size')) is not int or not 288 <= m['size'] <= 0x400000
            or not re.fullmatch('[0-9a-f]{64}', m.get('sha256', ''))):
        raise ValueError('Release identity/size mismatch')
    if version_tuple(expected) <= version_tuple(current):
        raise ValueError('Release must be newer than running version')
    canonical = f"{BOARD}\n{expected}\n{m['size']}\n{m['sha256']}\n".encode()
    signature = hmac.new(token.encode(), canonical, hashlib.sha256).hexdigest()
    if not hmac.compare_digest(signature, m.get('hmac_sha256', '')):
        raise ValueError('Manifest signature mismatch')
    return m


def preflight(client, device, backend, token, expected):
    page = client.request(device + '/update')
    match = re.search(r"const nonce='([0-9a-f]{32})'", page)
    if not match:
        raise ValueError('Update page nonce unavailable')
    headers = {'X-Setup-Nonce': match[1]}
    native = client.get(device + '/api/tailnet', headers)
    if native.get('connected') is not True or native.get('acl_ready') is not True or 'http://' + native.get('ip', '') != device:
        raise ValueError('Native Tailscale identity/readiness mismatch')
    status = client.get(device + '/api/status')
    from urllib.parse import urlsplit
    if status.get('backendTransport') != 'tailscale' or status.get('backendHost') != urlsplit(backend).hostname:
        raise ValueError('Firmware does not attest a fixed native Tailscale backend')
    if client.get(device + '/api/health').get('ok') is not True:
        raise ValueError('Backend health probe failed')
    update = client.get(device + '/api/update', headers)
    if update.get('canStart') is not True:
        reason = update.get('reason') if isinstance(update.get('reason'), str) else 'unknown'
        raise ValueError('OTA preflight blocked: ' + reason)
    if status.get('clockReady') is not True or status.get('ringing') is not False:
        raise ValueError('Clock or alarm guard not ready')
    manifest = verify_manifest(client.get(backend + '/api/device/update',
                               {'Authorization': 'Bearer ' + token}), token, expected, status['version'])
    return headers, status, manifest



def verify_after(after, before, native, device, backend):
    from urllib.parse import urlsplit
    if (after.get('backendTransport') != 'tailscale'
            or after.get('backendHost') != urlsplit(backend).hostname):
        raise ValueError('Updated backend transport mismatch')
    for key in ('rotation', 'localSchedule'):
        if after.get(key) != before.get(key):
            raise ValueError('Saved setting changed')
    if ('screenTimeoutMinutes' in before
            and after.get('screenTimeoutMinutes') != before['screenTimeoutMinutes']):
        raise ValueError('Saved screen timeout changed')
    if ('screenBrightness' in before
            and after.get('screenBrightness') != before['screenBrightness']):
        raise ValueError('Saved screen brightness changed')
    if ('firstConsecutiveOnly' in before
            and after.get('firstConsecutiveOnly') != before['firstConsecutiveOnly']):
        raise ValueError('Saved consecutive-workday setting changed')
    if before.get('localSchedule') is True:
        for key in ('revision', 'alarmCount'):
            if key not in before or after.get(key) != before[key]:
                raise ValueError('Saved local schedule changed')
    return (native.get('connected') is True and native.get('acl_ready') is True
            and 'http://' + native.get('ip', '') == device)


def wait_seconds(value):
    seconds = int(value)
    if not 60 <= seconds <= 1800:
        raise argparse.ArgumentTypeError('Wait must be 60..1800 seconds')
    return seconds


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True, type=tailnet_url)
    parser.add_argument('--backend', required=True, type=tailnet_url)
    parser.add_argument('--token-file', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--wait-seconds', type=wait_seconds, default=900)
    parser.add_argument('--start', action='store_true')
    parser.add_argument('--power-confirmed', action='store_true')
    args = parser.parse_args()
    if args.start and not args.power_confirmed:
        parser.error('--start requires explicit --power-confirmed')
    if args.token_file.stat().st_mode & 0o077:
        parser.error('Token file must be private (chmod 600)')
    token = args.token_file.read_text().strip()
    if not token or '\n' in token or '\r' in token:
        parser.error('Invalid token file')
    client = Client()
    headers, before, manifest = preflight(client, args.device, args.backend, token, args.version)
    print('Preflight passed; release', manifest['version'], manifest['sha256'])
    if not args.start:
        return
    headers['Content-Type'] = 'application/x-www-form-urlencoded'
    # Never retry this mutation: an ambiguous response requires read-only inspection.
    client.request(args.device + '/api/update/start', headers, b'power_confirmed=1')
    deadline = time.monotonic() + args.wait_seconds
    while time.monotonic() < deadline:
        time.sleep(5)
        try:
            after = client.get(args.device + '/api/status')
            if after.get('version') != args.version:
                progress = client.get(args.device + '/api/update', headers)
                if progress.get('busy') is False:
                    raise ValueError('Device ended update without target version; inspect /update')
                if type(progress.get('received')) is int and type(progress.get('total')) is int:
                    print('Download bytes:', progress['received'], '/', progress['total'])
                continue
            page = client.request(args.device + '/update')
            nonce = re.search(r"const nonce='([0-9a-f]{32})'", page)
            if not nonce:
                continue
            native = client.get(args.device + '/api/tailnet', {'X-Setup-Nonce': nonce[1]})
            if not verify_after(after, before, native, args.device, args.backend):
                continue
            if client.get(args.device + '/api/health').get('ok') is not True:
                continue
            print('Updated version reachable over native Tailscale; rotation/mode and local schedule metadata checks passed')
            return
        except (OSError, json.JSONDecodeError):
            continue
    raise ValueError('Update not verified within configured wait; do not automatically retry or flash')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        # Never echo network bodies, URLs with credentials, nonce or token.
        print('OTA stopped:', type(exc).__name__, '(inspect private diagnostics; no automatic retry)')
        raise SystemExit(1)
