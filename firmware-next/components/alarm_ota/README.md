# alarm_ota 0.1.0 — ESP-IDF 5.3 / ESP32-S3

This component authenticates a release manifest, streams an application into the inactive OTA slot, verifies its size/hash/board/version, and selects it only immediately before restarting. The current application and NVS are never erased by this component. It contains no HTTP server or credentials in source.

**Hardware OTA, interrupted-power recovery, rollback, and alarm timing have not been tested on the physical CUBE with this component.** Host tests cover the pure policy/canonical contract; an ESP-IDF target build checks API integration. Neither is evidence of hardware recovery. Do not flash hardware as part of these tests.

## Required bootstrap

The running firmware must already include this component and have a **rollback-enabled bootloader**, two OTA application partitions, and an `otadata` partition of 0x2000 bytes. An application-only update cannot retrofit a different bootloader or partition table. Installing that initial arrangement is a separate controlled bootstrap; this component does not repair an incomplete USB flash or a board that cannot boot/enumerate.

Set these in the integrating project's `sdkconfig.defaults`:

```ini
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_ESP_TASK_WDT_INIT=y
CONFIG_ESP_TASK_WDT_PANIC=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
# CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK is not set
```

Initialization fails closed if rollback/watchdog/S3 requirements are missing. eFuse anti-rollback must remain disabled for this component: it changes recovery semantics and can burn security-version eFuses during IDF confirmation. Secure Boot provisioning is a separate process; no fuse configuration is performed here.

The CMake project name MUST be the exact board ID, e.g. `project(xingzhi-cube-1.54tft-wifi)`. `PROJECT_VER` must be strict numeric `major.minor.patch` (no suffixes or leading zeroes). Both become the standard ESP application descriptor. Incoming image `project_name` must match the manifest/device board and `version` must match the signed manifest. Incoming version must be greater than the currently running version.

## Manifest and signing contract

Required fields map directly to `alarm_ota_manifest_t`:

```json
{"board":"xingzhi-cube-1.54tft-wifi","version":"0.3.0","size":1000000,"sha256":"<64 lowercase hex>","hmac_sha256":"<64 lowercase hex>"}
```

`size` is the exact uploaded app `.bin` byte count. Canonical bytes are UTF-8/ASCII with LF separators and a final LF:

```text
board\nversion\nsize\nsha256\n
```

There are no spaces; size is unsigned decimal without leading zeroes. SHA256 is lowercase. Sign those bytes with HMAC-SHA256 using the literal UTF-8 bytes of `DEVICE_TOKEN` (do not hex-decode the token). The 32-byte MAC is lowercase hexadecimal in `hmac_sha256`. `tests/manifest-vector.json` is a shared, public test-only vector. It is not a production credential.

The device downloads manifest and binary from the trusted backend using its bearer token. Keep this token out of browser JavaScript, URLs, and logs. HMAC authenticates the release metadata even over HTTP inside a protected WireGuard/Tailnet transport; the binary's streaming SHA256 binds it to that authenticated metadata. SHA256 alone is not source authentication.

JSON parsing belongs to the caller: reject duplicate required keys, oversized/non-string identity/hash fields, fractional/negative/overflowing sizes, and truncation rather than silently copying into fixed arrays. URL fields are not part of this signed manifest: construct the download URL from a fixed configured backend plus the validated SHA256, never accept an arbitrary manifest-provided host.

## Integration API

See `include/alarm_ota.h` and `include/alarm_ota_policy.h`.

1. On every boot, before exposing update routes, call `alarm_ota_boot_self_test(test, context, timeout_ms)`. For a pending image it registers an unfed task-watchdog user and a one-shot restart deadline. Only a successful callback within its deadline invokes `esp_ota_mark_app_valid_cancel_rollback`. Failure requests invalidation/rollback. If the timer or CPU hangs, the task watchdog's panic/reset path is the additional fallback. Do not ignore a returned error. Non-pending images return without running the callback.
2. Self-test should verify NVS readability, schedule integrity and scheduler startup, GPIO21 power hold, display/button initialization, audio-driver initialization, and availability of the local management interface. It must not depend on Internet reachability, otherwise an offline update boot could be rejected unnecessarily. Driver checks do not prove that the speaker is audible.
3. Call `alarm_ota_init` once, supplying board ID, device token, live authorization callback, and a scheduler guard callback. The token is copied internally. The first call must run in a single startup task before HTTP workers are enabled.
4. `alarm_ota_begin` verifies policy and manifest HMAC. `alarm_ota_write` accepts ordered chunks of 1..16384 bytes. The first image descriptor is buffered and checked before any inactive-slot erase. Only one upload can run at a time; opaque generation handles reject stale requests.
5. `alarm_ota_finish` requires exact size, SHA256 match, IDF image validation, and a descriptor reread. It does not select a boot slot.
6. After acknowledging the user-facing request, invoke `alarm_ota_activate` from a worker. It rechecks authorization, deadline, and the alarm guard, then selects the new slot and immediately restarts. **Serialize this call against schedule edits/snooze mutations with the integrating scheduler's lock/reservation.** A snapshot callback alone cannot prevent a different task adding a new imminent alarm between the final check and reboot. Callback implementations must avoid recursively acquiring the same scheduler lock.
7. Run `alarm_ota_maintenance` roughly once per second from a worker independent of the HTTP upload. It aborts a stalled or newly unsafe upload. Explicit `alarm_ota_abort` supports authenticated cancellation. Failed/partial transfers leave the current slot selected.

Callbacks must be fast and non-blocking. Every mutating request (`begin`, `write`, `finish`, `activate`, `abort`) invokes the authorization callback with that request's context. It must validate the authenticated user/session and CSRF/origin policy; an OTA handle is not an authorization capability. No callback context pointer is retained between API calls. Methods are serialized with a try-lock and can return `ESP_ERR_TIMEOUT` when another operation is in progress. Retry at the application layer without creating overlapping sessions.

The guard rejects untrusted time, an uninitialized schedule, ringing/snoozed alarms, an already-due alarm, or a next alarm within the inclusive quiet window (minimum five minutes). `next_alarm_epoch` must account for every local/backend/snoozed source, not just the visible backend list. Total begin-to-activation deadline is bounded to 30..600 seconds and is checked throughout.

`operator_confirmed_power` means **the authenticated operator explicitly confirmed a stable power connection for this update session**. It is not voltage measurement or automatic USB detection. This CUBE's GPIO38 charging signal cannot prove external power when the battery is full. Do not hard-code the field true or infer it from charging state. Expire confirmation with the current update session, and clear it on logout/cancel/reboot.

## Tests

```sh
./tests/run_host_tests.sh
python3 tests/test_manifest_contract.py
```

The first test builds the actual pure-C policy with strict warnings and UndefinedBehaviorSanitizer. Cases cover malformed/unterminated versions, integer boundaries, downgrade/reinstall rejection, board mismatch, hash representation, digest comparison, canonical format, schedule/clock/power gates, inclusive alarm boundaries, and stream-size overflow. The second compiles that same C canonicalizer and compares it with a Python HMAC golden vector, including signed-field tampering.

These host tests do not simulate flash, power interruption, FreeRTOS timers, or task-watchdog resets. Physical acceptance must separately cover interrupted download/power, hash/HMAC/image-identity rejection, reboot before validation, hung/failed self-test, restored schedules, actual audible alarm operation, and prevention of updates near alarms.

## Primary API references

- [ESP-IDF 5.3 OTA / rollback lifecycle](https://docs.espressif.com/projects/esp-idf/en/v5.3.4/esp32s3/api-reference/system/ota.html)
- [ESP-IDF 5.3 task watchdog](https://docs.espressif.com/projects/esp-idf/en/v5.3.4/esp32s3/api-reference/system/wdts.html)
- [Standard ESP application descriptor](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_app_format/include/esp_app_desc.h)
