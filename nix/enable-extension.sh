#!/bin/sh

if [ "$#" -ne 2 ]; then
    exit 2
fi

helper=$1
tools=$2
state_home=${XDG_STATE_HOME:-$HOME/.local/state}
marker_dir=$state_home/keysharp-desktop
case "${XDG_CURRENT_DESKTOP:-}" in
    *Cinnamon*|*cinnamon*) desktop=cinnamon ;;
    *GNOME*|*Gnome*|*gnome*) desktop=gnome ;;
    *) desktop=other ;;
esac
marker=$marker_dir/extension-enabled-$desktop

[ -e "$marker" ] && exit 0

attempt=0
while [ "$attempt" -lt 5 ]; do
    result=$("$helper" enable-extension) || :
    [ -z "$result" ] || printf '%s\n' "$result"

    case "$result" in
        status=already_live*|status=enabled*|status=needs_relogin*|status=already_listed*)
            umask 077
            "$tools/mkdir" -p "$marker_dir" || exit 1
            : > "$marker" || exit 1
            exit 0
            ;;
        status=no_bus*|status=no_shell*)
            attempt=$((attempt + 1))
            [ "$attempt" -lt 5 ] || exit 0
            "$tools/sleep" 2
            ;;
        *)
            exit 0
            ;;
    esac
done
