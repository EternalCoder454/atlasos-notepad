#!/bin/bash
# Runs every Nth test function of a QtTest binary, so ctest -j can run one
# slow suite as several processes: shard.sh <count> <index> <binary> [prefix...]
# Each shard is its own process, with its own temp XDG dirs and bus.
set -euo pipefail
count=$1 index=$2 exe=$3
shift 3
mapfile -t all < <("$exe" -functions | sed -n 's/^\([A-Za-z0-9_]*\)()$/\1/p')
picked=()
for i in "${!all[@]}"; do
    (( i % count == index )) && picked+=("${all[i]}")
done
(( ${#picked[@]} )) || { echo "shard $index/$count: no tests" >&2; exit 0; }
exec "$@" "$exe" "${picked[@]}"
