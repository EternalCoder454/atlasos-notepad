#!/usr/bin/env python3
"""Guard against a redirected framework (the telamon-framework crates), run from the repo root:

    ci/check-cargo.py <git url> <rev> [Cargo.lock]

Fails unless every telamon-framework package in Cargo.lock comes from that URL
at that rev (one with no source, i.e. a path or patched one, fails too), there
is at least one, and no other git source appears (a renamed dependency could
otherwise bring in other code under the crate's name). Among the files git
tracks, which is all a CI checkout holds, it also fails on [patch] or [replace]
in any Cargo.toml, on any .cargo/config (cargo reads one from every parent of
its working directory, build/ included), on any symlink, and on any file that
doesn't parse. The files are parsed as TOML, never matched line by line, so no
spelling of a key gets past it.
"""

import os
import subprocess
import sys
import tomllib


def norm(name):
    return name.lower().replace("_", "-")


def load(path, errors):
    try:
        with open(path, "rb") as f:
            return tomllib.load(f)
    except (OSError, tomllib.TOMLDecodeError) as e:
        errors.append(f"{path}: cannot read: {e}")
        return None


def check_lock(path, want, errors):
    lock = load(path, errors)
    if lock is None:
        return
    for key in ("patch", "replace"):
        if key in lock:
            errors.append(f"{path}: has a [{key}] table")
    packages = lock.get("package", [])
    if not isinstance(packages, list):
        errors.append(f"{path}: package is not an array of tables")
        return
    pinned = 0
    for pkg in packages:
        if not isinstance(pkg, dict):
            errors.append(f"{path}: a package entry is not a table")
            continue
        name = pkg.get("name")
        src = pkg.get("source")
        if not isinstance(name, str):
            errors.append(f"{path}: a package has no name")
            continue
        if norm(name).startswith("telamon-framework"):
            pinned += 1
            if src != want:
                errors.append(f"{path}: package {name}: source {src!r}, want {want!r}")
        elif isinstance(src, str) and src != want and (
            src.startswith("git+") or "atlas-framework" in norm(src)
        ):
            errors.append(f"{path}: package {name} comes from {src!r}")
    if pinned == 0:
        errors.append(f"{path}: no telamon-framework package")


def check_tree(errors):
    try:
        out = subprocess.run(
            ["git", "ls-files", "-z", "--stage"],
            check=True, capture_output=True, timeout=60,
        ).stdout
    except (OSError, subprocess.SubprocessError) as e:
        errors.append(f"git ls-files failed: {e}")
        return
    for entry in out.split(b"\0"):
        if not entry:
            continue
        meta, _, raw = entry.partition(b"\t")
        path = os.fsdecode(raw)
        parts = path.split("/")
        if meta.split(b" ")[0] == b"120000":
            errors.append(f"{path}: symlinks are not allowed")
            continue
        if len(parts) > 1 and parts[-2] == ".cargo" and parts[-1] in ("config", "config.toml"):
            errors.append(f"{path}: a .cargo/config is not allowed")
        if parts[-1] == "Cargo.toml":
            doc = load(path, errors)
            if doc is None:
                continue
            for table in (doc, doc.get("workspace", {})):
                for key in ("patch", "replace"):
                    if isinstance(table, dict) and key in table:
                        errors.append(f"{path}: {key} is not allowed")


def main(argv):
    if len(argv) not in (3, 4):
        print("usage: check-cargo.py <git url> <rev> [Cargo.lock]", file=sys.stderr)
        return 2
    url, rev = argv[1], argv[2]
    lock = argv[3] if len(argv) == 4 else "Cargo.lock"
    errors = []
    check_lock(lock, f"git+{url}?rev={rev}#{rev}", errors)
    check_tree(errors)
    for e in errors:
        print(e, file=sys.stderr)
    if errors:
        print(f"Every telamon-framework package must come from {url} at rev {rev}, unredirected (above)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
