#!/bin/bash
# Build the Notepad RPM inside a fedora:44 container, as root.
#   packaging/build-rpm.sh <out dir> [rpmbuild options]
# The binary RPM (no source, no debuginfo) is copied to <out dir>.
# Cargo needs network access.
# ATLAS_LOCAL_RPMS=<dir> installs the RPMs in <dir> first: atlas-framework's
# (atlas-ui), which Notepad builds against and no repository has.
set -euo pipefail

main() {
    out=${1:?usage: build-rpm.sh <out dir> [rpmbuild options]}
    shift

    here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
    src=$(dirname "$here")
    spec=$here/atlas-notepad.spec
    version=$(awk '/^Version:/ {print $2; exit}' "$spec")

    dnf -y install rpm-build dnf5-plugins tar gzip >&2
    if [ -n "${ATLAS_LOCAL_RPMS:-}" ]; then
        # Atlas.Ui and its fonts. dnf brings their dependencies; rpm then puts
        # these exact files in place even when that version is installed.
        local_rpms=()
        for name in atlas-ui atlas-symbols-fonts; do
            found=()
            for f in "$ATLAS_LOCAL_RPMS/$name"-[0-9]*.rpm; do
                [ -e "$f" ] && [[ $f != *.src.rpm ]] && found+=("$f")
            done
            if [ "${#found[@]}" != 1 ]; then
                echo "$ATLAS_LOCAL_RPMS needs exactly one $name RPM, found ${#found[@]}: ${found[*]}" >&2
                exit 1
            fi
            local_rpms+=("${found[0]}")
        done
        dnf -y install "${local_rpms[@]}" >&2
        rpm -U --replacepkgs --oldpackage "${local_rpms[@]}" >&2
    fi
    dnf -y builddep "$spec" >&2

    top=$(mktemp -d)
    trap 'rm -rf "$top"' EXIT
    mkdir -p "$top"/{SOURCES,BUILD,RPMS,SRPMS,SPECS}
    tar -C "$src" \
        --exclude=./.git --exclude=./.claude --exclude=./target --exclude=./out --exclude=./build \
        --transform "s,^\./,atlas-notepad-$version/," \
        -czf "$top/SOURCES/atlas-notepad-$version.tar.gz" .

    rpmbuild -bb "$@" --define "_topdir $top" "$spec"

    mkdir -p "$out"
    find "$top/RPMS" -name '*.rpm' ! -name '*.src.rpm' ! -name '*debuginfo*' ! -name '*debugsource*' \
        -exec cp -v {} "$out"/ \;
}

main "$@"
exit $?
