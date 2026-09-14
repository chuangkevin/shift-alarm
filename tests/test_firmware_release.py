import asyncio
import hashlib
import hmac
import json
import os
import threading

import pytest
from fastapi.testclient import TestClient
from starlette.requests import ClientDisconnect
import app

client = TestClient(app.app)
headers = {'Authorization': 'Bearer test-device-token'}


@pytest.fixture(autouse=True)
def device_token(monkeypatch):
    monkeypatch.setattr(app, 'DEVICE_TOKEN', 'test-device-token')


@pytest.fixture
def release(tmp_path, monkeypatch):
    monkeypatch.setattr(app, 'DATA', tmp_path)
    d = tmp_path / 'releases'
    d.mkdir()
    raw = bytearray(10_000)
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


def test_firmware_full_stream_has_exact_headers_and_bytes(release):
    d, m = release
    expected = (d / (m['sha256'] + '.bin')).read_bytes()
    with client.stream('GET', f"/api/device/firmware/{m['sha256']}.bin", headers=headers) as response:
        body = b''.join(response.iter_bytes())
    assert response.status_code == 200
    assert response.headers['content-length'] == str(len(expected))
    assert response.headers['content-type'] == 'application/octet-stream'
    assert response.headers['accept-ranges'] == 'bytes'
    assert response.headers['cache-control'] == 'no-store'
    assert response.headers['x-content-type-options'] == 'nosniff'
    assert body == expected


def test_firmware_binary_opens_active_digest_once_with_no_follow(release, monkeypatch):
    d, m = release
    calls = []
    original_open = app.os.open

    def record_open(path, flags):
        calls.append((path, flags))
        return original_open(path, flags)

    monkeypatch.setattr(app.os, 'open', record_open)
    response = client.get(f"/api/device/firmware/{m['sha256']}.bin", headers=headers)
    assert response.status_code == 200
    assert len(calls) == 1
    assert calls[0][0] == d / (m['sha256'] + '.bin')
    assert calls[0][1] == os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0)


def test_firmware_open_ended_range_has_exact_headers_and_bytes(release):
    d, m = release
    expected = (d / (m['sha256'] + '.bin')).read_bytes()
    start = 4097
    with client.stream(
        'GET', f"/api/device/firmware/{m['sha256']}.bin",
        headers={**headers, 'Range': f'bytes={start}-'},
    ) as response:
        body = b''.join(response.iter_bytes())
    assert response.status_code == 206
    assert response.headers['content-length'] == str(len(expected) - start)
    assert response.headers['content-range'] == f'bytes {start}-{len(expected) - 1}/{len(expected)}'
    assert response.headers['accept-ranges'] == 'bytes'
    assert body == expected[start:]


@pytest.mark.parametrize('range_header', [
    'bytes=', 'bytes=abc-', 'bytes=0-1', 'bytes=0-,4096-', 'bytes=-4096',
    'items=0-', 'bytes=10000-', 'bytes=10001-', 'bytes=' + ('9' * 5000) + '-',
])
def test_firmware_rejects_unsupported_or_out_of_bounds_ranges(release, range_header):
    _, m = release
    response = client.get(
        f"/api/device/firmware/{m['sha256']}.bin",
        headers={**headers, 'Range': range_header},
    )
    assert response.status_code == 416
    assert response.headers['content-range'] == f"bytes */{m['size']}"
    assert response.headers['accept-ranges'] == 'bytes'


def test_firmware_rejects_unauthorized_digest_and_path_traversal(release):
    _, m = release
    other_digest = '0' * 64
    assert client.get(f'/api/device/firmware/{other_digest}.bin', headers=headers).status_code == 404
    assert client.get('/api/device/firmware/..%2Factive.json.bin', headers=headers).status_code == 404


@pytest.mark.parametrize('replacement_type', ['file', 'symlink'])
def test_firmware_stream_keeps_validated_descriptor_after_path_swap(release, tmp_path, monkeypatch, replacement_type):
    d, m = release
    binary = d / (m['sha256'] + '.bin')
    expected = binary.read_bytes()
    replacement = bytes([0x5a]) * len(expected)
    original_available = app.available_firmware

    def swap_after_validation(keep_open=False):
        opened = original_available(keep_open)
        if keep_open and opened:
            binary.unlink()
            if replacement_type == 'file':
                binary.write_bytes(replacement)
            else:
                target = tmp_path / 'replacement.bin'
                target.write_bytes(replacement)
                binary.symlink_to(target)
        return opened

    monkeypatch.setattr(app, 'available_firmware', swap_after_validation)
    response = client.get(f"/api/device/firmware/{m['sha256']}.bin", headers=headers)
    assert response.status_code == 200
    assert response.content == expected
    assert response.content != replacement


def test_firmware_stream_uses_continuous_configured_chunks(release, monkeypatch):
    d, m = release
    read_threads = []
    loop_thread = threading.get_ident()
    original_read = app.read_firmware_descriptor

    def record_read(descriptor, size):
        read_threads.append(threading.get_ident())
        return original_read(descriptor, size)

    async def reject_sleep(_delay):
        pytest.fail('firmware stream must not insert an application-level delay')

    monkeypatch.setattr(app.asyncio, 'sleep', reject_sleep)
    monkeypatch.setattr(app, 'read_firmware_descriptor', record_read)
    raw_descriptor = os.open(d / (m['sha256'] + '.bin'), os.O_RDONLY)
    descriptor = app.FirmwareDescriptor(raw_descriptor)

    async def consume():
        return [chunk async for chunk in app.firmware_chunks(descriptor, 0, m['size'])]

    chunks = asyncio.run(consume())
    assert [len(chunk) for chunk in chunks] == [4096, 4096, 1808]
    assert b''.join(chunks) == (d / (m['sha256'] + '.bin')).read_bytes()
    assert app.FIRMWARE_CHUNK_SIZE == 4096
    assert not hasattr(app, 'FIRMWARE_CHUNK_DELAY_SECONDS')
    assert read_threads and all(thread != loop_thread for thread in read_threads)
    with pytest.raises(OSError):
        os.fstat(raw_descriptor)


def test_firmware_stream_closes_descriptor_once_when_consumer_disconnects(release, monkeypatch):
    d, m = release
    raw_descriptor = os.open(d / (m['sha256'] + '.bin'), os.O_RDONLY)
    descriptor = app.FirmwareDescriptor(raw_descriptor)
    closes = []
    original_close = app.close_firmware_descriptor

    def record_close(value):
        closes.append(value)
        original_close(value)

    monkeypatch.setattr(app, 'close_firmware_descriptor', record_close)

    async def disconnect():
        stream = app.firmware_chunks(descriptor, 0, m['size'])
        assert len(await anext(stream)) == 4096
        await stream.aclose()

    asyncio.run(disconnect())
    assert closes == [raw_descriptor]
    with pytest.raises(OSError):
        os.fstat(raw_descriptor)


def test_full_response_closes_descriptor_once(release, monkeypatch):
    _, m = release
    closes = []
    original_close = app.close_firmware_descriptor

    def record_close(descriptor):
        closes.append(descriptor)
        original_close(descriptor)

    monkeypatch.setattr(app, 'close_firmware_descriptor', record_close)
    response = client.get(f"/api/device/firmware/{m['sha256']}.bin", headers=headers)
    assert response.status_code == 200
    assert len(closes) == 1


def test_response_closes_descriptor_if_header_send_fails(release, monkeypatch):
    d, m = release
    raw_descriptor = os.open(d / (m['sha256'] + '.bin'), os.O_RDONLY)
    descriptor = app.FirmwareDescriptor(raw_descriptor)
    closes = []
    original_close = app.close_firmware_descriptor

    def record_close(value):
        closes.append(value)
        original_close(value)

    monkeypatch.setattr(app, 'close_firmware_descriptor', record_close)
    response = app.FirmwareStreamingResponse(
        descriptor,
        app.firmware_chunks(descriptor, 0, m['size']),
        headers={'Content-Length': str(m['size'])},
    )

    async def fail_before_body(message):
        assert message['type'] == 'http.response.start'
        raise OSError('disconnected')

    async def receive():
        return {'type': 'http.disconnect'}

    async def run_response():
        with pytest.raises(ClientDisconnect):
            await response(
                {'type': 'http', 'method': 'GET', 'path': '/', 'asgi': {'spec_version': '2.4'}},
                receive,
                fail_before_body,
            )

    asyncio.run(run_response())
    assert closes == [raw_descriptor]


def test_manifest_validation_closes_descriptor_once(release, monkeypatch):
    closes = []
    original_close = app.close_firmware_descriptor

    def record_close(descriptor):
        closes.append(descriptor)
        original_close(descriptor)

    monkeypatch.setattr(app, 'close_firmware_descriptor', record_close)
    assert client.get('/api/device/update', headers=headers).status_code == 200
    assert len(closes) == 1


def test_invalid_range_closes_validated_descriptor_once(release, monkeypatch):
    _, m = release
    closes = []
    original_close = app.close_firmware_descriptor

    def record_close(descriptor):
        closes.append(descriptor)
        original_close(descriptor)

    monkeypatch.setattr(app, 'close_firmware_descriptor', record_close)
    response = client.get(
        f"/api/device/firmware/{m['sha256']}.bin",
        headers={**headers, 'Range': 'bytes=-1'},
    )
    assert response.status_code == 416
    assert len(closes) == 1


def test_inactive_digest_closes_validated_descriptor_once(release, monkeypatch):
    closes = []
    original_close = app.close_firmware_descriptor

    def record_close(descriptor):
        closes.append(descriptor)
        original_close(descriptor)

    monkeypatch.setattr(app, 'close_firmware_descriptor', record_close)
    response = client.get('/api/device/firmware/' + ('0' * 64) + '.bin', headers=headers)
    assert response.status_code == 404
    assert len(closes) == 1


def test_validation_error_closes_descriptor_once(release, monkeypatch):
    d, m = release
    (d / (m['sha256'] + '.bin')).write_bytes(b'x' * m['size'])
    closes = []
    original_close = app.close_firmware_descriptor

    def record_close(descriptor):
        closes.append(descriptor)
        original_close(descriptor)

    monkeypatch.setattr(app, 'close_firmware_descriptor', record_close)
    assert client.get('/api/device/update', headers=headers).status_code == 503
    assert len(closes) == 1

def test_corruption_prevents_offer_and_download(release):
    d, m = release
    (d / (m['sha256'] + '.bin')).write_bytes(b'bad')
    assert client.get('/api/device/update', headers=headers).status_code == 503
    assert client.get('/api/device/firmware/'+m['sha256']+'.bin', headers=headers).status_code == 503


def test_symlink_firmware_is_never_offered_or_downloaded(release, tmp_path):
    d, m = release
    binary = d / (m['sha256'] + '.bin')
    target = tmp_path / 'outside.bin'
    target.write_bytes(binary.read_bytes())
    binary.unlink()
    binary.symlink_to(target)
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


def test_local_proxy_qr_is_restricted_to_lan_addresses():
    assert client.get('/api/qr.svg', params={'url': 'http://192.168.18.160:8080'}).status_code == 200
    assert client.get('/api/qr.svg', params={'url': 'http://192.168.18.160'}).status_code == 200
    assert client.get('/api/qr.svg', params={'url': 'http://192.168.18.160:80'}).status_code == 200
    for url in ['http://127.0.0.1:8080', 'http://0.0.0.0:8080', 'http://8.8.8.8:8080', 'http://192.168.1.1:8082', 'https://192.168.1.1:8080', 'http://user@192.168.1.1:8080']:
        assert client.get('/api/qr.svg', params={'url': url}).status_code == 422
