#!/bin/sh
set -eu
component_dir=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT INT TERM
cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
  -I"$component_dir/include" "$component_dir/alarm_ota_policy.c" \
  "$component_dir/tests/test_policy.c" -o "$test_dir/policy-test"
"$test_dir/policy-test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
  -I"$component_dir/include" "$component_dir/alarm_ota_policy.c" \
  "$component_dir/tests/test_staged_harness.c" -o "$test_dir/staged-test"
"$test_dir/staged-test"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsanitize=undefined \
  -I"$component_dir/tests/host_shims" -I"$component_dir/include" \
  "$component_dir/alarm_ota.c" "$component_dir/alarm_ota_policy.c" \
  "$component_dir/tests/host_shims/host_runtime.c" \
  "$component_dir/tests/test_real_component.c" -o "$test_dir/real-component-test"
run_empty() { case_dir="$test_dir/$1"; mkdir "$case_dir"; ALARM_OTA_TEST_DIR="$case_dir" "$test_dir/real-component-test" "$1"; }
run_staged() { case_dir="$test_dir/$1"; mkdir "$case_dir"; ALARM_OTA_TEST_DIR="$case_dir" "$test_dir/real-component-test" stage; ALARM_OTA_TEST_DIR="$case_dir" "$test_dir/real-component-test" "$1"; }
run_staged install
case_dir="$test_dir/store-fault"; mkdir "$case_dir"; ALARM_OTA_TEST_DIR="$case_dir" "$test_dir/real-component-test" store-fault; ALARM_OTA_TEST_DIR="$case_dir" "$test_dir/real-component-test" recover
run_staged observe-recover
run_empty observe-absent
run_empty malformed-clear-fail
run_empty maintenance-guard-sample
run_staged discard-confirm-fail
run_staged discard-success
run_staged flash-fail
run_staged sha-mismatch
run_staged descriptor-fail
run_staged target-fail
run_staged target-activation-fail
run_staged charging-drop
run_staged boot-fail
