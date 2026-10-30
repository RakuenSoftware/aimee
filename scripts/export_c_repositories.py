#!/usr/bin/env python3
"""Materialize the language-specific module repositories.

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


def notification_grant(module_id: str, contract: dict[str, object]) -> tuple[str, str]:
    """A process's publish= and subscribe= grant lines: the change notifications
    it announces, and those it is named a subscriber of."""
    document = load_json(process_contracts.CONTRACTS)
    publish = ",".join(str(kind) for kind in process_contracts.notification_kinds(contract))
    subscribe = ",".join(
        str(kind) for kind in process_contracts.subscriptions(document).get(module_id, []))
    return publish, subscribe


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


def module_owned_files(module_id: str, descriptor: dict[str, object]) -> list[str]:
    """Return descriptor-owned source-tree inputs for the module repository."""
    result = [f"src/modules/{module_id}/module.yaml"]
    for key in ("sources", "private_headers", "public_headers", "contracts", "tests", "docs",
                "go_sources", "go_tests", "go_assets"):
        values = descriptor.get(key, [])
        if not isinstance(values, list) or not all(isinstance(item, str) for item in values):
            raise ExportError(f"{module_id}: descriptor field {key} must be a string array")
        result.extend(values)
    if len(result) != len(set(result)):
        raise ExportError(f"{module_id}: duplicate owned file")
    return result


def module_repository_files(module_id: str, descriptor: dict[str, object]) -> list[str]:
    """Return the files a module repository carries: exactly the ones it owns."""
    return module_owned_files(module_id, descriptor)


def go_module_main(module_id: str, principal_ref: int,
                   stages: list[dict[str, object]]) -> str:
    """Generate one independently buildable Go process entry point."""
    entries = "\n".join(
        f"\t\t{{EventKind: {stage['event_kind']}, StageID: {stage['id']}}},"
        for stage in stages
    )
    handler = "handler.NewDefaultHandler()" if module_id == "delegates" else "handler.Handle"
    if module_id in {"economizer", "egress"}:
        handler = "handler.NewHandler()"
    extra_imports = ""
    watchdog = ""
    if module_id == "providers":
        watchdog = "\tif handled, code := handler.ModelServicesBootstrap(os.Args); handled { os.Exit(code) }\n    if handled, code := handler.RunBootstrapLookup(os.Args); handled { os.Exit(code) }\n"
    cleanup = "\tdefer handler.Close()\n" if module_id == "postgres" else ""
    setup = ""
    process_setup = ""
    process_options = ""
    if module_id in {"config", "providers"}:
        handler = "moduleHandler"
        setup = """\tmoduleHandler, err := handler.NewDefaultHandler()
\tif err != nil {
\t\tfmt.Fprintf(os.Stderr, "module initialization: %v\\n", err)
\t\tos.Exit(1)
\t}
"""
    if module_id == "delegates":
        handler = "moduleHandler"
        process_setup = """\tmoduleHandler, err := handler.NewProcessHandler(ctx, os.Args[1])
\tif err != nil { fmt.Fprintln(os.Stderr, err); os.Exit(1) }
"""
    if module_id == "aimee":
        extra_imports = '\t"github.com/JBailes/aimee/server-go/modules/aimee/process"\n'
        handler = "moduleHandler.Handle"
        process_setup = """\tannouncer := process.NewAnnouncer()
\tmoduleHandler, err := process.New(ctx, os.Args[1], announcer)
\tif err != nil {
\t\tfmt.Fprintf(os.Stderr, "module initialization: %v\\n", err)
\t\tos.Exit(1)
\t}
"""
        process_options = "\t\tStarted: func(publisher ring.Publisher) { announcer.Start(ctx, publisher) },\n"
    if module_id == "memory":
        handler = "moduleHandler"
        process_setup = """\tmoduleHandler, err := handler.NewProcessHandler(ctx, os.Args[1], os.Getenv("AIMEE_MODULE_PLACEMENT"))
\tif err != nil {
\t\tfmt.Fprintf(os.Stderr, "module initialization: %v\\n", err)
\t\tos.Exit(1)
\t}
"""
    if module_id == "postgres":
        extra_imports = '\t"github.com/JBailes/aimee/server-go/modules/postgres/storage"\n'
        handler = "moduleHandler"
        watchdog = """\tif len(os.Args) == 2 && os.Args[1] == "__aimee_postgres_local_session" {
\t\tif err := handler.ServeLocalSession(context.Background(), os.Stdin, os.Stdout); err != nil {
\t\t\tfmt.Fprintln(os.Stderr, "PostgreSQL local session failed"); os.Exit(1)
\t\t}
\t\treturn
\t}
\tif handled, code := storage.Bootstrap(os.Args); handled { os.Exit(code) }
"""
        process_setup = """\tsqlHandler := handler.NewSQLHandler()
\tsessionHandler := handler.NewSessionHandler(ctx)
\tmoduleHandler := func(invocation ring.ModuleInvocation, frame []byte) ([]byte, ring.ModuleStatus) {
\t\tswitch invocation.StageID {
\t\tcase handler.StageSQL: return sqlHandler(invocation, frame)
\t\tcase handler.StageSession: return sessionHandler(invocation, frame)
\t\tdefault: return handler.Handle(invocation, frame)
\t\t}
\t}
"""
    if module_id in {"server", "kb"}:
        extra_imports = '\t"github.com/JBailes/aimee/server-go/modules/module-runtime/identity"\n'
        handler = "moduleHandler"
        watchdog = """\tif handled, code := identity.Bootstrap(os.Args); handled { os.Exit(code) }
\tif len(os.Args) > 1 && os.Args[1] == "__aimee_supervise_modules" {
\t\tif len(os.Args) != 5 { os.Exit(2) }
\t\tctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
\t\tdefer stop()
\t\tif err := handler.Supervise(ctx, os.Args[2], os.Args[3], os.Args[4]); err != nil {
\t\t\tfmt.Fprintln(os.Stderr, err); os.Exit(1)
\t\t}
\t\treturn
\t}
"""
        setup = """\tmoduleHandler, err := handler.NewHandler(os.Getenv("AIMEE_HOME"))
\tif err != nil { fmt.Fprintln(os.Stderr, err); os.Exit(1) }
"""
    if module_id == "sandbox":
        handler = "handler.NewHandler(moduleStore)"
        setup = """\tmoduleStore, err := handler.NewStore(os.Getenv("AIMEE_HOME"))
\tif err != nil { fmt.Fprintln(os.Stderr, err); os.Exit(1) }
"""
    handler_import = f'\thandler "github.com/JBailes/aimee/server-go/modules/{module_id}"\n' if module_id != "aimee" else ""
    return f"""package main

import (
\t"context"
\t"fmt"
\t"os"
\t"os/signal"
\t"syscall"

\t"github.com/RakuenSoftware/ouroboros-ring/go/ring"
{handler_import}{extra_imports})

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
{process_setup}\
\tconfig := ring.ModuleProcessConfig{{
\t\tSocketPath: os.Args[1], ModuleName: "{module_id}",
\t\tPrincipalClass: {PRINCIPAL_CLASS}, PrincipalRef: {principal_ref},
\t\tStages: []ring.ModuleStage{{
{entries}
\t\t}},
\t\tHandler: {handler},
{process_options}\t}}
\tif err := ring.RunModuleProcess(ctx, config); err != nil {{
\t\tfmt.Fprintf(os.Stderr, "aimee-module-{module_id}: %v\\n", err)
\t\tos.Exit(1)
\t}}
}}
"""


GO_SHARED_CONTRACTS = {
    "server-go/ir": {"aimee", "memory", "economizer", "delegates", "governance", "roundtable", "providers"},
    "server-go/memory": {"memory"},
    "server-go/internal/retrievalmetrics": {"memory", "benchmarks"},
    "server-go/modules/module-runtime/identity": {"server", "kb", "memory"},
    "server-go/modules/module-runtime/supervisor": {"server", "kb"},
    "server-go/config": {"config", "providers", "memory"},
    "server-go/modules/egress": {"providers", "memory", "git", "roundtable", "delegates"},
    "server-go/modules/audit": {"memory"},
    "server-go/modules/translation": {"providers"},
    "server-go/modules/execution-policy": {"aimee", "tools"},
    "server-go/delegate": {"delegates", "roundtable", "workflows"},
    "server-go/aimee": {"aimee", "economizer"},
    "server-go/db": {"aimee", "memory", "postgres"},
}


def go_process_shared_sources(module_id: str) -> list[str]:
    """Return shared contracts and helpers needed by independently built Go modules."""
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
    if module_id == "memory":
        # Integration tests drive the real PostgreSQL provider's encoded
        # contract. Include that test dependency under its own owner directory;
        # the memory runtime still imports only the bus-facing db contract.
        sources.extend(
            path.relative_to(ROOT).as_posix()
            for path in (ROOT / "server-go/modules/postgres").glob("*.go")
            if not path.name.endswith("_test.go")
        )
        # Frozen corpora are data dependencies of exported Go tests, not native
        # executable test registrations in the module descriptor.
        sources.extend([
            "tests/eval/memory_mr05_coverage_cases.json",
            "tests/eval/memory_mr09/baseline-policy.json",
            "tests/eval/memory_mr09/diversity-policy.json",
            "tests/eval/memory_mr09/diversity-v2-policy.json",
            "tests/eval/memory_mr09/code-navigation-pilot.json",
        ])
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
        pin["serve"] = [stage["event_kind"] for stage in process_contracts.process_stages(contract)]
    return pin


def go_language_version() -> str:
    """Keep isolated exports on the toolchain contract of the source module."""
    for line in (ROOT / "server-go/go.mod").read_text(encoding="utf-8").splitlines():
        if line.startswith("go "):
            return line.split()[1]
    raise ExportError("server-go/go.mod: missing Go language version")


def go_module_requirements(module_id: str) -> tuple[list[str], list[str]]:
    """Return direct and indirect requirements for an isolated Go export."""
    direct = ["golang.org/x/sys", "github.com/RakuenSoftware/ouroboros-ring/go"]
    indirect: list[str] = []
    if module_id == "config":
        direct.append("go.yaml.in/yaml/v3")
    if module_id == "memory":
        direct.append("golang.org/x/text")
    # The memory export includes PostgreSQL integration tests, while its
    # production storage remains the pure-Go bus client.
    if module_id in {"postgres", "memory"}:
        direct.append("github.com/jackc/pgx/v5")
        indirect.extend([
            "github.com/jackc/pgpassfile",
            "github.com/jackc/pgservicefile",
            "github.com/jackc/puddle/v2",
            "golang.org/x/sync",
        ])
        if module_id == "postgres":
            indirect.append("golang.org/x/text")
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
        stages = process_contracts.process_stages(contract)
        assert isinstance(principal_ref, int) and isinstance(stages, list)
        serve = ",".join(str(stage["event_kind"]) for stage in stages)
        publish, subscribe = notification_grant(module_id, contract)
        write_text(
            repository / "grants/module.grant.in",
            f"""version=1
principal_class={PRINCIPAL_CLASS}
principal_ref={principal_ref}
uid=self
executable=@CMAKE_INSTALL_FULL_BINDIR@/{binary}
publish={publish}
subscribe={subscribe}
request=
serve={serve}
""",
        )
        if runtime == "go":
            go_sources = descriptor.get("go_sources", [])
            if (not isinstance(go_sources, list) or not go_sources) and \
                    "external_source" not in descriptor:
                raise ExportError(f"{module_id}: Go process has no go_sources")
            shared_sources = go_process_shared_sources(module_id)
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

go {go_language_version()}

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
                for relative in ["runtime/main.go", *go_sources, *shared_sources,
                                 *descriptor.get("contracts", []), *descriptor.get("go_assets", []),
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
{', '.join(contract['placements'])} ring. The exported repository includes the
exact canonical Go bus client/runtime snapshot, caller contract, handler,
validation, and persistent store. It contains no C implementation or bridge.
"""
            else:
                runtime_text = f"""It builds `{binary}` as a separate Go process for the
{', '.join(contract['placements'])} ring. The exported repository includes the
exact canonical Go bus client/runtime snapshot and its repository-owned handler.
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
      - run: go test{" ./server-go/config" if module_id == "config" else ""} ./server-go/modules/{module_id}
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
        pin["serve"] = [stage["event_kind"] for stage in process_contracts.process_stages(contract)]
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
        module_repository_files(module_id, descriptor)
        enabled = module_id in required or descriptor.get("enabled_by_default") is True
        principal_ref = contract["principal_ref"]
        stages = process_contracts.process_stages(contract)
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
        if runtime != "go":
            raise ExportError(f"{module_id}: unsupported process runtime {runtime!r}")
        go_sources = descriptor.get("go_sources", [])
        if (not isinstance(go_sources, list) or not go_sources) and \
                "external_source" not in descriptor:
            raise ExportError(f"{module_id}: Go process has no go_sources")
        if hosted_by is None:
            go_modules.append(module_id)
        serve = ",".join(str(stage["event_kind"]) for stage in stages)
        publish, subscribe = notification_grant(module_id, contract)
        grant = f"""version=1
principal_class={PRINCIPAL_CLASS}
principal_ref={principal_ref}
uid=self
executable={executable}
publish={publish}
subscribe={subscribe}
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
    host_services = [{"owner": module_id, "stage": stage["id"], "event_kind": stage["event_kind"], "hosted_by": stage["hosted_by"], "principal_class": 0, "principal_ref": 0, "placements": contract["placements"]}
                     for module_id, contract in contracts.items() if contract["execution"] == "process"
                     for stage in contract["stages"] if "hosted_by" in stage]
    write_text(output_root / "host-services.json", json.dumps({"schema_version": 1, "services": host_services}, indent=2)+"\n")
    # Bus clients request stages but serve none, so they get a grant and no
    # manifest row: nothing supervises them, they are already running.
    contract_doc = load_json(process_contracts.CONTRACTS)
    subscribed = process_contracts.subscriptions(contract_doc)
    for client in contract_doc.get("clients", []):
        request = ",".join(str(kind) for kind in client["request"])
        publish = ",".join(str(kind) for kind in client.get("publish", []))
        subscribe = ",".join(str(kind) for kind in subscribed.get(client["id"], []))
        client_grant = f"""version=1
principal_class={PRINCIPAL_CLASS}
principal_ref={client["principal_ref"]}
uid=self
executable={client["executable"]}
publish={publish}
subscribe={subscribe}
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
             "process_count": count,
             "runtimes": runtimes, "placements": placement_rows},
            indent=2,
        ) + "\n",
    )
    return count


def refresh_lock_from_repositories(repository_root: Path) -> int:
    """Pin the lock to clean, tagged repositories updated from an export."""
    lock = load_json(LOCK)
    version = CORE_VERSION_FILE.read_text(encoding="utf-8").strip()
    modules = lock.get("modules")
    if "core" in lock or not isinstance(modules, list):
        raise ExportError(f"{LOCK}: invalid lock structure")
    inventory = load_json(INVENTORY)
    required = inventory.get("required")
    optional = inventory.get("optional")
    if not isinstance(required, list) or not isinstance(optional, list):
        raise ExportError(f"{INVENTORY}: required/optional must be arrays")
    required_ids = set(required)
    known_ids = required_ids | set(optional)
    contracts = process_contracts.validate()
    entries = modules
    for entry in entries:
        repository_id = entry.get("id")
        if not isinstance(repository_id, str):
            raise ExportError(f"{LOCK}: repository entry has no id")
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
        directory_name = f"aimee-module-{repository_id}"
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
    print(f"export_c_repositories: wrote {len(modules)} repositories to {output_root}")
    print(f"export_c_repositories: wrote lock {LOCK}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
