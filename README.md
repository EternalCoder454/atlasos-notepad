# Notepad

Notepad (`telamon-notepad`) is the text editor of Telamon OS: tabs, an editor and a
status bar in the Telamon look, with a WYSIWYG Markdown view. It is a system app
on Telamon OS and the default editor for plain text and Markdown.

Rust and Qt 6 Quick with Kirigami, built with CMake and Corrosion. The shared
Atlas look (`Telamon.Ui`) and the opt-in crash reports come from
[atlas-framework](https://github.com/EternalCoder454/atlas-framework).

## Building

Builds and tests run in a Fedora 44 container (`scripts/dev.sh`). The first
run needs the atlas-framework RPMs (`telamon-ui`, `telamon-symbols-fonts`):

```sh
ATLAS_LOCAL_RPMS=<dir with the RPMs> scripts/dev.sh cargo test --workspace
scripts/dev.sh bash -c 'cmake -S apps/telamon-notepad -B build/s1 -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/s1'
# the RPM, built in the container (the RPMs' dir inside the repo, e.g. out/fw-rpms)
scripts/dev.sh env ATLAS_LOCAL_RPMS=/src/out/fw-rpms packaging/build-rpm.sh /src/out/rpm
```

Design notes, including the performance targets: [docs/DESIGN.md](docs/DESIGN.md).
Threat model, rules and the tests that hold them: [docs/SECURITY.md](docs/SECURITY.md).

## License

MIT (see `LICENSE`). The icons are Material Symbols, Apache-2.0 (see `NOTICE`).
