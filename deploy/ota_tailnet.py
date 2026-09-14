"""Tailscale OTA read-only preflight, download, install, or discard. Mutations are never retried."""
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


def device_preflight(client, device):
    page = client.request(device + '/update')
    match = re.search(r"const nonce='([0-9a-f]{32})'", page)
    if not match:
        raise ValueError('Update page nonce unavailable')
    headers = {'X-Setup-Nonce': match[1]}
    status = client.get(device + '/api/status')
    update = client.get(device + '/api/update', headers)
    required = ('currentVersion', 'phase', 'busy', 'markerFault', 'canDownload', 'downloadReason',
                'canInstall', 'installReason', 'session', 'sequence')
    if any(key not in update for key in required) or update['currentVersion'] != status.get('version'):
        raise ValueError('Firmware does not provide the two-phase OTA contract')
    return headers, status, update


def preflight(client, device, backend=None, token=None, expected=None, action='download'):
    headers, status, update = device_preflight(client, device)
    if action == 'install':
        if update.get('canInstall') is not True:
            raise ValueError('OTA install blocked: ' + str(update.get('installReason', 'unknown')))
        staged = update.get('staged')
        if expected and (not isinstance(staged, dict) or staged.get('version') != expected):
            raise ValueError('Staged version mismatch')
        return headers, status, update
    if action == 'discard':
        if not isinstance(update.get('staged'), dict) and update.get('markerFault') is not True:
            raise ValueError('OTA discard blocked: no-staged-update')
        return headers, status, update
    if action is None:
        return headers, status, update
    if update.get('canDownload') is not True:
        raise ValueError('OTA download blocked: ' + str(update.get('downloadReason', 'unknown')))
    if not backend or token is None or not expected:
        raise ValueError('Download preflight requires backend, token and version')
    native = client.get(device + '/api/tailnet', headers)
    if native.get('connected') is not True or native.get('acl_ready') is not True or 'http://' + native.get('ip', '') != device:
        raise ValueError('Native Tailscale identity/readiness mismatch')
    from urllib.parse import urlsplit
    if status.get('backendTransport') != 'tailscale' or status.get('backendHost') != urlsplit(backend).hostname:
        raise ValueError('Firmware does not attest a fixed native Tailscale backend')
    if client.get(device + '/api/health').get('ok') is not True:
        raise ValueError('Backend health probe failed')
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
    for key in ('screenTimeoutMinutes', 'screenBrightness', 'firstConsecutiveOnly'):
        if key in before and after.get(key) != before[key]:
            raise ValueError('Saved setting changed')
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


def make_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True, type=tailnet_url)
    parser.add_argument('--backend', type=tailnet_url)
    parser.add_argument('--token-file', type=Path)
    parser.add_argument('--version')
    parser.add_argument('--wait-seconds', type=wait_seconds, default=900)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--download', action='store_true')
    mode.add_argument('--install', action='store_true')
    mode.add_argument('--discard', action='store_true')
    parser.set_defaults(read_only=True)
    return parser


def read_token(parser, path):
    if path is None:
        parser.error('--download requires --token-file')
    if path.stat().st_mode & 0o077:
        parser.error('Token file must be private (chmod 600)')
    token = path.read_text().strip()
    if not token or '\n' in token or '\r' in token:
        parser.error('Invalid token file')
    return token


def main():
    parser = make_parser()
    args = parser.parse_args()
    action = 'download' if args.download else 'install' if args.install else 'discard' if args.discard else None
    token = None
    if action == 'download':
        if not args.backend or not args.version:
            parser.error('--download requires --backend and --version')
        token = read_token(parser, args.token_file)
    if action == 'install' and not args.version:
        parser.error('--install requires --version')
    client = Client()
    headers, before, detail = preflight(client, args.device, args.backend, token, args.version, action)
    if action is None:
        print('Read-only preflight:', detail['phase'], 'download=' + detail['downloadReason'],
              'install=' + detail['installReason'])
        return
    path = '/api/update/' + action
    client.request(args.device + path, headers, b'')  # exactly one mutation POST
    print('Accepted', action, 'request; no mutation retry')
    if action == 'discard':
        return
    deadline = time.monotonic() + args.wait_seconds
    while time.monotonic() < deadline:
        time.sleep(5)
        try:
            _, after, update = device_preflight(client, args.device)
            if action == 'download':
                staged = update.get('staged')
                if isinstance(staged, dict) and staged.get('version') == args.version and update.get('busy') is False:
                    print('Staged version verified by device:', args.version)
                    return
                if update.get('busy') is False and update.get('phase') == 'error':
                    raise ValueError('Device ended download; inspect read-only update status')
                if type(update.get('received')) is int and type(update.get('total')) is int:
                    print('Download bytes:', update['received'], '/', update['total'])
                continue
            if after.get('version') != args.version:
                continue
            if args.backend:
                page = client.request(args.device + '/update')
                nonce = re.search(r"const nonce='([0-9a-f]{32})'", page)
                if not nonce:
                    continue
                native = client.get(args.device + '/api/tailnet', {'X-Setup-Nonce': nonce[1]})
                if not verify_after(after, before, native, args.device, args.backend):
                    continue
            print('Installed version reachable; saved-setting checks passed where requested')
            return
        except (OSError, json.JSONDecodeError):
            continue
    raise ValueError('Operation not verified within configured wait; do not automatically retry or flash')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print('OTA stopped:', type(exc).__name__, '(inspect private diagnostics; no automatic retry)')
        raise SystemExit(1)
