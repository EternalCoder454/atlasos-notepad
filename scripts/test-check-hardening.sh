#!/bin/bash
# Tests scripts/check-hardening.sh against small programs built with and without
# the flags it looks for: it must pass the hardened one and name the missing
# hardening in each of the others. Needs gcc (the package's build dependencies
# have it). Run from anywhere: scripts/test-check-hardening.sh
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
check=$here/check-hardening.sh
command -v gcc >/dev/null || { echo "test-check-hardening: gcc is needed" >&2; exit 2; }

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cat >"$dir/hello.c" <<'C'
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    char buf[64];
    strncpy(buf, argc > 1 ? argv[1] : "hello", sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    puts(buf);
    return 0;
}
C
# A function only the tests' build may have (Remote::setForceKio, mangled).
cat >"$dir/hook.c" <<'C'
#include <stdio.h>
int _ZN6Remote12setForceKioEb(int on) { return on; }
int main(int argc, char **argv) {
    (void)argv;
    printf("%d\n", _ZN6Remote12setForceKioEb(argc));
    return 0;
}
C

# shellcheck disable=SC2054 # the commas are the linker flags' own
good=(-O2 -fPIE -pie -fstack-protector-strong -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
failures=0

# build <name> <source> <flags...>
build() {
    local name=$1 src=$2
    shift 2
    gcc "$@" -o "$dir/$name" "$dir/$src"
}

# expect_pass <name> [check options]
expect_pass() {
    local name=$1
    shift
    if "$check" "$@" "$dir/$name" >"$dir/out" 2>&1; then
        echo "ok   $name passes"
    else
        echo "FAIL $name should pass:" >&2
        cat "$dir/out" >&2
        failures=$((failures + 1))
    fi
}

# expect_fail <name> <text the message must have> [check options]
expect_fail() {
    local name=$1 text=$2
    shift 2
    if "$check" "$@" "$dir/$name" >"$dir/out" 2>&1; then
        echo "FAIL $name should fail ($text)" >&2
        failures=$((failures + 1))
    elif grep -q -- "$text" "$dir/out"; then
        echo "ok   $name fails: $text"
    else
        echo "FAIL $name failed, but not for '$text':" >&2
        cat "$dir/out" >&2
        failures=$((failures + 1))
    fi
}

build good hello.c "${good[@]}"
expect_pass good
expect_pass good --cxx

build nopie hello.c -O2 -fno-pie -no-pie -fstack-protector-strong -Wl,-z,relro,-z,now
expect_fail nopie "not a position-independent executable"

build lazy hello.c -O2 -fPIE -pie -fstack-protector-strong -Wl,-z,relro,-z,lazy
expect_fail lazy "no BIND_NOW"

build partial hello.c -O2 -fPIE -pie -fstack-protector-strong -Wl,-z,norelro
expect_fail partial "GNU_RELRO"

build execstack hello.c -O2 -fPIE -pie -fstack-protector-strong -Wl,-z,relro,-z,now -Wl,-z,execstack
expect_fail execstack "executable stack"

build rpath hello.c -O2 -fPIE -pie -fstack-protector-strong -Wl,-z,relro,-z,now -Wl,-rpath,/opt/lib -Wl,--disable-new-dtags
expect_fail rpath "RPATH or RUNPATH"

build runpath hello.c -O2 -fPIE -pie -fstack-protector-strong -Wl,-z,relro,-z,now -Wl,-rpath,/opt/lib -Wl,--enable-new-dtags
expect_fail runpath "RPATH or RUNPATH"

# Only the C++ program is asked for stack protectors and the absence of the hook.
build nocanary hello.c -O2 -fPIE -pie -fno-stack-protector -D_FORTIFY_SOURCE=0 -Wl,-z,relro,-z,now
expect_pass nocanary
expect_fail nocanary "no stack protector" --cxx

build hook hook.c "${good[@]}"
expect_pass hook
expect_fail hook "KIO hook" --cxx

# Stripped, the hook cannot be seen: said, not silently passed.
cat >"$dir/hook2.c" <<'C'
#include <stdio.h>
#include <string.h>
int _ZN6Remote12setForceKioEb(int on) { return on; }
int main(int argc, char **argv) {
    char buf[64];
    strncpy(buf, argc > 1 ? argv[1] : "hello", sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    puts(buf);
    return _ZN6Remote12setForceKioEb(0);
}
C
build hookstripped hook2.c "${good[@]}"
expect_fail hookstripped "KIO hook" --cxx
strip -s "$dir/hookstripped"
expect_pass hookstripped --cxx
expect_fail hookstripped "--require-symbols" --cxx --require-symbols
expect_pass good --cxx --require-symbols
out=$("$check" --cxx "$dir/hookstripped" 2>&1 || true)
if ! grep -q "is stripped" <<<"$out"; then
    echo "FAIL hookstripped: no note that the hook cannot be told" >&2
    failures=$((failures + 1))
fi

# Not an ELF file, or not there at all.
printf 'not a program\n' >"$dir/text"
expect_fail text "not an ELF file"
expect_fail missing "not a file"

# CET is a note unless it is asked for.
case "$(uname -m)" in
    x86_64)
        build nocet hello.c -O2 -fPIE -pie -fstack-protector-strong -fcf-protection=none -Wl,-z,relro,-z,now
        expect_pass nocet
        expect_fail nocet "Intel CET" --require-cet
        build cet hello.c "${good[@]}" -fcf-protection=full
        expect_pass cet --require-cet
        ;;
esac

if [ "$failures" -ne 0 ]; then
    echo "test-check-hardening: $failures failure(s)" >&2
    exit 1
fi
echo "test-check-hardening: all ok"
