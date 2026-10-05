#!/usr/bin/env python3
"""Guard against a redirected atlas-framework, run from the repo root:

    ci/check-cargo.py <git url> <rev> [Cargo.lock]

Fails unless every atlas-framework package in Cargo.lock comes from that URL
at that rev (one with no source, i.e. a path or patched one, fails too), there
is at least one, and no other package's source mentions atlas-framework. Also
fails on [patch] or [replace] in any Cargo.toml, on a .cargo/config, on any
symlink in the tree (cargo follows them, this walk does not), and on any file
that doesn't parse. The files are parsed as TOML, never matched line by line,
so no spelling of a key gets past it.
"""

import os
import sys
import tomllib

SKIP_TOP = {".git", "target", "build", "out"}


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
        if norm(name).startswith("atlas-framework"):
            pinned += 1
            if src != want:
                errors.append(f"{path}: package {name}: source {src!r}, want {want!r}")
        elif isinstance(src, str) and "atlas-framework" in norm(src):
            errors.append(f"{path}: package {name} comes from {src!r}")
    if pinned == 0:
        errors.append(f"{path}: no atlas-framework package")


def check_tree(errors):
    for root, dirs, files in os.walk("."):
        if root == ".":
            dirs[:] = [d for d in dirs if d not in SKIP_TOP]
        else:
            dirs[:] = [d for d in dirs if d != ".git"]
        for entry in dirs + files:
            path = os.path.join(root, entry)
            if os.path.islink(path):
                errors.append(f"{path}: symlinks are not allowed")
        if os.path.basename(root) == ".cargo" and {"config", "config.toml"} & set(files):
            errors.append(f"{root}: a .cargo/config is not allowed")
        if "Cargo.toml" in files:
            path = os.path.join(root, "Cargo.toml")
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
        print(f"Every atlas-framework package must come from {url} at rev {rev}, unredirected (above)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
