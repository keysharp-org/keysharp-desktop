#!/bin/sh
set -eu

# Starts a private X server for the X11 tests. Without Xvfb the tests skip
# rather than fail, because the build must not require an X server; the skip
# is visible in the ctest output and CI installs Xvfb so it is never taken
# silently. A gate that passes when it did not run is not a gate.
#
# Readiness is decided by connecting, not by waiting for a socket file: Xvfb
# serves over TCP when it cannot bind a UNIX listener, which happens whenever
# /tmp/.X11-unix is not mode 1777, and the file would then never appear.

binary=$1
server=""

if ! command -v Xvfb >/dev/null 2>&1; then
    echo "Xvfb not installed: skipping X server tests"
    exit 77
fi

cleanup() {
    if [ -n "$server" ]; then
        kill "$server" 2>/dev/null || true
        wait "$server" 2>/dev/null || true
    fi
}
trap cleanup EXIT HUP INT TERM

display=99
while [ "$display" -lt 110 ]; do
    # An odd width, so the scanline pad is non-trivial and a stride computed
    # as width * 4 rather than read from the server shows up.
    Xvfb ":${display}" -screen 0 1279x1024x24 >/dev/null 2>&1 &
    server=$!
    attempt=0
    while [ "$attempt" -lt 40 ]; do
        # Our own server has to be alive BEFORE the display is probed. If it
        # exited, this display belongs to someone else -- a leftover server, or
        # another job on the same machine -- and the probe would succeed
        # against theirs, running the whole suite on a display this script
        # neither started nor controls, against whatever state it carries.
        if ! kill -0 "$server" 2>/dev/null; then
            break
        fi
        if KSD_TEST_DISPLAY=":${display}" DISPLAY=":${display}"             KSD_TEST_PROBE=1 "$binary" 2>/dev/null; then
            # Deliberately not exec. Replacing the shell would drop the EXIT
            # trap with it, orphaning the server: every successful run leaked
            # one, and the next run then adopted it by the path above and
            # inherited its state.
            # DISPLAY as well as KSD_TEST_DISPLAY: a test that exercises the
            # session-resolution path reads the display back out of
            # /proc/<pid>/environ, which holds the environment this process was
            # executed with and cannot be added to afterwards.
            KSD_TEST_DISPLAY=":${display}" DISPLAY=":${display}" "$binary"
            status=$?
            cleanup
            server=""
            exit "$status"
        fi
        sleep 0.1
        attempt=$((attempt + 1))
    done
    cleanup
    server=""
    display=$((display + 1))
done

echo "no usable X server after 11 attempts: skipping"
exit 77
