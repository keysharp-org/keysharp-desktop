#!/bin/sh
set -eu

# Parse without executing compositor-specific imports or globals. Prefer GJS's
# SpiderMonkey parser; Node provides a fallback when GJS is unavailable.

source_dir=$1

if command -v gjs >/dev/null 2>&1; then
    engine=gjs
elif command -v node >/dev/null 2>&1; then
    engine=node
else
    echo "no gjs and no node: skipping provider syntax check"
    exit 77
fi

temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

status=0
for provider in gnome cinnamon kwin; do
    if [ "$provider" = kwin ]; then
        file="$source_dir/providers/kwin/contents/code/main.js"
    else
        file="$source_dir/providers/$provider/extension.js"
    fi

    # Script mode must reject module-only constructs that Cinnamon and KWin
    # cannot load; GNOME's extension uses ES modules.
    if grep -qE '^[[:space:]]*(import|export)[[:space:]]' "$file"; then
        mode=module
    else
        mode=script
    fi

    if [ "$engine" = gjs ]; then
        gjs -c '
const GLib = imports.gi.GLib;
const [ok, bytes] = GLib.file_get_contents(ARGV[0]);
if (!ok)
    throw new Error("cannot read " + ARGV[0]);
const source = imports.byteArray.toString(bytes);
Reflect.parse(source, ARGV[1] === "module" ? {target: "module"} : {});
' "$file" "$mode" >"$temporary/out" 2>&1 && parsed=0 || parsed=1
    elif [ "$mode" = module ]; then
        node --input-type=module --check <"$file" >"$temporary/out" 2>&1 && parsed=0 || parsed=1
    else
        node --input-type=commonjs --check <"$file" >"$temporary/out" 2>&1 && parsed=0 || parsed=1
    fi

    if [ "$parsed" -eq 0 ]; then
        echo "  $provider ok ($engine, $mode)"
    else
        echo "  $provider FAILED to parse ($engine, $mode)" >&2
        grep -v 'Gjs-CRITICAL' "$temporary/out" >&2 || true
        status=1
    fi
done

exit "$status"
