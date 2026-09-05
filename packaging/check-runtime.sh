#!/bin/sh
# Check runtime dependencies before copying files or starting services.
set -eu
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
unset CDPATH ENV BASH_ENV LD_LIBRARY_PATH LD_PRELOAD 2>/dev/null || true

install_missing=false
case "${1:-}" in
    '') ;;
    --install) install_missing=true ;;
    -h|--help) echo "Usage: check-runtime.sh [--install]"; exit 0 ;;
    *) echo "Usage: check-runtime.sh [--install]" >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || exit 2
archive_root=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
payload=$archive_root/payload/usr/local

check_runtime() {
    [ -d /run/systemd/system ] || {
        echo "keysharp-desktop requires a running systemd system manager." >&2
        return 1
    }
    for required in systemctl pkcheck; do
        command -v "$required" >/dev/null 2>&1 || {
            echo "keysharp-desktop requires $required." >&2
            return 1
        }
    done
    LD_LIBRARY_PATH="$payload/lib" "$payload/bin/keysharp-desktop" info >/dev/null
}

if check_runtime; then exit 0; fi
if [ "$install_missing" != true ]; then
    echo "Install the runtime dependencies listed in docs/install.md, or run sudo ./check-runtime.sh --install." >&2
    exit 1
fi
[ "$(id -u)" = 0 ] || { echo "--install requires root." >&2; exit 1; }
# A missing system manager cannot be fixed by adding libraries to this environment.
[ -d /run/systemd/system ] || exit 1
if command -v apt-get >/dev/null 2>&1; then
    apt-get update
    apt-get install -y libglib2.0-0 libxcb1 libxcb-shm0 libxcb-randr0 libxcb-shape0 libxcb-composite0 libxkbcommon0 libxkbcommon-x11-0 libwayland-client0 polkitd systemd
elif command -v dnf >/dev/null 2>&1; then
    dnf install -y glib2 libxcb libxkbcommon libxkbcommon-x11 wayland-libs polkit systemd
elif command -v zypper >/dev/null 2>&1; then
    zypper --non-interactive install libglib-2_0-0 libxcb1 libxcb-shm0 libxcb-randr0 libxcb-shape0 libxcb-composite0 libxkbcommon0 libxkbcommon-x11-0 libwayland-client0 polkit systemd
elif command -v pacman >/dev/null 2>&1; then
    pacman -S --needed --noconfirm glib2 libxcb libxkbcommon libxkbcommon-x11 wayland polkit systemd
else
    echo "No supported package manager. Install runtime dependencies through this distribution's package manager." >&2
    exit 1
fi
check_runtime
