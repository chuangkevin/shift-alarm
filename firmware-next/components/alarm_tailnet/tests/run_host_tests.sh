#!/usr/bin/env bash
set -euo pipefail
# Linux/macOS; needs C11 compiler, pthreads, and cJSON headers/source or libcjson.
# Prefer ESP-IDF's exact cJSON: CJSON_DIR="$IDF_PATH/components/json/cJSON".
test_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
components_dir="$(cd -- "$test_dir/../.." && pwd)"
compiler="${CC:-cc}"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/alarm-tailnet-tests.XXXXXX")"
cleanup() { rm -f "$test_build_dir/security_test" "$test_build_dir/peer_map_test" "$test_build_dir/core_guard_test" "$test_build_dir/lifecycle_test"; rmdir "$test_build_dir"; }
trap cleanup EXIT
common=(-std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations "-I$test_dir/stubs" "-I$components_dir/microlink/include")
if [[ -n "${SANITIZERS:-undefined}" ]]; then common+=("-fsanitize=${SANITIZERS:-undefined}"); fi
cjson_dir="${CJSON_DIR:-${IDF_PATH:+$IDF_PATH/components/json/cJSON}}"
cjson=()
if [[ -n "$cjson_dir" && -f "$cjson_dir/cJSON.c" ]]; then
    common+=("-I$cjson_dir"); cjson+=("$cjson_dir/cJSON.c")
elif command -v pkg-config >/dev/null && pkg-config --exists libcjson; then
    # pkg-config emits compiler/linker arguments, not shell commands.
    read -r -a cjson_cflags <<< "$(pkg-config --cflags libcjson)"
    read -r -a cjson_libs <<< "$(pkg-config --libs libcjson)"
    common+=("${cjson_cflags[@]}"); cjson+=("${cjson_libs[@]}")
else
    echo 'Set CJSON_DIR to cJSON sources, set IDF_PATH, or install libcjson-dev + pkg-config.' >&2
    exit 2
fi
"$compiler" "${common[@]}" "$test_dir/security_test.c" "$components_dir/microlink/src/ml_security.c" "${cjson[@]}" -o "$test_build_dir/security_test"
"$test_build_dir/security_test"
"$compiler" "${common[@]}" "$test_dir/peer_map_test.c" "$components_dir/microlink/src/ml_security.c" "$components_dir/microlink/src/ml_peer_map.c" "${cjson[@]}" -o "$test_build_dir/peer_map_test"
"$test_build_dir/peer_map_test"
"$compiler" "${common[@]}" -pthread "-I$components_dir/wireguard_lwip/src" "$test_dir/core_guard_test.c" -o "$test_build_dir/core_guard_test"
"$test_build_dir/core_guard_test"
"$compiler" "${common[@]}" -pthread "-I$test_dir/../include" "$test_dir/lifecycle_test.c" "$test_dir/../alarm_tailnet.c" -o "$test_build_dir/lifecycle_test"
"$test_build_dir/lifecycle_test"
