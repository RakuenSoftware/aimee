#!/usr/bin/env python3
"""Materialize the versioned C core and language-specific module repositories.

The monorepo remains a checked vendored mirror during the migration.  This
export is deliberately fail-closed: it only writes into a new output directory,
copies the files declared by each canonical module descriptor, initializes each
result as an independent Git repository, and emits the exact commit/source pins
consumed by ``check_c_repository_lock.py``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
from pathlib import PurePosixPath
import re
import shutil
import subprocess
import sys

import validate_module_process_contracts as process_contracts


ROOT = Path(__file__).resolve().parent.parent
INVENTORY = ROOT / "tests/baselines/modules/canonical-inventory.yaml"
LOCK = ROOT / "dependencies/aimee-repositories.lock.json"
CORE_VERSION_FILE = ROOT / "src/core/VERSION"
REMOTE_ROOT = "https://github.com/RakuenSoftware"
HOSTED_BY_EXECUTABLE = {"wfe": "/usr/local/bin/aimee-wfe"}
PRINCIPAL_CLASS = 1
C_BUILD_KEYS = {"include_roots", "pkg_config", "system_libraries"}
# Optional validated preprocessor switches let a descriptor reproduce its
# audited standalone mode. Third-party sources a module compiles but does not
# own are also optional; they remain restricted to src/vendor/ so "not owned"
# cannot quietly mean "owned by somebody else".
C_BUILD_OPTIONAL_KEYS = {
    "compile_definitions", "generated_headers", "header_dependencies", "shared_sources",
    "vendor_sources",
}
VENDOR_ROOT = "src/vendor/"
# First-party utilities that belong to no module and that several compile.
# The same rule vendor_sources follows -- one copy, owned by nobody, compiled by
# whoever needs it -- applied to code we wrote. The list is explicit rather than
# a directory prefix because "shared" must stay a decision somebody made, not a
# door onto the core tree: a module reaching for storage or config through here
# would be linking the daemon back together one file at a time.
#
# A copy would be the alternative, and a copy of a growable string is not free:
# DB2 already promoted its own from src/dstr.c, and a fix to one is silently not
# a fix to the others.
SHARED_SOURCES = ("src/dstr.c",)
BUILD_TOKEN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:+-]*$")
C_DEFINE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
CMAKE_TARGET_PACKAGES = {
    "OpenSSL::Crypto": "OpenSSL",
    "OpenSSL::SSL": "OpenSSL",
    "Threads::Threads": "Threads",
    "ZLIB::ZLIB": "ZLIB",
}


class ExportError(RuntimeError):
    """An operator-readable export failure."""


def load_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ExportError(f"cannot load {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ExportError(f"{path}: expected a JSON object")
    return value


def digest_files(paths: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in sorted(paths):
        relative = path.relative_to(ROOT).as_posix()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
        digest.update(b"\n")
    return digest.hexdigest()


def copy_file(relative: str, destination: Path) -> None:
    source = ROOT / relative
    if not source.is_file() or source.is_symlink():
        raise ExportError(f"declared repository file is missing or not regular: {relative}")
    target = destination / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def write_text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")


def git(repository: Path, *args: str, env: dict[str, str] | None = None) -> str:
    command = ["git", "-C", str(repository), *args]
    try:
        completed = subprocess.run(
            command,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = getattr(exc, "stderr", "") or str(exc)
        raise ExportError(f"{' '.join(command)}: {detail.strip()}") from exc
    return completed.stdout.strip()


def initialize_repository(repository: Path, remote: str, timestamp: str, version: str) -> str:
    git(repository, "init", "--quiet", "--initial-branch=main")
    git(repository, "remote", "add", "origin", remote)
    git(repository, "add", ".")
    environment = os.environ.copy()
    environment.update(
        {
            "GIT_AUTHOR_DATE": timestamp,
            "GIT_COMMITTER_DATE": timestamp,
            "GIT_AUTHOR_NAME": "Aimee repository export",
            "GIT_AUTHOR_EMAIL": "repository-export@aimee.local",
            "GIT_COMMITTER_NAME": "Aimee repository export",
            "GIT_COMMITTER_EMAIL": "repository-export@aimee.local",
        }
    )
    git(repository, "commit", "--quiet", "-m", "chore: initialize extracted repository", env=environment)
    git(repository, "tag", f"v{version}")
    return git(repository, "rev-parse", "HEAD")


def source_timestamp() -> str:
    try:
        return subprocess.run(
            ["git", "-C", str(ROOT), "show", "-s", "--format=%cI", "HEAD"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ExportError(f"cannot read source commit timestamp: {exc}") from exc


def core_files() -> list[Path]:
    files = [path for path in (ROOT / "src/core").rglob("*") if path.is_file() and not path.is_symlink()]
    if not files:
        raise ExportError("src/core contains no files")
    return files


def export_core(output_root: Path, timestamp: str) -> dict[str, object]:
    repository = output_root / "aimee-core-c"
    repository.mkdir()
    shutil.copytree(ROOT / "src/core", repository, dirs_exist_ok=True)
    shutil.copytree(ROOT / "examples/c-connection-client", repository / "examples/connection-client")
    shutil.copytree(ROOT / "examples/c-event-bus-module", repository / "examples/event-bus-module")
    shutil.copy2(ROOT / "LICENSE", repository / "LICENSE")
    shutil.copy2(ROOT / "NOTICE", repository / "NOTICE")
    version = CORE_VERSION_FILE.read_text(encoding="utf-8").strip()
    for example in ("connection-client", "event-bus-module"):
        cmake_file = repository / "examples" / example / "CMakeLists.txt"
        cmake_text = cmake_file.read_text(encoding="utf-8")
        cmake_text = re.sub(
            r"find_package\(aimee-core [^) ]+ EXACT CONFIG REQUIRED\)",
            f"find_package(aimee-core {version} EXACT CONFIG REQUIRED)",
            cmake_text,
        )
        write_text(cmake_file, cmake_text)
    write_text(
        repository / "README.md",
        f"""# Aimee C core

Version `{version}` is the single C implementation of connection, TLS/mTLS,
Bearer authentication, deadlines/cancellation, and HTTP framing used by the
thin client, server, and KB.  On Linux it also provides the local shared-memory
event bus used independently inside each server or KB container.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /opt/aimee-core
```
""",
    )
    write_text(
        repository / ".github/workflows/ci.yml",
        """name: core-c
on: [push, pull_request]
permissions:
  contents: read
jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=\"$PWD/prefix\"
        shell: bash
      - run: cmake --build build --config Release --parallel 2 && cmake --install build --config Release
        shell: bash
      - run: cmake -S examples/connection-client -B consumer -DCMAKE_PREFIX_PATH=\"$PWD/prefix\" && cmake --build consumer --config Release --parallel 2
        shell: bash
      - if: runner.os == 'Linux'
        run: cmake -S examples/event-bus-module -B module -DCMAKE_PREFIX_PATH=\"$PWD/prefix\" && cmake --build module --config Release --parallel 2
        shell: bash
""",
    )
    remote = f"{REMOTE_ROOT}/aimee-core-c.git"
    commit = initialize_repository(repository, remote, timestamp, version)
    return {
        "id": "aimee-core-c",
        "repository": remote,
        "ref": f"v{version}",
        "version": version,
        "commit": commit,
        "source_sha256": digest_files(core_files()),
    }


def module_owned_files(module_id: str, descriptor: dict[str, object]) -> list[str]:
    """Return descriptor-owned source-tree inputs for the module repository."""
    result = [f"src/modules/{module_id}/module.yaml"]
    for key in ("sources", "private_headers", "public_headers", "contracts", "tests", "docs",
                "go_sources", "go_tests"):
        values = descriptor.get(key, [])
        if not isinstance(values, list) or not all(isinstance(item, str) for item in values):
            raise ExportError(f"{module_id}: descriptor field {key} must be a string array")
        result.extend(values)
    build = descriptor.get("c_build")
    if isinstance(build, dict):
        for header in c_generated_headers(module_id, build):
            entries = header["entries"]
            assert isinstance(entries, list)
            result.extend(str(entry["source"]) for entry in entries)
    if len(result) != len(set(result)):
        raise ExportError(f"{module_id}: duplicate owned file")
    return result


def module_repository_files(module_id: str, descriptor: dict[str, object]) -> list[str]:
    """Return owned files plus explicit non-owned standalone build inputs."""
    owned = module_owned_files(module_id, descriptor)
    build = descriptor.get("c_build")
    dependencies = c_header_dependencies(module_id, build) if isinstance(build, dict) else []
    result = [*owned, *dependencies]
    if len(result) != len(set(result)):
        raise ExportError(f"{module_id}: duplicate repository file")
    return result


NAME_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def c_process_init_symbol(module_id: str, descriptor: dict) -> str | None:
    """The symbol the process must call before it serves, if it declares one.

    A module owning process-wide state -- an open database, a cache -- has to
    build it before the runtime registers its stages. Declaring the symbol here
    rather than hard-coding a call keeps the generated main the same shape for
    every module, and makes "this module needs setting up" a reviewable line in
    its descriptor instead of something the reader has to infer from absence.
    """
    symbol = descriptor.get("c_init")
    if symbol is None:
        return None
    if not isinstance(symbol, str) or not NAME_PATTERN.fullmatch(symbol):
        raise ExportError(f"{module_id}: c_init must be a C identifier")
    return symbol


def module_main(
    module_id: str,
    principal_ref: int,
    stages: list[dict[str, object]],
    has_handler: bool = False,
    init_symbol: str | None = None,
) -> str:
    entries = "\n".join(
        f"   {{{stage['event_kind']}u, {stage['id']}u}},"
        for stage in stages
    )
    handler_declaration = """
extern aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *, const uint8_t *, uint32_t, uint8_t *, uint32_t,
    uint32_t *, void *);
""" if has_handler else ""
    handler_value = "aimee_module_handler" if has_handler else "NULL"
    # A module that owns process-wide state has to open it before it serves.
    # Without this the process attaches, registers its stages and answers every
    # one of them against an unopened store -- which reads to a caller as "the
    # row is not there" rather than "this module cannot serve", so the failure
    # is invisible exactly where it matters.
    init_declaration = f"""
extern int {init_symbol}(void);
""" if init_symbol else ""
    init_call = f"""   if ({init_symbol}() != 0)
   {{
      fprintf(stderr, "{module_id}: {init_symbol} failed; refusing to serve\\n");
      return 1;
   }}
""" if init_symbol else ""
    return f"""#include <aimee/core/event_bus/module_runtime.h>

#include <stdio.h>
{handler_declaration}{init_declaration}

static const aimee_module_stage_t stages[] = {{
{entries}
}};

int main(int argc, char **argv)
{{
   if (argc != 2)
   {{
      fprintf(stderr, "usage: %s DAEMON_MODULE_BUS_SOCKET\\n", argv[0]);
      return 2;
   }}
{init_call}   const aimee_module_process_config_t config = {{
       .socket_path = argv[1],
       .module_name = "{module_id}",
       .principal_class = {PRINCIPAL_CLASS}u,
       .principal_ref = {principal_ref}u,
       .stages = stages,
       .stage_count = sizeof stages / sizeof stages[0],
       .handler = {handler_value},
   }};
   return aimee_module_process_run(&config);
}}
"""


def c_header_dependencies(module_id: str, build: dict[str, object]) -> list[str]:
    """Return validated non-owned headers copied into an isolated C export."""
    dependencies = build.get("header_dependencies", [])
    if not isinstance(dependencies, list) or not all(
            isinstance(item, str) for item in dependencies):
        raise ExportError(f"{module_id}: c_build.header_dependencies must be a string array")
    if dependencies != sorted(set(dependencies)):
        raise ExportError(
            f"{module_id}: c_build.header_dependencies must be sorted and unique")
    module_prefix = f"src/modules/{module_id}/"
    for dependency in dependencies:
        pure = PurePosixPath(dependency)
        if (not dependency.startswith("src/") or "\\" in dependency or pure.is_absolute() or
                "." in pure.parts or ".." in pure.parts or
                pure.as_posix() != dependency or pure.suffix != ".h"):
            raise ExportError(f"{module_id}: unsafe header dependency {dependency!r}")
        if dependency.startswith(module_prefix):
            raise ExportError(
                f"{module_id}: module-local header dependency {dependency!r} must be owned")
    return list(dependencies)


def c_generated_headers(module_id: str, build: dict[str, object]) -> list[dict[str, object]]:
    """Return validated deterministic text-header declarations."""
    generated = build.get("generated_headers", [])
    if not isinstance(generated, list):
        raise ExportError(f"{module_id}: c_build.generated_headers must be an array")
    result: list[dict[str, object]] = []
    previous_output = ""
    for index, header in enumerate(generated):
        if not isinstance(header, dict) or set(header) != {"entries", "output"}:
            raise ExportError(f"{module_id}: generated header {index} has invalid keys")
        output = header["output"]
        pure_output = PurePosixPath(output) if isinstance(output, str) else None
        if (not isinstance(output, str) or "\\" in output or output <= previous_output or
                pure_output is None or pure_output.name != output or pure_output.suffix != ".h"):
            raise ExportError(
                f"{module_id}: generated header outputs must be sorted unique .h basenames")
        previous_output = output
        entries = header["entries"]
        if not isinstance(entries, list) or not entries:
            raise ExportError(f"{module_id}: {output} entries must be a nonempty array")
        parsed: list[dict[str, str]] = []
        previous_entry: tuple[str, str] | None = None
        seen_symbols: set[str] = set()
        for entry in entries:
            if not isinstance(entry, dict) or set(entry) != {"source", "symbol"}:
                raise ExportError(f"{module_id}: {output} entry has invalid keys")
            source, symbol = entry["source"], entry["symbol"]
            pure_source = PurePosixPath(source) if isinstance(source, str) else None
            if (not isinstance(source, str) or not source or "\\" in source or
                    pure_source is None or pure_source.is_absolute() or
                    "." in pure_source.parts or ".." in pure_source.parts or
                    pure_source.as_posix() != source):
                raise ExportError(f"{module_id}: unsafe generated input {source!r}")
            if not isinstance(symbol, str) or not C_DEFINE.fullmatch(symbol):
                raise ExportError(f"{module_id}: unsafe generated symbol {symbol!r}")
            key = (symbol, source)
            if previous_entry is not None and key <= previous_entry:
                raise ExportError(f"{module_id}: {output} entries must be sorted and unique")
            if symbol in seen_symbols:
                raise ExportError(f"{module_id}: {output} symbols must be unique")
            previous_entry = key
            seen_symbols.add(symbol)
            parsed.append({"source": source, "symbol": symbol})
        result.append({"output": output, "entries": parsed})
    return result


def c_process_build(module_id: str, descriptor: dict[str, object]) -> tuple[
        list[str], list[str], list[str], list[dict[str, object]], list[str], list[str]]:
    """Return validated C sources, includes, definitions, codegen, pkg-config, and links."""
    sources = descriptor.get("sources")
    if not isinstance(sources, list) or not sources or not all(isinstance(item, str) for item in sources):
        raise ExportError(f"{module_id}: C process must declare descriptor-owned sources")
    c_sources = [item for item in sources if PurePosixPath(item).suffix == ".c"]
    if len(c_sources) != len(sources):
        raise ExportError(f"{module_id}: C process sources must all be .c files")
    if c_sources != sorted(set(c_sources)):
        raise ExportError(f"{module_id}: C process sources must be sorted and unique")
    build = descriptor.get("c_build")
    if not isinstance(build, dict) or not C_BUILD_KEYS <= set(build) or \
            not set(build) <= C_BUILD_KEYS | C_BUILD_OPTIONAL_KEYS:
        raise ExportError(f"{module_id}: C process must declare exact c_build fields")
    vendored = build.get("vendor_sources", [])
    if not isinstance(vendored, list) or not all(isinstance(item, str) for item in vendored):
        raise ExportError(f"{module_id}: c_build.vendor_sources must be a string array")
    if vendored != sorted(set(vendored)):
        raise ExportError(f"{module_id}: c_build.vendor_sources must be sorted and unique")
    for entry in vendored:
        pure = PurePosixPath(entry)
        if (not entry.startswith(VENDOR_ROOT) or pure.is_absolute() or ".." in pure.parts or
                pure.suffix != ".c" or pure.as_posix() != entry):
            raise ExportError(
                f"{module_id}: vendor_sources entry {entry!r} must be a .c file under "
                f"{VENDOR_ROOT}; anything else is a source some module owns")

    shared = build.get("shared_sources", [])
    if not isinstance(shared, list) or not all(isinstance(item, str) for item in shared):
        raise ExportError(f"{module_id}: c_build.shared_sources must be a string array")
    if shared != sorted(set(shared)):
        raise ExportError(f"{module_id}: c_build.shared_sources must be sorted and unique")
    for entry in shared:
        if entry not in SHARED_SOURCES:
            raise ExportError(
                f"{module_id}: shared_sources entry {entry!r} is not one of the shared "
                f"first-party utilities ({', '.join(SHARED_SOURCES)}); anything else is a "
                f"source some module owns")

    definitions = build.get("compile_definitions", [])
    if not isinstance(definitions, list) or not all(
            isinstance(item, str) for item in definitions):
        raise ExportError(f"{module_id}: c_build.compile_definitions must be a string array")
    if definitions != sorted(set(definitions)):
        raise ExportError(
            f"{module_id}: c_build.compile_definitions must be sorted and unique")
    for definition in definitions:
        if not C_DEFINE.fullmatch(definition):
            raise ExportError(
                f"{module_id}: unsafe c_build.compile_definitions token {definition!r}")

    c_header_dependencies(module_id, build)

    parsed: dict[str, list[str]] = {}
    for field in sorted(C_BUILD_KEYS):
        entries = build[field]
        if not isinstance(entries, list) or not all(isinstance(item, str) for item in entries):
            raise ExportError(f"{module_id}: c_build.{field} must be a string array")
        if entries != sorted(set(entries)):
            raise ExportError(f"{module_id}: c_build.{field} must be sorted and unique")
        if field == "include_roots" and not entries:
            raise ExportError(f"{module_id}: c_build.include_roots must not be empty")
        for entry in entries:
            if field == "include_roots":
                pure = PurePosixPath(entry)
                if (not entry or "\\" in entry or pure.is_absolute() or "." in pure.parts or
                        ".." in pure.parts or pure.as_posix() != entry):
                    raise ExportError(f"{module_id}: unsafe include root {entry!r}")
            elif not BUILD_TOKEN.fullmatch(entry):
                raise ExportError(f"{module_id}: unsafe c_build.{field} token {entry!r}")
            elif field == "system_libraries" and "::" in entry and entry not in CMAKE_TARGET_PACKAGES:
                raise ExportError(f"{module_id}: unsupported imported CMake target {entry!r}")
        parsed[field] = entries
    # Vendored sources compile with the module but are not its own: they are
    # appended here rather than merged into `sources`, so ownership keeps
    # meaning what it says everywhere else.
    # Sorted as one list: the bundle requires it, and vendor_sources only ever
    # satisfied that by accident, since "src/vendor" happened to sort after the
    # module's own paths. A shared source under src/ does not.
    return (sorted(c_sources + list(shared) + list(vendored)), parsed["include_roots"],
            definitions,
            c_generated_headers(module_id, build),
            parsed["pkg_config"], parsed["system_libraries"])


def c_process_cmake(module_id: str, binary: str, version: str,
                    descriptor: dict[str, object]) -> str:
    """Generate the standalone build for a descriptor-owned C process."""
    sources, include_roots, definitions, generated, pkg_config, libraries = c_process_build(
        module_id, descriptor)
    source_lines = "\n".join(f"    {source}" for source in sources)
    generated_setup = ""
    generated_source_lines = ""
    generated_include_line = ""
    if generated:
        commands: list[str] = [
            "find_package(Python3 REQUIRED COMPONENTS Interpreter)",
            'set(MODULE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")',
        ]
        outputs: list[str] = []
        for header in generated:
            output = str(header["output"])
            entries = header["entries"]
            assert isinstance(entries, list)
            output_expr = f'${{MODULE_GENERATED_DIR}}/{output}'
            outputs.append(output_expr)
            entry_lines = "\n".join(
                f'            --entry {entry["symbol"]} '
                f'"${{CMAKE_CURRENT_SOURCE_DIR}}/{entry["source"]}"'
                for entry in entries
            )
            dependency_lines = "\n".join(
                f'        "${{CMAKE_CURRENT_SOURCE_DIR}}/{entry["source"]}"'
                for entry in entries
            )
            commands.append(f"""add_custom_command(
    OUTPUT "{output_expr}"
    COMMAND ${{CMAKE_COMMAND}} -E make_directory "${{MODULE_GENERATED_DIR}}"
    COMMAND ${{Python3_EXECUTABLE}}
            "${{CMAKE_CURRENT_SOURCE_DIR}}/scripts/generate_c_embedded_header.py"
            --output "{output_expr}"
{entry_lines}
    DEPENDS
        "${{CMAKE_CURRENT_SOURCE_DIR}}/scripts/generate_c_embedded_header.py"
{dependency_lines}
    VERBATIM
)
""")
        generated_setup = "\n".join(commands) + "\n"
        generated_source_lines = "\n".join(f'    "{output}"' for output in outputs) + "\n"
        generated_include_line = "    ${MODULE_GENERATED_DIR}\n"
    include_lines = "\n".join(
        f"    ${{CMAKE_CURRENT_SOURCE_DIR}}/{root}" for root in include_roots
    )
    definition_lines = "\n".join(f"    {definition}" for definition in definitions)
    definition_block = ""
    if definition_lines:
        definition_block = f"""target_compile_definitions({binary} PRIVATE
{definition_lines}
)
"""
    package_names = sorted({
        CMAKE_TARGET_PACKAGES[library]
        for library in libraries
        if library in CMAKE_TARGET_PACKAGES
    })
    discovery: list[str] = [f"find_package({package} REQUIRED)" for package in package_names]
    link_items = ["aimee::aimee-core-event-bus-client"]
    if pkg_config:
        discovery.extend([
            "find_package(PkgConfig REQUIRED)",
            f"pkg_check_modules(MODULE_PKG REQUIRED IMPORTED_TARGET {' '.join(pkg_config)})",
        ])
        link_items.append("PkgConfig::MODULE_PKG")
    link_items.extend(libraries)
    discovery_text = "\n".join(discovery)
    if discovery_text:
        discovery_text += "\n"
    link_lines = "\n".join(f"    {item}" for item in link_items)
    return f"""cmake_minimum_required(VERSION 3.16)
project(aimee_module_{module_id.replace('-', '_')} VERSION {version} LANGUAGES C)

include(GNUInstallDirs)
find_package(aimee-core {version} EXACT CONFIG REQUIRED)
{discovery_text}{generated_setup}if(NOT TARGET aimee::aimee-core-event-bus-client)
    message(FATAL_ERROR "{binary} requires the Linux event-bus client")
endif()
add_executable({binary}
    runtime/main.c
{generated_source_lines}\
{source_lines}
)
target_compile_features({binary} PRIVATE c_std_11)
target_include_directories({binary} PRIVATE
{generated_include_line}\
{include_lines}
)
{definition_block}target_link_libraries({binary} PRIVATE
{link_lines}
)
configure_file(grants/module.grant.in ${{CMAKE_CURRENT_BINARY_DIR}}/{module_id}.grant @ONLY)
install(TARGETS {binary} RUNTIME DESTINATION ${{CMAKE_INSTALL_BINDIR}})
install(FILES ${{CMAKE_CURRENT_BINARY_DIR}}/{module_id}.grant
        DESTINATION ${{CMAKE_INSTALL_DATADIR}}/aimee/module-grants)
"""


def c_process_has_handler(sources: list[str]) -> bool:
    """A C process owns a handler when any declared source is its module adapter."""
    return any(PurePosixPath(source).name == "module_adapter.c" for source in sources)


def go_module_main(module_id: str, principal_ref: int,
                   stages: list[dict[str, object]]) -> str:
    """Generate one independently buildable Go process entry point."""
    entries = "\n".join(
        f"\t\t{{EventKind: {stage['event_kind']}, StageID: {stage['id']}}},"
        for stage in stages
    )
    handler = "handler.NewDefaultHandler()" if module_id == "delegates" else "handler.Handle"
    watchdog = """\tif handled, code := handler.RunWatchdog(os.Args); handled {
\t\tos.Exit(code)
\t}
""" if module_id == "delegates" else ""
    cleanup = "\tdefer handler.Close()\n" if module_id == "postgres" else ""
    setup = ""
    if module_id == "config":
        handler = "moduleHandler"
        setup = """\tmoduleHandler, err := handler.NewDefaultHandler()
\tif err != nil {
\t\tfmt.Fprintf(os.Stderr, "aimee-module-config: %v\\n", err)
\t\tos.Exit(1)
\t}
"""
    return f"""package main

import (
\t"context"
\t"fmt"
\t"os"
\t"os/signal"
\t"syscall"

\t"github.com/JBailes/aimee/server-go/bus"
\thandler "github.com/JBailes/aimee/server-go/modules/{module_id}"
)

func main() {{
{watchdog}\
{cleanup}\
{setup}\
\tif len(os.Args) != 2 {{
\t\tfmt.Fprintf(os.Stderr, "usage: %s DAEMON_MODULE_BUS_SOCKET\\n", os.Args[0])
\t\tos.Exit(2)
\t}}
\tctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
\tdefer stop()
\tconfig := bus.ModuleProcessConfig{{
\t\tSocketPath: os.Args[1], ModuleName: "{module_id}",
\t\tPrincipalClass: {PRINCIPAL_CLASS}, PrincipalRef: {principal_ref},
\t\tStages: []bus.ModuleStage{{
{entries}
\t\t}},
\t\tHandler: {handler},
\t}}
\tif err := bus.RunModuleProcess(ctx, config); err != nil {{
\t\tfmt.Fprintf(os.Stderr, "aimee-module-{module_id}: %v\\n", err)
\t\tos.Exit(1)
\t}}
}}
"""


def go_bus_sources(module_id: str | None = None) -> list[str]:
    """Return the canonical, non-test Go bus implementation shared by exports."""
    return sorted(
        path.relative_to(ROOT).as_posix()
        for path in (ROOT / "server-go/bus").glob("*.go")
        if not path.name.endswith("_test.go") and
        (path.name != "concurrent_module_caller.go" or module_id in {"delegates", "roundtable"})
    )


# Caller-side contracts that live outside any implementation module, mapped to
# the modules that import them. Each is deliberately not owned by the module it
# talks to: every peer that calls delegates may import server-go/delegate, and
# every peer that keeps state in DB1 may import server-go/db1, without importing
# the serving module. Add entries here in lockstep with the caller's process
# contract and runtime-bundle coverage.
GO_SHARED_CONTRACTS = {
    "server-go/config": {"config"},
    "server-go/delegate": {"delegates", "roundtable"},
    "server-go/db1": {"economizer"},
}


def go_process_shared_sources(module_id: str) -> list[str]:
    """Return shared caller contracts needed by independently built Go modules."""
    sources: list[str] = []
    for directory, importers in GO_SHARED_CONTRACTS.items():
        if module_id not in importers:
            continue
        sources.extend(
            path.relative_to(ROOT).as_posix()
            for path in (ROOT / directory).glob("*.go")
            if not path.name.endswith("_test.go") or
            (module_id == "config" and directory == "server-go/config")
        )
    return sorted(sources)


def go_dependency_version(module_path: str) -> str:
    """Read an external dependency version from the canonical Go module."""
    text = (ROOT / "server-go/go.mod").read_text(encoding="utf-8")
    match = re.search(
        rf"^\s*{re.escape(module_path)}\s+(v\S+)\s*(?://.*)?$", text, re.MULTILINE
    )
    if match is None:
        raise ExportError(f"server-go/go.mod: missing {module_path} dependency")
    return match.group(1)


def external_module_pin(
    module_id: str,
    classification: str,
    descriptor: dict[str, object],
    contract: dict[str, object],
) -> dict[str, object] | None:
    """Return the canonical pin for an externally maintained module."""
    external = descriptor.get("external_source")
    if external is None:
        return None
    if not isinstance(external, dict):
        raise ExportError(f"{module_id}: malformed external_source")
    module_path = external.get("module")
    repository_url = external.get("repository")
    if not isinstance(module_path, str) or not isinstance(repository_url, str):
        raise ExportError(f"{module_id}: malformed external_source")
    dependency_version = go_dependency_version(module_path)
    source_paths = [ROOT / item for item in module_owned_files(module_id, descriptor)]
    pin: dict[str, object] = {
        "id": module_id,
        "classification": classification,
        "repository": repository_url + ".git",
        "ref": dependency_version,
        "version": dependency_version,
        "commit": dependency_version.rsplit("-", 1)[-1],
        "execution": contract["execution"],
        "placements": contract["placements"],
        "source_sha256": digest_files(source_paths),
    }
    if contract["execution"] == "process":
        pin["runtime"] = contract["runtime"]
        pin["principal_class"] = PRINCIPAL_CLASS
        pin["principal_ref"] = contract["principal_ref"]
        pin["serve"] = [stage["event_kind"] for stage in contract["stages"]]
    return pin


def go_module_requirements(module_id: str) -> tuple[list[str], list[str]]:
    """Return direct and indirect requirements for an isolated Go export."""
    direct = ["golang.org/x/sys"]
    indirect: list[str] = []
    if module_id == "config":
        direct.append("go.yaml.in/yaml/v3")
    if module_id == "postgres":
        direct.append("github.com/jackc/pgx/v5")
        indirect.extend([
            "github.com/jackc/pgpassfile",
            "github.com/jackc/pgservicefile",
            "github.com/jackc/puddle/v2",
            "golang.org/x/sync",
            "golang.org/x/text",
        ])
    direct_lines = [
        f"\t{module} {go_dependency_version(module)}" for module in sorted(direct)
    ]
    indirect_lines = [
        f"\t{module} {go_dependency_version(module)} // indirect"
        for module in sorted(indirect)
    ]
    return direct_lines, indirect_lines


def export_module(
    output_root: Path,
    module_id: str,
    classification: str,
    contract: dict[str, object],
    timestamp: str,
    version: str,
) -> dict[str, object]:
    descriptor_path = ROOT / f"src/modules/{module_id}/module.yaml"
    descriptor = load_json(descriptor_path)
    if descriptor.get("id") != module_id:
        raise ExportError(f"{descriptor_path}: descriptor id mismatch")
    pin = external_module_pin(module_id, classification, descriptor, contract)
    if pin is not None:
        return pin
    owned = module_owned_files(module_id, descriptor)
    repository_files = module_repository_files(module_id, descriptor)
    declared_sources = descriptor.get("sources", [])
    assert isinstance(declared_sources, list)
    has_handler = c_process_has_handler(declared_sources)
    init_symbol = c_process_init_symbol(module_id, descriptor)
    repository = output_root / f"aimee-module-{module_id}"
    repository.mkdir()
    for relative in repository_files:
        copy_file(relative, repository)
    shutil.copy2(ROOT / "LICENSE", repository / "LICENSE")
    binary = f"aimee-module-{module_id}"
    execution = contract["execution"]
    runtime = contract.get("runtime")
    if execution == "process":
        principal_ref = contract["principal_ref"]
        stages = contract["stages"]
        assert isinstance(principal_ref, int) and isinstance(stages, list)
        serve = ",".join(str(stage["event_kind"]) for stage in stages)
        write_text(
            repository / "grants/module.grant.in",
            f"""version=1
principal_class={PRINCIPAL_CLASS}
principal_ref={principal_ref}
uid=self
executable=@CMAKE_INSTALL_FULL_BINDIR@/{binary}
publish=
subscribe=
request=
serve={serve}
""",
        )
        if runtime == "c":
            build = descriptor.get("c_build")
            assert isinstance(build, dict)
            if c_generated_headers(module_id, build):
                copy_file("scripts/generate_c_embedded_header.py", repository)
            write_text(
                repository / "runtime/main.c",
                module_main(module_id, principal_ref, stages, has_handler, init_symbol),
            )
            write_text(
                repository / "CMakeLists.txt",
                c_process_cmake(module_id, binary, version, descriptor),
            )
            handler_text = (
                "Its repository-owned handler implements the declared stage contract."
                if has_handler
                else "The boundary returns typed `capability_absent` until its "
                     "repository-owned handler is ported; it never echoes a request or "
                     "reports false success."
            )
            runtime_text = f"""It builds `{binary}` as a separate C process for the
{', '.join(contract['placements'])} bus. Its generated grant serves exactly the
declared stage event kinds. {handler_text}
"""
            workflow = f"""name: module
on: [push, pull_request]
permissions:
  contents: read
jobs:
  linux:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/checkout@v4
        with:
          repository: RakuenSoftware/aimee-core-c
          ref: v{version}
          path: _aimee-core
      - run: cmake -S _aimee-core -B _core-build -DCMAKE_BUILD_TYPE=Release
      - run: cmake --build _core-build --parallel 2
      - run: cmake --install _core-build --prefix "$PWD/_prefix"
      - run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/_prefix"
      - run: cmake --build build --parallel 2
      - run: test -x build/{binary}
"""
        elif runtime == "go":
            go_sources = descriptor.get("go_sources", [])
            if (not isinstance(go_sources, list) or not go_sources) and \
                    "external_source" not in descriptor:
                raise ExportError(f"{module_id}: Go process has no go_sources")
            bus_sources = go_bus_sources(module_id)
            if not bus_sources:
                raise ExportError("canonical Go bus has no production sources")
            shared_sources = go_process_shared_sources(module_id)
            for relative in bus_sources:
                copy_file(relative, repository)
            for relative in shared_sources:
                copy_file(relative, repository)
            shutil.copy2(ROOT / "server-go/go.sum", repository / "go.sum")
            direct_requirements, indirect_requirements = go_module_requirements(module_id)
            require_text = "\n".join(direct_requirements)
            indirect_text = ""
            if indirect_requirements:
                indirect_text = "\nrequire (\n" + "\n".join(indirect_requirements) + "\n)\n"
            write_text(
                repository / "go.mod",
                f"""module github.com/JBailes/aimee

go 1.25.0

require (
{require_text}
){indirect_text}""",
            )
            write_text(
                repository / "runtime/main.go",
                go_module_main(module_id, principal_ref, stages),
            )
            cmake_dependencies = "\n".join(
                f"        ${{CMAKE_CURRENT_SOURCE_DIR}}/{relative}"
                for relative in ["runtime/main.go", *go_sources, *bus_sources, *shared_sources,
                                 "go.mod", "go.sum"]
            )
            write_text(
                repository / "CMakeLists.txt",
                f"""cmake_minimum_required(VERSION 3.16)
project(aimee_module_{module_id.replace('-', '_')} VERSION {version} LANGUAGES NONE)

include(GNUInstallDirs)
find_program(GO_EXECUTABLE NAMES go REQUIRED)
set(MODULE_BINARY "${{CMAKE_CURRENT_BINARY_DIR}}/{binary}")
add_custom_command(
    OUTPUT "${{MODULE_BINARY}}"
    COMMAND ${{CMAKE_COMMAND}} -E env CGO_ENABLED=0 ${{GO_EXECUTABLE}} build -trimpath
            -o "${{MODULE_BINARY}}" ./runtime
    WORKING_DIRECTORY "${{CMAKE_CURRENT_SOURCE_DIR}}"
    DEPENDS
{cmake_dependencies}
    VERBATIM)
add_custom_target({binary} ALL DEPENDS "${{MODULE_BINARY}}")
configure_file(grants/module.grant.in ${{CMAKE_CURRENT_BINARY_DIR}}/{module_id}.grant @ONLY)
install(PROGRAMS "${{MODULE_BINARY}}" DESTINATION ${{CMAKE_INSTALL_BINDIR}})
install(FILES ${{CMAKE_CURRENT_BINARY_DIR}}/{module_id}.grant
        DESTINATION ${{CMAKE_INSTALL_DATADIR}}/aimee/module-grants)
""",
            )
            if module_id == "config":
                runtime_text = f"""It builds `{binary}` as a pure-Go process for the
{', '.join(contract['placements'])} bus. The exported repository includes the
exact canonical Go bus client/runtime snapshot, caller contract, handler,
validation, and persistent store. It contains no C implementation or bridge.
"""
            else:
                runtime_text = f"""It builds `{binary}` as a separate Go process for the
{', '.join(contract['placements'])} bus. The exported repository includes the
exact canonical Go bus client/runtime snapshot and its repository-owned handler;
the retained C adapter is a wire-parity fixture, not the production executable.
"""
            workflow = f"""name: module
on: [push, pull_request]
permissions:
  contents: read
jobs:
  linux:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with:
          go-version: '1.25.x'
      - run: go test ./server-go/bus{" ./server-go/config" if module_id == "config" else ""} ./server-go/modules/{module_id}
      - run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
      - run: cmake --build build --parallel 2
      - run: test -x build/{binary}
"""
        else:
            raise ExportError(f"{module_id}: unsupported process runtime {runtime!r}")
        runtime_text += """
The daemon admits the process only when its installed absolute executable path,
UID, principal class, principal reference, and event-kind grants match the
installed `.grant` file. Copy that generated grant into each declared daemon
policy directory under `modules.d`.
"""
    else:
        write_text(
            repository / "CMakeLists.txt",
            f"""cmake_minimum_required(VERSION 3.16)
project(aimee_module_{module_id.replace('-', '_')} VERSION {version} LANGUAGES NONE)
include(GNUInstallDirs)
install(DIRECTORY src/ DESTINATION ${{CMAKE_INSTALL_DATADIR}}/aimee/sources/{module_id}/src)
install(DIRECTORY docs/ DESTINATION ${{CMAKE_INSTALL_DATADIR}}/aimee/sources/{module_id}/docs)
""",
        )
        runtime_text = """This component executes inside the trusted C core. It
does not receive a bus principal, a process shim, or a grant. The repository is
the independently versioned source owner consumed by the server/KB core build.
"""
        workflow = """name: source-package
on: [push, pull_request]
permissions:
  contents: read
jobs:
  package:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: python3 -m json.tool SOURCE_MANIFEST.json >/dev/null
      - run: cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$PWD/_prefix"
      - run: cmake --install build
"""
    repository_note = """The descriptor-owned production sources, tests, contracts,
and documentation are preserved at their canonical paths so their migration
history remains auditable.
"""
    if runtime != "go":
        repository_note += """Any non-owned compatibility headers required by the standalone C build are
declared separately, copied at canonical paths, and bound into the source digest.
"""
    write_text(repository / ".github/workflows/ci.yml", workflow)
    write_text(
        repository / "README.md",
        f"""# Aimee module: {module_id}

This is the independent `{module_id}` source-ownership repository.

{runtime_text.rstrip()}

{repository_note.rstrip()}
""",
    )
    source_paths = [ROOT / item for item in repository_files]
    manifest = {
        "schema_version": 1,
        "module": module_id,
        "classification": classification,
        "execution": execution,
        "runtime": runtime,
        "placements": contract["placements"],
        "principal_class": PRINCIPAL_CLASS if execution == "process" else None,
        "principal_ref": contract.get("principal_ref"),
        "stages": contract.get("stages", []),
        "source_sha256": digest_files(source_paths),
        "owned_files": owned,
        "repository_files": repository_files,
    }
    write_text(repository / "SOURCE_MANIFEST.json", json.dumps(manifest, indent=2) + "\n")
    remote = f"{REMOTE_ROOT}/aimee-module-{module_id}.git"
    commit = initialize_repository(repository, remote, timestamp, version)
    pin = {
        "id": module_id,
        "classification": classification,
        "repository": remote,
        "ref": f"v{version}",
        "version": version,
        "commit": commit,
        "execution": execution,
        "placements": contract["placements"],
        "source_sha256": manifest["source_sha256"],
    }
    if execution == "process":
        pin["runtime"] = contract["runtime"]
        pin["principal_class"] = PRINCIPAL_CLASS
        pin["principal_ref"] = contract["principal_ref"]
        pin["serve"] = [stage["event_kind"] for stage in contract["stages"]]
    return pin


def export_runtime_bundle(output_root: Path) -> int:
    """Emit build inputs and admission policy without creating Git repositories."""
    if output_root.exists():
        raise ExportError(f"refusing to overwrite existing output root: {output_root}")
    output_root.mkdir(parents=True)
    inventory = load_json(INVENTORY)
    required = set(inventory.get("required", []))
    contracts = process_contracts.validate()
    placement_rows: dict[str, list[str]] = {"server": [], "kb": []}
    runtimes: dict[str, str] = {}
    go_modules: list[str] = []
    c_builds: list[dict[str, object]] = []
    count = 0
    for module_id, contract in contracts.items():
        if contract["execution"] != "process":
            continue
        descriptor = load_json(ROOT / f"src/modules/{module_id}/module.yaml")
        module_repository_files(module_id, descriptor)
        enabled = module_id in required or descriptor.get("enabled_by_default") is True
        principal_ref = contract["principal_ref"]
        stages = contract["stages"]
        assert isinstance(principal_ref, int) and isinstance(stages, list)
        binary = f"aimee-module-{module_id}"
        # The grant pins the exact executable that may attach as this principal.
        # An externally hosted process is a different program at a different path.
        hosted_by = contract.get("hosted_by")
        executable = (HOSTED_BY_EXECUTABLE[hosted_by] if hosted_by
                      else f"/usr/local/libexec/aimee-modules/{binary}")
        runtime = contract["runtime"]
        assert isinstance(runtime, str)
        runtimes[module_id] = runtime
        if runtime == "c":
            sources, include_roots, definitions, generated, pkg_config, libraries = c_process_build(
                module_id, descriptor
            )
            has_handler = c_process_has_handler(sources)
            init_symbol = c_process_init_symbol(module_id, descriptor)
            main_path = f"src/{binary}.c"
            write_text(
                output_root / main_path,
                module_main(module_id, principal_ref, stages, has_handler, init_symbol),
            )
            build_row: dict[str, object] = {
                "id": module_id,
                "binary": binary,
                "main": main_path,
                "sources": sources,
                "include_roots": include_roots,
                "compile_definitions": definitions,
                "pkg_config": pkg_config,
                "system_libraries": libraries,
            }
            header_dependencies = c_header_dependencies(module_id, descriptor["c_build"])
            if header_dependencies:
                build_row["header_dependencies"] = header_dependencies
            if generated:
                build_row["generated_headers"] = generated
            c_builds.append(build_row)
        else:
            go_sources = descriptor.get("go_sources", [])
            if (not isinstance(go_sources, list) or not go_sources) and \
                    "external_source" not in descriptor:
                raise ExportError(f"{module_id}: Go process has no go_sources")
            if hosted_by is None:
                go_modules.append(module_id)
        serve = ",".join(str(stage["event_kind"]) for stage in stages)
        grant = f"""version=1
principal_class={PRINCIPAL_CLASS}
principal_ref={principal_ref}
uid=self
executable={executable}
publish=
subscribe=
request=
serve={serve}
"""
        for placement in contract["placements"]:
            write_text(output_root / "grants" / placement / f"{module_id}.grant", grant)
            # A process hosted by an already-supervised program is never spawned by
            # the module supervisor. Listing it would start a second holder of the
            # principal, and the bus denies a live duplicate.
            if enabled and hosted_by is None:
                placement_rows[placement].append(f"{module_id}\t{executable}")
        count += 1
    # Bus clients request stages but serve none, so they get a grant and no
    # manifest row: nothing supervises them, they are already running.
    contract_doc = load_json(process_contracts.CONTRACTS)
    for client in contract_doc.get("clients", []):
        request = ",".join(str(kind) for kind in client["request"])
        client_grant = f"""version=1
principal_class={PRINCIPAL_CLASS}
principal_ref={client["principal_ref"]}
uid=self
executable={client["executable"]}
publish=
subscribe=
request={request}
serve=
"""
        for placement in client["placements"]:
            write_text(output_root / "grants" / placement / f"{client['id']}.grant", client_grant)
    for placement, rows in placement_rows.items():
        write_text(output_root / f"{placement}.modules", "\n".join(rows) + "\n")
    write_text(output_root / "go.modules", "\n".join(go_modules) + "\n")
    c_builds.sort(key=lambda row: str(row["id"]))
    write_text(
        output_root / "c-build.json",
        json.dumps({"schema_version": 1, "modules": c_builds}, indent=2) + "\n",
    )
    write_text(
        output_root / "MANIFEST.json",
        json.dumps(
            {"schema_version": 1, "contracts": str(process_contracts.CONTRACTS.relative_to(ROOT)),
             "c_build": "c-build.json", "process_count": count,
             "runtimes": runtimes, "placements": placement_rows},
            indent=2,
        ) + "\n",
    )
    return count


def refresh_lock_from_repositories(repository_root: Path) -> int:
    """Pin the lock to clean, tagged repositories updated from an export."""
    lock = load_json(LOCK)
    version = CORE_VERSION_FILE.read_text(encoding="utf-8").strip()
    core = lock.get("core")
    modules = lock.get("modules")
    if not isinstance(core, dict) or not isinstance(modules, list):
        raise ExportError(f"{LOCK}: invalid lock structure")
    inventory = load_json(INVENTORY)
    required = inventory.get("required")
    optional = inventory.get("optional")
    if not isinstance(required, list) or not isinstance(optional, list):
        raise ExportError(f"{INVENTORY}: required/optional must be arrays")
    required_ids = set(required)
    known_ids = required_ids | set(optional)
    contracts = process_contracts.validate()
    entries = [core, *modules]
    for entry in entries:
        repository_id = entry.get("id")
        if not isinstance(repository_id, str):
            raise ExportError(f"{LOCK}: repository entry has no id")
        if repository_id != "aimee-core-c":
            if repository_id not in known_ids:
                raise ExportError(f"{LOCK}: unknown module repository {repository_id}")
            descriptor = load_json(ROOT / f"src/modules/{repository_id}/module.yaml")
            external_pin = external_module_pin(
                repository_id,
                "required" if repository_id in required_ids else "optional",
                descriptor,
                contracts[repository_id],
            )
            if external_pin is not None:
                entry.clear()
                entry.update(external_pin)
                continue
        directory_name = repository_id if repository_id == "aimee-core-c" else f"aimee-module-{repository_id}"
        repository = repository_root / directory_name
        if not (repository / ".git").is_dir():
            raise ExportError(f"missing Git repository: {repository}")
        if git(repository, "status", "--porcelain"):
            raise ExportError(f"repository is not clean: {repository}")
        expected_remote = entry.get("repository")
        if git(repository, "remote", "get-url", "origin") != expected_remote:
            raise ExportError(f"unexpected origin for {repository_id}")
        head = git(repository, "rev-parse", "HEAD")
        if git(repository, "rev-parse", f"v{version}^{{commit}}") != head:
            raise ExportError(f"{repository_id}: v{version} does not name HEAD")
        entry["version"] = version
        entry["ref"] = f"v{version}"
        entry["commit"] = head
        if repository_id == "aimee-core-c":
            entry["source_sha256"] = digest_files(core_files())
        else:
            descriptor = load_json(ROOT / f"src/modules/{repository_id}/module.yaml")
            repository_files = module_repository_files(repository_id, descriptor)
            entry["source_sha256"] = digest_files(
                [ROOT / relative for relative in repository_files]
            )
    LOCK.write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")
    return len(entries)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-root",
        type=Path,
        default=ROOT.parent / "aimee-module-repositories",
        help="new directory which will contain the independent repositories",
    )
    parser.add_argument(
        "--runtime-bundle",
        type=Path,
        help="emit process sources, placement manifests, and grants instead of Git repositories",
    )
    parser.add_argument(
        "--refresh-lock-root",
        type=Path,
        help="pin the lock to clean vVERSION tags in an existing repository set",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.runtime_bundle is not None and args.refresh_lock_root is not None:
        print("export_c_repositories: error: select one output mode", file=sys.stderr)
        return 2
    if args.refresh_lock_root is not None:
        try:
            count = refresh_lock_from_repositories(args.refresh_lock_root.resolve())
        except ExportError as exc:
            print(f"export_c_repositories: error: {exc}", file=sys.stderr)
            return 1
        print(f"export_c_repositories: refreshed {count} exact repository pins")
        return 0
    output_root = (args.runtime_bundle or args.output_root).resolve()
    try:
        if args.runtime_bundle is not None:
            count = export_runtime_bundle(output_root)
            print(f"export_c_repositories: wrote runtime bundle for {count} processes to "
                  f"{output_root}")
            return 0
        if output_root.exists():
            raise ExportError(f"refusing to overwrite existing output root: {output_root}")
        output_root.mkdir(parents=True)
        inventory = load_json(INVENTORY)
        required = inventory.get("required")
        optional = inventory.get("optional")
        if not isinstance(required, list) or not isinstance(optional, list):
            raise ExportError(f"{INVENTORY}: required/optional must be arrays")
        timestamp = source_timestamp()
        version = CORE_VERSION_FILE.read_text(encoding="utf-8").strip()
        contracts = process_contracts.validate()
        lock: dict[str, object] = {
            "schema_version": 1,
            "core": export_core(output_root, timestamp),
            "modules": [],
        }
        modules: list[dict[str, object]] = []
        ordered = [(item, "required") for item in required] + [
            (item, "optional") for item in optional
        ]
        for module_id, classification in ordered:
            if not isinstance(module_id, str):
                raise ExportError(f"{INVENTORY}: module id must be a string")
            modules.append(
                export_module(
                    output_root,
                    module_id,
                    classification,
                    contracts[module_id],
                    timestamp,
                    version,
                )
            )
        lock["modules"] = modules
        LOCK.parent.mkdir(parents=True, exist_ok=True)
        LOCK.write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")
    except ExportError as exc:
        print(f"export_c_repositories: error: {exc}", file=sys.stderr)
        return 1
    print(f"export_c_repositories: wrote {1 + len(modules)} repositories to {output_root}")
    print(f"export_c_repositories: wrote lock {LOCK}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
