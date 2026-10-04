#!/bin/bash
# Spike S2's figures: open, memory, typing and scrolling for large files
# (out/s2, written by bench/make-large.py), Markdown and plain, at 1x, in Xvfb.
# Run inside the dev container:  scripts/dev.sh scripts/bench-s2.sh [build dir] [files...]
set -uo pipefail
bin=${1:-build/s1}/atlas-notepad
shift || true
files=("${@:-out/s2/md-1m.md out/s2/md-10m.md out/s2/line-5m.txt}")
run=$(mktemp -d)
export XDG_CONFIG_HOME=$run/config XDG_DATA_HOME=$run/data XDG_CACHE_HOME=$run/cache XDG_STATE_HOME=$run/state
for f in ${files[@]}; do
    for plain in 0 1; do
        echo "== $f$([ $plain = 1 ] && echo ', plain TextEdit')"
        timeout 600 dbus-run-session -- xvfb-run -a -s "-screen 0 1920x1080x24" \
            env QT_QPA_PLATFORM=xcb QT_QPA_UPDATE_IDLE_TIME=0 NP_BENCH_PLAIN=$plain \
            "$bin" --bench "$f" 2>"$run/stderr" | grep -v '^file:'
        [ "${PIPESTATUS[0]}" = 124 ] && echo "timed out after 600 s"
    done
done
rm -rf "$run"
