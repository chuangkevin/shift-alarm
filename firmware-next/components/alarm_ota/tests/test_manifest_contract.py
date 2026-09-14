"""Golden protocol contract: compile the actual C canonicalizer, verify with stdlib HMAC.
This is NOT an emulation of flash hardware or an execution test of mbedTLS.
"""
import ctypes
import hashlib
import hmac
import json
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
vector = json.loads((root / 'tests/manifest-vector.json').read_text())
class Manifest(ctypes.Structure):
    _fields_ = [('board', ctypes.c_char * 32), ('version', ctypes.c_char * 32),
                ('size', ctypes.c_uint32), ('sha256', ctypes.c_char * 65),
                 ('hmac_sha256', ctypes.c_char * 65)]
class Staged(ctypes.Structure):
    _fields_ = [('schema', ctypes.c_uint8), ('manifest', Manifest),
                ('target_subtype', ctypes.c_uint8), ('target_address', ctypes.c_uint32),
                ('record_hmac_sha256', ctypes.c_char * 65)]
with tempfile.TemporaryDirectory() as work:
    library = Path(work) / 'policy.so'
    subprocess.run(['cc', '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), str(root / 'alarm_ota_policy.c'),
                    '-o', str(library)], check=True)
    lib = ctypes.CDLL(str(library))
    fn = lib.alarm_ota_manifest_canonical
    fn.argtypes = [ctypes.POINTER(Manifest), ctypes.c_void_p, ctypes.c_size_t]
    fn.restype = ctypes.c_size_t
    m = Manifest(**{k: v.encode() if isinstance(v, str) else v for k, v in vector['manifest'].items()})
    out = ctypes.create_string_buffer(320)
    n = fn(ctypes.byref(m), out, len(out))
    assert out.raw[:n] == vector['canonical'].encode()
    signed = hmac.new(vector['test_key'].encode(), out.raw[:n], hashlib.sha256).hexdigest()
    assert signed == vector['manifest']['hmac_sha256']
    for field, value in [('board', b'other-board'), ('version', b'1.2.4'), ('size', 1025), ('sha256', b'0'*64)]:
        old = getattr(m, field)
        setattr(m, field, value)
        n = fn(ctypes.byref(m), out, len(out))
        assert n and hmac.new(vector['test_key'].encode(), out.raw[:n], hashlib.sha256).hexdigest() != signed
        setattr(m, field, old)
    staged_fn = lib.alarm_ota_staged_canonical
    staged_fn.argtypes = [ctypes.POINTER(Staged), ctypes.c_void_p, ctypes.c_size_t]
    staged_fn.restype = ctypes.c_size_t
    staged = Staged(schema=1, manifest=m, target_subtype=17, target_address=0x410000)
    n = staged_fn(ctypes.byref(staged), out, len(out))
    assert n
    staged_mac = hmac.new(vector['test_key'].encode(), out.raw[:n], hashlib.sha256).hexdigest()
    for field, value in [('schema', 2), ('target_subtype', 18), ('target_address', 0x10000)]:
        old = getattr(staged, field)
        setattr(staged, field, value)
        changed = staged_fn(ctypes.byref(staged), out, len(out))
        assert changed == 0 or hmac.new(vector['test_key'].encode(), out.raw[:changed], hashlib.sha256).hexdigest() != staged_mac
        setattr(staged, field, old)
print('alarm_ota C canonical / Python HMAC golden contract passed')
