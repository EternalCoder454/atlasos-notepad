#!/bin/bash
# Guard against a redirected atlas-framework, run from the repo root:
#   ci/check-cargo.sh <git url> <rev> [Cargo.lock]
# Fails unless every atlas-framework-* package in Cargo.lock comes from that
# URL at that rev (a package with no source, i.e. a path or patched one, fails
# too), and there is at least one, and no other package's source mentions
# atlas-framework. Also fails on [patch] or [replace] in any Cargo.toml.
set -euo pipefail

main() {
    url=${1:?usage: check-cargo.sh <git url> <rev> [Cargo.lock]}
    rev=${2:?usage: check-cargo.sh <git url> <rev> [Cargo.lock]}
    lock=${3:-Cargo.lock}
    want="source = \"git+$url?rev=$rev#$rev\""

    awk -v want="$want" '
        function flush() {
            if (name ~ /^atlas-framework-/) {
                n++
                if (src != want) {
                    printf "package %s: %s\n", name, (src == "" ? "no source" : src)
                    bad = 1
                }
            } else if (src ~ /atlas-framework/) {
                printf "package %s: %s\n", name, src
                bad = 1
            }
            name = ""; src = ""
        }
        /^\[\[package\]\]/ { flush(); next }
        /^name = / { name = $0; sub(/^name = "/, "", name); sub(/"$/, "", name) }
        /^source = / { src = $0 }
        END { flush(); if (n == 0) { print "no atlas-framework-* package"; bad = 1 } exit bad }
    ' "$lock" || {
        echo "$lock must take every atlas-framework-* package from $url at rev $rev, and nothing else from it (above)" >&2
        exit 1
    }

    # Nothing may redirect the pinned source either.
    if grep -rnE --include=Cargo.toml --exclude-dir=target --exclude-dir=build \
        "^[[:space:]]*(\\[[[:space:]]*[\"']?(patch|replace)[\"']?[[:space:]]*[].]|[\"']?(patch|replace)[\"']?[[:space:]]*[.=])" .; then
        echo "patch and replace are not allowed in any Cargo.toml" >&2
        exit 1
    fi
}

main "$@"
exit $?
