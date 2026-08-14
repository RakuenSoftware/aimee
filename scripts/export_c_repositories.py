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
    result = [f"src/modules/{module_id}/module.yaml"]
    for key in ("sources", "private_headers", "public_headers", "tests", "docs",
                "go_sources", "go_tests"):
        values = descriptor.get(key, [])
        if not isinstance(values, list) or not all(isinstance(item, str) for item in values):
            raise ExportError(f"{module_id}: descriptor field {key} must be a string array")
        result.extend(values)
    if len(result) != len(set(result)):
        raise ExportError(f"{module_id}: duplicate owned file")
    return result


def module_main(
    module_id: str,
    principal_ref: int,
    stages: list[dict[str, object]],
    has_handler: bool = False,
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
    return f"""#include <aimee/core/event_bus/module_runtime.h>

#include <stdio.h>
{handler_declaration}

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
   const aimee_module_process_config_t config = {{
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


def go_process_shared_sources(module_id: str) -> list[str]:
    """Return shared caller contracts needed by independently built Go modules.

    ``server-go/delegate`` is intentionally outside any implementation module:
    every module that calls delegates may import it. Add such module IDs here in
    lockstep with their process contract and runtime-bundle coverage.
    """
    if module_id not in {"delegates", "roundtable"}:
        return []
    return sorted(
        path.relative_to(ROOT).as_posix()
        for path in (ROOT / "server-go/delegate").glob("*.go")
        if not path.name.endswith("_test.go")
    )


def go_xsys_version() -> str:
    """Read the one external bus dependency from the canonical Go module."""
    text = (ROOT / "server-go/go.mod").read_text(encoding="utf-8")
    match = re.search(r"^\s*golang\.org/x/sys\s+(v\S+)\s*(?://.*)?$", text, re.MULTILINE)
    if match is None:
        raise ExportError("server-go/go.mod: missing golang.org/x/sys dependency")
    return match.group(1)


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
    owned = module_owned_files(module_id, descriptor)
    adapter_source = f"src/modules/{module_id}/module_adapter.c"
    has_handler = adapter_source in owned
    repository = output_root / f"aimee-module-{module_id}"
    repository.mkdir()
    for relative in owned:
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
            write_text(
                repository / "runtime/main.c",
                module_main(module_id, principal_ref, stages, has_handler),
            )
            write_text(
                repository / "CMakeLists.txt",
                f"""cmake_minimum_required(VERSION 3.16)
project(aimee_module_{module_id.replace('-', '_')} VERSION {version} LANGUAGES C)

include(GNUInstallDirs)
find_package(aimee-core {version} EXACT CONFIG REQUIRED)
if(NOT TARGET aimee::aimee-core-event-bus-client)
    message(FATAL_ERROR "{binary} requires the Linux event-bus client")
endif()
add_executable({binary} runtime/main.c{f' {adapter_source}' if has_handler else ''})
target_compile_features({binary} PRIVATE c_std_11)
target_include_directories({binary} PRIVATE src/modules/{module_id}/include)
target_link_libraries({binary} PRIVATE aimee::aimee-core-event-bus-client)
configure_file(grants/module.grant.in ${{CMAKE_CURRENT_BINARY_DIR}}/{module_id}.grant @ONLY)
install(TARGETS {binary} RUNTIME DESTINATION ${{CMAKE_INSTALL_BINDIR}})
install(FILES ${{CMAKE_CURRENT_BINARY_DIR}}/{module_id}.grant
        DESTINATION ${{CMAKE_INSTALL_DATADIR}}/aimee/module-grants)
""",
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
            if not isinstance(go_sources, list) or not go_sources:
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
            write_text(
                repository / "go.mod",
                f"""module github.com/JBailes/aimee

go 1.25.0

require golang.org/x/sys {go_xsys_version()}
""",
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
      - run: go test ./server-go/bus ./server-go/modules/{module_id}
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
    write_text(repository / ".github/workflows/ci.yml", workflow)
    write_text(
        repository / "README.md",
        f"""# Aimee module: {module_id}

This is the independent `{module_id}` source-ownership repository.

{runtime_text}

The descriptor-owned production sources, headers, tests, and documentation are
preserved at their canonical paths so their migration history remains auditable.
""",
    )
    source_paths = [ROOT / item for item in owned]
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
    count = 0
    for module_id, contract in contracts.items():
        if contract["execution"] != "process":
            continue
        descriptor = load_json(ROOT / f"src/modules/{module_id}/module.yaml")
        adapter_source = f"src/modules/{module_id}/module_adapter.c"
        owned = module_owned_files(module_id, descriptor)
        has_handler = adapter_source in owned
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
            generated = module_main(module_id, principal_ref, stages, has_handler)
            if has_handler:
                generated += "\n" + (ROOT / adapter_source).read_text(encoding="utf-8")
            write_text(output_root / "src" / f"{binary}.c", generated)
        else:
            go_sources = descriptor.get("go_sources", [])
            if not isinstance(go_sources, list) or not go_sources:
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
    write_text(
        output_root / "MANIFEST.json",
        json.dumps(
            {"schema_version": 1, "contracts": str(process_contracts.CONTRACTS.relative_to(ROOT)),
             "process_count": count, "runtimes": runtimes, "placements": placement_rows},
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
    entries = [core, *modules]
    for entry in entries:
        repository_id = entry.get("id")
        if not isinstance(repository_id, str):
            raise ExportError(f"{LOCK}: repository entry has no id")
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
            owned = module_owned_files(repository_id, descriptor)
            entry["source_sha256"] = digest_files([ROOT / relative for relative in owned])
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
