import asyncio
import json
import os
import tempfile
os.environ['ALARM_DATA'] = tempfile.mkdtemp()
os.environ['DEVICE_TOKEN'] = 'test-device-token'
from fastapi.testclient import TestClient
import app
import pytest
from datetime import datetime
client = TestClient(app.app)
UI = {'X-Alarm-UI': '1'}
DEV = {'Authorization': 'Bearer test-device-token'}

def test_recognition_token_budget_has_safe_minimum():
    assert app.recognition_token_budget('50') == 400
    assert app.recognition_token_budget('400') == 400
    assert app.recognition_token_budget('800') == 800
    assert app.recognition_token_budget('invalid') == 6000


def test_recognition_models_are_ordered_and_deduplicated():
    assert app.recognition_models('gemini-3.8-flash', 'gemini-flash,gemini-3.8-flash') == (
        'gemini-3.8-flash', 'gemini-flash', 'go/deepseek-v4-flash-vision-exp'
    )


def test_recognition_switches_model_after_transient_failure(monkeypatch):
    calls = []
    days = [{'date': f'2026-09-{day:02}', 'source_label': '休假', 'classification': 'off'}
            for day in range(1, 31)]

    class Reply:
        def __init__(self, status):
            self.status_code = status

        def json(self):
            return {'choices': [{'message': {'content': json.dumps({'month': '2026-09', 'days': days})}}]}

    class Client:
        async def __aenter__(self):
            return self

        async def __aexit__(self, *args):
            pass

        async def post(self, _url, headers, json):
            calls.append(json['model'])
            return Reply(503 if len(calls) == 1 else 200)

    async def no_wait(_seconds):
        pass

    monkeypatch.setattr(app, 'NEWAPI_MODELS', ('primary-vision', 'backup-vision'))
    monkeypatch.setattr(app.httpx, 'AsyncClient', lambda **_kwargs: Client())
    monkeypatch.setattr(app.asyncio, 'sleep', no_wait)
    result = asyncio.run(app.recognize(b'image', '2026-09'))
    assert result.month == '2026-09'
    assert calls == ['primary-vision', 'backup-vision']


def test_recognition_switches_model_after_transport_timeout(monkeypatch):
    calls = []
    days = [{'date': f'2026-09-{day:02}', 'source_label': '休假', 'classification': 'off'}
            for day in range(1, 31)]

    class Reply:
        status_code = 200
        def json(self):
            return {'choices': [{'message': {'content': json.dumps({'month': '2026-09', 'days': days})}}]}

    class Client:
        async def __aenter__(self): return self
        async def __aexit__(self, *args): pass
        async def post(self, _url, headers, json):
            calls.append(json['model'])
            if len(calls) == 1:
                raise app.httpx.ReadTimeout('slow route')
            return Reply()

    async def no_wait(_seconds): pass
    monkeypatch.setattr(app, 'NEWAPI_MODELS', ('primary-vision', 'backup-vision'))
    monkeypatch.setattr(app.httpx, 'AsyncClient', lambda **_kwargs: Client())
    monkeypatch.setattr(app.asyncio, 'sleep', no_wait)
    result = asyncio.run(app.recognize(b'image', '2026-09'))
    assert result.month == '2026-09'
    assert calls == ['primary-vision', 'backup-vision']

def month(m='2026-09'):
    return {'month': m, 'days': [{'date': d, 'source_label': '上班' if d.endswith('-13') else '休假', 'classification': 'work' if d.endswith('-13') else 'off'} for d in sorted(app.month_dates(m))]}

@pytest.fixture(autouse=True)
def clean_db():
    with app.database() as c:
        c.execute('DELETE FROM months')
        c.execute('DELETE FROM drafts')
        c.execute('DELETE FROM kv')
        app.put(c, 'settings', {'enabled': False, 'times': []})
        app.put(c, 'revision', 'initial')

def test_reject_duplicate_or_partial_month_without_destroying_existing():
    good = month()
    assert client.post('/api/months', json=good, headers=UI).status_code == 200
    bad = month()
    bad['days'][0] = bad['days'][1]
    assert client.post('/api/months', json=bad, headers=UI).status_code == 422
    assert client.get('/api/state').json()['months'][0]['days'] == good['days']

def test_epoch_taipei_and_skip_review_off():
    m = month()
    m['days'][13]['classification'] = 'review'
    client.post('/api/months', json=m, headers=UI)
    client.put('/api/settings', json={'enabled': True, 'times': ['06:30', '06:40']}, headers=UI)
    now = int(datetime(2026, 9, 12, tzinfo=app.TZ).timestamp())
    with app.database() as c:
        a = app.schedule(c, now)
    assert len(a) == 2
    assert a[0]['epoch'] == int(datetime(2026, 9, 13, 6, 30, tzinfo=app.TZ).timestamp())

def test_settings_requires_times_and_rejects_invalid():
    for times in ([], ['24:00'], ['06:00', '06:00']):
        assert client.put('/api/settings', json={'enabled': True, 'times': times}, headers=UI).status_code == 422
    assert client.get('/api/state').json()['settings']['enabled'] is False

def test_csrf_and_device_auth():
    assert client.put('/api/settings', json={'enabled': False, 'times': []}).status_code == 403
    assert client.put('/api/settings', json={'enabled': False, 'times': []}, headers={**UI, 'Origin': 'https://evil.example'}).status_code == 403
    assert client.get('/api/device/schedule').status_code == 401
    assert client.get('/api/device/schedule', headers=DEV).status_code == 200
    assert client.get('/api/state', headers={'Host': 'attacker.example'}).status_code == 400

def test_revision_heartbeat_and_single_test_alarm():
    before = client.get('/api/state').json()['revision']
    client.post('/api/months', json=month(), headers=UI)
    assert client.get('/api/state').json()['revision'] != before
    client.post('/api/device/heartbeat', json={'revision': 'abc', 'status': 'ready'}, headers=DEV)
    assert client.get('/api/state').json()['device']['revision'] == 'abc'
    client.post('/api/test-alarm', headers=UI)
    client.post('/api/test-alarm', headers=UI)
    assert len(client.get('/api/device/schedule', headers=DEV).json()['alarms']) == 1

def test_heartbeat_battery_contract_and_old_device_compatibility(monkeypatch):
    monkeypatch.setattr(app.time, 'time', lambda: 2_000_000_000)
    old = {'revision': 'old', 'status': 'ready'}
    assert client.post('/api/device/heartbeat', json=old, headers=DEV).status_code == 200
    assert client.get('/api/state').json()['device']['battery'] is None
    for percent in (0, 100):
        battery = {'schema': 1, 'valid': True, 'percent': percent, 'charging': False,
                   'sample_age_seconds': 300}
        assert client.post('/api/device/heartbeat', json={**old, 'battery': battery}, headers=DEV).status_code == 200
        state = client.get('/api/state').json()
        assert state['device']['battery'] == battery
        assert state['last_valid_battery'] == {**battery, 'received_at': 2_000_000_000,
                                               'observed_at': 1_999_999_700}

def test_unknown_battery_preserves_last_valid_snapshot(monkeypatch):
    now = [2_000_000_000]
    monkeypatch.setattr(app.time, 'time', lambda: now[0])
    valid = {'schema': 1, 'valid': True, 'percent': 57, 'charging': True,
             'sample_age_seconds': 5}
    client.post('/api/device/heartbeat', json={'revision': 'a', 'battery': valid}, headers=DEV)
    now[0] += 30
    unknown = {'schema': 1, 'valid': False, 'percent': None, 'charging': None,
               'sample_age_seconds': None}
    assert client.post('/api/device/heartbeat', json={'revision': 'b', 'battery': unknown}, headers=DEV).status_code == 200
    state = client.get('/api/state').json()
    assert state['device']['battery'] == unknown
    assert state['last_valid_battery']['percent'] == 57
    assert state['last_valid_battery']['received_at'] == 2_000_000_000
    assert state['last_valid_battery']['observed_at'] == 1_999_999_995

@pytest.mark.parametrize('battery', [
    {'schema': 1, 'valid': True, 'percent': True, 'charging': False, 'sample_age_seconds': 0},
    {'schema': 1, 'valid': True, 'percent': 50, 'charging': 1, 'sample_age_seconds': 0},
    {'schema': 1, 'valid': True, 'percent': 50, 'charging': False, 'sample_age_seconds': True},
    {'schema': 1, 'valid': True, 'percent': -1, 'charging': False, 'sample_age_seconds': 0},
    {'schema': 1, 'valid': True, 'percent': 101, 'charging': False, 'sample_age_seconds': 0},
    {'schema': 1, 'valid': True, 'percent': 50, 'charging': False, 'sample_age_seconds': 301},
    {'schema': 1, 'valid': True, 'percent': None, 'charging': False, 'sample_age_seconds': 0},
    {'schema': 1, 'valid': False, 'percent': 50, 'charging': None, 'sample_age_seconds': None},
    {'schema': 1, 'valid': False, 'percent': None, 'charging': None, 'sample_age_seconds': 0},
    {'schema': 2, 'valid': False, 'percent': None, 'charging': None, 'sample_age_seconds': None},
    {'schema': True, 'valid': False, 'percent': None, 'charging': None, 'sample_age_seconds': None},
    {'schema': 1, 'valid': False, 'percent': None, 'charging': None, 'sample_age_seconds': None, 'extra': 1},
])
def test_heartbeat_rejects_invalid_battery_contract(battery):
    response = client.post('/api/device/heartbeat', json={'revision': 'a', 'battery': battery}, headers=DEV)
    assert response.status_code == 422

@pytest.mark.parametrize('payload', [
    {'revision': 'old', 'status': 'ready', 'unknown': 1},
    {'revision': 'old', 'status': 'online'},
    {'revision': 'old', 'status': 'ready', 'next_alarm': True},
    {'revision': 7, 'status': 'ready'},
    {'revision': 'old', 'status': 'ready', 'ip': 1234},
])
def test_heartbeat_rejects_invalid_outer_envelope(payload):
    assert client.post('/api/device/heartbeat', json=payload, headers=DEV).status_code == 422

def test_offline_page_is_read_only_safe_and_supports_head(monkeypatch):
    monkeypatch.setattr(app.time, 'time', lambda: 2_000_000_100)
    battery = {'schema': 1, 'valid': True, 'percent': 20, 'charging': True,
               'sample_age_seconds': 10}
    client.post('/api/device/heartbeat', json={'revision': 'a', 'battery': battery}, headers=DEV)
    response = client.get('/device-offline')
    assert response.status_code == 200
    assert '裝置目前離線' in response.text
    assert '20%' in response.text and '充電中' in response.text
    assert app.VERSION in response.text and '非即時資料' in response.text
    assert '2,000,000,100' not in response.text
    for forbidden in ('nonce', 'token', '100.126.', 'Traceback', '<form', '<button'):
        assert forbidden not in response.text
    head = client.head('/device-offline')
    assert head.status_code == 200 and head.content == b''

def test_offline_page_csp_allows_only_its_hashed_style():
    import base64
    import hashlib
    expected = (
        "default-src 'none'; "
        f"style-src 'sha256-{app.OFFLINE_STYLE_SHA256}'; "
        "script-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'"
    )
    response = client.get('/device-offline')
    assert response.headers['content-security-policy'] == expected
    style = response.text.split('<style>', 1)[1].split('</style>', 1)[0]
    actual_hash = base64.b64encode(hashlib.sha256(style.encode()).digest()).decode()
    assert response.text.count('<style>') == 1
    assert actual_hash == app.OFFLINE_STYLE_SHA256
    assert '<script' not in response.text
    assert "'unsafe-inline'" not in expected
    assert "script-src 'self'" not in expected
    assert client.head('/device-offline').headers['content-security-policy'] == expected
    assert client.get('/api/health').headers['content-security-policy'] == (
        "default-src 'self'; img-src 'self' blob: data:; style-src 'self'; "
        "script-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'"
    )

def test_offline_readiness_marker_is_path_and_method_specific():
    assert client.get('/device-offline').headers['x-shift-alarm-offline'] == 'true'
    assert client.head('/device-offline').headers['x-shift-alarm-offline'] == 'true'
    assert 'x-shift-alarm-offline' not in client.get('/api/health').headers

def test_backend_root_redirects_to_device_calendar():
    client.post('/api/device/heartbeat', json={'revision': 'abc', 'status': 'ready', 'ip': '192.168.18.160'}, headers=DEV)
    response = client.get('/', follow_redirects=False)
    assert response.status_code == 307
    assert response.headers['location'] == 'http://192.168.18.160/calendar'

def test_month_boundaries_leap_year():
    assert len(app.month_dates('2028-02')) == 29
    assert len(app.month_dates('2026-02')) == 28
    with pytest.raises(ValueError): app.month_dates('2026-13')

def test_invalid_upload_keeps_schedule():
    app.NEWAPI_KEY = 'test-not-a-real-key'
    client.post('/api/months', json=month(), headers=UI)
    r = client.post('/api/import', data={'month': '2026-09'}, files={'file': ('x.png', b'not-an-image', 'image/png')}, headers=UI)
    assert r.status_code == 422
    assert len(client.get('/api/state').json()['months']) == 1

def test_qr_and_no_secret_exposure():
    r = client.get('/api/qr.svg')
    assert r.status_code == 200
    assert '<svg' in r.text
    assert 'test-device-token' not in client.get('/api/state').text

def test_recognition_deadline_cancels_and_preserves_schedule(monkeypatch):
    import asyncio
    import io
    from PIL import Image
    cancelled = []
    async def stalled(*args):
        try:
            await asyncio.sleep(10)
        finally:
            cancelled.append(True)
    monkeypatch.setattr(app, 'RECOGNITION_SECONDS', 0.01)
    monkeypatch.setattr(app, 'NEWAPI_KEY', 'test-key')
    monkeypatch.setattr(app, 'recognize', stalled)
    client.post('/api/months', json=month(), headers=UI)
    b = io.BytesIO()
    Image.new('RGB', (2, 2)).save(b, 'PNG')
    r = client.post('/api/import', data={'month': '2026-09'}, files={'file': ('x.png', b.getvalue(), 'image/png')}, headers=UI)
    assert r.status_code == 504
    assert cancelled == [True]
    assert not app.import_lock.locked()
    assert len(client.get('/api/state').json()['months']) == 1
    with app.database() as c:
        assert c.execute('SELECT count(*) FROM drafts').fetchone()[0] == 0


def test_recognition_seconds_stays_bounded():
    assert app.recognition_seconds('180') == 180
    assert app.recognition_seconds('10') == 30
    assert app.recognition_seconds('9999') == 200
    assert app.recognition_seconds('not-a-number') == 180


def test_recognition_reads_reasoning_when_content_is_null(monkeypatch):
    days = [{'date': f'2026-09-{day:02}', 'source_label': '休假', 'classification': 'off'}
            for day in range(1, 31)]

    class Reply:
        status_code = 200
        def json(self):
            return {'choices': [{'message': {'content': None,
                                             'reasoning': json.dumps({'month': '2026-09', 'days': days})}}]}

    class Client:
        async def __aenter__(self): return self
        async def __aexit__(self, *args): pass
        async def post(self, _url, headers, json): return Reply()

    monkeypatch.setattr(app.httpx, 'AsyncClient', lambda **_kwargs: Client())
    result = asyncio.run(app.recognize(b'image', '2026-09'))
    assert result.month == '2026-09' and len(result.days) == 30
