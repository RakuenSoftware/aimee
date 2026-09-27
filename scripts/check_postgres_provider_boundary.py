#!/usr/bin/env python3
"""Require one PostgreSQL provider for both application roles.

Knowledge algorithms and schema belong to KB. Native callers may use the
PostgreSQL module's transport client; only that module may open database
connections. Independent database test fixtures remain permitted.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re

RETIRED = ("src/modules/db2", "server-go/modules/db2", "server-go/db2")
C_DRIVER = re.compile(r'#\s*include\s*[<"]libpq(?:-fe)?\.h[>"]|\bPQ(?:connect\w*|exec\w*|sendQuery\w*|reset\w*|finish)\s*\(')
GO_DRIVER = re.compile(r'\b(?:pgxpool\.(?:New|NewWithConfig)|(?:pgx|pgconn)\.Connect\w*|sql\.Open)\s*\(')
C_COMMENTS = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)

def check(root: Path) -> list[str]:
    errors = []
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
