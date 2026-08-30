#!/bin/sh
set -eu

PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
unset CDPATH ENV BASH_ENV LD_LIBRARY_PATH LD_PRELOAD 2>/dev/null || true

skip_compatible=false

usage() {
    printf '%s\n' "usage: sudo $0 [--skip-if-compatible]"
}

protocol_version_compatible() {
    [ "$1" = keysharp-desktop/session-v1 ] \
        && [ "$2" = 1 ] && [ "$3" = 2 ]
}

is_root_protected_chain() {
    current=$1
    while :; do
        metadata=$(stat -Lc '%u %a' -- "$current" 2>/dev/null) || return 1
        set -- $metadata
        [ "$1" = 0 ] || return 1
        # Multi-user Nix uses a sticky, group-writable store root. Sticky-bit
        # ownership still prevents build users from replacing root-owned paths.
        if [ $((0$2 & 022)) -ne 0 ]; then
            [ "$current" = /nix/store ] \
                && [ $((0$2 & 002)) -eq 0 ] \
                && [ $((0$2 & 01000)) -ne 0 ] \
                || return 1
        fi
        [ "$current" = / ] && break
        current=${current%/*}
        [ -n "$current" ] || current=/
    done
}

is_root_protected_executable() {
    resolved=$(readlink -f -- "$1" 2>/dev/null) || return 1
    [ -f "$resolved" ] && [ -x "$resolved" ] || return 1
    is_root_protected_chain "$1" && is_root_protected_chain "$resolved"
}

is_root_protected_file() {
    resource_resolved=$(readlink -f -- "$1" 2>/dev/null) || return 1
    [ -f "$resource_resolved" ] && [ -s "$resource_resolved" ] || return 1
    is_root_protected_chain "$1" \
        && is_root_protected_chain "$resource_resolved"
}

policy_configuration_matches() {
    [ -s "$1" ] || return 1
    awk '
        {
            line = $0
            sub(/^[[:space:]]*/, "", line)
            sub(/[[:space:]]*$/, "", line)
        }
        line == "<action id=\"org.keysharp.desktop.grant\">" {
            if (inside) invalid = 1
            inside = 1
            actions++
            next
        }
        inside && line ~ /^<allow_(any|inactive|active)>/ {
            if (line == "<allow_any>no</allow_any>") allow_any++
            else if (line == "<allow_inactive>no</allow_inactive>") allow_inactive++
            else if (line == "<allow_active>auth_self</allow_active>") allow_active++
            else invalid = 1
            next
        }
        inside && line ~ /^<message>/ {
            if (line == "<message>$(polkit.message)</message>") messages++
            else invalid = 1
            next
        }
        inside && line == "</action>" {
            inside = 0
            closes++
        }
        END {
            if (invalid || inside || actions != 1 || closes != 1 ||
                messages != 1 || allow_any != 1 || allow_inactive != 1 ||
                allow_active != 1)
                exit 1
        }
    ' "$1"
}

tmpfiles_configuration_matches() {
    [ -s "$1" ] || return 1
    awk '
        /^[[:space:]]*($|#)/ { next }
        NF == 7 && $1 == "d" && $2 == "/var/lib/keysharp-permissions" &&
            $3 == "0700" && $4 == "root" && $5 == "root" &&
            $6 == "-" && $7 == "-" { persistent_root++; next }
        NF == 7 && $1 == "d" && $2 == "/var/lib/keysharp-permissions/v1" &&
            $3 == "0700" && $4 == "root" && $5 == "root" &&
            $6 == "-" && $7 == "-" { version_root++; next }
        NF == 7 && $1 == "d" && $2 == "/run/keysharp-permissions" &&
            $3 == "0755" && $4 == "root" && $5 == "root" &&
            $6 == "-" && $7 == "-" { runtime_root++; next }
        { invalid = 1 }
        END {
            if (invalid || persistent_root != 1 || version_root != 1 ||
                runtime_root != 1)
                exit 1
        }
    ' "$1"
}

desktop_entry_configuration_matches() {
    [ -s "$1" ] || return 1
    alternate_binary=${3:-$2}
    awk -F= -v expected_exec="$2 serve" \
        -v alternate_exec="$alternate_binary serve" '
        /^[[:space:]]*\[/ {
            section = $0
            sub(/^[[:space:]]*/, "", section)
            sub(/[[:space:]]*$/, "", section)
            inside = section == "[Desktop Entry]"
            if (inside) sections++
            next
        }
        inside {
            key = $1
            sub(/^[[:space:]]*/, "", key)
            sub(/[[:space:]]*$/, "", key)
            value = substr($0, index($0, "=") + 1)
            sub(/^[[:space:]]*/, "", value)
            sub(/[[:space:]]*$/, "", value)
            if (key == "Type") {
                types++
                if (value != "Application") invalid = 1
            } else if (key == "Exec") {
                execs++
                if (value != expected_exec && value != alternate_exec) invalid = 1
            } else if (key == "NoDisplay") {
                hidden++
                if (value != "true") invalid = 1
            } else if (key == "X-KDE-DBUS-Restricted-Interfaces") {
                restrictions++
                item_count = split(value, items, ",")
                for (item = 1; item <= item_count; item++) {
                    sub(/^[[:space:]]*/, "", items[item])
                    sub(/[[:space:]]*$/, "", items[item])
                    if (items[item] == "org.kde.kwin.Screenshot") screenshot = 1
                    if (items[item] == "org.kde.KWin.ScreenShot2") screenshot2 = 1
                }
            }
        }
        END {
            if (invalid || sections != 1 || types != 1 || execs != 1 ||
                hidden != 1 || restrictions != 1 || !screenshot || !screenshot2)
                exit 1
        }
    ' "$1"
}

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

refresh_invoking_user_manager() {
    restart_broker=$1
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
    if [ "$restart_broker" = true ]; then
        /usr/sbin/runuser -u "$invoking_name" -- /usr/bin/env -i \
            HOME="$invoking_home" USER="$invoking_name" \
            LOGNAME="$invoking_name" LANG=C PATH=/usr/bin:/bin \
            XDG_RUNTIME_DIR="$invoking_runtime" \
            DBUS_SESSION_BUS_ADDRESS="unix:path=$invoking_runtime/bus" \
            /usr/bin/systemctl --user --no-pager try-restart \
            keysharp-desktop.service >/dev/null 2>&1 || return 1
    fi
    /usr/sbin/runuser -u "$invoking_name" -- /usr/bin/env -i \
        HOME="$invoking_home" USER="$invoking_name" LOGNAME="$invoking_name" \
        LANG=C PATH=/usr/bin:/bin XDG_RUNTIME_DIR="$invoking_runtime" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$invoking_runtime/bus" \
        /usr/bin/systemctl --user --no-pager start keysharp-desktop.socket \
        >/dev/null 2>&1
}

refresh_or_handoff_user_socket() {
    restart_broker=$1
    if refresh_invoking_user_manager "$restart_broker"; then
        printf '%s\n' \
            "Refreshed keysharp-desktop for the invoking user's active session."
    else
        printf '%s\n' \
            "No active invoking user manager was safely resolved." \
            "New logins start the globally enabled socket; already logged-in users should run:" \
            "  systemctl --user daemon-reload" \
            "  systemctl --user try-restart keysharp-desktop.service" \
            "  systemctl --user start keysharp-desktop.socket"
    fi
    printf '%s\n' \
        "Other logged-in users are not refreshed automatically; after an upgrade they should run:" \
        "  systemctl --user daemon-reload" \
        "  systemctl --user try-restart keysharp-desktop.service" \
        "or log out."
}

has_required_resources() {
    resource_root=$1
    for relative in \
        lib/systemd/user/keysharp-desktop.service \
        lib/systemd/user/keysharp-desktop.socket \
        lib/systemd/system/keysharp-desktop-authority.service \
        lib/systemd/system/keysharp-desktop-authority.socket \
        lib/tmpfiles.d/keysharp-desktop-permissions.conf \
        share/applications/org.keysharp.DesktopCapture.desktop \
        share/gnome-shell/extensions/keysharp@keysharp.io/extension.js \
        share/gnome-shell/extensions/keysharp@keysharp.io/metadata.json \
        share/cinnamon/extensions/keysharp@keysharp.io/extension.js \
        share/cinnamon/extensions/keysharp@keysharp.io/metadata.json; do
        [ -f "$resource_root/$relative" ] \
            && [ -s "$resource_root/$relative" ] || return 1
    done
}

resource_configuration_matches() {
    resource_root=$1
    expected_binary=$2
    policy_path=$3
    alternate_binary=${4:-$2}
    has_required_resources "$resource_root" \
        && policy_configuration_matches "$policy_path" \
        && tmpfiles_configuration_matches \
            "$resource_root/lib/tmpfiles.d/keysharp-desktop-permissions.conf" \
        && desktop_entry_configuration_matches \
            "$resource_root/share/applications/org.keysharp.DesktopCapture.desktop" \
            "$expected_binary" "$alternate_binary"
}

installed_resources_are_protected() {
    resource_root=$1
    policy_path=$2
    is_root_protected_file "$policy_path" || return 1
    for relative in \
        lib/systemd/user/keysharp-desktop.service \
        lib/systemd/user/keysharp-desktop.socket \
        lib/systemd/system/keysharp-desktop-authority.service \
        lib/systemd/system/keysharp-desktop-authority.socket \
        lib/tmpfiles.d/keysharp-desktop-permissions.conf \
        share/applications/org.keysharp.DesktopCapture.desktop \
        share/gnome-shell/extensions/keysharp@keysharp.io/extension.js \
        share/gnome-shell/extensions/keysharp@keysharp.io/metadata.json \
        share/cinnamon/extensions/keysharp@keysharp.io/extension.js \
        share/cinnamon/extensions/keysharp@keysharp.io/metadata.json; do
        is_root_protected_file "$resource_root/$relative" || return 1
    done
}

systemd_unit_loaded() {
    [ "$(systemctl show --property=LoadState --value "$1" 2>/dev/null || true)" = loaded ]
}

global_user_unit_available() {
    listed=$(systemctl --global list-unit-files --no-legend --no-pager "$1" \
        2>/dev/null | awk 'NR == 1 { print $1 }')
    [ "$listed" = "$1" ]
}

global_user_unit_enabled() {
    [ "$(systemctl --global is-enabled "$1" 2>/dev/null || true)" = enabled ]
}

global_user_unit_path() {
    for unit_directory in \
        /etc/xdg/systemd/user \
        /etc/systemd/user \
        /run/systemd/user \
        /run/current-system/sw/etc/systemd/user \
        /run/current-system/sw/share/systemd/user \
        /run/current-system/sw/lib/systemd/user \
        /usr/local/share/systemd/user \
        /usr/share/systemd/user \
        /usr/local/lib/systemd/user \
        /usr/lib/systemd/user; do
        unit_path=$unit_directory/$1
        if [ ! -e "$unit_path" ] && [ ! -L "$unit_path" ]; then
            continue
        fi
        [ -f "$unit_path" ] || return 1
        unit_resolved=$(readlink -f -- "$unit_path" 2>/dev/null) || return 1
        is_root_protected_chain "$unit_path" \
            && is_root_protected_chain "$unit_resolved" || return 1
        for dropin_directory in \
            /etc/xdg/systemd/user/$1.d \
            /etc/systemd/user/$1.d \
            /run/systemd/user/$1.d \
            /run/current-system/sw/etc/systemd/user/$1.d \
            /run/current-system/sw/share/systemd/user/$1.d \
            /run/current-system/sw/lib/systemd/user/$1.d \
            /usr/local/share/systemd/user/$1.d \
            /usr/share/systemd/user/$1.d \
            /usr/local/lib/systemd/user/$1.d \
            /usr/lib/systemd/user/$1.d; do
            for dropin in "$dropin_directory"/*.conf; do
                [ -e "$dropin" ] || continue
                return 1
            done
        done
        printf '%s\n' "$unit_path"
        return 0
    done
    return 1
}

unit_property() {
    awk -v wanted_section="$2" -v wanted_key="$3" '
        /^[[:space:]]*\[/ {
            section = $0
            gsub(/^[[:space:]]*\[|\][[:space:]]*$/, "", section)
            next
        }
        section == wanted_section && $0 ~ "^[[:space:]]*" wanted_key "[[:space:]]*=" {
            value = substr($0, index($0, "=") + 1)
            sub(/^[[:space:]]*/, "", value)
            sub(/[[:space:]]*$/, "", value)
            if (value == "")
                count = 0
            else
                count++
        }
        END { if (count == 1) print value }
    ' "$1"
}

global_user_service_exec_matches() {
    unit_path=$(global_user_unit_path "$1") || return 1
    service_exec=$(unit_property "$unit_path" Service ExecStart)
    [ -n "$service_exec" ] || return 1
    service_path=${service_exec%% *}
    service_resolved=$(readlink -f -- "$service_path" 2>/dev/null || true)
    expected_resolved=$(readlink -f -- "$2" 2>/dev/null || true)
    is_root_protected_executable "$service_path" \
        && [ "$service_resolved" = "$expected_resolved" ] \
        && [ "$service_exec" = "$service_path $3" ]
}

global_user_socket_matches() {
    unit_path=$(global_user_unit_path "$1") || return 1
    listen_stream=$(unit_property "$unit_path" Socket ListenStream)
    accept=$(unit_property "$unit_path" Socket Accept)
    socket_mode=$(unit_property "$unit_path" Socket SocketMode)
    directory_mode=$(unit_property "$unit_path" Socket DirectoryMode)
    [ -n "$accept" ] || accept=no
    [ "$listen_stream" = "$2" ] \
        && [ "$accept" = no ] \
        && [ "$socket_mode" = "$3" ] \
        && [ "$directory_mode" = "$4" ]
}

systemd_exec_matches() {
    unit=$1
    expected=$2
    expected_arguments=$3
    systemd_unit_loaded "$unit" || return 1
    service_exec=$(systemctl show --property=ExecStart --value "$unit" 2>/dev/null || true)
    service_path=$(printf '%s\n' "$service_exec" \
        | sed -n 's/.*[ {]path=\([^ ;}]*\).*/\1/p' | sed -n '1p')
    service_argv=$(printf '%s\n' "$service_exec" \
        | sed -n 's/.*argv\[\]=\([^;]*\) ;.*/\1/p' | sed -n '1p')
    service_resolved=$(readlink -f -- "$service_path" 2>/dev/null || true)
    expected_resolved=$(readlink -f -- "$expected" 2>/dev/null || true)
    [ -n "$service_path" ] \
        && is_root_protected_executable "$service_path" \
        && [ "$service_resolved" = "$expected_resolved" ] \
        && [ "$service_argv" = "$service_path $expected_arguments" ]
}

systemd_socket_matches() {
    systemd_unit_loaded "$1" \
        && [ "$(systemctl show --property=Listen --value "$1" 2>/dev/null || true)" \
            = "$2 (Stream)" ] \
        && [ "$(systemctl show --property=Accept --value "$1" 2>/dev/null || true)" \
            = no ] \
        && [ "$(systemctl show --property=SocketMode --value "$1" 2>/dev/null || true)" \
            = "$3" ] \
        && [ "$(systemctl show --property=DirectoryMode --value "$1" 2>/dev/null || true)" \
            = "$4" ]
}

installation_complete() {
    component_binary=$1
    resolved=$(readlink -f -- "$component_binary" 2>/dev/null) || return 1
    install_prefix=${resolved%/bin/keysharp-desktop}
    [ "$install_prefix" != "$resolved" ] || return 1
    command -v systemctl >/dev/null 2>&1 || return 1
    global_user_unit_available keysharp-desktop.service || return 1
    global_user_unit_available keysharp-desktop.socket || return 1
    global_user_unit_enabled keysharp-desktop.socket || return 1
    global_user_service_exec_matches keysharp-desktop.service "$resolved" serve \
        || return 1
    global_user_socket_matches keysharp-desktop.socket \
        %t/keysharp-desktop/keysharp-desktop.sock 0600 0700 || return 1

    case "$install_prefix" in
        /usr/local) policy=/usr/share/polkit-1/actions/org.keysharp.desktop.policy ;;
        /nix/store/*) policy=/run/current-system/sw/share/polkit-1/actions/org.keysharp.desktop.policy ;;
        *) policy="$install_prefix/share/polkit-1/actions/org.keysharp.desktop.policy" ;;
    esac
    resource_configuration_matches \
        "$install_prefix" "$resolved" "$policy" "$component_binary" \
        && installed_resources_are_protected "$install_prefix" "$policy" \
        || return 1

    systemd_exec_matches keysharp-desktop-authority.service "$resolved" authority \
        && systemd_socket_matches keysharp-desktop-authority.socket \
            /run/keysharp-desktop/authority.sock 0666 0755
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

while [ "$#" -gt 0 ]; do
    case "$1" in
        --skip-if-compatible) skip_compatible=true ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' "keysharp-desktop: run this installer as root" >&2
    exit 1
fi

if [ "$skip_compatible" = true ]; then
    for candidate in /usr/bin/keysharp-desktop \
        /run/current-system/sw/bin/keysharp-desktop \
        /usr/local/bin/keysharp-desktop; do
        is_root_protected_executable "$candidate" || continue
        installed_info=$("$candidate" version 2>/dev/null || true)
        installed_protocol=$(printf '%s\n' "$installed_info" \
            | sed -n 's/^protocol_name=//p' | sed -n '1p')
        installed_major=$(printf '%s\n' "$installed_info" \
            | sed -n 's/^protocol_major=//p' | sed -n '1p')
        installed_minor=$(printf '%s\n' "$installed_info" \
            | sed -n 's/^protocol_minor=//p' | sed -n '1p')
        case "$installed_minor" in
            ''|*[!0-9]*) installed_minor=0 ;;
        esac
        if protocol_version_compatible "$installed_protocol" \
            "$installed_major" "$installed_minor" \
            && installation_complete "$candidate"; then
            printf '%s\n' \
                "A compatible keysharp-desktop is already installed; leaving it unchanged."
            refresh_or_handoff_user_socket false
            exit 0
        fi
    done
fi

if layered_install_present; then
    printf '%s\n' \
        "A package-managed or differently rooted keysharp-desktop installation is present." \
        "Refusing to layer a portable install over it; repair or upgrade that installation through its owner." >&2
    exit 1
fi

archive_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
payload="$archive_root/payload/usr/local"
policy="$archive_root/payload/usr/share/polkit-1/actions/org.keysharp.desktop.policy"
if [ ! -x "$payload/bin/keysharp-desktop" ] \
    || ! resource_configuration_matches \
        "$payload" /usr/local/bin/keysharp-desktop "$policy" \
    || [ ! -x "$archive_root/uninstall.sh" ]; then
    printf '%s\n' "keysharp-desktop: release payload is incomplete" >&2
    exit 1
fi

install -d -m 0755 /usr/local
cp -R "$payload/." /usr/local/
install -D -m 0644 "$policy" \
    /usr/share/polkit-1/actions/org.keysharp.desktop.policy
chown root:root /usr/local/bin/keysharp-desktop
chmod 0755 /usr/local/bin/keysharp-desktop
install -m 0755 "$archive_root/uninstall.sh" \
    /usr/local/share/doc/keysharp-desktop/uninstall.sh

if command -v systemd-tmpfiles >/dev/null 2>&1; then
    systemd-tmpfiles --create \
        /usr/local/lib/tmpfiles.d/keysharp-desktop-permissions.conf
fi
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload
    systemctl enable --now keysharp-desktop-authority.socket
    systemctl try-restart keysharp-desktop-authority.service
    systemctl --global enable keysharp-desktop.socket
fi

printf '%s\n' \
    "Installed keysharp-desktop." \
    "Uninstall with /usr/local/share/doc/keysharp-desktop/uninstall.sh."
refresh_or_handoff_user_socket true
