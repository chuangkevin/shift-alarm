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
    client.post('/api/device/heartbeat', json={'revision': 'abc', 'status': 'online'}, headers=DEV)
    assert client.get('/api/state').json()['device']['revision'] == 'abc'
    client.post('/api/test-alarm', headers=UI)
    client.post('/api/test-alarm', headers=UI)
    assert len(client.get('/api/device/schedule', headers=DEV).json()['alarms']) == 1

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
