#!/bin/sh
set -eu

PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
unset CDPATH ENV BASH_ENV LD_LIBRARY_PATH LD_PRELOAD 2>/dev/null || true

prefix=${PREFIX:-/usr/local}
build_dir=${BUILD_DIR:-build-install}
# Set here as well as in the resolver, because the resolver returns early on
# any of its safety checks and this is read unconditionally under `set -u`.
extension_note="Shell extension not enabled. On GNOME or Cinnamon, run this as yourself: keysharp-desktop enable-extension"

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
    # Before the service is refreshed, so the daemon finds the provider on its
    # first start rather than sitting in its retry loop.
    #
    # Advisory in every direction: the extension is per-user and per-desktop,
    # so a KWin or X11 machine has nothing to enable, and an install must not
    # fail because of it. The command reports 3 when it wrote the setting but
    # the user still has to log back in -- which is a success, not an error, and
    # is why this is a case rather than an `if`.
    #
    # env -i leaves PATH as /usr/bin:/bin, which does not contain the default
    # /usr/local prefix, so the binary is named by absolute path built from the
    # prefix this install actually used.
    extension_status=0
    /usr/sbin/runuser -u "$invoking_name" -- /usr/bin/env -i \
        HOME="$invoking_home" USER="$invoking_name" LOGNAME="$invoking_name" \
        LANG=C PATH=/usr/bin:/bin XDG_RUNTIME_DIR="$invoking_runtime" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$invoking_runtime/bus" \
        "$prefix/bin/keysharp-desktop" enable-extension >/dev/null 2>&1 \
        || extension_status=$?
    case "$extension_status" in
        0) extension_note="Shell extension enabled, or not needed on this desktop." ;;
        3) extension_note="Shell extension enabled. Log out and back in to load it." ;;
        *) extension_note="Could not enable the shell extension. Run this as yourself: keysharp-desktop enable-extension" ;;
    esac

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
    "$extension_note" \
    "$user_handoff" \
    "Other logged-in users are not refreshed automatically; after an upgrade they should run:" \
    "  systemctl --user daemon-reload" \
    "  systemctl --user try-restart keysharp-desktop.service" \
    "or log out."
