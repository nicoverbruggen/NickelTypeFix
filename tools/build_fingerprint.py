#!/usr/bin/env python3
"""Identify the source and package inputs of a development build."""

import hashlib
from pathlib import Path


def fingerprint(root):
    paths = {Path("Makefile"), Path("build.sh"), Path("tools/build_fingerprint.py")}
    for directory in ("src", "NickelHook"):
        paths.update(
            p.relative_to(root)
            for p in (root / directory).rglob("*")
            if p.is_file() and p.suffix in {".c", ".cc", ".cpp", ".h", ".mk"}
        )
    paths.update(p.relative_to(root) for p in (root / "res").rglob("*") if p.is_file())
    digest = hashlib.sha256()
    for path in sorted(paths):
        data = (root / path).read_bytes()
        digest.update(path.as_posix().encode("utf-8") + b"\0")
        digest.update(str(len(data)).encode("ascii") + b"\0")
        digest.update(data)
    return digest.hexdigest()[:12]


if __name__ == "__main__":
    print(fingerprint(Path(__file__).resolve().parent.parent))
