#!/bin/bash
# Install everything CI builds with, in a fedora:44 container, as root:
#   ci/install-deps.sh <dir with the atlas-ui and atlas-symbols-fonts RPMs>
# Used by ci/Containerfile (the prebuilt CI image) and, when no image is
# available yet, directly by the workflow's jobs. The package list mirrors
# scripts/dev.sh, minus the GUI test tools CI doesn't use (Xvfb, ImageMagick,
# xdotool), plus what the workflow runs: ccache, qmllint (qt6-qtdeclarative-devel),
# rpmlint and git/tar/zstd for the actions.
# The RPMs are also copied to /opt/atlas-rpms.
set -euo pipefail

main() {
    rpms=${1:?usage: install-deps.sh <rpm dir>}
    here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
    spec=$here/../packaging/atlas-notepad.spec
    [ -f "$spec" ] || { echo "install-deps.sh: no $spec" >&2; exit 1; }

    # Exactly one of each, as packaging/build-rpm.sh requires.
    files=()
    for name in atlas-ui atlas-symbols-fonts; do
        found=()
        for f in "$rpms/$name"-[0-9]*.rpm; do
            [ -e "$f" ] && [[ $f != *.src.rpm ]] && found+=("$f")
        done
        if [ "${#found[@]}" != 1 ]; then
            echo "install-deps.sh: $rpms needs exactly one $name RPM, found ${#found[@]}: ${found[*]}" >&2
            exit 1
        fi
        files+=("${found[0]}")
    done

    dnf -y install --setopt=install_weak_deps=False \
        dnf5-plugins rpm-build clippy rustfmt dbus-daemon \
        qt6-qtbase-gui kf6-qqc2-desktop-style breeze-icon-theme default-fonts-core-sans \
        ccache qt6-qtdeclarative-devel rpmlint git tar zstd
    # dnf brings their dependencies; rpm then puts these exact files in place,
    # also when this version or a newer one is installed.
    dnf -y install "${files[@]}"
    rpm -U --replacepkgs --oldpackage "${files[@]}"
    dnf -y builddep "$spec"

    install -d /opt/atlas-rpms
    cp -f "${files[@]}" /opt/atlas-rpms/
}

main "$@"
exit $?
