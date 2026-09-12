import hashlib
import hmac
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
import tempfile
import contextlib
import io
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ota_tailnet as ota

class Fake:
    def __init__(self):
        self.calls = []
        self.status = {'version': '0.2.7', 'clockReady': True, 'ringing': False,
                       'backendTransport': 'tailscale', 'backendHost': '100.126.226.79'}
    def request(self, url, headers=None, body=None):
        self.calls.append((url, body))
        assert body is None
        return "const nonce='" + 'a' * 32 + "'"
    def get(self, url, headers=None):
        self.calls.append((url, None))
        if url.endswith('/api/tailnet'):
            return {'connected': True, 'acl_ready': True, 'ip': '100.90.212.116'}
        if url.endswith('/api/status'): return self.status
        if url.endswith('/api/update'): return {'ready': True, 'busy': False}
        if url.endswith('/api/device/update'): return release()
        if url.endswith('/api/health'): return {'ok': True}
        raise AssertionError(url)

def release():
    m = {'board': ota.BOARD, 'version': '0.2.8', 'size': 1024, 'sha256': 'b' * 64}
    raw = f"{ota.BOARD}\n0.2.8\n1024\n{'b'*64}\n".encode()
    m['hmac_sha256'] = hmac.new(b'test-secret', raw, hashlib.sha256).hexdigest()
    return {'available': True, 'manifest': m}

class Tests(unittest.TestCase):
    def test_terminal_failure_does_not_retry_post(self):
        class Ended(Fake):
            def request(self, url, headers=None, body=None):
                if body is not None:
                    self.calls.append((url, body))
                    return 'accepted'
                return super().request(url, headers, body)
        f = Ended()
        with tempfile.TemporaryDirectory() as directory:
            token = Path(directory) / 'token'; token.write_text('test-secret'); token.chmod(0o600)
            argv = ['ota_tailnet.py', '--device', 'http://100.90.212.116',
                    '--backend', 'http://100.126.226.79:8237', '--token-file', str(token),
                    '--version', '0.2.8', '--start', '--power-confirmed']
            with patch.object(sys, 'argv', argv), patch.object(ota, 'Client', return_value=f), \
                 patch.object(ota.time, 'sleep'), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(ValueError, 'ended update'):
                    ota.main()
        self.assertEqual(sum(body is not None for _, body in f.calls), 1)

    def test_wait_bounds(self):
        self.assertEqual(ota.wait_seconds('900'), 900)
        for value in ('59', '1801'):
            with self.assertRaises(ota.argparse.ArgumentTypeError): ota.wait_seconds(value)

    def test_urls(self):
        self.assertEqual(ota.tailnet_url('http://100.90.212.116'), 'http://100.90.212.116')
        for url in ['http://192.168.18.160', 'http://localhost', 'https://100.90.212.116',
                    'http://100.90.212.116/foo', 'http://user@100.90.212.116',
                    'http://100.90.212.116?x=1', 'http://100.90.212.116:8081']:
            with self.assertRaises(ValueError): ota.tailnet_url(url)
    def test_hmac_and_version(self):
        ota.verify_manifest(release(), 'test-secret', '0.2.8', '0.2.7')
        for token, expected, current in [('wrong', '0.2.8', '0.2.7'),
                                         ('test-secret', '0.2.9', '0.2.7'),
                                         ('test-secret', '0.2.8', '0.2.8')]:
            with self.assertRaises(ValueError): ota.verify_manifest(release(), token, expected, current)
    def test_preflight_is_read_only(self):
        f = Fake()
        ota.preflight(f, 'http://100.90.212.116', 'http://100.126.226.79:8237', 'test-secret', '0.2.8')
        self.assertTrue(all(body is None for _, body in f.calls))
    def test_lan_and_missing_transport_refused(self):
        for transport in ('lan', None):
            f = Fake(); f.status['backendTransport'] = transport
            with self.assertRaises(ValueError):
                ota.preflight(f, 'http://100.90.212.116', 'http://100.126.226.79:8237', 'test-secret', '0.2.8')
    def test_post_update_transport_identity_and_local_schedule(self):
        before = {'rotation': 90, 'localSchedule': True, 'revision': 'saved', 'alarmCount': 4}
        after = dict(before, backendTransport='tailscale', backendHost='100.126.226.79')
        native = {'connected': True, 'acl_ready': True, 'ip': '100.90.212.116'}
        def check():
            return ota.verify_after(after, before, native, 'http://100.90.212.116', 'http://100.126.226.79:8237')
        self.assertTrue(check())
        for key, bad in [('backendTransport', 'lan'), ('backendHost', '192.168.18.31'),
                         ('rotation', 0), ('revision', 'lost'), ('alarmCount', 0)]:
            old = after[key]; after[key] = bad
            with self.assertRaises(ValueError): check()
            after[key] = old
        native['ip'] = '100.90.212.117'; self.assertFalse(check())
        native['ip'] = '100.90.212.116'
        before['localSchedule'] = after['localSchedule'] = False
        after['alarmCount'] = 3; after['revision'] = 'synced'
        self.assertTrue(check())

    def test_redirect_refused(self):
        with self.assertRaises(ValueError): ota.NoRedirect().redirect_request(None)

if __name__ == '__main__': unittest.main()
