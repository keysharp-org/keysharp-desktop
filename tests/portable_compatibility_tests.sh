#!/bin/sh
set -eu

source_dir=$1
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

sed '/^while \[ "$#" -gt 0 \]; do/,$d' \
    "$source_dir/packaging/install-release.sh" > "$temporary/functions.sh"
# shellcheck source=/dev/null
. "$temporary/functions.sh"

protocol_version_compatible keysharp-desktop/session-v1 1 2
! protocol_version_compatible keysharp-desktop/session-v1 1 1
! protocol_version_compatible keysharp-desktop/session-v1 1 3
! protocol_version_compatible keysharp-desktop/session-v2 1 2

resource_root=$temporary/resources
policy=$temporary/org.keysharp.desktop.policy
mkdir -p \
    "$resource_root/lib/systemd/user" \
    "$resource_root/lib/systemd/system" \
    "$resource_root/lib/tmpfiles.d" \
    "$resource_root/share/applications" \
    "$resource_root/share/gnome-shell/extensions/keysharp@keysharp.io" \
    "$resource_root/share/cinnamon/extensions/keysharp@keysharp.io"

cp "$source_dir/data/keysharp-desktop.service.in" \
    "$resource_root/lib/systemd/user/keysharp-desktop.service"
cp "$source_dir/data/keysharp-desktop.socket" \
    "$resource_root/lib/systemd/user/keysharp-desktop.socket"
cp "$source_dir/data/keysharp-desktop-authority.service.in" \
    "$resource_root/lib/systemd/system/keysharp-desktop-authority.service"
cp "$source_dir/data/keysharp-desktop-authority.socket" \
    "$resource_root/lib/systemd/system/keysharp-desktop-authority.socket"
cp "$source_dir/data/keysharp-desktop-permissions.conf" \
    "$resource_root/lib/tmpfiles.d/keysharp-desktop-permissions.conf"
sed 's|@CMAKE_INSTALL_FULL_BINDIR@|/usr/local/bin|' \
    "$source_dir/data/org.keysharp.DesktopCapture.desktop.in" \
    > "$resource_root/share/applications/org.keysharp.DesktopCapture.desktop"
cp "$source_dir/providers/gnome/extension.js" \
    "$resource_root/share/gnome-shell/extensions/keysharp@keysharp.io/extension.js"
cp "$source_dir/providers/gnome/metadata.json" \
    "$resource_root/share/gnome-shell/extensions/keysharp@keysharp.io/metadata.json"
cp "$source_dir/providers/cinnamon/extension.js" \
    "$resource_root/share/cinnamon/extensions/keysharp@keysharp.io/extension.js"
cp "$source_dir/providers/cinnamon/metadata.json" \
    "$resource_root/share/cinnamon/extensions/keysharp@keysharp.io/metadata.json"
cp "$source_dir/data/org.keysharp.desktop.policy" "$policy"

resource_configuration_matches \
    "$resource_root" /usr/local/bin/keysharp-desktop "$policy"

: > "$policy"
! resource_configuration_matches \
    "$resource_root" /usr/local/bin/keysharp-desktop "$policy"
cp "$source_dir/data/org.keysharp.desktop.policy" "$policy"
sed 's/<allow_active>auth_self<\/allow_active>/<allow_active>yes<\/allow_active>/' \
    "$policy" > "$temporary/insecure.policy"
! policy_configuration_matches "$temporary/insecure.policy"

printf '%s\n' 'r /var/lib/keysharp-permissions - - - - -' \
    >> "$resource_root/lib/tmpfiles.d/keysharp-desktop-permissions.conf"
! resource_configuration_matches \
    "$resource_root" /usr/local/bin/keysharp-desktop "$policy"
cp "$source_dir/data/keysharp-desktop-permissions.conf" \
    "$resource_root/lib/tmpfiles.d/keysharp-desktop-permissions.conf"

sed 's|Exec=/usr/local/bin/keysharp-desktop serve|Exec=/tmp/keysharp-desktop serve|' \
    "$resource_root/share/applications/org.keysharp.DesktopCapture.desktop" \
    > "$temporary/wrong.desktop"
! desktop_entry_configuration_matches \
    "$temporary/wrong.desktop" /usr/local/bin/keysharp-desktop

gnome_extension=$resource_root/share/gnome-shell/extensions/keysharp@keysharp.io/extension.js
cp "$gnome_extension" "$temporary/gnome-extension.js"
: > "$gnome_extension"
! resource_configuration_matches \
    "$resource_root" /usr/local/bin/keysharp-desktop "$policy"
cp "$temporary/gnome-extension.js" "$gnome_extension"

rm -f "$resource_root/lib/systemd/user/keysharp-desktop.socket"
! has_required_resources "$resource_root"

protected_file=$(command -v sh)
is_root_protected_file "$protected_file"
ln -s "$protected_file" "$temporary/unprotected-link"
! is_root_protected_file "$temporary/unprotected-link"

printf '%s\n' "portable compatibility checks passed"
