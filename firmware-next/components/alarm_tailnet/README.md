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
exercise hardware. No firmware was flashed and no real Tailnet secrets were
used. Hardware acceptance still needs actual AuthURL approval, TCP/8237 and
TCP/80 traffic, certificate rejection, reconnect, manual reauth, expiry,
revocation and memory-headroom checks with the display UI active.
