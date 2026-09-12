#!/usr/bin/env python3
"""Preserve explicit IPv4 bind addresses in Arduino's dual-stack server."""
import hashlib
from pathlib import Path
import sys
source=Path(sys.argv[1])/'libraries/Network/src/NetworkServer.cpp'
backup=source.with_suffix('.cpp.alarm-original')
original=(backup if backup.exists() else source).read_bytes()
if hashlib.sha256(original).hexdigest()!='64622acea7750b27674e0fde7c4b990e76a8c0f8b3736c5308ef24a149bcdf34':
    raise SystemExit('NetworkServer upstream SHA256 differs; review bind patch before upgrading')
s=original.decode().replace('struct sockaddr_in6 server;', 'struct sockaddr_in6 server{};');start=s.index('  if (_addr.type() == IPv4) {',s.index('void NetworkServer::begin('));end=s.index('  server.sin6_port =',start)
s=s[:start]+'''  memset(server.sin6_addr.s6_addr, 0, 16);
  if (_addr.type() == IPv4) {
    // IPv4-mapped addresses have twelve prefix bytes. Preserve wildcard binds.
    if (uint32_t(_addr) != 0) {
      server.sin6_addr.s6_addr[10] = 0xFF;
      server.sin6_addr.s6_addr[11] = 0xFF;
      memcpy(server.sin6_addr.s6_addr + 12, (uint8_t *)&_addr[0], 4);
    }
  } else {
    memcpy(server.sin6_addr.s6_addr, (uint8_t *)&_addr[0], 16);
  }
'''+s[end:]
if not backup.exists():backup.write_bytes(original)
if source.read_text()!=s:source.write_text(s)
print('Arduino explicit IPv4 bind preserved in dual-stack NetworkServer')
