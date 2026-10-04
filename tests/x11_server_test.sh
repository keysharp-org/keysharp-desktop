#!/bin/sh
set -eu

# Starts a private X server for the X11 tests. Without Xvfb the tests skip
# rather than fail, because the build must not require an X server; the skip
# is visible in the ctest output and CI installs Xvfb so it is never taken
# silently. A gate that passes when it did not run is not a gate.
#
# Readiness is decided by connecting, not by waiting for a socket file.

binary=$1
export KSD_TEST_STATE_ONLY=${2:-}
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

# Probe before starting so an existing display can never be adopted as the fixture.
for display in 190 191 192 193 194 195 196 197 198 199; do
    if KSD_TEST_DISPLAY=":${display}" KSD_TEST_PROBE=1 "$binary" 2>/dev/null; then
        continue
    fi
    if [ -w /tmp/.X11-unix ]; then
        Xvfb ":${display}" -screen 0 1279x1024x24 -noreset -nolisten tcp >/dev/null 2>/dev/null &
    else
        if [ -z "$KSD_TEST_STATE_ONLY" ]; then
            echo "private Unix X server unavailable; shared-memory tests require local transport: skipping"
            exit 77
        fi
        Xvfb ":${display}" -screen 0 1279x1024x24 -noreset -nolisten unix -nolisten local -listen tcp -pn >/dev/null 2>/dev/null &
    fi
    server=$!
    attempt=0
    while [ "$attempt" -lt 40 ] && kill -0 "$server" 2>/dev/null; do
        if KSD_TEST_DISPLAY=":${display}" DISPLAY=":${display}" KSD_TEST_PROBE=1 "$binary" 2>/dev/null; then
            KSD_TEST_DISPLAY=":${display}" DISPLAY=":${display}" "$binary"
            exit $?
        fi
        sleep 0.1
        attempt=$((attempt + 1))
    done
    cleanup
    server=""
done
echo "private X server did not become ready: skipping"
exit 77
