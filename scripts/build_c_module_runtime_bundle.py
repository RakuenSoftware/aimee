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
import tempfile
from typing import NoReturn

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
from generate_c_embedded_header import GenerationError, generate


ROOT = Path(__file__).resolve().parent.parent
BUILD_MANIFEST = "c-build.json"
MODULE_ID = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
BUILD_TOKEN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:+-]*$")
MODULE_KEYS = {
    "id", "binary", "main", "sources", "include_roots", "pkg_config", "system_libraries",
}
MODULE_OPTIONAL_KEYS = {"compile_definitions", "generated_headers", "header_dependencies"}
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


def generated_headers(value: object, label: str) -> list[dict[str, object]]:
    if value is None:
        return []
    if not isinstance(value, list):
        fail(f"{label} must be an array")
    result: list[dict[str, object]] = []
    previous_output = ""
    for index, header in enumerate(value):
        item_label = f"{label}[{index}]"
        if not isinstance(header, dict) or set(header) != {"entries", "output"}:
            fail(f"{item_label} must contain only entries and output")
        output = header["output"]
        pure_output = PurePosixPath(output) if isinstance(output, str) else None
        if (not isinstance(output, str) or "\\" in output or output <= previous_output or
                pure_output is None or pure_output.name != output or pure_output.suffix != ".h"):
            fail(f"{label} outputs must be sorted unique .h basenames")
        previous_output = output
        entries = header["entries"]
        if not isinstance(entries, list) or not entries:
            fail(f"{item_label}.entries must be a nonempty array")
        parsed: list[dict[str, str]] = []
        previous_entry: tuple[str, str] | None = None
        seen_symbols: set[str] = set()
        for entry_index, entry in enumerate(entries):
            entry_label = f"{item_label}.entries[{entry_index}]"
            if not isinstance(entry, dict) or set(entry) != {"source", "symbol"}:
                fail(f"{entry_label} must contain only source and symbol")
            source, symbol = entry["source"], entry["symbol"]
            source = safe_relative(source, f"{entry_label}.source")
            if not isinstance(symbol, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol):
                fail(f"{entry_label}.symbol is not a safe C identifier")
            key = (symbol, source)
            if previous_entry is not None and key <= previous_entry:
                fail(f"{item_label}.entries must be sorted and unique")
            if symbol in seen_symbols:
                fail(f"{item_label}.entry symbols must be unique")
            previous_entry = key
            seen_symbols.add(symbol)
            parsed.append({"source": source, "symbol": symbol})
        result.append({"output": output, "entries": parsed})
    return result


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
        if (not isinstance(module, dict) or not MODULE_KEYS <= set(module) or
                not set(module) <= MODULE_KEYS | MODULE_OPTIONAL_KEYS):
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
        definitions = string_array(module.get("compile_definitions", []),
                                   f"{identifier}.compile_definitions")
        if any(not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", item)
               for item in definitions):
            fail(f"{identifier}.compile_definitions contains an unsafe C identifier")
        generated_headers(module.get("generated_headers"), f"{identifier}.generated_headers")
        dependencies = string_array(
            module.get("header_dependencies", []),
            f"{identifier}.header_dependencies", paths=True,
        )
        if any(PurePosixPath(dependency).suffix != ".h" for dependency in dependencies):
            fail(f"{identifier}: header_dependencies must all be header files")
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


def materialize_generated_headers(module: dict[str, object], root: Path,
                                  destination: Path) -> Path | None:
    identifier = module["id"]
    assert isinstance(identifier, str)
    specifications = generated_headers(
        module.get("generated_headers"), f"{identifier}.generated_headers"
    )
    if not specifications:
        return None
    destination.mkdir(parents=True, exist_ok=True)
    for header in specifications:
        output = header["output"]
        entries = header["entries"]
        assert isinstance(output, str) and isinstance(entries, list)
        inputs = [
            (str(entry["symbol"]), real_file(root, str(entry["source"]),
                                             f"{identifier}.generated_headers"))
            for entry in entries
        ]
        try:
            generate(destination / output, inputs)
        except GenerationError as exc:
            fail(f"{identifier}: cannot generate {output}: {exc}")
    return destination


def compiler_command(module: dict[str, object], root: Path, bundle: Path, output: Path,
                     cc: str, pkg_config: str,
                     generated_include: Path | None = None) -> list[str]:
    identifier = module["id"]
    assert isinstance(identifier, str)
    sources = module["sources"]
    include_roots = module["include_roots"]
    definitions = module.get("compile_definitions", [])
    packages = module["pkg_config"]
    libraries = module["system_libraries"]
    assert all(isinstance(value, list) for value in (
        sources, include_roots, definitions, packages, libraries
    ))
    main = real_file(bundle, module["main"], f"{identifier}.main")
    owned_sources = [real_file(root, item, f"{identifier}.sources") for item in sources]
    for dependency in module.get("header_dependencies", []):
        real_file(root, dependency, f"{identifier}.header_dependencies")
    core_sources = [real_file(root, item, "event-bus source")
                    for item in CORE_EVENT_BUS_SOURCES]
    include_paths = [real_directory(root, "src/core/event_bus/include", "event-bus include")]
    if module.get("generated_headers"):
        if generated_include is None:
            fail(f"{identifier}: generated header directory was not materialized")
        include_paths.append(real_directory(generated_include.parent, generated_include.name,
                                            f"{identifier}.generated_headers"))
    include_paths.extend(
        real_directory(root, item, f"{identifier}.include_roots")
        for item in include_roots
    )
    cflags, pkg_libraries = pkg_config_flags(pkg_config, packages)
    system_flags = [
        IMPORTED_TARGET_FLAGS.get(library, f"-l{library}") for library in libraries
    ]
    binary = output / module["binary"]
    # The dialect tracks the tree that owns these sources: src/Makefile:107 and
    # CMakeLists.txt:12,156-158 already build them with _GNU_SOURCE, section
    # splitting, and -Wno-format-truncation. A bundle that compiles the same
    # sources under different flags does not prove those sources build.
    # _GNU_SOURCE is defined empty because the event-bus sources define it that
    # way themselves, and an implicit =1 would be a -Werror redefinition.
    #
    # --gc-sections is load-bearing here, not an optimization. A module process
    # serves a few stages, not its module's whole surface; without per-function
    # sections db1 has to satisfy every symbol its 62 sources mention, which
    # drags in another module's config.c and the yaml parser behind it.
    return [
        cc, "-std=c11", "-D_GNU_SOURCE=", "-Os", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-Wno-format-truncation", "-Wno-unused-result",
        "-ffunction-sections", "-fdata-sections",
        *(f"-I{path}" for path in include_paths), *(f"-D{item}" for item in definitions),
        *cflags, str(main),
        *(str(path) for path in owned_sources), *(str(path) for path in core_sources),
        *pkg_libraries, *system_flags, "-Wl,--gc-sections", "-o", str(binary),
    ]


def placed_modules(bundle: Path, placement: str) -> set[str]:
    """Return the module IDs a placement is allowed to run.

    The grants directory is the authority, not <placement>.modules: the latter
    lists what the image STARTS, while a grant is what the image may run at all.
    A module that is granted but not started by default (db2 in kb) still needs
    its binary present, or the supervisor comes up unhealthy.

    An image must compile only its own placement. Building another placement's
    process drags that process's build dependencies into an image with no reason
    to carry them -- kb has no sqlite, server has no need of db2 -- and leaves an
    executable in an image that is never allowed to run it.
    """
    if not MODULE_ID.fullmatch(placement):
        fail(f"invalid placement name: {placement!r}")
    directory = bundle / "grants" / placement
    try:
        entries = sorted(item.name for item in directory.iterdir())
    except OSError as exc:
        fail(f"cannot read grants for placement {placement!r}: {exc}")
    identifiers: set[str] = set()
    for name in entries:
        if not name.endswith(".grant"):
            fail(f"grants/{placement}: unexpected entry {name!r}")
        identifier = name[: -len(".grant")]
        if not MODULE_ID.fullmatch(identifier):
            fail(f"grants/{placement}: invalid module id {identifier!r}")
        identifiers.add(identifier)
    return identifiers


def build(bundle: Path, output: Path, root: Path, cc: str, pkg_config: str,
          placement: str | None = None) -> int:
    modules = load_builds(bundle)
    if placement is not None:
        placed = placed_modules(bundle, placement)
        modules = [module for module in modules if module["id"] in placed]
    output.mkdir(parents=True, exist_ok=True)
    for module in modules:
        with tempfile.TemporaryDirectory(prefix=f"aimee-{module['id']}-generated-") as temporary:
            generated = materialize_generated_headers(module, root, Path(temporary))
            command = compiler_command(
                module, root, bundle, output, cc, pkg_config, generated
            )
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
    parser.add_argument("--placement",
                        help="build only the C processes this placement runs")
    args = parser.parse_args(argv)
    try:
        count = build(args.bundle.resolve(), args.output.resolve(), args.root.resolve(),
                      args.cc, args.pkg_config, args.placement)
    except BuildError as exc:
        print(f"build_c_module_runtime_bundle: error: {exc}", file=sys.stderr)
        return 1
    print(f"build_c_module_runtime_bundle: ok ({count} C module(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
