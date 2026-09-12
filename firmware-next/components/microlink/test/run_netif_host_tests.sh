#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${IDF_PATH:?Set IDF_PATH to ESP-IDF 5.3.2}"
bin=$(mktemp /tmp/alarm-netif-test.XXXXXX)
trap 'rm -f "$bin"' EXIT
case $(uname -s) in Darwin) strip=-Wl,-dead_strip;; *) strip=-Wl,--gc-sections;; esac
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -fsanitize=undefined -I"$here/netif_host_include" -I"$IDF_PATH/components/lwip/lwip/src/include" "$here/netif_route_test.c" "$IDF_PATH/components/lwip/lwip/src/core/netif.c" "$IDF_PATH/components/lwip/lwip/src/core/ipv4/ip4.c" "$IDF_PATH/components/lwip/lwip/src/core/def.c" "$IDF_PATH/components/lwip/lwip/src/core/ip.c" "$IDF_PATH/components/lwip/lwip/src/core/ipv4/ip4_addr.c" "$strip" -o "$bin"
"$bin"
