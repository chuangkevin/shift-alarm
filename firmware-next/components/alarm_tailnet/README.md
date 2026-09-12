# Alarm native Tailnet component

Vendored `microlink` and its `wireguard_lwip` dependency come from
`https://github.com/CamM2325/microlink` commit
`216da3300f0493b0860247d43f7af5ce29df63a5`. Their licenses are retained.
This wrapper targets ESP-IDF 5.3.2 / ESP32-S3 with PSRAM. It starts no web
server and includes none of the upstream configuration UI. LAN/AP settings
and HTTP authorization remain owned by the main application.

## Application contract

Initialize NVS and networking before requesting `alarm_tailnet_start(name)`.
All lifecycle APIs enqueue requests without waiting for network or shutdown:
`ESP_OK` means accepted, not connected. A single worker owns the MicroLink
instance, waits for SNTP before registration, performs reauth/stop, and
publishes cached status. `alarm_tailnet_get_status()` copies that snapshot
under a short critical section; it never dereferences the live client.
An initial allocation/start failure is retryable through `reauth()` even if
no client exists. A shutdown timeout retains the live context for retry.
The worker/command queue intentionally remain available after `stop()`.

Status includes state, AuthURL, VPN IP, expiry, data-plane readiness,
`last_error`, observed peer count, peer capacity, and `capacity_exceeded`.
AuthURL is for the authorized local admin UI only and must not be logged.
An expired or machine-unauthorized node cannot forward data.

No auth key is embedded. Initial registration obtains AuthURL, then submits
Followup requests. Interactive reauth rotates the node key, sends OldNodeKey,
and preserves machine/DISCO identity. NVS failures fail initialization;
there is no ephemeral identity fallback. Peer maps are session-scoped and
are neither restored from nor written to NVS. Device NVS/flash encryption
remains a provisioning concern; this component does not modify eFuses.

## Build requirements and capacity

Enable these in sdkconfig:

```ini
CONFIG_SPIRAM=y
CONFIG_ML_MAX_PEERS=64
CONFIG_LWIP_TCPIP_CORE_LOCKING=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
CONFIG_MBEDTLS_CHACHA20_C=y
CONFIG_MBEDTLS_POLY1305_C=y
CONFIG_MBEDTLS_CHACHAPOLY_C=y
```

`CONFIG_ML_MAX_PEERS` is the single limit for the wrapper, MicroLink owner,
WireGuard peer array, control-map reconciliation and security snapshot.
The observed 57-peer tailnet fits the default 64; 65 peers are rejected
explicitly with `capacity_exceeded`, observed/capacity counts, and
`ESP_ERR_INVALID_SIZE`, rather than silently omitting a backend peer.
WireGuard's large device/peer allocation requires PSRAM. MicroLink allocations
of 4 KiB or greater also fail rather than consume internal RAM on PSRAM
failure. Its four task stacks plus the wrapper worker total 50 KiB;
TLS/lwIP/RTOS allocations still need internal-RAM headroom. Heap/runtime
acceptance on the actual display application remains required.

DERP requires certificate-chain and hostname verification with ESP-IDF's
certificate bundle. SNTP is necessary for TLS and expiry validation. Noise
control traffic authenticates against the upstream pinned Tailscale server
public key; custom control servers and Tailnet Lock are unsupported. A
self-node restricted to unsigned peer API access is blocked.

## Peer reconciliation and packet filtering

Control peers are keyed by numeric NodeID. Full `Peers`, `PeersChanged`,
numeric `PeersRemoved`, and array `PeersChangedPatch` are reconciled into an
authoritative map. Key, DiscoKey, endpoints, DERP and expiry changes are
applied; unknown NodeIDs are ignored as specified by tailcfg. Endpoint-only
updates preserve established WireGuard sessions. Key/address changes remove
the old crypto peer first. Full maps replace previous membership. Expired,
unauthorized and unsigned-only peers are excluded from data-plane membership.

Data is suspended during peer updates. A generation-tagged owner-queue
acknowledgement re-enables traffic only after the matching peer mutations
have completed; failures or reconnection invalidate older acknowledgements.
Peer installation failures remain blocked. Each packet also checks current
peer membership/expiry, so stale encrypted sessions cannot bypass revocation.

WireGuard plaintext IPv4 TCP/UDP ingress is default-deny. `PacketFilter` and
named `PacketFilters` deltas are applied from initial/streaming maps; a new
control session starts without the old policy. Supported rules match IPv4
addresses/CIDRs or `*`, TCP/UDP IPProto, and destination port ranges. IPv6,
fragments, deprecated SrcBits/Bits, IP ranges, application capability grants
and unrecognized rule fields never open a networking port. Empty/removal
policies admit no unsolicited ingress. Node objects decode omitted fields
to Go zero values: absent expiry means no expiry, absent Expired means false,
and absent MachineAuthorized means false. Absent/null Node means unchanged.

Outbound IPv4 TCP/UDP is allowed only after registration, policy and peer
synchronization. The remote peer enforces its ingress ACL. A 16-entry,
120-second exact reverse-tuple table allows replies (including backend
TCP/8237); policy changes clear it. Device TCP/80 needs an inbound ACL allow.
The first outbound packet triggers a WG handshake; TCP retains its retry
buffer while that handshake completes. The virtual route remains available
before a session exists. LAN/AP packets never pass through this filter.
MagicDNS remains a peer-list lookup API rather than a system DNS resolver;
use the backend's Tailnet IPv4 address.

WireGuard entry points and direct crypto-peer accesses use the ESP-IDF
TCPIP core lock, with nested/current-owner detection. UDP output is raw lwIP
under that lock, and DERP enqueue is nonblocking. Netif initialization and
removal use TCPIP callbacks. Shutdown waits for task exit acknowledgements
and a FIFO netif-cleanup barrier before freeing client/key memory.

## Verification

Run the reusable host suite with ESP-IDF's exact cJSON sources:

```sh
IDF_PATH=/path/to/esp-idf ./tests/run_host_tests.sh
```

Alternatively set `CJSON_DIR`, or on Linux install `libcjson-dev` and
`pkg-config`. A C11 compiler and pthreads are required. The default sanitizer
is UndefinedBehaviorSanitizer (`SANITIZERS` can choose other sanitizers).
The tests cover auth/Followup, ACL/deltas/expiry and Node zero values, peer
NodeID/key/endpoint/expiry changes and revocation, 57/64/65 capacity, four
concurrent core-lock callers, and nonblocking lifecycle/status with a slow
mock shutdown and retry after initial allocation failure. The DERP transport
has separate regression coverage maintained alongside its parser.

An independent ESP-IDF 5.3.2 smoke application compiles and links the full
reachable lifecycle. AddressSanitizer could not initialize on the development
Mac, so no ASan success is claimed. None of these tests enroll a real node or
exercise hardware. These host checks do not access real Tailnet secrets.
The 0.2.2 device has completed real enrollment and reports 58/64 peers, but
both backend TCP/8237 and incoming TCP/80 timed out. Control-plane
`connected` / `acl_ready` is not proof of working WireGuard or TCP traffic.
Hardware acceptance of this repair remains pending; this debugging unit
must not flash, reboot or open the device's serial port.

### Data-path repair and region routing

Map endpoints are now DISCO candidates rather than active WG destinations.
The first native BSD-socket connection starts with DERP. A direct handshake
also queues the **same** initiation over DERP, preserving its response index
while avoiding an unreachable UDP endpoint stranding the connection.
Authenticated relayed input selects DERP for subsequent encrypted replies;
the previous code restored a stale UDP endpoint after its DERP handshake
response, which blackholed a relayed TCP SYN's SYN-ACK.

The 0.2.2 client also opened only its own home DERP connection (configured
region 9 / Dallas). Read-only Tailscale status on the backend showed its home
relay as Hong Kong and the device's as Dallas.
[Upstream DERP protocol documentation](https://github.com/tailscale/tailscale/blob/main/derp/README.md)
specifies no routing between regions. The repaired owner task keeps home
pinned and opens at most **two** additional destination-region TLS sessions.
The destination region comes from the current authorized peer snapshot;
unknown regions fail explicitly rather than falling back to Dallas.

Only WG traffic opens a new remote session. Background DISCO can reuse an
existing connection but cannot allocate one per peer. Recent incoming
traffic may reuse its live receiving session for replies (a 30-second
transport hint, never an authentication or ACL grant). Each TLS context has
exactly one owner for read, write, reconnect and free. Remote cleanup does
not clear the home-connected event. Shutdown interrupts all three sockets
and the owner releases every TLS context before its exit acknowledgement.

The two remote slots are reused per region; a third active region increments
`derp_capacity_drops`. A slot can be replaced after 60 seconds without WG
transmit demand, using least-recently-used order. Connection failures back off
1, 2, 4, 8, 16, then 30 seconds. Pending packets may be dropped during connect
or backoff; WG/TCP retransmissions drive retry. Cold cross-region connections
can therefore outlast an application's first short HTTP timeout.

Budget three DERP TCP sockets out of the application's 20-socket limit.
Use `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` with the verified 8 MB PSRAM board;
three TLS contexts plus control TLS otherwise consume substantial internal
RAM (at least 20 KB of record buffers per TLS connection in this build).
No additional task stacks or identities are created per region.

The cached status exposes `derp_home_connected`, `derp_home_region`,
`derp_remote_connected`, `derp_frames_tx`, `derp_frames_rx`,
`derp_connect_failures`, `derp_capacity_drops`, `derp_queue_drops`, and
`derp_route_drops`. These are transport diagnostics, not evidence of HTTP
success. Actual bidirectional HTTP, certificate rejection, reconnect,
manual reauth, expiry, revocation and memory headroom with the display active
still require hardware acceptance after integration.

### Native routing follow-up

After 0.2.3, real DERP receive counters advanced while HTTP still timed out
and DERP transmit remained zero. The native interface now uses `netif_add`
instead of splicing `netif_list`: the latter left interface number zero and
aliased lwIP's loopback index. Later control-plane self-address changes also
use `netif_set_ipaddr` on the core-locked owner path rather than leaving the
interface at its initial address.

The actual SDK host routing regression proves index aliasing, but also proves
that ordinary unbound IPv4 routing still selected the old manual interface.
Therefore this integrity fix alone is **not** an established explanation for
the device's HTTP failure. The magicsock initialization does not install the
normal-mode timer; its active periodic path already keeps LINK_UP before the
first handshake.

Additional cached diagnostics separate the next verification stages:
`wg_netif_ip`, `wg_netif_mask`, `wg_netif_index`, `wg_netif_up`,
`wg_netif_link_up`, `wg_peer_count`, `wg_sessions`, `wg_out_packets`,
`wg_out_dropped`, `wg_last_out_src`, `wg_last_out_dst`, `wg_lookup_misses`,
`wg_derp_enqueue`, `wg_derp_enqueue_fail`, `wg_udp_tx`, `wg_rx_packets`,
`wg_in_packets`, and `wg_in_dropped`. IPv4 integers are in host order.
Output/inbound counters run at the plaintext policy boundary; raw RX counts
include WG handshake packets. These contain no keys or payloads. The owner
copies netif/peer state under core→policy lock order; HTTP reads only a cached
snapshot. Real bidirectional HTTP still needs validation after integration.

### ESP-IDF callback compatibility (required)

Use `CONFIG_LWIP_PPP_SUPPORT=y` and rebuild **all** IDF components. In IDF
5.3 this selects `LWIP_ESP_NETIF_DATA=1`, reserving a separate client-data slot
for the SDK's `esp_netif_t` pointer. The custom WG interface leaves that slot
NULL, so global DHCP/IPv6 callbacks ignore it while its `state` remains owned
by WireGuard. PPP support here does not create a PPP instance; cellular remains
disabled. A per-source macro override is unsafe because it changes netif ABI.

The 0.2.4 hardware backtrace showed `netif_add` → `netif_set_addr` → SDK DHCP
callback dereferencing WG state as `esp_netif_t` when client-data separation
was disabled. The native component now rejects that configuration at compile
time. The earlier minimal lwIP routing test did not include ESP-netif's global
callback, so it could not detect this integration failure.

Run `IDF_PATH=/path/to/esp-idf python3 ../microlink/test/test_esp_netif_callback.py`
from this directory. It compiles the actual SDK configuration selection,
getter and complete DHCP callback with controlled event/IP dependencies:
the unsafe configuration trips UBSan, the separated slot safely ignores custom
driver state, and a real ESP-netif fixture still receives its IP-change event.
The compile guard is tested in both configurations. This remains a host
regression; the corrected full firmware requires forward hardware validation.

### Remote peer eligibility

The 0.2.5 runtime reached the WG netif with 57 mapped peers but zero installed
WG peers: all outgoing packets were rejected before DERP enqueue. Real control
peer objects omit `MachineAuthorized`; requiring it to be true excluded every
peer. Remote peers supplied by control now retain eligibility regardless of
that field, matching the official Tailscale WG conversion. Own-node and register
authorization, ACLs, peer expiry, and `UnsignedPeerAPIOnly` restrictions remain
enforced. See [official WGCfg peer conversion](https://github.com/tailscale/tailscale/blob/v1.80.3/wgengine/wgcfg/nmcfg/nmcfg.go).

The host peer-map regression uses omitted-field fixtures, explicit false,
expiry and unsigned-only removal, plus 57/64-peer capacity cases. With only
the new fixtures, the previous implementation fails its initial queued-ADD
assertion; with the fix all four Tailnet host suites pass under UBSan.
This identifies a pre-transport blocker; successful real TCP still requires
validation of the forward firmware candidate.

### Coordinator recovery diagnostics

Status now exposes numeric `coord_stage`, `coord_last_reason`, `coord_reconnects`,
`coord_successes`, monotonic `coord_stage_since_ms` / `coord_last_failure_ms`,
and separate `policy_ready`, `peers_ready`, `node_authorized`, `peer_generation`.
These are cached under the existing security lock; they contain no control
response text, authorization URL, identity or keys. The last failure survives
successful reconnection so transient failures remain observable. Counters reset
when the native client is destroyed. Success counts completion of initial map
fetch, not application TCP success. Stage timestamps describe the current
state-machine iteration and can lag one iteration after a transition.

Stages: 0 idle, 1 STUN, 2 DNS/connect, 3 TCP (reserved by current state machine),
4 Noise, 5 H2 preface, 6 register, 7 map fetch, 8 long poll, 9 reconnect/backoff.
Reasons: 0 none, 1 forced reconnect, 2 DNS/TCP failure, 3 Noise failure,
4 H2 preface failure, 5 browser approval pending, 6 register failure,
7 initial map failure, 8 watchdog, 9 expired key, 10 PING send failure,
11 long-poll failure. Reconnect counts include explicit requests and pending
browser approval, not just network failures.

In the 0.2.6 diagnostic interval, all six decrypted inbound packets and all
24 outgoing packets were dropped, and subsequent status showed connected=false
and acl_ready=false. This identifies a post-connection security closure, not
a proven incoming-only ACL failure. The 150-second boot capture ends before
this transition. Fresh successful full-map recovery has a RESET/ADD/SYNC_DONE
generation path; no permanent readiness restoration defect was demonstrated.
The additional fields make the next runtime observation discriminate control
reconnect, policy compilation, peer installation and own-node authorization.

### Incoming ACL diagnostics and IPv4 ranges

Both source and destination ACL selectors now accept inclusive IPv4 address
ranges, as specified by tailcfg FilterRule (alongside existing wildcard, IP
and CIDR forms). Reversed, malformed and out-of-range selectors deny access.
Host tests cover both endpoints, single-address ranges and rejection cases.
This corrects a protocol-format omission; it does not prove the live device
filter uses ranges. No ACL or authorization requirement is bypassed.

Safe status fields `wg_last_in_src`, `wg_last_in_dst` (host-order IPv4),
`wg_last_in_port`, `wg_last_in_drop` identify the latest rejected incoming
packet. Address/port fields update only after a valid TCP/UDP header, so on
malformed/protocol rejection they may describe an earlier packet. Drop codes:
1 malformed/fragmented IPv4; 2 unsupported protocol; 3 policy/peer/auth/expiry
gate; 4 unknown or expired peer; 5 invalid outgoing source (reserved here);
6 wrong incoming destination; 7 no matching ACL rule. The last drop reason
persists after successful packets.

`acl_rule_count`, `acl_range_count`, `acl_unsupported_count` summarize the
filter at the latest inbound rejection. They contain counts only, never raw
filter text, capabilities, keys or authentication URLs. Unsupported counts
include unknown/deprecated fields and nonempty capability grants; multiple
unsupported fields may belong to one rule. IPv6 selectors are not counted as
unsupported because they correctly do not match this IPv4-only data path.
### Control streaming framing

Long-poll now retains incomplete HTTP/2 headers and payloads between Noise
records, then assembles each stream-5 MapResponse using its official four-byte
little-endian length. Every complete map is processed in order, including
multiple maps per DATA frame. Other streams' DATA never contaminates the map
body or its flow-control accounting. Padding is excluded from map bytes but
included in flow-control bytes. PING/SETTINGS replies remain handled.

Limits are 16 KiB per HTTP/2 frame (the default receive MAX_FRAME_SIZE; no
larger size is advertised) and 256 KiB per map. Buffers allocate through the
existing PSRAM allocator and are released on frame/map completion or reconnect,
disconnect and shutdown. Invalid framing/JSON, allocation failure, GOAWAY,
map-stream END_STREAM or reset trigger a fresh authoritative fetch. A failed
long-poll request no longer falsely transitions to connected. Unknown HTTP/2
frame types remain ignored as required by HTTP/2.

The previous loop discarded frame tails and all but the last DATA payload,
and wrote a terminator beyond a full 65536-byte receive buffer. The new parser
provides separately allocated terminated map bodies. Host tests feed every
chunk size across split headers/prefixes/bodies, multiple maps, other-stream
DATA, padded DATA, invalid lengths and reconnect reset under UBSan. Run the
microlink test/run_host_tests.sh suite. This repairs deterministic protocol
framing defects; 0.2.7 remained connected beyond 270 seconds, so these defects
are not asserted to explain the earlier runtime disconnection.
