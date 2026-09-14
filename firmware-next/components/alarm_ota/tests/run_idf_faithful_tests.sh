#!/bin/sh
set -eu
if [ "$#" -ne 1 ] || [ ! -f "$1" ]; then
  echo "usage: $0 /path/to/credential-free-app.bin" >&2
  exit 2
fi
component_dir=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT INT TERM
cc -std=c11 -D_POSIX_C_SOURCE=200809L -DALARM_OTA_USE_OPENSSL \
  -Wall -Wextra -Werror -Wno-deprecated-declarations -fsanitize=undefined \
  -I"$component_dir/tests/host_shims" -I"$component_dir/include" \
  "$component_dir/alarm_ota.c" "$component_dir/alarm_ota_policy.c" \
  "$component_dir/tests/host_shims/host_runtime.c" \
  "$component_dir/tests/test_real_component.c" -lcrypto -o "$test_dir/real-component-test"
mkdir "$test_dir/vector" "$test_dir/image"
ALARM_OTA_TEST_DIR="$test_dir/vector" "$test_dir/real-component-test" crypto-vector
ALARM_OTA_TEST_DIR="$test_dir/image" "$test_dir/real-component-test" real-image "$1"
echo "real SHA-256/HMAC and ESP-IDF 5.3.2 app image component checks passed"
