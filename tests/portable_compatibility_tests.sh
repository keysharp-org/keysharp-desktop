#!/bin/sh
set -eu

source_dir=$1
temporary=$(mktemp -d)
upgrade_pid=
cleanup() {
    if [ -n "$upgrade_pid" ]; then
        kill "$upgrade_pid" 2>/dev/null || true
        wait "$upgrade_pid" 2>/dev/null || true
    fi
    rm -rf -- "$temporary"
}
trap cleanup EXIT HUP INT TERM

sed '/^while \[ "$#" -gt 0 \]; do/,$d' \
    "$source_dir/packaging/install-release.sh" > "$temporary/functions.sh"
# shellcheck source=/dev/null
. "$temporary/functions.sh"

resource_root=$temporary/resources
policy=$temporary/org.keysharp.desktop.policy
mkdir -p \
    "$resource_root/lib/systemd/user" \
    "$resource_root/lib/systemd/system" \
    "$resource_root/libexec" \
    "$resource_root/lib/tmpfiles.d" \
    "$resource_root/share/applications" \
    "$resource_root/share/gnome-shell/extensions/keysharp@keysharp.io" \
    "$resource_root/share/cinnamon/extensions/keysharp@keysharp.io"

cp "$source_dir/data/keysharp-desktop.service.in" \
    "$resource_root/lib/systemd/user/keysharp-desktop.service"
cp "$source_dir/data/keysharp-desktop-authority.service.in" \
    "$resource_root/lib/systemd/system/keysharp-desktop-authority.service"
cp "$source_dir/data/keysharp-desktop-authority.socket" \
    "$resource_root/lib/systemd/system/keysharp-desktop-authority.socket"
cp "$source_dir/data/keysharp-desktop-permissions.conf" \
    "$resource_root/lib/tmpfiles.d/keysharp-desktop-permissions.conf"
cp /bin/true \
    "$resource_root/libexec/keysharp-desktop-capture-worker"
chmod 0700 "$resource_root/libexec/keysharp-desktop-capture-worker"
sed 's|@KEYSHARP_DESKTOP_CAPTURE_WORKER_PATH@|/usr/local/libexec/keysharp-desktop-capture-worker|' \
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

sed 's|Exec=/usr/local/libexec/keysharp-desktop-capture-worker|Exec=/tmp/keysharp-desktop-capture-worker|' \
    "$resource_root/share/applications/org.keysharp.DesktopCapture.desktop" \
    > "$temporary/wrong.desktop"
! desktop_entry_configuration_matches \
    "$temporary/wrong.desktop" \
    /usr/local/libexec/keysharp-desktop-capture-worker

gnome_extension=$resource_root/share/gnome-shell/extensions/keysharp@keysharp.io/extension.js
cp "$gnome_extension" "$temporary/gnome-extension.js"
: > "$gnome_extension"
! resource_configuration_matches \
    "$resource_root" /usr/local/bin/keysharp-desktop "$policy"
cp "$temporary/gnome-extension.js" "$gnome_extension"

rm -f "$resource_root/lib/systemd/user/keysharp-desktop.service"
! has_required_resources "$resource_root"
cp "$source_dir/data/keysharp-desktop.service.in" \
    "$resource_root/lib/systemd/user/keysharp-desktop.service"
rm -f "$resource_root/libexec/keysharp-desktop-capture-worker"
! has_required_resources "$resource_root"

protected_file=$(command -v sh)
is_root_protected_file "$protected_file"
ln -s "$protected_file" "$temporary/unprotected-link"
! is_root_protected_file "$temporary/unprotected-link"

live_executable=$temporary/live-executable
cp /bin/sleep "$live_executable"
chmod 0755 "$live_executable"
old_inode=$(stat -c '%i' "$live_executable")
"$live_executable" 30 &
upgrade_pid=$!
atomic_install_file /bin/true "$live_executable" 0755
new_inode=$(stat -c '%i' "$live_executable")
[ "$old_inode" != "$new_inode" ]
kill -0 "$upgrade_pid"
"$live_executable"
kill "$upgrade_pid"
wait "$upgrade_pid" 2>/dev/null || true
upgrade_pid=

mkdir -p "$temporary/live-lib" "$temporary/payload-lib/lib"
printf '%s\n' old > "$temporary/live-lib/libkeysharp-desktop.so.0.2.0"
printf '%s\n' new > "$temporary/payload-lib/lib/libkeysharp-desktop.so.0.2.0"
ln -s libkeysharp-desktop.so.0.2.0 \
    "$temporary/payload-lib/lib/libkeysharp-desktop.so.0"
ln -s libkeysharp-desktop.so.0 \
    "$temporary/payload-lib/lib/libkeysharp-desktop.so"
old_inode=$(stat -c '%i' \
    "$temporary/live-lib/libkeysharp-desktop.so.0.2.0")
atomic_install_file \
    "$temporary/payload-lib/lib/libkeysharp-desktop.so.0.2.0" \
    "$temporary/live-lib/libkeysharp-desktop.so.0.2.0" 0755
new_inode=$(stat -c '%i' \
    "$temporary/live-lib/libkeysharp-desktop.so.0.2.0")
[ "$old_inode" != "$new_inode" ]
[ "$(cat "$temporary/live-lib/libkeysharp-desktop.so.0.2.0")" = new ]
atomic_install_symlink \
    "$temporary/payload-lib/lib/libkeysharp-desktop.so.0" \
    "$temporary/live-lib/libkeysharp-desktop.so.0"
atomic_install_symlink \
    "$temporary/payload-lib/lib/libkeysharp-desktop.so" \
    "$temporary/live-lib/libkeysharp-desktop.so"
[ "$(readlink "$temporary/live-lib/libkeysharp-desktop.so.0")" \
    = libkeysharp-desktop.so.0.2.0 ]
[ "$(readlink "$temporary/live-lib/libkeysharp-desktop.so")" \
    = libkeysharp-desktop.so.0 ]
[ "$(cat "$temporary/live-lib/libkeysharp-desktop.so")" = new ]

printf '%s\n' "portable compatibility checks passed"
