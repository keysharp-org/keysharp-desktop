#!/bin/sh
set -eu

# Parses both provider extensions. gjs is preferred because it is SpiderMonkey, the engine
# that actually runs them, so it accepts the GJS dialect exactly; node is the fallback and is
# what CI uses. Reflect.parse is a SpiderMonkey extension that parses without executing, which
# matters because these files touch global and imports.ui.main at load and cannot be run
# outside a compositor. Neither engine present is a skip, not a failure: the build must not
# require a JavaScript runtime.
#
# The two providers are not in the same dialect and must not be parsed as though they were.
# The GNOME extension is a real ES module, because GNOME 45 moved extensions to ESM
# ("import GLib from 'gi://GLib'"), while the Cinnamon one is a plain script using the older
# "imports.gi" form. Parsing a script as a module hides nothing, but parsing a module as a
# script fails outright, and parsing a script as a module accepts top-level await and
# import.meta that the real engine would reject. The mode is therefore detected per file.

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
for provider in gnome cinnamon; do
    file="$source_dir/providers/$provider/extension.js"

    if grep -qE '^[[:space:]]*(import|export)[[:space:]]' "$file"; then
        mode=module
    else
        mode=script
    fi

    # The engine status is read on its own. Piping it through a filter would report the
    # filter's status instead, and grep answers 1 when it matches nothing, so a clean parse
    # would be read as a failure.
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
