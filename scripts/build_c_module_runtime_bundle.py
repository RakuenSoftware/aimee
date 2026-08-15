#!/usr/bin/env python3
"""Compile descriptor-owned C module processes from a runtime-bundle manifest."""

from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import sys
from typing import NoReturn


ROOT = Path(__file__).resolve().parent.parent
BUILD_MANIFEST = "c-build.json"
MODULE_ID = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
BUILD_TOKEN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:+-]*$")
MODULE_KEYS = {
    "id", "binary", "main", "sources", "include_roots", "pkg_config",
    "system_libraries",
}
CORE_EVENT_BUS_SOURCES = [
    "src/core/event_bus/bus_attach.c",
    "src/core/event_bus/bus_client.c",
    "src/core/event_bus/bus_endpoint.c",
    "src/core/event_bus/bus_region.c",
    "src/core/event_bus/bus_ring.c",
    "src/core/event_bus/bus_wire.c",
    "src/core/event_bus/module_protocol.c",
    "src/core/event_bus/module_runtime.c",
]
IMPORTED_TARGET_FLAGS = {
    "OpenSSL::Crypto": "-lcrypto",
    "OpenSSL::SSL": "-lssl",
    "Threads::Threads": "-pthread",
    "ZLIB::ZLIB": "-lz",
}


class BuildError(RuntimeError):
    """A fail-closed runtime-bundle build error."""


def fail(message: str) -> NoReturn:
    raise BuildError(message)


def strict_json(raw: bytes) -> object:
    def no_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
        value: dict[str, object] = {}
        for key, item in pairs:
            if key in value:
                fail(f"C build manifest contains duplicate key {key!r}")
            value[key] = item
        return value

    if raw.startswith(b"\xef\xbb\xbf"):
        fail("C build manifest begins with a UTF-8 BOM")
    try:
        return json.loads(raw.decode("utf-8", "strict"), object_pairs_hook=no_duplicates)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        fail(f"cannot parse {BUILD_MANIFEST}: {exc}")


def safe_relative(value: object, label: str) -> str:
    if not isinstance(value, str):
        fail(f"{label} must be a string")
    pure = PurePosixPath(value)
    if (not value or "\\" in value or pure.is_absolute() or "." in pure.parts or
            ".." in pure.parts or pure.as_posix() != value):
        fail(f"{label} is not a safe relative path: {value!r}")
    return value


def string_array(value: object, label: str, *, paths: bool = False,
                 nonempty: bool = False) -> list[str]:
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        fail(f"{label} must be a string array")
    if nonempty and not value:
        fail(f"{label} must not be empty")
    if value != sorted(set(value)):
        fail(f"{label} must be sorted and unique")
    for item in value:
        if paths:
            safe_relative(item, label)
        elif not BUILD_TOKEN.fullmatch(item):
            fail(f"{label} contains unsafe token {item!r}")
    return value


def load_builds(bundle: Path) -> list[dict[str, object]]:
    try:
        value = strict_json((bundle / BUILD_MANIFEST).read_bytes())
    except OSError as exc:
        fail(f"cannot load {BUILD_MANIFEST}: {exc}")
    if not isinstance(value, dict) or set(value) != {"schema_version", "modules"}:
        fail("C build manifest top-level keys differ from v1")
    modules = value["modules"]
    if value["schema_version"] != 1 or not isinstance(modules, list):
        fail("C build manifest schema_version/modules are invalid")
    previous = ""
    result: list[dict[str, object]] = []
    for index, module in enumerate(modules):
        if not isinstance(module, dict) or set(module) != MODULE_KEYS:
            fail(f"module {index}: keys differ from C build v1")
        identifier = module["id"]
        if (not isinstance(identifier, str) or not MODULE_ID.fullmatch(identifier) or
                identifier <= previous):
            fail(f"module {index}: IDs must be valid, sorted, and unique")
        previous = identifier
        if module["binary"] != f"aimee-module-{identifier}":
            fail(f"{identifier}: binary does not match module identity")
        main = safe_relative(module["main"], f"{identifier}.main")
        if main != f"src/aimee-module-{identifier}.c":
            fail(f"{identifier}: generated main path is not canonical")
        sources = string_array(module["sources"], f"{identifier}.sources", paths=True,
                               nonempty=True)
        if any(PurePosixPath(source).suffix != ".c" for source in sources):
            fail(f"{identifier}: sources must all be C translation units")
        string_array(module["include_roots"], f"{identifier}.include_roots", paths=True,
                     nonempty=True)
        string_array(module["pkg_config"], f"{identifier}.pkg_config")
        libraries = string_array(module["system_libraries"],
                                 f"{identifier}.system_libraries")
        for library in libraries:
            if "::" in library and library not in IMPORTED_TARGET_FLAGS:
                fail(f"{identifier}: unsupported imported target {library!r}")
        result.append(module)
    return result


def real_file(root: Path, relative: str, label: str) -> Path:
    lexical = root.joinpath(*PurePosixPath(relative).parts)
    try:
        resolved_root = root.resolve(strict=True)
        resolved = lexical.resolve(strict=True)
        resolved.relative_to(resolved_root)
    except (OSError, ValueError) as exc:
        fail(f"{label} is missing or escapes its root: {relative}: {exc}")
    if resolved != lexical or not resolved.is_file():
        fail(f"{label} is not a real regular file: {relative}")
    return resolved


def real_directory(root: Path, relative: str, label: str) -> Path:
    lexical = root.joinpath(*PurePosixPath(relative).parts)
    try:
        resolved_root = root.resolve(strict=True)
        resolved = lexical.resolve(strict=True)
        resolved.relative_to(resolved_root)
    except (OSError, ValueError) as exc:
        fail(f"{label} is missing or escapes its root: {relative}: {exc}")
    if resolved != lexical or not resolved.is_dir():
        fail(f"{label} is not a real directory: {relative}")
    return resolved


def pkg_config_flags(executable: str, packages: list[str]) -> tuple[list[str], list[str]]:
    if not packages:
        return [], []
    results: list[list[str]] = []
    for mode in ("--cflags", "--libs"):
        try:
            completed = subprocess.run(
                [executable, mode, *packages], check=True, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
        except (OSError, subprocess.CalledProcessError) as exc:
            detail = getattr(exc, "stderr", "") or str(exc)
            fail(f"pkg-config {mode} failed: {detail.strip()}")
        try:
            results.append(shlex.split(completed.stdout, posix=True))
        except ValueError as exc:
            fail(f"pkg-config {mode} returned malformed flags: {exc}")
    return results[0], results[1]


def compiler_command(module: dict[str, object], root: Path, bundle: Path, output: Path,
                     cc: str, pkg_config: str) -> list[str]:
    identifier = module["id"]
    assert isinstance(identifier, str)
    sources = module["sources"]
    include_roots = module["include_roots"]
    packages = module["pkg_config"]
    libraries = module["system_libraries"]
    assert all(isinstance(value, list) for value in (
        sources, include_roots, packages, libraries
    ))
    main = real_file(bundle, module["main"], f"{identifier}.main")
    owned_sources = [real_file(root, item, f"{identifier}.sources") for item in sources]
    core_sources = [real_file(root, item, "event-bus source")
                    for item in CORE_EVENT_BUS_SOURCES]
    include_paths = [real_directory(root, "src/core/event_bus/include", "event-bus include")]
    include_paths.extend(
        real_directory(root, item, f"{identifier}.include_roots")
        for item in include_roots
    )
    cflags, pkg_libraries = pkg_config_flags(pkg_config, packages)
    system_flags = [
        IMPORTED_TARGET_FLAGS.get(library, f"-l{library}") for library in libraries
    ]
    binary = output / module["binary"]
    return [
        cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        *(f"-I{path}" for path in include_paths), *cflags, str(main),
        *(str(path) for path in owned_sources), *(str(path) for path in core_sources),
        *pkg_libraries, *system_flags, "-o", str(binary),
    ]


def build(bundle: Path, output: Path, root: Path, cc: str, pkg_config: str) -> int:
    modules = load_builds(bundle)
    output.mkdir(parents=True, exist_ok=True)
    for module in modules:
        command = compiler_command(module, root, bundle, output, cc, pkg_config)
        try:
            subprocess.run(command, check=True)
        except (OSError, subprocess.CalledProcessError) as exc:
            fail(f"{module['id']}: compiler failed: {exc}")
        binary = output / str(module["binary"])
        if not binary.is_file() or binary.is_symlink():
            fail(f"{module['id']}: compiler did not create {binary}")
    return len(modules)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--pkg-config", default="pkg-config")
    args = parser.parse_args(argv)
    try:
        count = build(args.bundle.resolve(), args.output.resolve(), args.root.resolve(),
                      args.cc, args.pkg_config)
    except BuildError as exc:
        print(f"build_c_module_runtime_bundle: error: {exc}", file=sys.stderr)
        return 1
    print(f"build_c_module_runtime_bundle: ok ({count} C module(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
