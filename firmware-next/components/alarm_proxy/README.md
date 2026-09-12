# Alarm LAN proxy 0.1.0

IDF 5.3 component. Initialize once with a fixed IPv4 backend/port, then start only
after station Wi-Fi is available and AP provisioning has ended. Main serializes
init/start/stop. `alarm_proxy_stop()` cancels listener and current sessions; they
drain asynchronously within the socket timeout (normally one second). Restart
returns `ESP_ERR_INVALID_STATE` until draining has completed; retry from main.

Listens on **the current station IPv4 only, port 8080**, never `INADDR_ANY`.
Station address changes trigger rebinding. Accepts same-subnet IPv4 peers and
explicitly excludes Tailscale's 100.64/10 source range. No HTTP listener exists
on the Tailscale or AP interface. Main must stop this component while AP setup is
active. Status exposes only counters and the LAN address, no keys.

The browser visits `http://<device-LAN-IP>:8080/`. Host must match that exact
authority, preventing DNS-rebinding hostnames. Backend Host is replaced with the
fixed configured address. An Origin equal to that exact browser origin is
rewritten to the backend origin; all other Origins remain unchanged. The proxy
never adds X-Alarm-UI or credentials. Backend CSRF/device-token enforcement still
applies. Forwarded proxy metadata is removed.

At most two sessions; 120-second total session deadline. Requests have at most
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
