#!/bin/sh
set -eu
component_dir=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f "$test_dir/policy-test"; rmdir "$test_dir"' EXIT INT TERM
cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
  -I"$component_dir/include" "$component_dir/alarm_ota_policy.c" \
  "$component_dir/tests/test_policy.c" -o "$test_dir/policy-test"
"$test_dir/policy-test"
