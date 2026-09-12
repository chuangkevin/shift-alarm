import hashlib
import hmac
import json
import pytest
from fastapi.testclient import TestClient
import app

client = TestClient(app.app)
headers = {'Authorization': 'Bearer ' + app.DEVICE_TOKEN}

@pytest.fixture
def release(tmp_path, monkeypatch):
    monkeypatch.setattr(app, 'DATA', tmp_path)
    d = tmp_path / 'releases'
    d.mkdir()
    raw = bytearray(512)
    raw[0] = 0xe9
    raw[12:14] = (9).to_bytes(2, 'little')
    raw[32:36] = (0xabcd5432).to_bytes(4, 'little')
    raw[48:53] = b'0.2.1'
    board = app.FIRMWARE_BOARD.encode()
    raw[80:80+len(board)] = board
    digest = hashlib.sha256(raw).hexdigest()
    (d / (digest + '.bin')).write_bytes(raw)
    m = {'board': app.FIRMWARE_BOARD, 'version': '0.2.1', 'size': len(raw), 'sha256': digest}
    (d / 'active.json').write_text(json.dumps(m))
    return d, m

def test_device_release_requires_auth_and_has_matching_signature(release):
    d, m = release
    assert client.get('/api/device/update').status_code == 401
    result = client.get('/api/device/update', headers=headers).json()['manifest']
    canonical = f"{m['board']}\n{m['version']}\n{m['size']}\n{m['sha256']}\n".encode()
    assert result['hmac_sha256'] == hmac.new(app.DEVICE_TOKEN.encode(), canonical, hashlib.sha256).hexdigest()
    assert client.get(result['path']).status_code == 401
    assert client.get(result['path'], headers=headers).content == (d / (m['sha256'] + '.bin')).read_bytes()

def test_corruption_prevents_offer_and_download(release):
    d, m = release
    (d / (m['sha256'] + '.bin')).write_bytes(b'bad')
    assert client.get('/api/device/update', headers=headers).status_code == 503
    assert client.get('/api/device/firmware/'+m['sha256']+'.bin', headers=headers).status_code == 503

def test_manifest_cannot_relabel_image(release):
    d, m = release
    m['version'] = '0.2.2'
    (d / 'active.json').write_text(json.dumps(m))
    assert client.get('/api/device/update', headers=headers).status_code == 503

def test_no_release_is_not_an_error(tmp_path, monkeypatch):
    monkeypatch.setattr(app, 'DATA', tmp_path)
    assert client.get('/api/device/update', headers=headers).json() == {'available': False, 'manifest': None}
