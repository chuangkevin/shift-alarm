#!/usr/bin/env python3
import re
import subprocess
import tempfile
from pathlib import Path

sources = [
    Path('firmware-next/main/calendar_page.h'),
    Path('firmware-next/main/wifi_page.h'),
]
checked = 0
with tempfile.TemporaryDirectory() as directory:
    for source in sources:
        text = source.read_text()
        for index, script in enumerate(re.findall(r'<script>(.*?)</script>', text, re.S), 1):
            script = script.replace('WIFI_NONCE', '0' * 32)
            path = Path(directory) / f'{source.stem}-{index}.js'
            path.write_text(script)
            subprocess.run(['node', '--check', str(path)], check=True)
            checked += 1
assert checked >= 2
print(f'Embedded JavaScript syntax checks passed: {checked}')
