# DERP host regressions

Run `sh run_host_tests.sh` from this directory. Requires a host C compiler and
Python 3; no hardware, network connection or ESP-IDF toolchain is used.

- `derp_stream_test.c` uses the actual stream helper with a fake TLS reader to
  test split headers, retry, idle, EOF and deadline handling.
- `derp_cache_test.c` includes the actual cache policy header and checks bounded
  capacity, idle replacement, region reuse, background demand and retry delay.
- `test_derp_cleanup.py` extracts the exact production cleanup/session functions
  into a host fixture. Counting TLS and socket stubs verify three independent
  contexts, repeated failures, idempotency and preservation of the home event.
- `test_derp_routes.py` extracts actual destination/live-session helpers and the
  owner-loop home-transition block. Fixtures cover different home/peer regions,
  return hints, expiry/closed sessions, unknown keys, and a changed home removing
  the duplicate remote cache entry and resetting backoff.

All fixtures compile with UndefinedBehaviorSanitizer. They validate production
control flow against controlled dependencies; they do not validate real TLS
servers, cryptography, socket scheduling, physical radios or end-to-end Tailnet
connectivity. TLS/token secret handling is not proven by counting stubs.
