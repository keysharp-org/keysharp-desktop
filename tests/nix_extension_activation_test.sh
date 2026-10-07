#!/bin/sh
set -eu

source_dir=$1
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
mkdir "$scratch/tools"

cat > "$scratch/tools/sleep" <<'EOF'
#!/bin/sh
[ "$#" -eq 1 ] && [ "$1" = 2 ] || exit 2
echo sleep >> "$MOCK_SLEEPS"
EOF
chmod +x "$scratch/tools/sleep"

cat > "$scratch/helper" <<'EOF'
#!/bin/sh
[ "$1" = enable-extension ] || exit 2
[ "$#" -eq 2 ] && [ "$2" = --automatic ] || exit 2
count=0
[ ! -f "$MOCK_COUNT" ] || count=$(cat "$MOCK_COUNT")
count=$((count + 1))
printf '%s\n' "$count" > "$MOCK_COUNT"
echo installed_path=/usr/share/gnome-shell/extensions/keysharp@keysharp.io
marker=$MOCK_STATE_HOME/keysharp-desktop/extension-enabled-$MOCK_DESKTOP
if [ -e "$marker" ]; then
    echo status=not_applicable
    exit 0
fi
case "$MOCK_SCENARIO:$count" in
    retry:1) echo status=no_shell; exit 1 ;;
    retry:2) echo status=no_bus; exit 1 ;;
    unready:*) echo status=no_shell; exit 1 ;;
    enabled:*|retry:*)
        umask 077
        mkdir -p "$MOCK_STATE_HOME/keysharp-desktop" || exit 1
        : > "$marker" || exit 1
        echo status=enabled
        exit 0
        ;;
    not_applicable:*|explicitly_disabled:*) echo status=not_applicable; exit 0 ;;
    kill_switch:*) echo status=kill_switch; exit 1 ;;
    *) exit 2 ;;
esac
EOF
chmod +x "$scratch/helper"

script=$source_dir/nix/enable-extension.sh
run_helper()
{
    MOCK_SCENARIO=$1 MOCK_COUNT=$2 MOCK_STATE_HOME=$3 MOCK_DESKTOP=$5 \
        MOCK_SLEEPS="$scratch/sleeps" XDG_STATE_HOME="$scratch/inherited-state" \
        XDG_CURRENT_DESKTOP=$4 \
        sh "$script" "$scratch/helper" "$scratch/tools" >/dev/null
}

run_helper enabled "$scratch/count" "$scratch/state" GNOME gnome
[ -f "$scratch/state/keysharp-desktop/extension-enabled-gnome" ]
[ ! -e "$scratch/inherited-state/keysharp-desktop/extension-enabled-gnome" ]
run_helper kill_switch "$scratch/count" "$scratch/state" GNOME gnome
[ "$(cat "$scratch/count")" -eq 2 ]
run_helper enabled "$scratch/count" "$scratch/state" X-Cinnamon cinnamon
[ -f "$scratch/state/keysharp-desktop/extension-enabled-cinnamon" ]
[ ! -e "$scratch/inherited-state/keysharp-desktop/extension-enabled-cinnamon" ]
[ "$(cat "$scratch/count")" -eq 3 ]

mkdir -p "$scratch/inherited-state/keysharp-desktop"
: > "$scratch/inherited-state/keysharp-desktop/extension-enabled-gnome"
run_helper enabled "$scratch/later-count" "$scratch/later-state" GNOME gnome
[ -f "$scratch/later-state/keysharp-desktop/extension-enabled-gnome" ]
[ "$(cat "$scratch/later-count")" -eq 1 ]

for scenario in not_applicable explicitly_disabled kill_switch; do
    run_helper "$scenario" "$scratch/$scenario-count" \
        "$scratch/$scenario-state" GNOME gnome
    [ ! -e "$scratch/$scenario-state/keysharp-desktop/extension-enabled-gnome" ]
    [ "$(cat "$scratch/$scenario-count")" -eq 1 ]
done

run_helper retry "$scratch/retry-count" "$scratch/retry-state" GNOME gnome
[ -f "$scratch/retry-state/keysharp-desktop/extension-enabled-gnome" ]
[ "$(cat "$scratch/retry-count")" -eq 3 ]
[ "$(wc -l < "$scratch/sleeps")" -eq 2 ]
run_helper unready "$scratch/unready-count" "$scratch/unready-state" GNOME gnome
[ ! -e "$scratch/unready-state/keysharp-desktop/extension-enabled-gnome" ]
[ "$(cat "$scratch/unready-count")" -eq 5 ]
[ "$(wc -l < "$scratch/sleeps")" -eq 6 ]
run_helper enabled "$scratch/kde-count" "$scratch/kde-state" KDE kwin
[ ! -e "$scratch/kde-count" ]
[ ! -e "$scratch/kde-state" ]
