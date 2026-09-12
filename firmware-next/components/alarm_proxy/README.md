# Alarm HTTP gateway 0.2.3

Listens on port 80 on local network interfaces. Accepts same-subnet station/AP
clients, and native Tailnet clients on the Tailnet destination (WireGuard ACLs
apply before decrypted traffic reaches this listener). Validates Host against
the actual destination IP, optionally with :80. It runs during initial pairing.

Local routes /, /display, /calendar, /tailnet, /update and their APIs go only to
127.0.0.1:8081. The internal server binds loopback and cannot be opened from LAN.
/schedule redirects locally to /calendar. An explicit allowlist forwards AI
APIs and backend assets; recognition errors remain inline in the unified calendar.
Other paths remain local and yield a local 404. Backend connection failures
return a Chinese 503 without discarding calendar edits. No browser link requires :8080.

Main serializes start/stop. Stop cancels sessions asynchronously via generation
and deadline guards. IP-specific same-subnet admission is checked per accept.

The browser visits http://<device-IP>/display. Host must match that IP
authority, preventing DNS-rebinding hostnames. Backend Host is replaced with the
fixed configured address. An Origin equal to that exact browser origin is
rewritten to the backend origin; all other Origins remain unchanged. The proxy
never adds X-Alarm-UI or credentials. Backend CSRF/device-token enforcement still
applies. Forwarded proxy metadata is removed.

At most two sessions; 240-second total session deadline. Requests have at most
16 KiB headers and 12 MiB body. Bodies stream through a 4 KiB buffer, including
multipart uploads; no image is accumulated in RAM. Requests require unambiguous
Content-Length framing; chunked requests, duplicate Content-Length/Host/Origin,
CONNECT, Upgrade, Expect and connection-nominated headers are rejected. Each
session handles exactly one request and forces Connection: close upstream.
Upload forwarding and upstream header reads run together with `select`. A final
upstream response stops upload forwarding immediately, so an early 403/413 is
returned even when the server refuses to consume a large body. Informational
100/103 headers are forwarded without treating them as a final response;
unsolicited 101 upgrades and malformed response headers are rejected.
Responses stream unchanged (including chunked framing) until upstream close,
bounded to 12 MiB plus 16 KiB for framing. Over-limit/timeout streams are closed,
never relabeled as successful complete responses.

Backend connectivity uses normal lwIP TCP sockets. The native tailnet adapter
must install the route/interface used to reach the configured backend address.
There is no arbitrary target URL parameter and no release/upload action here.

Host parser and actual relay verification:

```sh
./test/run_host_tests.sh
```

The relay test compiles the production relay source with small host shims for
ESP time/netif/task APIs and uses real POSIX sockets plus a fake upstream. It
checks an early 403 that never reads the advertised 8 MiB body, fragmented
headers, 103 followed by a final response, a complete 256 KiB upload, and exact
plain/chunked response bytes. It does not emulate lwIP routing or FreeRTOS
listener lifecycle.

On-device verification remains necessary: AP stop/start, station IP changes,
two simultaneous browser connections, a full image upload and invalid Origin
returning the backend's 403. No hardware verification is implied by parser tests.
