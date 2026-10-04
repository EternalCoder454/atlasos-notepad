#!/bin/bash
# Run a command in the fedora:44 build container, with the repo at /src and
# the cargo and dnf caches in named podman volumes (shared with the other Atlas apps).
#   scripts/dev.sh <command...>     e.g. scripts/dev.sh cargo test --workspace
#   scripts/dev.sh                  an interactive shell
# The first run installs the build dependencies from the spec (cached after).
# Set CARGO_TARGET_DIR to /src/target/<name> to keep one target dir per task.
# Atlas.Ui comes installed (atlas-ui, from atlas-framework), which no
# repository has: the first run needs ATLAS_LOCAL_RPMS=<dir with its RPMs>
# (built with atlas-framework's packaging/build-rpm.sh). Given later, the
# image takes those RPMs when they are another build than the one it has.
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
image=localhost/atlas-notepad-dev:44
spec_sum=$(sha256sum "$repo/packaging/atlas-notepad.spec" | cut -d' ' -f1)

# The atlas-ui and atlas-symbols-fonts RPMs in $ATLAS_LOCAL_RPMS: exactly one
# of each, or nothing is changed.
local_rpms() {
    local dir=$1 name f
    local -a found
    for name in atlas-ui atlas-symbols-fonts; do
        found=()
        for f in "$dir/$name"-[0-9]*.rpm; do
            [ -e "$f" ] && [[ $f != *.src.rpm ]] && found+=("${f##*/}")
        done
        if [ "${#found[@]}" != 1 ]; then
            echo "dev.sh: $dir needs exactly one $name RPM, found ${#found[@]}: ${found[*]}" >&2
            return 1
        fi
        printf '%s\n' "${found[0]}"
    done
}

new=1
podman image exists "$image" && new=0
if [ "$new" = 1 ] && [ -z "${ATLAS_LOCAL_RPMS:-}" ]; then
    echo "dev.sh: the first run needs ATLAS_LOCAL_RPMS=<dir with the atlas-ui and atlas-symbols-fonts RPMs>" >&2
    exit 1
fi
if [ -n "${ATLAS_LOCAL_RPMS:-}" ]; then
    rpms=$(cd "$ATLAS_LOCAL_RPMS" && pwd)
    mapfile -t files < <(local_rpms "$rpms")
    [ "${#files[@]}" = 2 ] || exit 1
    # Which build: the label the image was committed with.
    build=$(cd "$rpms" && rpm -qp --qf '%{NEVRA}-%{BUILDTIME},' "${files[@]}")
    have=$([ "$new" = 1 ] || podman image inspect --format '{{index .Labels "atlas-ui"}}' "$image")
    if [ "$build" != "$have" ]; then
        # From the clean base each time (the dnf cache makes it quick), so
        # refreshes don't stack layers on the old image.
        # :z, as for /src below. The RPMs are copied in, not mounted: a :z
        # mount would relabel all of $ATLAS_LOCAL_RPMS (say, ~/Downloads).
        ctr=$(podman run -d -v "$repo/packaging":/packaging:ro,z \
            -v atlas-dnf:/var/cache/libdnf5 \
            registry.fedoraproject.org/fedora:44 sleep infinity)
        trap 'podman rm -f -t 0 "$ctr" >/dev/null' EXIT
        podman exec "$ctr" mkdir /rpms
        for f in "${files[@]}"; do
            podman cp "$rpms/$f" "$ctr:/rpms/$f"
        done
        podman exec "$ctr" bash -c '
            set -e
            echo keepcache=True >>/etc/dnf/dnf.conf
            dnf -y install dnf5-plugins rpm-build clippy rustfmt xorg-x11-server-Xvfb \
                dbus-daemon qt6-qtbase-gui kf6-qqc2-desktop-style breeze-icon-theme \
                ImageMagick xdotool
            cd /rpms
            dnf -y install "$@"
            # The exact files, also when this version or a newer one is installed.
            rpm -U --replacepkgs --oldpackage "$@"
            cd /
            rm -r /rpms
            dnf -y builddep /packaging/atlas-notepad.spec' bash "${files[@]}" >&2
        podman commit --change "LABEL atlas-ui=$build" --change "LABEL spec=$spec_sum" \
            "$ctr" "$image" >/dev/null
        podman rm -f -t 0 "$ctr" >/dev/null
        trap - EXIT
    fi
fi

# A changed spec (new BuildRequires) installs its build dependencies into the
# image, keeping its Atlas.Ui.
if [ "$(podman image inspect --format '{{index .Labels "spec"}}' "$image")" != "$spec_sum" ]; then
    ctr=$(podman run -d -v "$repo/packaging":/packaging:ro,z \
        -v atlas-dnf:/var/cache/libdnf5 "$image" sleep infinity)
    trap 'podman rm -f -t 0 "$ctr" >/dev/null' EXIT
    podman exec "$ctr" dnf -y builddep /packaging/atlas-notepad.spec >&2
    podman commit --change "LABEL spec=$spec_sum" "$ctr" "$image" >/dev/null
    podman rm -f -t 0 "$ctr" >/dev/null
    trap - EXIT
fi

tty=()
[ -t 0 ] && tty=(-it)
# :z (shared), not :Z: :Z gives each container a private label, which locks
# out any other dev container already running on the tree.
exec podman run --rm "${tty[@]}" \
    -v "$repo":/src:z -w /src \
    -v atlas-cargo:/root/.cargo/registry \
    -v atlas-cargo-git:/root/.cargo/git \
    -e CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-/src/target/dev}" \
    "$image" "${@:-bash}"
