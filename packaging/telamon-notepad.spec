# Notepad for Telamon OS.

# No debuginfo subpackage: the Rust flags below keep symbols (debuginfo=2,
# strip=none) and the binary is shipped as built.
%global debug_package %{nil}

Name:           telamon-notepad
Version:        0.2.0
Release:        2%{?dist}
Summary:        Notepad, the text editor of Telamon OS
License:        MIT AND Apache-2.0
URL:            https://github.com/EternalCoder454/atlasos-notepad
# Renamed from atlas-notepad: the image upgrades it in place, and what still
# says "atlas-notepad" (the command, the desktop file ID) keeps working in this
# release (see Legacy in cpp/legacy.h).
Obsoletes:      atlas-notepad < 0.2.0
Provides:       atlas-notepad = %{version}-%{release}
Source0:        telamon-notepad-%{version}.tar.gz

BuildRequires:  cargo
BuildRequires:  rust
# %%build_rustflags
BuildRequires:  rust-srpm-macros
BuildRequires:  gcc
BuildRequires:  gcc-c++
# readelf, for scripts/check-hardening.sh in %%check
BuildRequires:  binutils
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
BuildRequires:  cmake(KF6SyntaxHighlighting)
# the spell check tests
BuildRequires:  hunspell-en-US
# QML modules qmlcachegen resolves at build time (not linked). telamon-ui comes
# from atlas-framework, which is in no repository: install its RPMs first
# (build-rpm.sh does, given ATLAS_LOCAL_RPMS).
BuildRequires:  kf6-kirigami-devel
BuildRequires:  telamon-ui >= 2.0.9

Requires:       kf6-kirigami
Recommends:     kio-extras
# Telamon.Ui (1.4.0: the merged header, floating toolbar, popover, segmented control)
Requires:       telamon-ui >= 2.0.9
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
%autosetup -n telamon-notepad-%{version}

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
%global _vpath_srcdir apps/telamon-notepad
# No test binaries: %%check doesn't run them (they need a display), and they
# were half the compiles. CI's check job builds and runs them.
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
%cmake_build

%install
%cmake_install
# dnf refuses to remove a package listed here: Notepad is a system app on Telamon OS.
install -Dpm0644 packaging/protected.d/telamon-notepad.conf \
    %{buildroot}%{_sysconfdir}/dnf/protected.d/telamon-notepad.conf

%check
# The finished program is hardened as Fedora's flags make it: PIE, full RELRO,
# no executable stack, no RPATH, stack protectors, and none of the tests' KIO
# hook. Fails the build when a flag is lost (docs/SECURITY.md, "Build hardening").
bash scripts/check-hardening.sh --cxx --require-symbols %{buildroot}%{_bindir}/telamon-notepad
# No path into the build tree (checked as well as set: see %%build).
# grep: 0 = found, 1 = not found, anything else (no binary) fails too.
rc=0
grep -qF "%{_builddir}" %{buildroot}%{_bindir}/telamon-notepad || rc=$?
if [ "$rc" != 1 ]; then
    echo "telamon-notepad holds the build path %{_builddir} (grep status $rc)" >&2
    exit 1
fi
desktop-file-validate %{buildroot}%{_datadir}/applications/net.eterneon.telamon.notepad.desktop
desktop-file-validate %{buildroot}%{_datadir}/applications/net.eterneon.atlas.notepad.desktop
# The old command is a link to the new one.
test "$(readlink %{buildroot}%{_bindir}/atlas-notepad)" = telamon-notepad
appstream-util validate-relax --nonet \
    %{buildroot}%{_datadir}/metainfo/net.eterneon.telamon.notepad.metainfo.xml

%files
%license LICENSE NOTICE
%{_bindir}/telamon-notepad
%{_bindir}/atlas-notepad
%{_datadir}/applications/net.eterneon.telamon.notepad.desktop
%{_datadir}/applications/net.eterneon.atlas.notepad.desktop
%{_datadir}/metainfo/net.eterneon.telamon.notepad.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/net.eterneon.telamon.notepad.svg
%{_datadir}/icons/hicolor/16x16/apps/net.eterneon.telamon.notepad.svg
%config(noreplace) %{_sysconfdir}/dnf/protected.d/telamon-notepad.conf

%changelog
* Thu Oct 08 2026 Atlas <atlas@eterneon.net> - 0.2.0-2
- Secure phase (docs/SECURITY.md): a code line over 4,000 characters is no
  longer syntax highlighted (some definitions' regular expressions took
  seconds to minutes on it and held the window); property tests for the file
  reader and writer and the Markdown reader; a lint that keeps every text in
  the QML plain; the build fails when the program loses PIE, full RELRO, stack
  protectors or gains the tests' KIO hook (scripts/check-hardening.sh)
- atlas-framework and telamon-ui 2.0.9: the crash reports' text is scrubbed
  and capped by the framework before it is saved

* Wed Oct 07 2026 Atlas <atlas@eterneon.net> - 0.2.0-1
- Renamed to Telamon Notepad: telamon-notepad, net.eterneon.telamon.notepad,
  built on telamon-ui 2.0.0. atlas-notepad is obsoleted and provided; the old
  command and desktop file ID stay for this release
- Settings (atlas-notepadrc) and the session (tabs and unsaved text, in
  $XDG_STATE_HOME/atlas-notepad) are moved to the new names once, on first start
- A running atlas-notepad is handed the launch instead of losing its session

* Mon Oct 05 2026 Atlas <atlas@eterneon.net> - 0.1.0-4
- Large files: linear code highlighting, Notepad++-style line operations
- Freed heap returned after highlighting a large file
- The global menu shows the line operations' keys

* Sun Oct 04 2026 Atlas <atlas@eterneon.net> - 0.1.0-3
- Atlas.Ui 1.4.0: the merged window header, its status bar, segmented
  control, popover, scroll bars, font picker and confirm dialog replace
  Notepad's own

* Sun Oct 04 2026 Atlas <atlas@eterneon.net> - 0.1.0-2
- Built on atlas-framework (atlas-ui >= 1.3.0)
- dnf protected.d entry: not removable on AtlasOS

* Sat Oct 03 2026 Atlas <atlas@eterneon.net> - 0.1.0-1
- First package
