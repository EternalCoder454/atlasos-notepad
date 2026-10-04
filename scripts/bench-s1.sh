#!/bin/bash
# Spike S1's figures: typing and scrolling latency on bench/sample-50k.md,
# on the software backend at 1x and 1.5x, in Xvfb. Run inside the dev
# container:  scripts/dev.sh scripts/bench-s1.sh [build dir]
set -euo pipefail
bin=${1:-build/s1}/atlas-notepad
run=$(mktemp -d)
export XDG_CONFIG_HOME=$run/config XDG_DATA_HOME=$run/data XDG_CACHE_HOME=$run/cache XDG_STATE_HOME=$run/state
# Qt's xcb backend waits QT_QPA_UPDATE_IDLE_TIME (5 ms) before every frame it
# is asked for; Wayland paces frames by the compositor instead. Without the
# wait, the figures are the work itself.
#
# On a hybrid CPU the scheduler moves the app between fast and efficiency
# cores, and an efficiency core takes three or four times as long: the tail
# figures would measure the scheduler. Run on the fast cores (within 15% of
# the fastest's top clock) so runs compare; BENCH_CPUS (a taskset list)
# overrides, BENCH_CPUS=all doesn't pin.
cpus=${BENCH_CPUS:-}
if [ -z "$cpus" ]; then
    top=$(cat /sys/devices/system/cpu/cpu[0-9]*/cpufreq/cpuinfo_max_freq 2>/dev/null | sort -n | tail -n 1 || true)
    if [ -n "$top" ]; then
        for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/cpuinfo_max_freq; do
            freq=$(cat "$f" 2>/dev/null || echo 0)
            if [ $((${freq:-0} * 100)) -ge $((top * 85)) ]; then
                n=${f#/sys/devices/system/cpu/cpu}
                cpus+=${cpus:+,}${n%%/*}
            fi
        done
    fi
fi
pin=()
if [ -n "$cpus" ] && [ "$cpus" != all ] && command -v taskset >/dev/null; then
    # sysfs lists every CPU, a container may be allowed fewer.
    if taskset -c "$cpus" true 2>/dev/null; then
        pin=(taskset -c "$cpus")
        echo "pinned to CPUs $cpus"
    else
        echo "not pinned: can't run on CPUs $cpus here (BENCH_CPUS sets them)"
    fi
fi
for plain in 0 1; do
    for scale in 1 1.5; do
        echo "== scale $scale$([ $plain = 1 ] && echo ', plain TextEdit (baseline)')"
        dbus-run-session -- xvfb-run -a -s "-screen 0 1920x1080x24" \
            env QT_QPA_PLATFORM=xcb QT_QPA_UPDATE_IDLE_TIME=0 QT_SCALE_FACTOR=$scale NP_BENCH_PLAIN=$plain \
            "${pin[@]}" "$bin" --bench bench/sample-50k.md 2>"$run/stderr" || { grep -v -e portal -e fuse -e dbus-daemon "$run/stderr" | tail -20; exit 1; }
        grep -E 'warning|error|qrc:' "$run/stderr" | grep -v -e portal -e fuse | sort | uniq -c | head -5 || true
    done
done
rm -rf "$run"
