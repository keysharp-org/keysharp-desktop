#!/bin/sh
set -eu

source_dir=${1:?source directory is required}
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

helper=$source_dir/packaging/configure-library-path.sh
configuration=$temporary/ld.so.conf.d/keysharp-desktop.conf
mkdir "$temporary/bin"
cat > "$temporary/bin/ldconfig" <<'SH'
#!/bin/sh
echo refreshed >> "$KSD_LDCONFIG_LOG"
exit "${KSD_LDCONFIG_RESULT:-0}"
SH
chmod 0755 "$temporary/bin/ldconfig"
PATH=$temporary/bin:$PATH
KSD_LDCONFIG_LOG=$temporary/cache-refreshes
export PATH KSD_LDCONFIG_LOG

sh "$helper" /usr/local/lib "$configuration"
[ "$(wc -l < "$KSD_LDCONFIG_LOG")" -eq 1 ]
printf '%s\n' /usr/local/lib > "$temporary/expected"
cmp "$temporary/expected" "$configuration"
[ "$(stat -c '%a' "$configuration")" = 644 ]
original_inode=$(stat -c '%i' "$configuration")
sh "$helper" /usr/local/lib "$configuration"
[ "$(stat -c '%i' "$configuration")" = "$original_inode" ]
[ "$(wc -l < "$KSD_LDCONFIG_LOG")" -eq 2 ]

custom_library_dir=/opt/keysharp-desktop/lib64
sh "$helper" "$custom_library_dir" "$temporary/custom.conf"
printf '%s\n' "$custom_library_dir" > "$temporary/expected-custom"
cmp "$temporary/expected-custom" "$temporary/custom.conf"

printf '%s\n' /usr/local/lib /opt/other/lib > "$configuration"
cp "$configuration" "$temporary/modified"
if sh "$helper" /usr/local/lib "$configuration" 2> "$temporary/error"; then
    echo "modified linker configuration was overwritten" >&2
    exit 1
fi
cmp "$temporary/modified" "$configuration"

ln -s "$temporary/expected" "$temporary/symlink.conf"
if sh "$helper" /usr/local/lib "$temporary/symlink.conf" 2> "$temporary/error"; then
    echo "symlink linker configuration was accepted" >&2
    exit 1
fi
[ -L "$temporary/symlink.conf" ]
printf '%s\n' /usr/local/lib | cmp - "$temporary/expected"

ln -s "$temporary/missing" "$temporary/dangling.conf"
if sh "$helper" /usr/local/lib "$temporary/dangling.conf" 2> "$temporary/error"; then
    echo "dangling linker configuration was replaced" >&2
    exit 1
fi
[ -L "$temporary/dangling.conf" ]
[ ! -e "$temporary/missing" ]

mkdir "$temporary/directory.conf"
if sh "$helper" /usr/local/lib "$temporary/directory.conf" 2> "$temporary/error"; then
    echo "directory linker configuration was accepted" >&2
    exit 1
fi
[ -d "$temporary/directory.conf" ]
[ "$(wc -l < "$KSD_LDCONFIG_LOG")" -eq 3 ]

if KSD_LDCONFIG_RESULT=7 sh "$helper" /usr/local/lib "$temporary/failing.conf"; then
    echo "linker cache failure was ignored" >&2
    exit 1
else
    [ "$?" -eq 7 ]
fi
[ "$(wc -l < "$KSD_LDCONFIG_LOG")" -eq 4 ]

# Exercise the uninstaller's cleanup without touching the host configuration.
sed -n '/^configuration=\/etc\/ld.so.conf.d\/keysharp-desktop.conf$/,/^fi$/p' \
    "$source_dir/uninstall.sh" | sed '1d' > "$temporary/remove-configuration.sh"
[ -s "$temporary/remove-configuration.sh" ]
configuration=$configuration sh "$temporary/remove-configuration.sh"
cmp "$temporary/modified" "$configuration"
configuration=$temporary/custom.conf sh "$temporary/remove-configuration.sh"
cmp "$temporary/expected-custom" "$temporary/custom.conf"
configuration=$temporary/symlink.conf sh "$temporary/remove-configuration.sh"
[ -L "$temporary/symlink.conf" ]
configuration=$temporary/dangling.conf sh "$temporary/remove-configuration.sh"
[ -L "$temporary/dangling.conf" ]
configuration=$temporary/directory.conf sh "$temporary/remove-configuration.sh"
[ -d "$temporary/directory.conf" ]
printf '%s\n' /usr/local/lib > "$configuration"
configuration=$configuration sh "$temporary/remove-configuration.sh"
[ ! -e "$configuration" ]

echo "keysharp-desktop library path configuration checks passed"
