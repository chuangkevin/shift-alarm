#!/bin/sh
set -eu
component_dir=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
build_dir=$(mktemp -d)
trap 'rm -f "$build_dir/http-test" "$build_dir/stream-test"; rmdir "$build_dir"' EXIT INT TERM
c++ -std=c++17 -Wall -Wextra -Werror -I"$component_dir/include" \
 "$component_dir/proxy_http.cpp" "$component_dir/test/http_test.cpp" -o "$build_dir/http-test"
"$build_dir/http-test"
c++ -std=c++17 -Wall -Wextra -Werror -pthread -I"$component_dir/test/host_include" -I"$component_dir/include" \
 "$component_dir/proxy_http.cpp" "$component_dir/test/stream_test.cpp" -o "$build_dir/stream-test"
"$build_dir/stream-test"
