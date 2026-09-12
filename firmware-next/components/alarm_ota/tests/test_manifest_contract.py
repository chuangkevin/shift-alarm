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
    out = ctypes.create_string_buffer(160)
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
print('alarm_ota C canonical / Python HMAC golden contract passed')
