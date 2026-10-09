#!/bin/bash
# What the session bus sees of Notepad, with the real binaries, offscreen, in
# a home folder of its own. Run under a session bus (ctest does):
#   bus-surface.sh <telamon-notepad> <legacy_peer>
#   1. Telamon Notepad takes its name and serves org.freedesktop.Application at
#      its own path; the Qt application object (/MainApplication: quit(),
#      closeAllWindows(), setStyleSheet()) is not exported, and quit() is not
#      answered.
#   2. With an atlas-notepad running (legacy_peer: a real KDBusService under
#      net.eterneon.atlas.notepad), a launch is handed to it, and no window of
#      the new Notepad is started.
set -u
APP=$1 PEER=$2
command -v busctl >/dev/null || { echo "bus-surface: no busctl"; exit 77; }
T=$(mktemp -d /var/tmp/np-bus-surface.XXXXXX)
export HOME=$T/home XDG_CONFIG_HOME=$T/home/.config XDG_CACHE_HOME=$T/home/.cache \
    XDG_STATE_HOME=$T/home/.local/state XDG_DATA_HOME=$T/home/.local/share XDG_RUNTIME_DIR=$T/rt
export QT_QPA_PLATFORM=offscreen
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
pids=()
# shellcheck disable=SC2329  # run by the trap below
cleanup() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done
    rm -rf "$T"
}
trap cleanup EXIT
fail=0
wait_name() {
    for _ in $(seq 150); do
        busctl --user status "$1" >/dev/null 2>&1 && return 0
        sleep 0.2
    done
    return 1
}

# 2 first: the old name is taken, so a launch must be handed over and end.
printf 'one\n' >"$T/note.txt"
timeout 60 "$PEER" "$T/peer.out" >"$T/peer.log" 2>&1 &
pids+=($!)
wait_name net.eterneon.atlas.notepad || { echo "FAIL: legacy_peer did not take the old name"; cat "$T/peer.log"; exit 1; }
timeout 60 "$APP" "$T/note.txt" >"$T/forward.log" 2>&1
rc=$?
[ "$rc" = 0 ] || { echo "FAIL: a launch with an atlas-notepad running exited $rc (it should hand over and end)"; fail=1; }
if busctl --user status net.eterneon.telamon.notepad >/dev/null 2>&1; then
    echo "FAIL: the new Notepad started a window of its own instead of handing the launch over"; fail=1
fi
for _ in $(seq 25); do [ -s "$T/peer.out" ] && break; sleep 0.2; done
grep -qxF "$T/note.txt" "$T/peer.out" 2>/dev/null || { echo "FAIL: the old Notepad was not handed the file ($(tr '\n' ' ' <"$T/peer.out" 2>/dev/null))"; fail=1; }
wait "${pids[0]}" 2>/dev/null

# 1
timeout 90 "$APP" "$T/note.txt" >"$T/app.log" 2>&1 &
APID=$!
pids+=("$APID")
NAME=net.eterneon.telamon.notepad
wait_name "$NAME" || { echo "FAIL: Notepad did not take its bus name"; cat "$T/app.log"; exit 1; }
if busctl --user tree "$NAME" | grep -q '/MainApplication'; then
    echo "FAIL: /MainApplication is exported"; fail=1
fi
if busctl --user call "$NAME" /MainApplication org.qtproject.Qt.QCoreApplication quit >/dev/null 2>&1; then
    echo "FAIL: quit() was answered"; fail=1
fi
busctl --user call "$NAME" /net/eterneon/telamon/notepad org.freedesktop.Application Open 'asa{sv}' 1 "file://$T/note.txt" 0 >/dev/null 2>&1 \
    || { echo "FAIL: org.freedesktop.Application.Open does not work"; fail=1; }
# A second launch is handed to the running one.
timeout 30 "$APP" "$T/note.txt" >/dev/null 2>&1 || { echo "FAIL: a second launch did not hand over"; fail=1; }
kill -0 "$APID" 2>/dev/null || { echo "FAIL: Notepad is gone"; fail=1; }
[ "$fail" = 0 ] && echo PASS
exit "$fail"
