# alarm_ota 0.2.0 - ESP-IDF 5.3 / ESP32-S3

This component authenticates a release, streams it exclusively into the inactive OTA app slot, commits a small authenticated staged marker only after complete verification, and activates the staged image in a separate operation. It never erases the running app, NVS partition, partition table, or bootloader.

Hardware GPIO38 behavior, real-flash persistence, offline installation, power interruption, physical UI, and live OTA remain unverified. Host tests and an ESP-IDF build are code evidence only.

## Bootstrap

The running firmware must already have a rollback-enabled bootloader, two OTA app partitions, and 0x2000-byte `otadata`. Application-only OTA cannot retrofit these. Keep task-watchdog panic enabled and eFuse anti-rollback disabled. The boot rollback self-test remains local and must not depend on Internet.

The CMake project name must equal the board ID. `PROJECT_VER` is strict numeric `major.minor.patch`; incoming descriptor board/version must match the signed manifest and be newer than running.

## Signed records

Manifest canonical bytes remain:

```text
board\nversion\nsize\nsha256\n
```

The staged record contains bounded schema, manifest, target subtype/address, and record HMAC. Its canonical bytes are:

```text
schema\nboard\nversion\nsize\nsha256\nmanifest_hmac\ntarget_subtype\ntarget_address\n
```

Both HMACs use HMAC-SHA256 with the literal `DEVICE_TOKEN` bytes. The binary is never stored in NVS. URL fields are never signed or persisted.

## Lifecycle

1. Call `alarm_ota_boot_self_test` on every boot. Pending-image validation remains offline.
2. Call `alarm_ota_init` after Preferences is open, supplying auth, local guard, and marker load/store/clear callbacks.
3. Initialization starts in `ALARM_OTA_MARKER_FAULT`, not idle. Call `alarm_ota_load_staged`. It is strictly observational: it only invokes `marker_load`, never store/clear or flash mutation. A confirmed `ESP_ERR_NOT_FOUND` load enters idle; a valid authenticated marker enters staged. Unknown schema, corruption, bad HMAC, old/wrong board/version, descriptor mismatch, target mismatch, a target that became running, or load uncertainty remains `ALARM_OTA_MARKER_FAULT` and blocks download/install. Repeating observation may recover valid/absent state without writes. Corrupt/incompatible markers require explicit discard.
4. `alarm_ota_begin` authenticates the manifest and checks the current charging/alarm/clock/schedule guard. No partition erase occurs before the image prefix identifies the expected board/version.
5. `alarm_ota_write` accepts ordered chunks up to 16384 bytes and rechecks the live guard on every call. Charging invalid/false aborts the active transfer. Same-boot HTTP Range reconnect remains caller-owned.
6. `alarm_ota_finish` requires exact size, streaming SHA-256, `esp_ota_end`, descriptor reread, another live guard, marker HMAC, atomic marker put, and authenticated read-back equality. A store error or read-back uncertainty may mean the marker committed, so the component preserves the inactive image and enters marker fault rather than erasing or returning idle. A later read-only reload can recover the valid record as staged.
7. `alarm_ota_activate` re-authenticates both HMACs, derives the next inactive partition from ESP-IDF, matches subtype/address without trusting the persisted address, rejects the running slot, reads and hashes the full flash image, rereads descriptor identity, and repeatedly checks local guards. It clears and verifies marker absence before the final charging check and boot selection. Clear failure cannot select boot. If a guard changes in that narrow post-clear boundary, it atomically restores/read-checks the marker and refuses install. Boot selection failure leaves the current version running and no marker, requiring a new download.
8. `alarm_ota_discard_staged` may run from staged or marker fault. It clears only the marker and returns idle only after a load confirms `NOT_FOUND`; clear or confirmation uncertainty stays blocked. `alarm_ota_maintenance` applies only to an active transfer; a later charging drop keeps a completed staged image but blocks install.

Marker callbacks must use the existing `alarm_nvs` / `shift-alarm` Preferences namespace, one bounded blob, atomic put, and read-back verification. Do not write progress to NVS.

`charging_valid && charging` is mandatory at begin, each write/maintenance boundary, marker commit, full activation verification, and immediately before boot selection. GPIO38 is active-high charging indication, not reliable VBUS detection. A full battery can report not charging while USB is attached; the required fail-closed result is `charging-required`. There is no manual override.

Every mutation re-runs authorization. The integrating application must serialize install against schedule/snooze changes. A staged image remains independently installable when newer than running even if the backend active manifest later changes.

## Tests

```sh
./tests/run_host_tests.sh
python3 tests/test_manifest_contract.py
```

The fast host harness compiles the real `alarm_ota.c` with ABI-faithful ESP-IDF structures and external FreeRTOS/partition/NVS shims; its hash shim is only for fast fault injection. It proves repeated marker observation performs zero store/clear calls. In the pinned IDF job, `run_idf_faithful_tests.sh` recompiles the same component harness with OpenSSL SHA-256/HMAC, checks known vectors, and consumes that job's credential-free ESP-IDF-generated 0.3.10 app binary using the real 24-byte image header, 8-byte segment header, and 256-byte app descriptor offsets. Real hardware remains unverified.
