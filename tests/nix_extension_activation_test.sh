#!/bin/sh
set -eu

source_dir=$1
tools=${2:-$(dirname "$(command -v mkdir)")}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

cat > "$scratch/helper" <<'EOF'
#!/bin/sh
[ "$1" = enable-extension ] || exit 2
count=0
[ ! -f "$MOCK_COUNT" ] || count=$(cat "$MOCK_COUNT")
count=$((count + 1))
printf '%s\n' "$count" > "$MOCK_COUNT"
case "$MOCK_SCENARIO:$count" in
    retry:1) echo status=no_shell; exit 1 ;;
    enabled:*|retry:*) echo status=enabled; exit 0 ;;
    not_applicable:*) echo status=not_applicable; exit 0 ;;
    kill_switch:*) echo status=kill_switch; exit 1 ;;
    *) exit 2 ;;
esac
EOF
chmod +x "$scratch/helper"

script=$source_dir/nix/enable-extension.sh
marker=$scratch/state/keysharp-desktop/extension-enabled-gnome
MOCK_SCENARIO=enabled MOCK_COUNT="$scratch/count" XDG_STATE_HOME="$scratch/state" \
    XDG_CURRENT_DESKTOP=GNOME \
    sh "$script" "$scratch/helper" "$tools" >/dev/null
[ -f "$marker" ]
MOCK_SCENARIO=kill_switch MOCK_COUNT="$scratch/count" XDG_STATE_HOME="$scratch/state" \
    XDG_CURRENT_DESKTOP=GNOME \
    sh "$script" "$scratch/helper" "$tools" >/dev/null
[ "$(cat "$scratch/count")" -eq 1 ]
MOCK_SCENARIO=enabled MOCK_COUNT="$scratch/count" XDG_STATE_HOME="$scratch/state" \
    XDG_CURRENT_DESKTOP=X-Cinnamon \
    sh "$script" "$scratch/helper" "$tools" >/dev/null
[ -f "$scratch/state/keysharp-desktop/extension-enabled-cinnamon" ]
[ "$(cat "$scratch/count")" -eq 2 ]

marker=$scratch/other-state/keysharp-desktop/extension-enabled-gnome
MOCK_SCENARIO=not_applicable MOCK_COUNT="$scratch/other-count" \
    XDG_STATE_HOME="$scratch/other-state" XDG_CURRENT_DESKTOP=GNOME \
    sh "$script" "$scratch/helper" "$tools" >/dev/null
[ ! -e "$marker" ]
MOCK_SCENARIO=enabled MOCK_COUNT="$scratch/other-count" \
    XDG_STATE_HOME="$scratch/other-state" XDG_CURRENT_DESKTOP=GNOME \
    sh "$script" "$scratch/helper" "$tools" >/dev/null
[ -f "$marker" ]

marker=$scratch/retry-state/keysharp-desktop/extension-enabled-gnome
MOCK_SCENARIO=retry MOCK_COUNT="$scratch/retry-count" \
    XDG_STATE_HOME="$scratch/retry-state" XDG_CURRENT_DESKTOP=GNOME \
    sh "$script" "$scratch/helper" "$tools" >/dev/null
[ -f "$marker" ]
[ "$(cat "$scratch/retry-count")" -eq 2 ]

marker=$scratch/blocked-state/keysharp-desktop/extension-enabled-gnome
MOCK_SCENARIO=kill_switch MOCK_COUNT="$scratch/blocked-count" \
    XDG_STATE_HOME="$scratch/blocked-state" XDG_CURRENT_DESKTOP=GNOME \
    sh "$script" "$scratch/helper" "$tools" >/dev/null
[ ! -e "$marker" ]
