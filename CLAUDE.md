# Notepad (atlas-notepad)

AtlasOS's text editor: Rust + Qt 6.11 Quick + Kirigami, CMake + Corrosion.
Design and S1 figures: `docs/DESIGN.md`. Plan and roadmap: Atlas Notes,
`AtlasOS/Atlas Text Editor/Plan` and `/Roadmap` (tick a box only after it was
built and tested).

## Rules

- Never touch Kate, and never put Notepad into the AtlasOS image. Develop
  here and test in the VM with the RPM installed there.
- Never read the `Financials/*` or `QuickScript/Keys` notes.
- No GitHub repo or push without the user's go-ahead.
- Commits: `EternalHell <77252745+EternalCoder454@users.noreply.github.com>`.

## Commands

All builds and tests run in the fedora:44 dev container, never on the host.
Atlas.Ui is the installed atlas-ui RPM: the first `scripts/dev.sh` run (and
`packaging/build-rpm.sh`) needs `ATLAS_LOCAL_RPMS=<dir>` with atlas-framework's
RPMs at the commit pinned in `Cargo.toml` (ones you built: they are installed
as root without a signature check). To move to a newer Atlas.Ui, change
that rev and `atlas-ui >=` in the spec together, build the framework RPMs at
the new rev and run `scripts/dev.sh` once with `ATLAS_LOCAL_RPMS`. In the VM,
install atlas-ui and atlas-symbols-fonts before the Notepad RPM.

```sh
scripts/dev.sh cargo fmt --all
scripts/dev.sh cargo clippy --workspace --all-targets -- -D warnings
scripts/dev.sh cargo test --workspace
scripts/dev.sh bash -c 'cmake -S apps/atlas-notepad -B build/s1 -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/s1'
scripts/dev.sh ctest --test-dir build/s1 --output-on-failure
scripts/dev.sh scripts/bench-s1.sh build/s1
```

`build/`, `target/` and `out/` (screenshots, scratch) are gitignored.

## Speed

Latency is the point of this app. Any change to the editor path
(`cpp/highlighter.cpp`, `markdowneditor.cpp`, `decorations.cpp`,
`crates/notepad-core/src/markdown.rs`) gets a bench run before and after.
Targets: plain text < 1 ms mean; Markdown < 2.5 ms p95 at 1x, software
backend.
