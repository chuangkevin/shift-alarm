# Native lwIP netif registration regression

Run:

```sh
IDF_PATH=/path/to/esp-idf-5.3.2 sh run_netif_host_tests.sh
```

This compiles the actual SDK `netif.c`, `ip4.c`, `ip4_addr.c`, `ip.c` and `def.c`
under UBSan. The fixture registers a loopback-address interface and WiFi, then
compares the old zero-initialized/manual-list WG registration with `netif_add`.
No routing algorithm is copied into the test.

Verified behavior:

- Manual registration duplicates interface number zero. Looking up the original
  interface by its index incorrectly returns the manually prepended WG interface.
- Contrary to the proposed root-cause hypothesis, ordinary unbound IPv4 routing
  still selects that manually registered WG interface for 100.126.226.79 and
  WiFi for the LAN backend. `ip4_route` uses addresses/masks/flags, not indices.
- `netif_add` assigns a unique index and preserves both expected routes.
- Taking WG's link down causes the Tailnet destination to fall back to WiFi;
  restoring link-up restores WG routing.

The fixture uses a deliberately minimal IPv4-only, single-threaded host lwIP
configuration. It does not run real TCP/socket connections, the target loopback
packet queue, WireGuard, TLS, target core locks or hardware. Its loopback-address
interface demonstrates index identity; it does not claim to emulate full ESP
loopback initialization. The test proves the registration defect and the route
selection negative evidence, not that fixing the index alone restores HTTP.
