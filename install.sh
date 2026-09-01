#!/bin/sh
set -eu

PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
unset CDPATH ENV BASH_ENV LD_LIBRARY_PATH LD_PRELOAD 2>/dev/null || true

prefix=${PREFIX:-/usr/local}
build_dir=${BUILD_DIR:-build-install}

refresh_invoking_user_manager() {
    invoking_uid=${SUDO_UID:-}
    case "$invoking_uid" in
        ''|*[!0-9]*|0) return 1 ;;
    esac
    [ -x /usr/bin/getent ] && [ -x /usr/bin/awk ] && [ -x /usr/bin/stat ] \
        && [ -x /usr/sbin/runuser ] && [ -x /usr/bin/env ] \
        && [ -x /usr/bin/systemctl ] || return 1
    passwd_entry=$(/usr/bin/getent passwd "$invoking_uid" 2>/dev/null) || return 1
    invoking_name=$(printf '%s\n' "$passwd_entry" | /usr/bin/awk -F: \
        -v expected="$invoking_uid" '$3 == expected { print $1; exit }')
    invoking_home=$(printf '%s\n' "$passwd_entry" | /usr/bin/awk -F: \
        -v expected="$invoking_uid" '$3 == expected { print $6; exit }')
    case "$invoking_name" in
        ''|-*|*[!A-Za-z0-9._-]*) return 1 ;;
    esac
    case "$invoking_home" in
        /*) ;;
        *) return 1 ;;
    esac
    invoking_runtime=/run/user/$invoking_uid
    runtime_metadata=$(/usr/bin/stat -Lc '%u %a' -- "$invoking_runtime" \
        2>/dev/null) || return 1
    # shellcheck disable=SC2086 # deliberate split into uid and mode
    set -- $runtime_metadata
    [ "$1" = "$invoking_uid" ] && [ $((0$2 & 022)) -eq 0 ] \
        && [ ! -L "$invoking_runtime" ] \
        && [ -S "$invoking_runtime/bus" ] \
        && [ ! -L "$invoking_runtime/bus" ] \
        && [ "$(/usr/bin/stat -Lc '%u' -- "$invoking_runtime/bus" \
            2>/dev/null)" = "$invoking_uid" ] || return 1
    /usr/sbin/runuser -u "$invoking_name" -- /usr/bin/env -i \
        HOME="$invoking_home" USER="$invoking_name" LOGNAME="$invoking_name" \
        LANG=C PATH=/usr/bin:/bin XDG_RUNTIME_DIR="$invoking_runtime" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$invoking_runtime/bus" \
        /usr/bin/systemctl --user --no-pager daemon-reload >/dev/null 2>&1 \
        || return 1
    /usr/sbin/runuser -u "$invoking_name" -- /usr/bin/env -i \
        HOME="$invoking_home" USER="$invoking_name" LOGNAME="$invoking_name" \
        LANG=C PATH=/usr/bin:/bin XDG_RUNTIME_DIR="$invoking_runtime" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$invoking_runtime/bus" \
        /usr/bin/systemctl --user --no-pager restart \
        keysharp-desktop.service >/dev/null 2>&1 || return 1
}

cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build "$build_dir" --parallel
cmake --install "$build_dir"
[ ! -x /sbin/ldconfig ] || /sbin/ldconfig

if command -v systemd-tmpfiles >/dev/null 2>&1; then
    systemd-tmpfiles --create "$prefix/lib/tmpfiles.d/keysharp-desktop-permissions.conf" || true
fi
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
    systemctl enable --now keysharp-desktop-authority.socket || true
    systemctl try-restart keysharp-desktop-authority.service || true
    systemctl --global enable keysharp-desktop.service || true
fi

if refresh_invoking_user_manager; then
    user_handoff="Refreshed keysharp-desktop for the invoking user's active session."
else
    user_handoff="No active invoking user manager was safely resolved. New logins start the globally enabled service; already logged-in users should run:
  systemctl --user daemon-reload
  systemctl --user restart keysharp-desktop.service"
fi
printf '%s\n' \
    "Installed keysharp-desktop ${prefix}." \
    "$user_handoff" \
    "Other logged-in users are not refreshed automatically; after an upgrade they should run:" \
    "  systemctl --user daemon-reload" \
    "  systemctl --user try-restart keysharp-desktop.service" \
    "or log out."
