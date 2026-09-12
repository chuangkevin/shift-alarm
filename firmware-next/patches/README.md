# Bounded local WebServer input

The application uses Arduino's synchronous WebServer on the alarm scheduling
thread. Its upstream parser reads the complete body before middleware and has
no overall request deadline. This appliance replaces only `_parseRequest`, with
an intentionally restricted HTTP/1.1 parser. The legacy multipart implementation
remains compiled but is unreachable. No device route requires multipart/raw uploads.

CMake runs `apply_arduino_http.py` after IDF resolves Arduino. It requires version
3.1.3 and the exact upstream Parsing.cpp SHA256, keeps a local pristine copy,
and regenerates the patch on every configure. Managed dependencies and backup
are not source-controlled. Upgrade Arduino only after reviewing and updating
this patch. An absent application header gate rejects all requests.

Limits: 8 KiB aggregate request line + headers, 2 KiB per line, 40 headers,
98,304-byte JSON body, 4 KiB form body, 32 query/form arguments, and a 2-second
total header/body deadline. Network reads are nonblocking. Invalid requests
close the connection without waiting to consume the body. Host and privileged
schedule/test/stop bearer checks run before body reads; form nonce checks still
run inside routes. Unknown header names are bounded; duplicate Host, length,
content type, authorization, Origin and nonce are rejected. Transfer-Encoding,
Expect, multipart, invalid lengths and bodies on GET/HEAD are unsupported.

The main loop regains control at least once per parsing deadline even with a
slow sender; this is a bounded delay, not a real-time guarantee. Response sends
and outbound polling have separate limits. Host tests use the same parser with
a fake nonblocking client and clock; they cover accepted forms/maximum JSON,
oversized input, framing ambiguity, premature EOF, slow headers/body and patch
drift/idempotency. Run `sh patches/tests/run_host_tests.sh` from firmware-next;
after resolving managed dependencies, run `python3 patches/tests/test_patch.py`
for patch version, hash and idempotency verification (also required in the IDF CI job).
Target compilation and physical timing remain separate validation steps; these
host checks are not a claim of hardware validation.
