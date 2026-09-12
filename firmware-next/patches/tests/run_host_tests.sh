#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
binary=$(mktemp /tmp/alarm-http-test.XXXXXX)
trap 'rm -f "$binary"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=undefined "$here/http_parser_test.cpp" -o "$binary"
"$binary"
