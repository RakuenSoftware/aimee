#!/usr/bin/env python3
"""Provide an installed ouroboros ring that meets aimee's contract.

aimee does not build or carry the ring. It depends on the ring's contract
(github.com/RakuenSoftware/ouroboros-ring: spec/ and vectors/) through a release
range named in dependencies/ouroboros-ring.json, and consumes any installation of
the C API in that range through the ring's own package (pkg-config
ouroboros-ring / ouroboros-ring-client). Which installation is used:

  OURO_RING_PREFIX=/path   an installed ring (any implementation of the C API);
                           only its declared version is checked
  OURO_RING_SOURCE=/path   a ring checkout, built and installed with the ring's
                           own CMake build (for changing the ring and aimee
                           together); its VERSION must be in range
  neither                  the release named in the file, downloaded, checked
                           against its content digest, built with the ring's own
                           CMake build and installed under ROOT

Built rings use the caller's compiler (OURO_RING_CC, e.g. "ccache gcc") and extra
flags (OURO_RING_CFLAGS, e.g. sanitizers), one installation per combination.
ROOT/current always names the installation provided last, for tools that need
only the headers.

    provide_ouroboros_ring.py ROOT        print the pkg-config directory to use
    provide_ouroboros_ring.py --check-go  server-go/go.mod requires a release in range
    provide_ouroboros_ring.py --digest DIR  print the content digest of a tree
"""
from __future__ import annotations

import fcntl
import hashlib
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path
from typing import NoReturn

ROOT = Path(__file__).resolve().parent.parent
CONTRACT = ROOT / "dependencies/ouroboros-ring.json"
GO_MOD = ROOT / "server-go/go.mod"
GO_MODULE = "github.com/RakuenSoftware/ouroboros-ring/go"
SEMVER = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)$")
BASE_CFLAGS = "-Os -g -ffunction-sections -fdata-sections"


def fail(message: str) -> NoReturn:
    raise SystemExit(f"provide_ouroboros_ring: {message}")


def contract() -> dict:
    data = json.loads(CONTRACT.read_text())
    for key in ("repository", "compatible", "release", "release_tree_sha256"):
        if not isinstance(data.get(key), str) or not data[key]:
            fail(f"{CONTRACT.relative_to(ROOT)}: missing {key}")
    if not re.fullmatch(r"\d+(\.\d+)?", data["compatible"]):
        fail(f"{CONTRACT.relative_to(ROOT)}: compatible must be MAJOR or 0.MINOR")
    if not in_range(data["release"], data["compatible"]):
        fail(f"{CONTRACT.relative_to(ROOT)}: release {data['release']} is outside "
             f"compatible {data['compatible']}")
    return data


def in_range(version: str, compatible: str) -> bool:
    """Semver compatibility: 0.MINOR accepts 0.MINOR.*, MAJOR (>= 1) accepts MAJOR.*."""
    match = SEMVER.match(version.strip())
    if not match:
        return False
    major, minor, _ = (int(part) for part in match.groups())
    want = [int(part) for part in compatible.split(".")]
    if want[0] == 0:
        return major == 0 and len(want) == 2 and minor == want[1]
    return major == want[0]


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
                fail(f"ring archive has an unsafe path: {member.name!r}")
            target = into.joinpath(*parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                source = archive.extractfile(member)
                assert source is not None
                with source, open(target, "wb") as out:
                    shutil.copyfileobj(source, out)
            else:
                fail(f"ring archive has a non-regular entry: {member.name!r}")


def installed_pkgconfig(prefix: Path) -> Path:
    for libdir in ("lib", "lib64"):
        candidate = prefix / libdir / "pkgconfig"
        if (candidate / "ouroboros-ring.pc").is_file():
            return candidate
    fail(f"{prefix} has no lib/pkgconfig/ouroboros-ring.pc; it is not an installed ring")


def installed_version(pkgconfig: Path) -> str:
    for line in (pkgconfig / "ouroboros-ring.pc").read_text().splitlines():
        if line.startswith("Version:"):
            return line.split(":", 1)[1].strip()
    fail(f"{pkgconfig / 'ouroboros-ring.pc'} declares no Version")


def release_source(pin: dict, scratch: Path) -> Path:
    owner_repo = pin["repository"].removeprefix("https://github.com/").removesuffix(".git")
    url = f"https://codeload.github.com/{owner_repo}/tar.gz/refs/tags/{pin['release']}"
    try:
        with urllib.request.urlopen(url, timeout=60) as response:
            blob = response.read()
    except Exception as exc:  # noqa: BLE001 - every failure means the same thing here
        fail(f"cannot fetch ring release {pin['release']} from {url}: {exc}")
    extract(blob, scratch)
    tops = list(scratch.iterdir())
    if len(tops) != 1 or not tops[0].is_dir():
        fail("ring archive does not have one top-level directory")
    got = tree_digest(tops[0])
    if got != pin["release_tree_sha256"]:
        fail(f"ring release {pin['release']} content digest is {got}, but "
             f"{CONTRACT.relative_to(ROOT)} expects {pin['release_tree_sha256']}; "
             "a release tag must never change")
    return tops[0]


def build(source: Path, prefix: Path, cc: list[str], cflags: str) -> None:
    compiler = ["-DCMAKE_C_COMPILER=" + cc[-1]] if cc else []
    if len(cc) > 1:
        compiler.append("-DCMAKE_C_COMPILER_LAUNCHER=" + ";".join(cc[:-1]))
    with tempfile.TemporaryDirectory(dir=prefix.parent) as work:
        steps = [
            ["cmake", "-S", str(source / "c"), "-B", work, "-DBUILD_TESTING=OFF",
             "-DCMAKE_BUILD_TYPE=", "-DCMAKE_C_FLAGS=" + f"{BASE_CFLAGS} {cflags}".strip(),
             "-DCMAKE_INSTALL_PREFIX=" + str(prefix), "-DCMAKE_INSTALL_LIBDIR=lib", *compiler],
            ["cmake", "--build", work, "--parallel"],
            ["cmake", "--install", work],
        ]
        for step in steps:
            try:
                done = subprocess.run(step, stdout=sys.stderr, stderr=sys.stderr)
            except FileNotFoundError:
                shutil.rmtree(prefix, ignore_errors=True)
                fail("cmake is needed to build the ring release; install cmake, or point "
                     "OURO_RING_PREFIX at an installed ring")
            if done.returncode != 0:
                shutil.rmtree(prefix, ignore_errors=True)
                fail(f"building the ring failed at: {shlex.join(step)}")


def point_current(root: Path, prefix: Path) -> None:
    link = root / "current"
    staged = root / f".current-{os.getpid()}"
    staged.unlink(missing_ok=True)
    staged.symlink_to(prefix if prefix.is_absolute() else prefix.resolve())
    os.replace(staged, link)


def provide(root: Path) -> Path:
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    # Two builds of one tree (make and cmake, or two make runs) must not build
    # or repoint the same installation at once.
    with open(root / ".lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return provide_locked(root)


def remove_fetched_tree(root: Path) -> None:
    """Remove the ring source tree an older aimee fetched into this directory."""
    if not (root / ".ouroboros-ring-commit").is_file():
        return
    for entry in root.iterdir():
        if entry.name in {".lock", "current"} or entry.name.startswith(("v", "source-")) \
                and (entry / ".provided").exists():
            continue
        if entry.is_dir() and not entry.is_symlink():
            shutil.rmtree(entry)
        else:
            entry.unlink()


def provide_locked(root: Path) -> Path:
    pin = contract()
    remove_fetched_tree(root)

    given = os.environ.get("OURO_RING_PREFIX", "").strip()
    if given:
        prefix = Path(given).resolve()
        pkgconfig = installed_pkgconfig(prefix)
        version = installed_version(pkgconfig)
        if not in_range(version, pin["compatible"]):
            fail(f"OURO_RING_PREFIX {prefix} is ring {version}, outside compatible "
                 f"{pin['compatible']} in {CONTRACT.relative_to(ROOT)}")
        point_current(root, prefix)
        return pkgconfig

    cc = shlex.split(os.environ.get("OURO_RING_CC", "").strip())
    cflags = " ".join(os.environ.get("OURO_RING_CFLAGS", "").split())
    local = os.environ.get("OURO_RING_SOURCE", "").strip()
    if local:
        source = Path(local).resolve()
        version = (source / "VERSION").read_text().strip()
        if not in_range(version, pin["compatible"]):
            fail(f"OURO_RING_SOURCE {source} is ring {version}, outside compatible "
                 f"{pin['compatible']} in {CONTRACT.relative_to(ROOT)}")
        identity = f"source {tree_digest(source / 'c')}"
        label = f"source-{version}"
    else:
        identity = f"release {pin['release']} {pin['release_tree_sha256']}"
        label = pin["release"]
    key = hashlib.sha256(f"{identity}\0{shlex.join(cc)}\0{cflags}".encode()).hexdigest()[:12]
    prefix = root / f"{label}-{key}"
    stamp = prefix / ".provided"
    if not stamp.is_file():
        shutil.rmtree(prefix, ignore_errors=True)
        prefix.mkdir(parents=True)
        if local:
            build(source, prefix, cc, cflags)
        else:
            with tempfile.TemporaryDirectory(dir=root) as scratch:
                build(release_source(pin, Path(scratch)), prefix, cc, cflags)
        stamp.write_text(identity + "\n")
    pkgconfig = installed_pkgconfig(prefix)
    if not in_range(installed_version(pkgconfig), pin["compatible"]):
        fail(f"{prefix} installed ring {installed_version(pkgconfig)}, outside "
             f"compatible {pin['compatible']}")
    point_current(root, prefix)
    return pkgconfig


def check_go() -> None:
    pin = contract()
    for line in GO_MOD.read_text().splitlines():
        parts = line.split()
        if GO_MODULE in parts:
            version = parts[parts.index(GO_MODULE) + 1]
            if not in_range(version, pin["compatible"]):
                fail(f"server-go/go.mod requires {GO_MODULE} {version}; aimee needs a "
                     f"tagged release in compatible {pin['compatible']} "
                     f"({CONTRACT.relative_to(ROOT)}), not a commit")
            return
    fail(f"server-go/go.mod does not require {GO_MODULE}")


def main(argv: list[str]) -> int:
    if len(argv) == 3 and argv[1] == "--digest":
        print(tree_digest(Path(argv[2])))
    elif len(argv) == 2 and argv[1] == "--check-go":
        check_go()
    elif len(argv) == 2 and not argv[1].startswith("-"):
        print(provide(Path(argv[1])))
    else:
        print(__doc__, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
