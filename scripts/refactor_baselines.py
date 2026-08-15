#!/usr/bin/env python3
"""Freeze or verify deterministic public-surface baselines for the refactor."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_INDEX = Path("tests/baselines/refactor/index.json")
SCHEMA_VERSION = 1
SURFACES = {
    "cli-help": (
        "docs/gen/cli-commands.md",
        "src/server/server_main.c",
        "src/kb/kb_main.c",
        "src/gateway/gateway_main.c",
    ),
    "routes": ("docs/gen/v1-route-descriptor.json",),
    "configuration": ("docs/gen/configuration.md",),
    "public-headers": (
        "src/headers/*.h",
        "src/modules/*/include/**/*.h",
    ),
    "database-schemas": (
        "src/modules/db1/*.sql",
        "src/db2/*.sql",
        "deploy/migrations/*.sql",
    ),
    "packages": (
        "Dockerfile*",
        "install.sh",
        "install.ps1",
        "frontend/package.json",
        "frontend/package-lock.json",
        "editors/vscode/package.json",
        "editors/vscode/package-lock.json",
    ),
}


class BaselineError(ValueError):
    """A deterministic, operator-readable baseline failure."""


def _object_without_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise BaselineError(f"duplicate object key {key!r}")
        result[key] = value
    return result


def _normalized_bytes(path: Path) -> bytes:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise BaselineError(f"cannot read {path}: {exc}") from exc
    return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _tracked_files(root: Path) -> list[Path]:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            check=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise BaselineError(f"cannot enumerate tracked repository files: {exc}") from exc
    return [root / item.decode("utf-8") for item in result.stdout.split(b"\0") if item]


def _expand(patterns: tuple[str, ...], root: Path, tracked: list[Path]) -> list[Path]:
    paths: set[Path] = set()
    for pattern in patterns:
        matches = [
            path
            for path in tracked
            if fnmatch.fnmatchcase(path.relative_to(root).as_posix(), pattern) and path.is_file()
        ]
        if not matches:
            raise BaselineError(f"surface input pattern matched no files: {pattern}")
        paths.update(matches)
    return sorted(paths, key=lambda path: path.relative_to(root).as_posix())


def _make_install_recipe(root: Path, tracked: list[Path]) -> bytes:
    makefile = root / "src/Makefile"
    if makefile not in tracked or not makefile.is_file():
        raise BaselineError("src/Makefile must be a tracked regular file")
    lines = makefile.read_text(encoding="utf-8").splitlines()
    try:
        start = lines.index("install:")
    except ValueError as exc:
        raise BaselineError("src/Makefile has no exact install target") from exc
    body = []
    for line in lines[start + 1 :]:
        if line.startswith("\t"):
            body.append(line.rstrip())
            continue
        if not line:
            if body:
                body.append("")
            continue
        break
    while body and not body[-1]:
        body.pop()
    if not body:
        raise BaselineError("src/Makefile install target has no tab-indented recipe")
    return ("install:\n" + "\n".join(body) + "\n").encode()


def build_index(root: Path) -> dict[str, object]:
    tracked = _tracked_files(root)
    surfaces: dict[str, object] = {}
    for name, patterns in SURFACES.items():
        files = []
        for path in _expand(patterns, root, tracked):
            files.append(
                {
                    "path": path.relative_to(root).as_posix(),
                    "sha256": _digest(_normalized_bytes(path)),
                }
            )
        if name == "packages":
            files.append(
                {
                    "path": "src/Makefile#install",
                    "sha256": _digest(_make_install_recipe(root, tracked)),
                }
            )
            files.sort(key=lambda item: item["path"])
        surfaces[name] = {"files": files}
    if "public-headers" in SURFACES:
        public_headers = _expand(SURFACES["public-headers"], root, tracked)
        declaration_surface = b"".join(
            path.relative_to(root).as_posix().encode()
            + b"\0"
            + _normalized_bytes(path)
            + b"\0"
            for path in public_headers
        )
        surfaces["public-symbols"] = {
            "source": "complete normalized contents of public-headers",
            "sha256": _digest(declaration_surface),
        }
    return {
        "schema_version": SCHEMA_VERSION,
        "hash": "sha256",
        "normalization": "UTF-8/source bytes with CRLF and CR normalized to LF; sorted paths",
        "surfaces": surfaces,
    }


def _canonical(data: object) -> str:
    return json.dumps(data, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def _resolve_index(root: Path, value: Path) -> Path:
    candidate = value if value.is_absolute() else root / value
    resolved = Path(os.path.realpath(candidate))
    try:
        resolved.relative_to(root)
    except ValueError as exc:
        raise BaselineError(f"index must remain under repository root: {resolved}") from exc
    return resolved


def _require_freeze_intent(root: Path, accept_dirty: bool) -> None:
    try:
        dirty = subprocess.run(
            ["git", "-C", str(root), "status", "--porcelain", "--untracked-files=no"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise BaselineError(f"cannot inspect tracked working-tree state: {exc}") from exc
    if dirty and not accept_dirty:
        raise BaselineError("refusing to freeze tracked working-tree changes without --accept-dirty")


def check(root: Path, index_path: Path) -> None:
    try:
        expected = json.loads(
            index_path.read_text(encoding="utf-8"),
            object_pairs_hook=_object_without_duplicate_keys,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError(f"cannot load baseline index {index_path}: {exc}") from exc
    actual = build_index(root)
    if expected != actual:
        expected_text = _canonical(expected).splitlines()
        actual_text = _canonical(actual).splitlines()
        for line_no, (old, new) in enumerate(zip(expected_text, actual_text), 1):
            if old != new:
                detail = f"first difference at canonical line {line_no}: expected {old!r}, actual {new!r}"
                break
        else:
            detail = f"canonical lengths differ: expected {len(expected_text)}, actual {len(actual_text)}"
        raise BaselineError(
            f"surface drift: {detail}; inspect changes, then run scripts/refactor_baselines.py freeze"
        )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs="?", choices=("check", "freeze"), default="check")
    parser.add_argument("--index", type=Path, default=DEFAULT_INDEX)
    parser.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    parser.add_argument("--accept-dirty", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    root = Path(os.path.realpath(args.root))
    try:
        if not root.is_dir():
            raise BaselineError(f"repository root is not a directory: {root}")
        index_path = _resolve_index(root, args.index)
        if args.command == "freeze":
            _require_freeze_intent(root, args.accept_dirty)
            index_path.parent.mkdir(parents=True, exist_ok=True)
            index_path.write_text(_canonical(build_index(root)), encoding="utf-8")
            print(f"refactor_baselines: froze {index_path.relative_to(root)}")
        else:
            check(root, index_path)
            print(f"refactor_baselines: ok ({index_path.relative_to(root)})")
    except BaselineError as exc:
        print(f"refactor_baselines: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
