# Transport regression (host)

Run `IDF_PATH=/path/to/esp-idf-5.3.2 sh run_host_tests.sh` from this directory.
Only IDF's real lwIP headers are needed; no target toolchain or board access.
The test compiles the complete actual `../src/wireguardif.c` in its translation
unit with UBSan. Platform and crypto stubs provide deterministic authenticated
or rejected packets; output callbacks capture real transport decisions. This
is not a crypto, concurrency, DERP-region or physical networking test.

Coverage:

- An authenticated relayed INIT receives its handshake response over DERP.
- A relayed DATA packet is delivered to the IP input callback, and the following
  outbound reply uses DERP despite an old nonzero UDP endpoint.
- Authenticated direct DATA upgrades the route; normal DATA is not duplicated.
- Rejected INIT/DATA cannot switch an existing direct endpoint to DERP.
- The real netif output entrypoint schedules a first SYN handshake, then the
  actual periodic function emits its relay initiation.
- A direct candidate handshake also sends an identical initiation through DERP.

`WG_TEST_SOURCE=/absolute/path/to/wireguardif.c` runs the same tests against a
baseline source without modifying the worktree. On baseline commit
`c5a047cc910632424715a9ca0f63512ef1d562b4`, the test exits 134 at
`derp_count==2 && derp_type==4 && udp_count==0`: the post-DERP reply incorrectly
uses UDP. The repaired worktree passes all assertions under UBSan.

Unused firmware sections are removed with Darwin dead_strip / ELF gc-sections.
Warnings suppressed are existing unused parameters/variables/functions and
32-bit target printf formats when compiling on a 64-bit host. Arch, allocator,
clock and pbuf doubles live only in this test directory. No production source
is replaced, extracted or mirrored by this suite.
