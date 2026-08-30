#!/bin/sh
set -eu

PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
unset CDPATH ENV BASH_ENV LD_LIBRARY_PATH LD_PRELOAD 2>/dev/null || true

is_package_managed() {
    managed_path=$1
    managed_resolved=$(readlink -f -- "$managed_path" 2>/dev/null || true)
    if command -v dpkg-query >/dev/null 2>&1 \
        && { dpkg-query -S "$managed_path" >/dev/null 2>&1 \
            || { [ -n "$managed_resolved" ] \
                && dpkg-query -S "$managed_resolved" >/dev/null 2>&1; }; }; then
        return 0
    fi
    if command -v rpm >/dev/null 2>&1 \
        && { rpm -qf "$managed_path" >/dev/null 2>&1 \
            || { [ -n "$managed_resolved" ] \
                && rpm -qf "$managed_resolved" >/dev/null 2>&1; }; }; then
        return 0
    fi
    if command -v pacman >/dev/null 2>&1 \
        && { pacman -Qo "$managed_path" >/dev/null 2>&1 \
            || { [ -n "$managed_resolved" ] \
                && pacman -Qo "$managed_resolved" >/dev/null 2>&1; }; }; then
        return 0
    fi
    return 1
}

is_component_package_installed() {
    if command -v dpkg-query >/dev/null 2>&1 \
        && dpkg-query -W -f='${db:Status-Abbrev}' keysharp-desktop 2>/dev/null \
            | grep -q '^ii '; then
        return 0
    fi
    command -v rpm >/dev/null 2>&1 && rpm -q keysharp-desktop >/dev/null 2>&1 \
        && return 0
    command -v pacman >/dev/null 2>&1 && pacman -Q keysharp-desktop >/dev/null 2>&1 \
        && return 0
    return 1
}

layered_install_present() {
    portable_resolved=$(readlink -f -- /usr/local/bin/keysharp-desktop 2>/dev/null || true)
    for system_binary in /usr/bin/keysharp-desktop \
        /run/current-system/sw/bin/keysharp-desktop; do
        [ -e "$system_binary" ] || [ -L "$system_binary" ] || continue
        system_resolved=$(readlink -f -- "$system_binary" 2>/dev/null || true)
        [ -n "$portable_resolved" ] && [ "$system_resolved" = "$portable_resolved" ] \
            && continue
        return 0
    done
    for managed_path in /usr/local/bin/keysharp-desktop \
        /usr/share/polkit-1/actions/org.keysharp.desktop.policy; do
        [ -e "$managed_path" ] || [ -L "$managed_path" ] || continue
        is_package_managed "$managed_path" && return 0
    done
    is_component_package_installed
}

prepare_invoking_user_manager() {
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
            2>/dev/null)" = "$invoking_uid" ]
}

run_invoking_user_systemctl() {
    /usr/sbin/runuser -u "$invoking_name" -- /usr/bin/env -i \
        HOME="$invoking_home" USER="$invoking_name" LOGNAME="$invoking_name" \
        LANG=C PATH=/usr/bin:/bin XDG_RUNTIME_DIR="$invoking_runtime" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$invoking_runtime/bus" \
        /usr/bin/systemctl --user --no-pager "$@"
}

stop_invoking_user_manager() {
    prepare_invoking_user_manager \
        && run_invoking_user_systemctl stop keysharp-desktop.service \
            keysharp-desktop.socket >/dev/null 2>&1 \
        && run_invoking_user_systemctl daemon-reload >/dev/null 2>&1
}

reload_invoking_user_manager() {
    prepare_invoking_user_manager \
        && run_invoking_user_systemctl daemon-reload >/dev/null 2>&1
}

case "${1:-}" in
    "") ;;
    -h|--help)
        printf '%s\n' "usage: $0"
        exit 0
        ;;
    *)
        printf '%s\n' "usage: $0" >&2
        exit 2
        ;;
esac
if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' "keysharp-desktop: run this uninstaller as root" >&2
    exit 1
fi

if layered_install_present; then
    printf '%s\n' \
        "Refusing to remove a portable layer while another or package-managed keysharp-desktop installation is present." \
        "Remove the other installation first, uninstall this portable copy, then reinstall the desired package." >&2
    exit 1
fi

user_manager_stopped=false
if stop_invoking_user_manager; then
    user_manager_stopped=true
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl disable --now keysharp-desktop-authority.socket \
        keysharp-desktop-authority.service >/dev/null 2>&1 || true
    systemctl --global disable keysharp-desktop.socket >/dev/null 2>&1 || true
fi

rm -f -- \
    /usr/local/bin/keysharp-desktop \
    /usr/local/lib/systemd/user/keysharp-desktop.service \
    /usr/local/lib/systemd/user/keysharp-desktop.socket \
    /usr/local/lib/systemd/system/keysharp-desktop-authority.service \
    /usr/local/lib/systemd/system/keysharp-desktop-authority.socket \
    /usr/local/lib/tmpfiles.d/keysharp-desktop-permissions.conf \
    /usr/share/polkit-1/actions/org.keysharp.desktop.policy \
    /usr/local/share/applications/org.keysharp.DesktopCapture.desktop \
    /usr/local/include/keysharp_desktop/protocol.h
rm -rf -- \
    /usr/local/share/gnome-shell/extensions/keysharp@keysharp.io \
    /usr/local/share/cinnamon/extensions/keysharp@keysharp.io \
    /usr/local/share/doc/keysharp-desktop \
    /run/keysharp-desktop

printf '%s\n' "Removed keysharp-desktop."
printf '%s\n' \
    "Shared grants in /var/lib/keysharp-permissions/v1 were retained; revoke them with keysharp-desktop before uninstalling."

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi
if [ "$user_manager_stopped" = true ] && reload_invoking_user_manager; then
    printf '%s\n' \
        "Stopped keysharp-desktop and reloaded the invoking user's active manager."
else
    printf '%s\n' \
        "No active invoking user manager was safely stopped and reloaded." >&2
fi
printf '%s\n' \
    "Other logged-in users were not contacted; each should log out or run:" \
    "  systemctl --user stop keysharp-desktop.service keysharp-desktop.socket" \
    "  systemctl --user daemon-reload"
