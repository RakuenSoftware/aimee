#!/usr/bin/env python3
"""Materialize the versioned C core and module repositories from this tree.

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
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
INVENTORY = ROOT / "tests/baselines/modules/canonical-inventory.yaml"
LOCK = ROOT / "dependencies/aimee-repositories.lock.json"
CORE_VERSION_FILE = ROOT / "src/core/VERSION"
REMOTE_ROOT = "https://github.com/RakuenSoftware"
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


def initialize_repository(repository: Path, remote: str, timestamp: str) -> str:
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
    git(repository, "tag", "v0.1.0")
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
    commit = initialize_repository(repository, remote, timestamp)
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
    for key in ("sources", "private_headers", "public_headers", "tests", "docs"):
        values = descriptor.get(key, [])
        if not isinstance(values, list) or not all(isinstance(item, str) for item in values):
            raise ExportError(f"{module_id}: descriptor field {key} must be a string array")
        result.extend(values)
    if len(result) != len(set(result)):
        raise ExportError(f"{module_id}: duplicate owned file")
    return result


def module_main(module_id: str, principal_ref: int) -> str:
    return f"""#define _POSIX_C_SOURCE 200809L
#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static volatile sig_atomic_t running = 1;

static void stop(int signal_number)
{{
   (void)signal_number;
   running = 0;
}}

int main(int argc, char **argv)
{{
   if (argc != 2)
   {{
      fprintf(stderr, "usage: %s DAEMON_MODULE_BUS_SOCKET\\n", argv[0]);
      return 2;
   }}
   signal(SIGINT, stop);
   signal(SIGTERM, stop);
   int socket_fd = -1;
   bus_client_t client;
   if (bus_endpoint_connect(argv[1], &socket_fd) != 0 ||
       bus_client_attach_as(socket_fd, &client, {PRINCIPAL_CLASS}u, {principal_ref}u) !=
           BUS_CLIENT_OK)
   {{
      perror("{module_id}: event-bus attach");
      bus_endpoint_close(&socket_fd);
      return 1;
   }}
   bus_endpoint_close(&socket_fd);
   while (running && !bus_client_epoch_changed(&client))
   {{
      struct timespec now;
      if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
         bus_client_heartbeat(&client, (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec);
      bus_event_t event;
      bus_client_result_t result = bus_client_poll(&client, &event);
      if (result == BUS_CLIENT_EPOCH)
         break;
      if (result == BUS_CLIENT_OK && (event.frame.hdr_flags & BUS_F_REQUEST))
         (void)bus_client_reply(&client, event.frame.event_kind, event.frame.correlation_id,
                                event.payload, event.payload_len);
      const struct timespec idle = {{.tv_sec = 0, .tv_nsec = 1000000}};
      nanosleep(&idle, NULL);
   }}
   bus_client_detach(&client);
   return 0;
}}
"""


def export_module(
    output_root: Path,
    module_id: str,
    classification: str,
    principal_ref: int,
    timestamp: str,
) -> dict[str, object]:
    descriptor_path = ROOT / f"src/modules/{module_id}/module.yaml"
    descriptor = load_json(descriptor_path)
    if descriptor.get("id") != module_id:
        raise ExportError(f"{descriptor_path}: descriptor id mismatch")
    owned = module_owned_files(module_id, descriptor)
    repository = output_root / f"aimee-module-{module_id}"
    repository.mkdir()
    for relative in owned:
        copy_file(relative, repository)
    shutil.copy2(ROOT / "LICENSE", repository / "LICENSE")
    write_text(repository / "runtime/main.c", module_main(module_id, principal_ref))
    binary = f"aimee-module-{module_id}"
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
serve=
""",
    )
    write_text(
        repository / "CMakeLists.txt",
        f"""cmake_minimum_required(VERSION 3.16)
project(aimee_module_{module_id.replace('-', '_')} VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
find_package(aimee-core 0.1.0 EXACT CONFIG REQUIRED)
if(NOT TARGET aimee::aimee-core-event-bus-client)
    message(FATAL_ERROR "{binary} requires the Linux event-bus client")
endif()
add_executable({binary} runtime/main.c)
target_compile_features({binary} PRIVATE c_std_11)
target_link_libraries({binary} PRIVATE aimee::aimee-core-event-bus-client)
configure_file(grants/module.grant.in ${{CMAKE_CURRENT_BINARY_DIR}}/{module_id}.grant @ONLY)
install(TARGETS {binary} RUNTIME DESTINATION ${{CMAKE_INSTALL_BINDIR}})
install(FILES ${{CMAKE_CURRENT_BINARY_DIR}}/{module_id}.grant
        DESTINATION ${{CMAKE_INSTALL_DATADIR}}/aimee/module-grants)
""",
    )
    write_text(
        repository / "README.md",
        f"""# Aimee module: {module_id}

This is the independent `{module_id}` source-ownership repository.  It pins
`aimee-core` `0.1.0` exactly and builds `{binary}` as a separate process which
attaches to either an aimee-server or aimee-kb local shared-memory bus.

The daemon admits the process only when its installed absolute executable path,
UID, principal class, principal reference, and event-kind grants match the
installed `.grant` file.  Copy that generated grant into the daemon's
`modules.d/server` or `modules.d/kb` policy directory.  Populate the four empty
event lists only as the module's event contracts are externalized; an empty list
allows lifecycle attachment and heartbeat but no event injection.

The descriptor-owned production sources, headers, tests, and documentation are
preserved at their canonical paths so their migration history remains auditable.
""",
    )
    source_paths = [ROOT / item for item in owned]
    manifest = {
        "schema_version": 1,
        "module": module_id,
        "classification": classification,
        "principal_class": PRINCIPAL_CLASS,
        "principal_ref": principal_ref,
        "source_sha256": digest_files(source_paths),
        "owned_files": owned,
    }
    write_text(repository / "SOURCE_MANIFEST.json", json.dumps(manifest, indent=2) + "\n")
    remote = f"{REMOTE_ROOT}/aimee-module-{module_id}.git"
    commit = initialize_repository(repository, remote, timestamp)
    return {
        "id": module_id,
        "classification": classification,
        "repository": remote,
        "ref": "v0.1.0",
        "version": "0.1.0",
        "commit": commit,
        "principal_class": PRINCIPAL_CLASS,
        "principal_ref": principal_ref,
        "source_sha256": manifest["source_sha256"],
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-root",
        type=Path,
        default=ROOT.parent / "aimee-module-repositories",
        help="new directory which will contain the independent repositories",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    output_root = args.output_root.resolve()
    try:
        if output_root.exists():
            raise ExportError(f"refusing to overwrite existing output root: {output_root}")
        output_root.mkdir(parents=True)
        inventory = load_json(INVENTORY)
        required = inventory.get("required")
        optional = inventory.get("optional")
        if not isinstance(required, list) or not isinstance(optional, list):
            raise ExportError(f"{INVENTORY}: required/optional must be arrays")
        timestamp = source_timestamp()
        lock: dict[str, object] = {
            "schema_version": 1,
            "core": export_core(output_root, timestamp),
            "modules": [],
        }
        modules: list[dict[str, object]] = []
        ordered = [(item, "required") for item in required] + [
            (item, "optional") for item in optional
        ]
        for principal_ref, (module_id, classification) in enumerate(ordered, start=1):
            if not isinstance(module_id, str):
                raise ExportError(f"{INVENTORY}: module id must be a string")
            modules.append(
                export_module(output_root, module_id, classification, principal_ref, timestamp)
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
