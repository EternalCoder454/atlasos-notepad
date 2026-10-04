# Notepad for AtlasOS.

# No debuginfo subpackage: the Rust flags below keep symbols (debuginfo=2,
# strip=none) and the binary is shipped as built.
%global debug_package %{nil}

Name:           atlas-notepad
Version:        0.1.0
Release:        2%{?dist}
Summary:        Notepad, the text editor of AtlasOS
License:        MIT AND Apache-2.0
URL:            https://github.com/EternalCoder454/atlasos-notepad
Source0:        atlas-notepad-%{version}.tar.gz

BuildRequires:  cargo
BuildRequires:  rust
# %%build_rustflags
BuildRequires:  rust-srpm-macros
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  corrosion
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib
BuildRequires:  cmake(Qt6Core)
BuildRequires:  cmake(Qt6Concurrent)
BuildRequires:  cmake(Qt6PrintSupport)
BuildRequires:  cmake(Qt6Test)
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Qml)
BuildRequires:  cmake(Qt6Quick)
BuildRequires:  cmake(Qt6QuickControls2)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6QmlTools)
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtbase-private-devel
BuildRequires:  cmake(KF6DBusAddons)
BuildRequires:  cmake(KF6WindowSystem)
BuildRequires:  cmake(KF6Sonnet)
BuildRequires:  cmake(KF6KIO)
BuildRequires:  cmake(KF6GuiAddons)
# the spell check tests
BuildRequires:  hunspell-en-US
# QML modules qmlcachegen resolves at build time (not linked). atlas-ui comes
# from atlas-framework, which is in no repository: install its RPMs first
# (build-rpm.sh does, given ATLAS_LOCAL_RPMS).
BuildRequires:  kf6-kirigami-devel
BuildRequires:  atlas-ui >= 1.3.0

Requires:       kf6-kirigami
Recommends:     kio-extras
# Atlas.Ui (its 1.3.0 has the editor components and the Atlas form controls)
Requires:       atlas-ui >= 1.3.0
Requires:       kf6-qqc2-desktop-style
# spell check (dictionaries come from the system's langpacks)
Requires:       kf6-sonnet
# also the file dialogs and Qt.labs.platform's global menu
Requires:       qt6-qtdeclarative
Requires:       hicolor-icon-theme
# the app icon and Breeze's icons are SVG
Requires:       qt6-qtsvg
# printing
Requires:       qt6-qtbase-gui

%description
Notepad is a fast, simple text editor. It opens and saves plain text files as
they are, and shows Markdown formatted, with a toolbar for bold, lists, links
and headings.

%prep
%autosetup -n atlas-notepad-%{version}

%build
# NETWORK: cargo (Corrosion runs it with --locked) fetches crates.io and
# atlas-framework at the pinned commit during %%build. That works in podman
# and with `rpmbuild` on a networked machine, not in an offline mock/Koji build.
export CARGO_HOME=${CARGO_HOME:-%{_builddir}/cargo-home}
# No build paths in the binary (panic messages, debug info): %%build runs in
# the source directory. %%cmake keeps CFLAGS/CXXFLAGS when they are set.
# These flags split on spaces, so _topdir must have none (build-rpm.sh's hasn't).
export RUSTFLAGS="%{build_rustflags} --remap-path-prefix=$PWD=. --remap-path-prefix=$CARGO_HOME=cargo"
export CFLAGS="%{build_cflags} -ffile-prefix-map=$PWD=."
export CXXFLAGS="%{build_cxxflags} -ffile-prefix-map=$PWD=."
export CARGO_PROFILE_RELEASE_STRIP=none
%global _vpath_srcdir apps/atlas-notepad
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install
# dnf refuses to remove a package listed here: Notepad is a system app on AtlasOS.
install -Dpm0644 packaging/protected.d/atlas-notepad.conf \
    %{buildroot}%{_sysconfdir}/dnf/protected.d/atlas-notepad.conf

%check
# No path into the build tree (checked as well as set: see %%build).
# grep: 0 = found, 1 = not found, anything else (no binary) fails too.
rc=0
grep -qF "%{_builddir}" %{buildroot}%{_bindir}/atlas-notepad || rc=$?
if [ "$rc" != 1 ]; then
    echo "atlas-notepad holds the build path %{_builddir} (grep status $rc)" >&2
    exit 1
fi
desktop-file-validate %{buildroot}%{_datadir}/applications/net.eterneon.atlas.notepad.desktop
appstream-util validate-relax --nonet \
    %{buildroot}%{_datadir}/metainfo/net.eterneon.atlas.notepad.metainfo.xml

%files
%license LICENSE NOTICE
%{_bindir}/atlas-notepad
%{_datadir}/applications/net.eterneon.atlas.notepad.desktop
%{_datadir}/metainfo/net.eterneon.atlas.notepad.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/net.eterneon.atlas.notepad.svg
%{_datadir}/icons/hicolor/16x16/apps/net.eterneon.atlas.notepad.svg
%config(noreplace) %{_sysconfdir}/dnf/protected.d/atlas-notepad.conf

%changelog
* Sun Oct 04 2026 Atlas <atlas@eterneon.net> - 0.1.0-2
- Built on atlas-framework (atlas-ui >= 1.3.0)
- dnf protected.d entry: not removable on AtlasOS

* Sat Oct 03 2026 Atlas <atlas@eterneon.net> - 0.1.0-1
- First package
