#!/usr/bin/env python3
"""Reject flat public-header shadows and retired module include roots."""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import re
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
DESCRIPTORS = Path("src/modules")
SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp"}
VALIDATOR_PATH = Path(__file__).with_name("validate_module_descriptors.py")
VALIDATOR_SPEC = importlib.util.spec_from_file_location("module_descriptor_validator", VALIDATOR_PATH)
if VALIDATOR_SPEC is None or VALIDATOR_SPEC.loader is None:
    raise ImportError(f"cannot load descriptor validator at {VALIDATOR_PATH}")
validator = importlib.util.module_from_spec(VALIDATOR_SPEC)
VALIDATOR_SPEC.loader.exec_module(validator)
if type(getattr(validator, "MAX_BYTES", None)) is not int:
    raise ImportError("descriptor validator must export integer MAX_BYTES")


class HeaderLayoutError(ValueError):
    """One or more deterministic public-header layout violations."""


def _read_descriptor(path: Path) -> dict[str, object]:
    try:
        value = validator.load_json(path)
    except validator.DescriptorError as exc:
        raise HeaderLayoutError(f"rule=descriptor-input path={path}: {exc}") from exc
    if not isinstance(value, dict):
        raise HeaderLayoutError(f"rule=input path={path}: descriptor must be an object")
    return value


def _source_files(root: Path):
    tracked = subprocess.run(
        [
            "git", "-C", str(root), "ls-files", "--cached", "--others",
            "--exclude-standard", "-z", "--", "*.c", "*.cpp", "*.h", "*.hpp",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if tracked.returncode == 0:
        for raw in tracked.stdout.split(b"\0"):
            if not raw:
                continue
            try:
                relative = raw.decode("utf-8", errors="strict")
            except UnicodeError as exc:
                raise HeaderLayoutError("rule=input: git returned a non-UTF-8 path") from exc
            path = root / relative
            if path.is_symlink() or path.is_file():
                yield path
        return

    # Standalone mutation fixtures are not Git repositories. Keep that path
    # exhaustive while pruning repository-control and local-worktree trees.
    for path in root.rglob("*"):
        relative_parts = path.relative_to(root).parts
        if (any(part.startswith(".") for part in relative_parts) or
                path.suffix not in SOURCE_SUFFIXES):
            continue
        if path.is_symlink() or path.is_file():
            yield path


def _make_include_roots(makefile: str) -> set[str]:
    tokens = re.sub(r"\\\r?\n\s*", " ", makefile).split()
    roots: set[str] = set()
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-I" and index + 1 < len(tokens):
            roots.add(tokens[index + 1])
            index += 2
            continue
        if token.startswith("-I") and len(token) > 2:
            roots.add(token[2:])
        index += 1
    return roots


def _cmake_include_calls(cmake: str) -> list[str]:
    """Extract complete target_include_directories calls with balanced parentheses."""
    calls: list[str] = []
    start = re.compile(r"target_include_directories\s*\(", re.IGNORECASE)
    for match in start.finditer(cmake):
        depth = 0
        quote = False
        escaped = False
        for index in range(match.end() - 1, len(cmake)):
            char = cmake[index]
            if quote:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quote = False
                continue
            if char == '"':
                quote = True
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    calls.append(cmake[match.start():index + 1])
                    break
        else:
            raise HeaderLayoutError("rule=input path=CMakeLists.txt: unbalanced include command")
    return calls


def _cmake_include_roots(cmake: str) -> set[str]:
    roots: set[str] = set()
    for call in _cmake_include_calls(cmake):
        body = call[call.find("(") + 1:-1]
        try:
            roots.update(shlex.split(body, comments=False, posix=True))
        except ValueError as exc:
            raise HeaderLayoutError(f"rule=input path=CMakeLists.txt: {exc}") from exc
    return roots


def _read_required(root: Path, relative: str) -> str:
    try:
        return (root / relative).read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise HeaderLayoutError(f"rule=missing-input path={relative}") from exc
    except (OSError, UnicodeError) as exc:
        raise HeaderLayoutError(f"rule=input path={relative}: {exc}") from exc


def violations(root: Path) -> list[str]:
    """Return sorted violations derived from declared canonical public headers."""
    descriptors = root / DESCRIPTORS
    if not descriptors.is_dir():
        raise HeaderLayoutError(f"rule=config-root path={descriptors}: missing descriptor root")

    # One retired include spelling may fan out to every module that declares it.
    claims: dict[str, set[str]] = {}
    canonical_headers: list[tuple[str, str, str]] = []
    for path in sorted(descriptors.rglob("module.yaml")):
        value = _read_descriptor(path)
        identifier = value.get("id")
        headers = value.get("public_headers", [])
        if not isinstance(identifier, str) or not isinstance(headers, list):
            raise HeaderLayoutError(f"rule=input path={path}: invalid id or public_headers")
        for header in headers:
            if not isinstance(header, str):
                raise HeaderLayoutError(f"rule=input path={path}: public header must be a string")
            prefix = f"src/modules/{identifier}/include/aimee/{identifier}/"
            if not header.startswith(prefix) or header == prefix:
                raise HeaderLayoutError(
                    f"rule=input path={path}: public header is outside canonical include tree: "
                    f"{header}"
                )
            suffix = header[len(prefix):]
            for retired in {
                Path(header).name,
                suffix,
                f"modules/{identifier}/{suffix}",
                f"src/modules/{identifier}/{suffix}",
            }:
                claims.setdefault(retired, set()).add(identifier)
            canonical_headers.append((identifier, header, suffix))

    found: set[str] = set()
    for identifier, header, _suffix in canonical_headers:
        if not (root / header).is_file():
            found.add(
                f"module={identifier} rule=missing-canonical-header path={header}"
            )
    include = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
    for path in _source_files(root):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            found.add(f"module=* rule=source-symlink path={relative}")
            continue
        try:
            with path.open(encoding="utf-8") as source:
                for line in source:
                    match = include.match(line)
                    if match is None or match.group(2) not in claims:
                        continue
                    form = "angle" if match.group(1) == "<" else "quoted"
                    for identifier in claims[match.group(2)]:
                        found.add(
                            f"module={identifier} rule=retired-header-include "
                            f"path={relative} header={match.group(2)} form={form}"
                        )
        except (OSError, UnicodeError) as exc:
            raise HeaderLayoutError(f"rule=input path={relative}: {exc}") from exc

    makefile = _read_required(root, "src/Makefile")
    cmake = _read_required(root, "CMakeLists.txt")
    make_roots = _make_include_roots(makefile)
    cmake_roots = _cmake_include_roots(cmake)

    for identifier, _header, suffix in canonical_headers:
        legacy = f"src/modules/{identifier}/{suffix}"
        if (root / legacy).is_symlink() or (root / legacy).exists():
            found.add(f"module={identifier} rule=retired-header-path path={legacy}")

        legacy_parent = Path(suffix).parent.as_posix()
        make_candidates = {f"modules/{identifier}"}
        cmake_candidates = {f"${{AIMEE_SRC_DIR}}/modules/{identifier}"}
        if legacy_parent != ".":
            make_candidates.add(f"modules/{identifier}/{legacy_parent}")
            cmake_candidates.add(
                f"${{AIMEE_SRC_DIR}}/modules/{identifier}/{legacy_parent}"
            )
        for make_root in sorted(make_candidates & make_roots):
            found.add(
                f"module={identifier} rule=retired-include-root path=src/Makefile "
                f"value=-I{make_root}"
            )
        for cmake_root in sorted(cmake_candidates & cmake_roots):
            found.add(
                f"module={identifier} rule=retired-include-root path=CMakeLists.txt "
                f"value={cmake_root}"
            )
    return sorted(found)


def validate(root: Path) -> None:
    problems = violations(root)
    if problems:
        raise HeaderLayoutError("\n".join(problems))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-root", type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.config_root.resolve()
    try:
        validate(root)
    except (HeaderLayoutError, OSError, UnicodeError) as exc:
        for line in str(exc).splitlines():
            print(f"module-header-layout: ERROR {line}", file=sys.stderr)
        return 1
    print("module-header-layout: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
