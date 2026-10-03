# Notepad for AtlasOS.

# No debuginfo subpackage: the Rust flags below keep symbols (debuginfo=2,
# strip=none) and the binary is shipped as built.
%global debug_package %{nil}

Name:           atlas-notepad
Version:        0.1.0
Release:        1%{?dist}
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
# CMake fetches Atlas.Ui from atlasos-updater.
BuildRequires:  git-core
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib
BuildRequires:  cmake(Qt6Core)
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
# QML modules qmlcachegen resolves at build time (not linked)
BuildRequires:  kf6-kirigami-devel

Requires:       kf6-kirigami
Requires:       kf6-qqc2-desktop-style
Requires:       qt6-qtdeclarative
# the app icon and Breeze's icons are SVG
Requires:       qt6-qtsvg

%description
Notepad is a fast, simple text editor. It opens and saves plain text files as
they are, and shows Markdown formatted, with a toolbar for bold, lists, links
and headings.

%prep
%autosetup -n atlas-notepad-%{version}

%build
# NETWORK: cargo (Corrosion runs it with --locked) fetches crates.io, and
# CMake's FetchContent clones Atlas.Ui at the pinned commit, during %%build.
# That works in podman and with `rpmbuild` on a networked machine, not in an
# offline mock/Koji build.
export CARGO_HOME=${CARGO_HOME:-%{_builddir}/cargo-home}
export RUSTFLAGS="%{build_rustflags}"
export CARGO_PROFILE_RELEASE_STRIP=none
%global _vpath_srcdir apps/atlas-notepad
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/net.eterneon.atlas.notepad.desktop

%files
%license LICENSE NOTICE
%{_bindir}/atlas-notepad
%{_datadir}/applications/net.eterneon.atlas.notepad.desktop

%changelog
* Sat Oct 03 2026 Atlas <atlas@eterneon.net> - 0.1.0-1
- First package
