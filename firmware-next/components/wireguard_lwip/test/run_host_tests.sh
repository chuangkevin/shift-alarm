#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${IDF_PATH:?Set IDF_PATH to ESP-IDF 5.3.2 (headers only; no target tools required)}"
bin=$(mktemp /tmp/alarm-wg-test.XXXXXX)
trap 'rm -f "$bin"' EXIT
case $(uname -s) in Darwin) strip=-Wl,-dead_strip;; *) strip=-Wl,--gc-sections;; esac
: "${WG_TEST_SOURCE:=$here/../src/wireguardif.c}"
cc -DWG_TEST_SOURCE=\""$WG_TEST_SOURCE"\" -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-format -Wno-unused-function -ffunction-sections -fdata-sections -fsanitize=undefined -I"$here/host_include" -I"$here/../src" -I"$IDF_PATH/components/lwip/lwip/src/include" "$here/transport_test.c" "$strip" -o "$bin"
"$bin"
