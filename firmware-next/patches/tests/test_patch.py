from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
component=root.parent/'managed_components/espressif__arduino-esp32'
original=component/'libraries/WebServer/src/Parsing.cpp.alarm-original'
if not original.exists():original=original.with_suffix('')
with tempfile.TemporaryDirectory() as directory:
    target=Path(directory);source=target/'libraries/WebServer/src/Parsing.cpp';source.parent.mkdir(parents=True)
    source.write_bytes(original.read_bytes());(target/'idf_component.yml').write_text('version: 3.1.3\n')
    def run():return subprocess.run([sys.executable,str(root/'apply_arduino_http.py'),str(target)],capture_output=True)
    assert run().returncode==0
    patched=source.read_bytes();assert b'alarm_http_headers_allowed' in patched
    assert run().returncode==0 and source.read_bytes()==patched
    (target/'idf_component.yml').write_text('version: 3.1.4\n');assert run().returncode!=0
    (target/'idf_component.yml').write_text('version: 3.1.3\n')
    source.with_suffix('.cpp.alarm-original').write_text('different upstream');assert run().returncode!=0
print('parser patch idempotency/version/upstream drift checks PASS')
