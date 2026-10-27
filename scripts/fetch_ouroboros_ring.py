#!/usr/bin/env python3
"""Fetch the pinned ouroboros ring into the build tree.

aimee builds the ring from a commit of github.com/RakuenSoftware/ouroboros-ring
named in dependencies/ouroboros-ring.lock.json. Nothing of the ring is
committed here: the C sources are fetched, the Go module is required through
server-go/go.mod at the same commit, and the two are checked to agree.

The check is on content, not on the archive: a digest over every file's path
and bytes, which GitHub's tarball compression cannot change. A pin that does
not exist on GitHub, or a tree that does not match its digest, fails the build
rather than building something else.

    fetch_ouroboros_ring.py DEST             fetch into DEST (idempotent)
    fetch_ouroboros_ring.py --digest DIR     print the digest of an unpacked tree
    fetch_ouroboros_ring.py --check-go-pin   the go.mod pin names the lock's commit
"""
from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / "dependencies/ouroboros-ring.lock.json"
GO_MOD = ROOT / "server-go/go.mod"
GO_MODULE = "github.com/RakuenSoftware/ouroboros-ring/go"


def lock() -> dict:
    data = json.loads(LOCK.read_text())
    for key in ("repository", "commit", "tree_sha256"):
        if not isinstance(data.get(key), str) or not data[key]:
            raise SystemExit(f"{LOCK}: missing {key}")
    if len(data["commit"]) != 40:
        raise SystemExit(f"{LOCK}: commit must be a full 40-character SHA")
    return data


def tree_digest(root: Path) -> str:
    entries = []
    for path in sorted(p for p in root.rglob("*") if p.is_file() and not p.is_symlink()):
        rel = path.relative_to(root).as_posix()
        entries.append(f"{rel}\0{hashlib.sha256(path.read_bytes()).hexdigest()}\n")
    return hashlib.sha256("".join(entries).encode()).hexdigest()


def extract(blob: bytes, into: Path) -> None:
    # Not extractall(filter="data"): build images ship Pythons older than the
    # tarfile filters. The ring tree is plain files and directories, so anything
    # else -- a link, a device, a path that climbs out -- is refused, not skipped.
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as archive:
        for member in archive:
            parts = Path(member.name).parts
            if not parts or member.name.startswith("/") or ".." in parts:
                raise SystemExit(f"ouroboros-ring archive has an unsafe path: {member.name!r}")
            target = into.joinpath(*parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                source = archive.extractfile(member)
                assert source is not None
                with source, open(target, "wb") as out:
                    shutil.copyfileobj(source, out)
                os.chmod(target, 0o755 if member.mode & 0o111 else 0o644)
            else:
                raise SystemExit(
                    f"ouroboros-ring archive has a non-regular entry: {member.name!r}")


def fetch(dest: Path) -> None:
    pin = lock()
    stamp = dest / ".ouroboros-ring-commit"
    if stamp.is_file() and stamp.read_text().strip() == pin["commit"]:
        return
    owner_repo = pin["repository"].removeprefix("https://github.com/").removesuffix(".git")
    url = f"https://codeload.github.com/{owner_repo}/tar.gz/{pin['commit']}"
    try:
        with urllib.request.urlopen(url, timeout=60) as response:
            blob = response.read()
    except Exception as exc:  # noqa: BLE001 - every failure means the same thing here
        raise SystemExit(f"cannot fetch ouroboros-ring {pin['commit']} from {url}: {exc}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=dest.parent) as tmp:
        extract(blob, Path(tmp))
        tops = list(Path(tmp).iterdir())
        if len(tops) != 1 or not tops[0].is_dir():
            raise SystemExit("ouroboros-ring archive does not have one top-level directory")
        got = tree_digest(tops[0])
        if got != pin["tree_sha256"]:
            raise SystemExit(
                f"ouroboros-ring {pin['commit']} content digest is {got}, "
                f"but {LOCK.relative_to(ROOT)} pins {pin['tree_sha256']}")
        (tops[0] / ".ouroboros-ring-commit").write_text(pin["commit"] + "\n")
        if dest.exists():
            shutil.rmtree(dest)
        os.replace(tops[0], dest)


def check_go_pin() -> None:
    commit = lock()["commit"]
    for line in GO_MOD.read_text().splitlines():
        parts = line.split()
        if GO_MODULE in parts:
            version = parts[parts.index(GO_MODULE) + 1]
            if not version.endswith("-" + commit[:12]):
                raise SystemExit(
                    f"server-go/go.mod requires {GO_MODULE} {version}, which is not commit "
                    f"{commit[:12]} pinned in {LOCK.relative_to(ROOT)}")
            return
    raise SystemExit(f"server-go/go.mod does not require {GO_MODULE}")


def main(argv: list[str]) -> int:
    if len(argv) == 3 and argv[1] == "--digest":
        print(tree_digest(Path(argv[2])))
    elif len(argv) == 2 and argv[1] == "--check-go-pin":
        check_go_pin()
    elif len(argv) == 2 and not argv[1].startswith("-"):
        fetch(Path(argv[1]))
    else:
        print(__doc__, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
