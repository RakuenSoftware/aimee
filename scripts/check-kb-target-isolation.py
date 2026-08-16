#!/usr/bin/env python3
"""Check that the standalone aimee-kb target stays KB/service scoped.

This is a static guard for the service-split proposal.  Runtime linking checks
catch DB1/sqlite symbols after a build; this check catches accidental source
or link-rule drift before the binary is produced.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ALLOWED_TRANSPORT_SHIMS = {
    "posix/agent_bridge.o",
    "windows/agent_bridge.o",
}

ALLOWED_AGENT_NAMED_SOURCES = {
    "kb_service_agent.c",
    "modules/db2/c/agent_hints.c",
    "modules/db2/c/agent_outcomes.c",
    "modules/db2/c/kb_service_backend_agent.c",
    "modules/db2/c/server_registry.c",
}

STATUS_AUTHORITY_ONLINE_PRIVATE_SOURCES = {
    "kb/kb_mgmt_status_custody.c",
    "modules/db2/c/management_status_key.c",
    "modules/db2/c/management_status_runtime.c",
}

STATUS_PROVISIONER_PRIVATE_SOURCES = {
    "kb/kb_mgmt_status_provision.c",
    "modules/db2/c/management_status_provision.c",
}

STATUS_AUTHORITY_PRIVATE_SOURCES = (
    STATUS_AUTHORITY_ONLINE_PRIVATE_SOURCES | STATUS_PROVISIONER_PRIVATE_SOURCES
)

FORBIDDEN_SOURCE_PREFIXES = (
    "db1/",
    "server_",
    "server/",
    "delegate_",
    "cmd_agent",
    "cli_session",
    "kb_client",
    "agent_",
)

CMAKE_SRC_RE = re.compile(r"\$\{AIMEE_SRC_DIR\}/([A-Za-z0-9_./-]+\.c)")
MAKE_VAR_START = "__AIMEE_VAR_START__"
MAKE_VAR_END = "__AIMEE_VAR_END__"


def make_var(makefile: Path, name: str) -> str:
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as tmp:
        tmp.write(
            f"print-var:\n\t@printf '{MAKE_VAR_START}%s{MAKE_VAR_END}\\n' '$($(VAR))'\n"
        )
        tmp_path = tmp.name
    try:
        cmd = [
            "make",
            "-f",
            str(makefile),
            "-f",
            tmp_path,
            "--no-print-directory",
            "print-var",
            f"VAR={name}",
        ]
        proc = subprocess.run(
            cmd,
            cwd=str(makefile.parent),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    finally:
        os.unlink(tmp_path)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"make print-var {name} failed with {proc.returncode}")
    start = proc.stdout.rfind(MAKE_VAR_START)
    if start < 0:
        return proc.stdout.strip()
    start += len(MAKE_VAR_START)
    end = proc.stdout.find(MAKE_VAR_END, start)
    if end < 0:
        return proc.stdout[start:].strip()
    return proc.stdout[start:end].strip()


def words(value: str) -> list[str]:
    return [w for w in value.split() if w and not w.startswith("$(")]


def normalize_object(obj: str) -> str:
    for prefix in ("build/obj/kb/", "build/obj/", "$(OBJDIR)/kb/", "$(OBJDIR)/"):
        if obj.startswith(prefix):
            return obj[len(prefix):]
    return obj


def normalize_dependency_object(obj: str) -> str:
    for prefix in ("build/obj/", "$(OBJDIR)/"):
        if obj.startswith(prefix):
            return obj[len(prefix):]
    return obj


def source_from_object(obj: str) -> str:
    obj = normalize_object(obj)
    if obj.endswith(".o"):
        return obj[:-2] + ".c"
    return obj


def is_forbidden_source(src: str) -> bool:
    if src in ALLOWED_AGENT_NAMED_SOURCES:
        return False
    base = os.path.basename(src)
    if src.startswith("db1/"):
        return True
    return any(src.startswith(prefix) or base.startswith(prefix) for prefix in FORBIDDEN_SOURCE_PREFIXES)


def check_makefile(makefile: Path) -> list[str]:
    violations: list[str] = []

    source_vars = ("KB_SRCS", "KB_DATA_SRCS", "KB_CORE_SRCS", "DB2_SRCS", "DB2_PG_SRCS")
    for var in source_vars:
        for src in words(make_var(makefile, var)):
            if is_forbidden_source(src):
                violations.append(f"{var} includes forbidden source {src}")
            if not (makefile.parent / src).exists():
                violations.append(f"{var} references missing source {src}")
            if src in STATUS_AUTHORITY_PRIVATE_SOURCES:
                violations.append(f"{var} includes status-authority-private source {src}")

    authority_sources = set(words(make_var(makefile, "STATUS_AUTHORITY_SRCS")))
    if not authority_sources:
        violations.append("STATUS_AUTHORITY_SRCS group is missing or empty")
    else:
        missing = STATUS_AUTHORITY_ONLINE_PRIVATE_SOURCES - authority_sources
        for src in sorted(missing):
            violations.append(f"STATUS_AUTHORITY_SRCS omits private source {src}")
        for src in sorted(STATUS_PROVISIONER_PRIVATE_SOURCES & authority_sources):
            violations.append(f"STATUS_AUTHORITY_SRCS includes offline provisioner source {src}")
        for src in authority_sources:
            if not (makefile.parent / src).exists():
                violations.append(f"STATUS_AUTHORITY_SRCS references missing source {src}")

    provisioner_objects = {
        normalize_dependency_object(obj)
        for obj in words(make_var(makefile, "STATUS_PROVISIONER_OBJS"))
    }
    expected_provisioner_objects = {
        src.removesuffix(".c") + ".o" for src in STATUS_PROVISIONER_PRIVATE_SOURCES
    }
    for obj in sorted(expected_provisioner_objects - provisioner_objects):
        violations.append(f"STATUS_PROVISIONER_OBJS omits offline provisioner object {obj}")

    authority_objects = {
        normalize_dependency_object(obj)
        for obj in words(make_var(makefile, "STATUS_AUTHORITY_OBJS"))
    }
    if not authority_objects:
        violations.append("STATUS_AUTHORITY_OBJS group is missing or empty")
    else:
        expected_objects = {src.removesuffix(".c") + ".o" for src in authority_sources}
        for obj in sorted(expected_objects - authority_objects):
            violations.append(f"STATUS_AUTHORITY_OBJS omits authority object {obj}")
        all_objects = {
            normalize_dependency_object(obj) for obj in words(make_var(makefile, "ALL_OBJS"))
        }
        for obj in sorted(authority_objects - all_objects):
            violations.append(f"ALL_OBJS omits status authority dependency {obj}")

    ordinary_objects = {
        normalize_object(obj)
        for var in ("KB_OBJS", "KB_DB2_OBJS")
        for obj in words(make_var(makefile, var))
    }
    for src in sorted(STATUS_AUTHORITY_PRIVATE_SOURCES):
        private_obj = src.removesuffix(".c") + ".o"
        if private_obj in ordinary_objects or os.path.basename(private_obj) in ordinary_objects:
            violations.append(f"ordinary KB object inventory includes status symbol owner {private_obj}")

    for obj in words(make_var(makefile, "KB_PLATFORM_OBJS")):
        norm = normalize_object(obj)
        if norm in ALLOWED_TRANSPORT_SHIMS:
            continue
        src = source_from_object(obj)
        if is_forbidden_source(src):
            violations.append(f"KB_PLATFORM_OBJS includes forbidden object {obj}")

    l_kb = make_var(makefile, "L_KB")
    if "-lsqlite3" in l_kb:
        violations.append("L_KB links sqlite3; aimee-kb must not link DB1/sqlite")
    if "-lpq" not in l_kb and "libpq" not in l_kb:
        violations.append("L_KB does not link libpq; aimee-kb must own DB2/Postgres")

    return violations


def cmake_command_block(text: str, marker: str) -> str:
    start = text.find(marker)
    if start < 0:
        return ""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return ""


def check_cmake(cmake: Path) -> list[str]:
    if not cmake.exists():
        return [f"CMakeLists.txt not found: {cmake}"]
    text = cmake.read_text(encoding="utf-8")
    block = cmake_command_block(text, "add_executable(aimee-kb")
    violations: list[str] = []
    src_root = cmake.parent / "src"
    for rel in CMAKE_SRC_RE.findall(text):
        if rel.startswith("vendor/sqlite3/"):
            continue
        if not (src_root / rel).exists():
            violations.append(f"CMakeLists.txt references missing source ${{AIMEE_SRC_DIR}}/{rel}")
    if not block:
        # CMake is thin-client-only (it builds just the `aimee` CLI). aimee-kb —
        # like aimee-server/gateway/webchat — is a Linux/Docker component built
        # EXCLUSIVELY via `make -C src`, so it has no CMake target. Its KB source
        # isolation is enforced on the Makefile side (check_makefile); the absence
        # of a CMake aimee-kb target is expected, not a violation. (Any genuinely
        # missing sources CMake *does* reference are still reported above.)
        return violations
    for rel in CMAKE_SRC_RE.findall(block):
        if is_forbidden_source(rel):
            violations.append(f"CMake aimee-kb target includes forbidden source {rel}")
    for forbidden in ("${DB1_SRCS}", "${AGENT_SRCS}", "${SERVER_SRCS}", "${GIT_SRCS}"):
        if forbidden in block:
            violations.append(f"CMake aimee-kb target includes forbidden source group {forbidden}")
    link_window = cmake_command_block(text, "target_link_libraries(aimee-kb")
    if not link_window:
        violations.append("CMakeLists.txt has no target_link_libraries(aimee-kb ...) block")
        return violations
    if "SQLite::SQLite3" in link_window:
        violations.append("CMake aimee-kb target links SQLite::SQLite3")
    return violations


def plant_test() -> int:
    with tempfile.TemporaryDirectory(prefix="kb_target_isolation_") as tmp:
        root = Path(tmp)
        mk = root / "Makefile"
        mk.write_text(
            "KB_SRCS = kb_main.c agent_loop.c db1/db.c kb_client.c kb/kb_mgmt_status_custody.c\n"
            "KB_DATA_SRCS = kb.c missing_kb.c\n"
            "KB_CORE_SRCS = util.c\n"
            "DB2_SRCS = db2/db2_init.c\n"
            "DB2_PG_SRCS = db2/db_postgres.c\n"
            "KB_PLATFORM_OBJS = build/obj/posix/agent_bridge.o build/obj/delegate_driver.o\n"
            "L_KB = -lsqlite3 -lm\n",
            encoding="utf-8",
        )
        for src in (
            "kb_main.c",
            "agent_loop.c",
            "db1/db.c",
            "kb_client.c",
            "kb.c",
            "util.c",
            "db2/db2_init.c",
            "db2/db_postgres.c",
            "kb/kb_mgmt_status_custody.c",
        ):
            path = root / src
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* plant */\n", encoding="utf-8")
        src_dir = root / "src"
        src_dir.mkdir(parents=True, exist_ok=True)
        (src_dir / "kb_main.c").write_text("/* plant */\n", encoding="utf-8")
        (src_dir / "modules" / "db1").mkdir(parents=True, exist_ok=True)
        (src_dir / "modules" / "db1" / "db.c").write_text("/* plant */\n", encoding="utf-8")
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            'set(AIMEE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")\n'
            "add_executable(aimee-kb\n"
            "    ${AIMEE_SRC_DIR}/kb_main.c\n"
            "    ${AIMEE_SRC_DIR}/db1/db.c\n"
            "    ${AIMEE_SRC_DIR}/missing_from_cmake.c\n"
            ")\n"
            "target_link_libraries(aimee-kb PRIVATE Threads::Threads SQLite::SQLite3)\n",
            encoding="utf-8",
        )
        found = check_makefile(mk)
        found.extend(check_cmake(cmake))
        expected = {
            "KB_SRCS includes forbidden source agent_loop.c",
            "KB_SRCS includes forbidden source db1/db.c",
            "KB_SRCS includes forbidden source kb_client.c",
            "KB_SRCS includes status-authority-private source kb/kb_mgmt_status_custody.c",
            "STATUS_AUTHORITY_SRCS group is missing or empty",
            "STATUS_AUTHORITY_OBJS group is missing or empty",
            "KB_DATA_SRCS references missing source missing_kb.c",
            "CMakeLists.txt references missing source ${AIMEE_SRC_DIR}/missing_from_cmake.c",
            "KB_PLATFORM_OBJS includes forbidden object build/obj/delegate_driver.o",
            "L_KB links sqlite3; aimee-kb must not link DB1/sqlite",
            "L_KB does not link libpq; aimee-kb must own DB2/Postgres",
            "CMake aimee-kb target includes forbidden source db1/db.c",
            "CMake aimee-kb target links SQLite::SQLite3",
        }
        if expected.issubset(set(found)):
            print("kb-target-isolation plant: ok")
            return 0
        print("kb-target-isolation plant: failed", file=sys.stderr)
        for item in found:
            print(f"  found: {item}", file=sys.stderr)
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Check standalone aimee-kb target isolation.")
    parser.add_argument("--makefile", default="src/Makefile", help="Path to src/Makefile")
    parser.add_argument("--cmake", default="CMakeLists.txt", help="Path to CMakeLists.txt")
    parser.add_argument("--plant-test", action="store_true", help="Run self-test with planted violations")
    args = parser.parse_args()

    if args.plant_test:
        return plant_test()

    violations = check_makefile(Path(args.makefile).resolve())
    violations.extend(check_cmake(Path(args.cmake).resolve()))
    if violations:
        print("kb-target-isolation: violations found:", file=sys.stderr)
        for violation in violations:
            print(f"  - {violation}", file=sys.stderr)
        return 1
    print("kb-target-isolation: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
