#!/usr/bin/env python3
"""Require one PostgreSQL provider for both application roles.

Knowledge algorithms and schema belong to KB. Native callers may use the
PostgreSQL module's transport client; only that module may open database
connections. Independent database test fixtures remain permitted.
"""
from __future__ import annotations
import argparse
import json
import hashlib
from pathlib import Path
import re

RETIRED = ("src/modules/db2", "server-go/modules/db2", "server-go/db2")
C_DRIVER = re.compile(r'#\s*include\s*[<"]libpq(?:-fe)?\.h[>"]|\bPQ(?:connect\w*|exec\w*|sendQuery\w*|reset\w*|finish)\s*\(')
GO_DRIVER = re.compile(r'\b(?:pgxpool\.(?:New|NewWithConfig)|(?:pgx|pgconn)\.Connect\w*|sql\.Open)\s*\(')
C_COMMENTS = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)

# Old spellings survive only in credential rejection and the one-way data
# migration. Allow exact quoted literals, never identifiers or whole files.
LEGACY_LITERALS = {
    "src/config_client.c": {"db2_url", "AIMEE_DB2_URL", "db2", "db2_", "db2."},
    "src/config_client_contract.c": {"AIMEE_DB2_URL"},
    "src/modules/vault/vault_env_bootstrap.c": {"AIMEE_DB2_URL"},
    "src/modules/kb/c/schema.sql": {"db2_done"},
    "src/schema_data.h": {"db2_done"},
}
# Published migration bytes are immutable, including historical comments.
IMMUTABLE_MIGRATIONS = {'server-go/modules/aimee/families/schema_length_units.sql': '432b865b9097266414a2eb07d40e7e47975cca0f1ddc83baaebc6d8726a9d2f3', 'server-go/modules/aimee/families/schema_subject_erasure.sql': '6c7bb6800675e46439104818747f061784f706b6cd714fbd2730574d319018da'}
LEGACY_NAME = re.compile(r"(?<![A-Za-z0-9])(?:db2|DB2)(?:[A-Za-z0-9_]*)(?![A-Za-z0-9])")

def check_legacy_namespace(root: Path) -> list[str]:
    errors = []
    for tree in ("src", "server-go", "frontend", "config"):
        for path in (root / tree).rglob("*"):
            if not path.is_file() or path.suffix not in {".c", ".h", ".go", ".sql", ".tsx", ".ts", ".yaml"}:
                continue
            name = path.relative_to(root).as_posix()
            if name.startswith(("src/tests/", "src/vendor/", "src/build/", "frontend/node_modules/")) or path.name.endswith(("_test.go", ".test.ts", ".test.tsx")):
                continue
            if name in IMMUTABLE_MIGRATIONS:
                if hashlib.sha256(path.read_bytes()).hexdigest() != IMMUTABLE_MIGRATIONS[name]:
                    errors.append(f"published migration changed: {name}")
                continue
            text = path.read_text()
            for literal in LEGACY_LITERALS.get(name, set()):
                for quote in ('"', "'"):
                    text = text.replace(quote + literal + quote, "")
            if LEGACY_NAME.search(name) or LEGACY_NAME.search(text):
                errors.append(f"retired database namespace returned: {name}")
    return errors

def check(root: Path) -> list[str]:
    errors = check_legacy_namespace(root)
    for retired in RETIRED:
        if (root / retired).exists():
            errors.append(f"retired database provider returned: {retired}")
    contracts = json.loads((root / "src/modules/process-contracts.json").read_text())
    components = contracts["components"]
    if any(c["id"] == "db2" for c in components):
        errors.append("retired DB2 process contract returned")
    providers = [c for c in components if c["id"] == "postgres"]
    if len(providers) != 1:
        errors.append("exactly one PostgreSQL provider is required")
    else:
        provider = providers[0]
        if set(provider.get("placements", [])) != {"server", "kb"}:
            errors.append("PostgreSQL must serve both Server and KB")
        if provider.get("runtime") != "go" or provider.get("execution") != "process":
            errors.append("PostgreSQL must remain a supervised Go process")
        if {(s["id"], s["event_kind"]) for s in provider["stages"]} != {(1, 11265), (2, 11266), (3, 11267)}:
            errors.append("PostgreSQL health, SQL and native session stages must be registered")
    for path in (root / "src").rglob("*"):
        if not path.is_file() or path.suffix not in {".c", ".h"}:
            continue
        relative = path.relative_to(root).as_posix()
        if relative.startswith(("src/tests/", "src/vendor/", "src/build/")):
            continue
        if C_DRIVER.search(C_COMMENTS.sub("", path.read_text())):
            errors.append(f"native database driver escaped the provider: {relative}")
    for path in (root / "server-go").rglob("*.go"):
        relative = path.relative_to(root).as_posix()
        if path.name.endswith("_test.go") or relative.startswith((
            "server-go/modules/postgres/", "server-go/internal/workflowstore/workflowstoretest/")):
            continue
        if GO_DRIVER.search(path.read_text()):
            errors.append(f"database connection opened outside postgres: {relative}")
    makefile = (root / "src/Makefile").read_text()
    for line in makefile.splitlines():
        if re.match(r'L_(SERVER|KB)\s*=', line) and ("PQ_LIB" in line or "-lpq" in line):
            errors.append("native daemons must not link libpq")
    cmake = root / "CMakeLists.txt"
    if cmake.exists() and re.search(r"find_package\(PostgreSQL|pkg_check_modules\([^\n]*libpq|target_link_libraries\(aimee-(?:kb|server)[^)]*(?:LIBPQ|PostgreSQL)", cmake.read_text(), re.I):
        errors.append("CMake restored a native PostgreSQL driver")
    return errors

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = check(args.root)
    for error in errors:
        print(f"postgres-provider-boundary: {error}")
    if not errors:
        print("postgres-provider-boundary: sole provider serves Server and KB")
    return bool(errors)

if __name__ == "__main__":
    raise SystemExit(main())
