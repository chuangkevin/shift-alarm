#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/alarm-derp-tests.XXXXXX")
trap 'rm -f "$build/stream" "$build/cache"; rmdir "$build"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined "$here/derp_stream_test.c" -o "$build/stream"
"$build/stream"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined "$here/derp_cache_test.c" -o "$build/cache"
"$build/cache"
python3 "$here/test_derp_cleanup.py"
python3 "$here/test_derp_routes.py"
