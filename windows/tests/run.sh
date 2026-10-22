#!/bin/bash
# Runs UltraPDF.exe under Wine+Xvfb with a test script. Usage: run.sh script.txt [timeout]
export WINEPREFIX=/tmp/wl/pfx WINEDEBUG=-all DISPLAY=:99 LANG=C.UTF-8 LC_ALL=C.UTF-8
pgrep Xvfb >/dev/null || { Xvfb :99 -screen 0 1920x1080x24 >/dev/null 2>&1 & sleep 1; }
S=$(readlink -f "$1")
W=$(echo "$S" | sed 's|/|\\|g'); W="Z:$W"
mkdir -p /tmp/t
[ -n "$KEEP_STATE" ] || rm -f /tmp/wl/pfx/drive_c/users/*/AppData/Roaming/UltraPDF/state.txt
EXE=$(readlink -f "$(dirname "$0")/../UltraPDF.exe")
ULTRAPDF_TEST="$W" timeout ${2:-60} /tmp/wl/wine-10.0-amd64/bin/wine64 "$EXE" "${@:3}"
echo "exit: $?"
