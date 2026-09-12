#!/usr/bin/env python3
"""Fail closed on dependency drift; regenerate from a verified pristine source."""
import hashlib
from pathlib import Path
import re
import sys
root=Path(__file__).resolve().parent
component=Path(sys.argv[1])
if not re.search(r'^version: 3\.1\.3$', (component/'idf_component.yml').read_text(), re.M):
    raise SystemExit('Alarm HTTP patch requires Arduino 3.1.3; review the parser before upgrading')
source=component/'libraries/WebServer/src/Parsing.cpp'
backup=source.with_suffix('.cpp.alarm-original')
expected='b420424f380471f4ef3023fb208e03b1897a6c98241949f96debc2fced8c77ab'
original=(backup if backup.exists() else source).read_bytes()
if hashlib.sha256(original).hexdigest()!=expected:
    raise SystemExit('Alarm HTTP patch: pristine Parsing.cpp SHA256 mismatch')
s=original.decode();begin=s.index('static char *readBytesWithTimeout(');end=s.index('bool WebServer::_collectHeader(',begin)
patched=(s[:begin]+(root/'parse_request.inc').read_text()+'\n'+s[end:]).encode()
current=source.read_bytes()
# An earlier generated patch may differ when reviewed patch files change.
if not backup.exists():
    if current!=original:raise SystemExit('Alarm HTTP patch unexpected source changes')
    backup.write_bytes(original)
if current!=patched:source.write_bytes(patched)
header=source.parent/'alarm_http_request.h';data=(root/'alarm_http_request.h').read_bytes()
if not header.exists() or header.read_bytes()!=data:header.write_bytes(data)
print('Alarm bounded HTTP parser applied (Arduino 3.1.3, verified upstream SHA256)')
