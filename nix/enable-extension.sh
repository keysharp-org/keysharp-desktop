#!/bin/sh

if [ "$#" -ne 2 ]; then
    exit 2
fi

helper=$1
tools=$2
case "${XDG_CURRENT_DESKTOP:-}" in
    *Cinnamon*|*cinnamon*|*GNOME*|*Gnome*|*gnome*) ;;
    *) exit 0 ;;
esac

attempt=0
while [ "$attempt" -lt 5 ]; do
    result=$("$helper" enable-extension --automatic) || :
    [ -z "$result" ] || printf '%s\n' "$result"
    status=
    while IFS= read -r line; do
        case "$line" in
            status=*) status=${line#status=} ;;
        esac
    done <<EOF
$result
EOF

    case "$status" in
        no_bus|no_shell)
            attempt=$((attempt + 1))
            [ "$attempt" -lt 5 ] || exit 0
            "$tools/sleep" 2
            ;;
        *)
            exit 0
            ;;
    esac
done
