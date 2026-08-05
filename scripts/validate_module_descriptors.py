#!/usr/bin/env python3
"""Validate descriptor-v1 envelopes against the canonical module taxonomy."""

from __future__ import annotations

import argparse
import errno
import json
import os
from pathlib import Path
from pathlib import PurePosixPath
import re
import sys
import unicodedata
from typing import NoReturn


REPO_ROOT = Path(__file__).resolve().parent.parent
INVENTORY_PATH = Path("tests/baselines/modules/canonical-inventory.yaml")
SCHEMA_PATH = Path("src/modules/module.schema.json")
VERSION = 1
MAX_BYTES = 1_048_576
MAX_DEPTH = 32
MAX_ARRAY = 256
ID_RE = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
BASE_KEYS = {"descriptor_version", "id", "dependencies", "runtime_toggle"}
OWNERSHIP_FIELDS = (
    "sources", "private_headers", "public_headers", "tests", "docs", "go_sources", "go_tests",
)
DEFAULT_ON = {"runtime-web", "control-web", "sandbox"}
ROLE_EXTENSIONS = {
    "sources": {".c", ".cpp", ".S", ".s"},
    "private_headers": {".h", ".hpp"},
    "public_headers": {".h", ".hpp"},
    "tests": {".c", ".cpp", ".py", ".sh"},
    "docs": {".md"},
    "go_sources": {".go"},
    "go_tests": {".go"},
}


class DescriptorError(ValueError):
    """A descriptor failure with a stable rule and JSON pointer."""


def fail(rule: str, message: str, pointer: str = "") -> NoReturn:
    location = pointer or "/"
    raise DescriptorError(f"rule={rule} pointer={location}: {message}")


def _duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            fail("json-duplicate-key", f"duplicate key {key!r}")
        value[key] = item
    return value


def _reject_number(value: str) -> NoReturn:
    fail("json-number-domain", f"forbidden number {value!r}")


def _check_domain(value: object, pointer: str = "", depth: int = 0) -> None:
    if depth > MAX_DEPTH:
        fail("json-depth", f"nesting exceeds {MAX_DEPTH}", pointer)
    if isinstance(value, str):
        if any(0xD800 <= ord(char) <= 0xDFFF for char in value):
            fail("json-surrogate", "surrogate code point is forbidden", pointer)
    elif isinstance(value, list):
        if len(value) > MAX_ARRAY:
            fail("json-array-size", f"array exceeds {MAX_ARRAY} items", pointer)
        for index, item in enumerate(value):
            _check_domain(item, f"{pointer}/{index}", depth + 1)
    elif isinstance(value, dict):
        for key, item in value.items():
            _check_domain(key, pointer, depth + 1)
            _check_domain(item, f"{pointer}/{key}", depth + 1)


def load_json(path: Path) -> object:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        fail("input", f"cannot read {path}: {exc}")
    if len(raw) > MAX_BYTES:
        fail("input-size", f"{path} exceeds {MAX_BYTES} bytes")
    if raw.startswith(b"\xef\xbb\xbf"):
        fail("json-bom", f"{path} begins with a UTF-8 BOM")
    try:
        text = raw.decode("utf-8", "strict")
    except UnicodeDecodeError as exc:
        fail("json-encoding", f"{path} is not strict UTF-8: {exc}")
    try:
        value = json.loads(
            text,
            object_pairs_hook=_duplicates,
            parse_float=_reject_number,
            parse_constant=_reject_number,
        )
    except json.JSONDecodeError as exc:
        fail("json-parse", f"{path}: {exc.msg} at {exc.lineno}:{exc.colno}")
    _check_domain(value)
    return value


def module_id(value: object, pointer: str) -> str:
    if not isinstance(value, str):
        fail("module-id", "module ID must be a string", pointer)
    if len(value.encode("utf-8")) > 64:
        fail("module-id", "module ID exceeds 64 UTF-8 bytes", pointer)
    if unicodedata.normalize("NFC", value) != value or not ID_RE.fullmatch(value):
        fail("module-id", f"invalid normalized module ID {value!r}", pointer)
    return value


def load_inventory(repo: Path) -> tuple[set[str], set[str]]:
    value = load_json(repo / INVENTORY_PATH)
    if not isinstance(value, dict) or set(value) != {"schema_version", "required", "optional"}:
        fail("inventory-shape", "canonical inventory keys differ from v1")
    if type(value["schema_version"]) is not int or value["schema_version"] != 1:
        fail("inventory-version", "canonical inventory schema_version must be 1")
    groups: list[set[str]] = []
    for label in ("required", "optional"):
        entries = value[label]
        if not isinstance(entries, list) or not entries:
            fail("inventory-shape", f"inventory {label} must be a nonempty array")
        normalized = [module_id(item, f"/{label}/{index}") for index, item in enumerate(entries)]
        if len(normalized) != len(set(normalized)):
            fail("inventory-duplicate", f"inventory {label} contains duplicates")
        groups.append(set(normalized))
    if groups[0] & groups[1]:
        fail("inventory-overlap", "required and optional inventories overlap")
    return groups[0], groups[1]


def schema() -> dict[str, object]:
    module_pattern = ID_RE.pattern
    return {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": "https://aimee.local/schemas/module-descriptor-v1.json",
        "$comment": (
            "Generated by validate_module_descriptors.py; "
            "semantic policy is authoritative there."
        ),
        "type": "object",
        "additionalProperties": False,
        "required": sorted(BASE_KEYS),
        "properties": {
            "descriptor_version": {"const": VERSION},
            "id": {"type": "string", "pattern": module_pattern},
            "dependencies": {
                "type": "array",
                "items": {"type": "string", "pattern": module_pattern},
                "uniqueItems": True,
            },
            "enabled_by_default": {"type": "boolean"},
            "ownership_complete": {"type": "boolean"},
            "runtime_toggle": {
                "type": "object",
                "additionalProperties": False,
                "required": ["supported"],
                "properties": {"supported": {"type": "boolean"}},
            },
            **{
                field: {
                    "type": "array",
                    "items": {"type": "string"},
                    "uniqueItems": True,
                }
                for field in OWNERSHIP_FIELDS
            },
        },
    }


def schema_bytes() -> bytes:
    return (json.dumps(schema(), indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode()


def check_schema(repo: Path) -> None:
    try:
        actual = (repo / SCHEMA_PATH).read_bytes()
    except OSError as exc:
        fail("schema-input", f"cannot read generated schema: {exc}")
    if actual != schema_bytes():
        fail("schema-drift", f"{SCHEMA_PATH} differs from the authoritative model")


def validate_descriptor(value: object, required: set[str], optional: set[str]) -> str:
    if not isinstance(value, dict):
        fail("descriptor-shape", "descriptor must be an object")
    raw_id = value.get("id")
    identifier = module_id(raw_id, "/id")
    if identifier not in required | optional:
        fail("module-unknown", f"unknown module ID {identifier!r}", "/id")
    required_keys = BASE_KEYS | ({"enabled_by_default"} if identifier in optional else set())
    allowed_keys = required_keys | set(OWNERSHIP_FIELDS) | {"ownership_complete"}
    if not required_keys <= set(value) or not set(value) <= allowed_keys:
        fail(
            "descriptor-keys",
            f"keys mismatch; missing={sorted(required_keys-set(value))}, "
            f"unknown={sorted(set(value)-allowed_keys)}",
        )
    if type(value["descriptor_version"]) is not int or value["descriptor_version"] != VERSION:
        fail(
            "descriptor-version",
            f"descriptor_version must equal {VERSION}",
            "/descriptor_version",
        )
    dependencies = value["dependencies"]
    if not isinstance(dependencies, list):
        fail("dependencies-type", "dependencies must be an array", "/dependencies")
    parsed = [module_id(item, f"/dependencies/{index}") for index, item in enumerate(dependencies)]
    if len(parsed) > MAX_ARRAY:
        fail("dependencies-size", f"dependencies exceeds {MAX_ARRAY} items", "/dependencies")
    for index in range(1, len(parsed)):
        if parsed[index - 1] >= parsed[index]:
            rule = (
                "dependency-duplicate"
                if parsed[index - 1] == parsed[index]
                else "dependency-order"
            )
            fail(rule, "dependencies must be sorted and unique", f"/dependencies/{index}")
    for index, dependency in enumerate(parsed):
        if dependency == identifier:
            fail("dependency-self", "module cannot depend on itself", f"/dependencies/{index}")
        if dependency not in required | optional:
            fail(
                "dependency-unknown",
                f"unknown dependency {dependency!r}",
                f"/dependencies/{index}",
            )
    runtime = value["runtime_toggle"]
    if not isinstance(runtime, dict) or set(runtime) != {"supported"}:
        fail(
            "runtime-toggle-shape",
            "runtime_toggle must contain only supported",
            "/runtime_toggle",
        )
    if type(runtime["supported"]) is not bool:
        fail(
            "runtime-toggle-type",
            "runtime_toggle.supported must be boolean",
            "/runtime_toggle/supported",
        )
    if identifier in required and runtime["supported"]:
        fail(
            "required-runtime-toggle",
            "required modules cannot support runtime disablement",
            "/runtime_toggle/supported",
        )
    if identifier in optional and type(value["enabled_by_default"]) is not bool:
        fail("default-type", "enabled_by_default must be boolean", "/enabled_by_default")
    if "ownership_complete" in value and type(value["ownership_complete"]) is not bool:
        fail("ownership-complete-type", "ownership_complete must be boolean",
             "/ownership_complete")
    return identifier


def _contained(path: Path, boundary: Path) -> bool:
    try:
        path.relative_to(boundary)
    except ValueError:
        return False
    return True


def _resolve_owned(path: Path, pointer: str) -> Path:
    """Resolve an ownership path while keeping symlink-loop diagnostics stable."""
    try:
        return path.resolve()
    except RuntimeError as exc:
        fail("ownership-file", f"cannot resolve ownership path: {exc}", pointer)
    except OSError as exc:
        if exc.errno == errno.ELOOP:
            fail("ownership-file", f"cannot resolve ownership path: {exc}", pointer)
        if exc.errno == errno.ENAMETOOLONG:
            fail("ownership-path-normalized", f"ownership path is too long: {exc}", pointer)
        raise


def validate_owned_path(repo: Path, identifier: str, field: str, raw: object,
                        pointer: str) -> dict[str, str]:
    """Validate one declared ownership path through the shared role table."""
    if not isinstance(raw, str):
        fail("ownership-path-type", "ownership path must be a string", pointer)
    if not raw or "\\" in raw or raw.endswith("/"):
        fail("ownership-path-normalized", f"path is not normalized: {raw!r}", pointer)
    pure = PurePosixPath(raw)
    if pure.is_absolute() or "." in pure.parts or ".." in pure.parts or pure.as_posix() != raw:
        fail("ownership-path-normalized", f"path is not repository-relative: {raw!r}", pointer)

    role_prefixes = {
        "sources": PurePosixPath("src/modules") / identifier,
        "private_headers": PurePosixPath("src/modules") / identifier,
        "public_headers": PurePosixPath("src/modules") / identifier / "include/aimee" / identifier,
        "tests": PurePosixPath("src/tests"),
        "docs": PurePosixPath("docs/modules"),
        "go_sources": PurePosixPath("server-go/modules") / identifier,
        "go_tests": PurePosixPath("server-go/modules") / identifier,
    }
    prefix = role_prefixes[field]
    try:
        pure.relative_to(prefix)
    except ValueError:
        fail("ownership-role-boundary", f"{field} path is outside {prefix}: {raw}", pointer)

    resolved_repo = repo.resolve()
    lexical = resolved_repo.joinpath(*pure.parts)
    resolved = _resolve_owned(lexical, pointer)
    if not _contained(resolved, resolved_repo):
        fail("ownership-path-escape", f"path escapes repository: {raw}", pointer)
    if resolved != lexical:
        fail("ownership-path-symlink", f"ownership path must not traverse a symlink: {raw}", pointer)

    module_root = _resolve_owned(resolved_repo / "src/modules" / identifier, pointer)
    boundaries = {
        "sources": module_root,
        "private_headers": module_root,
        "public_headers": _resolve_owned(module_root / "include/aimee" / identifier, pointer),
        "tests": _resolve_owned(resolved_repo / "src/tests", pointer),
        "docs": _resolve_owned(resolved_repo / "docs/modules", pointer),
        "go_sources": _resolve_owned(resolved_repo / "server-go/modules" / identifier, pointer),
        "go_tests": _resolve_owned(resolved_repo / "server-go/modules" / identifier, pointer),
    }
    boundary = boundaries[field]
    if not _contained(resolved, boundary):
        fail("ownership-role-boundary", f"{field} path is outside {boundary}: {raw}", pointer)
    public_boundary = _resolve_owned(module_root / "include/aimee" / identifier, pointer)
    if field == "private_headers" and _contained(resolved, public_boundary):
        fail("ownership-role-boundary", f"private header is inside {public_boundary}: {raw}",
             pointer)
    if field == "tests" and not PurePosixPath(raw).name.startswith("test_"):
        fail("ownership-role", f"test path must use the test_ convention: {raw}", pointer)
    if field == "go_sources" and PurePosixPath(raw).name.endswith("_test.go"):
        fail("ownership-role", f"Go test must be declared in go_tests: {raw}", pointer)
    if field == "go_tests" and not PurePosixPath(raw).name.endswith("_test.go"):
        fail("ownership-role", f"Go test path must end in _test.go: {raw}", pointer)
    if field == "docs" and raw != f"docs/modules/{identifier}.md":
        fail("ownership-doc-canonical", f"expected docs/modules/{identifier}.md, actual {raw}",
             pointer)
    if PurePosixPath(raw).suffix not in ROLE_EXTENSIONS[field]:
        fail("ownership-role", f"extension does not match {field}: {raw}", pointer)
    if not resolved.is_file():
        fail("ownership-file", f"path is not an existing regular file: {raw}", pointer)
    return {"path": raw, "result": "PASS"}


def validate_complete_ownership(repo: Path, identifier: str,
                                value: dict[str, object]) -> None:
    """Require an opted-in descriptor to enumerate its owner-local implementation surface."""
    module_root = repo / "src/modules" / identifier
    public_root = module_root / "include/aimee" / identifier
    policies = {
        "sources": ROLE_EXTENSIONS["sources"],
        "private_headers": ROLE_EXTENSIONS["private_headers"],
    }
    found: dict[str, set[str]] = {}
    for role, extensions in policies.items():
        actual: set[str] = set()
        for path in module_root.rglob("*"):
            if path == module_root / "module.yaml":
                continue
            if _contained(path, public_root) or path.suffix not in extensions:
                continue
            relative = path.relative_to(repo).as_posix()
            if path.is_symlink():
                fail(
                    "ownership-complete-symlink",
                    f"{identifier} {role} path must not be a symlink: {relative}",
                    f"/{role}",
                )
            if not path.is_file():
                fail(
                    "ownership-complete-file",
                    f"{identifier} {role} path is not a regular file: {relative}",
                    f"/{role}",
                )
            actual.add(relative)
        found[role] = actual
        declared = set(value.get(role, []))
        missing = sorted(actual - declared)
        extra = sorted(declared - actual)
        if missing or extra:
            fail(
                "ownership-complete",
                f"{identifier} {role} mismatch for extensions {sorted(extensions)}; "
                f"missing={missing}, extra={extra}",
                f"/{role}",
            )
    go_root = repo / "server-go/modules" / identifier
    for role, is_test in (("go_sources", False), ("go_tests", True)):
        actual: set[str] = set()
        if go_root.is_dir():
            for path in go_root.rglob("*.go"):
                if path.name.endswith("_test.go") != is_test:
                    continue
                relative = path.relative_to(repo).as_posix()
                if path.is_symlink() or not path.is_file():
                    fail("ownership-complete-file",
                         f"{identifier} {role} path is not a regular file: {relative}",
                         f"/{role}")
                actual.add(relative)
        found[role] = actual
        declared = set(value.get(role, []))
        missing = sorted(actual - declared)
        extra = sorted(declared - actual)
        if missing or extra:
            fail("ownership-complete",
                 f"{identifier} {role} mismatch for Go files; missing={missing}, extra={extra}",
                 f"/{role}")
    if not any(found.values()):
        # An empty module root satisfies set equality vacuously, so the latch would
        # assert completeness for a module whose implementation has never been moved
        # under src/modules/<id>. That is migration debt, not completion. Keep this
        # ahead of the canonical-document check so an unmigrated module reports why it
        # cannot be latched rather than which field to add next.
        fail(
            "ownership-empty-domain",
            f"{identifier} has no module-local source or private header, so "
            "ownership_complete would be vacuous; the module is not migrated rather "
            "than broken. See docs/validation/core-modularization-class-a-migration.md",
            "/ownership_complete",
        )
    expected_doc = f"docs/modules/{identifier}.md"
    if value.get("docs") != [expected_doc]:
        fail("ownership-complete", f"{identifier} docs must equal [{expected_doc!r}]", "/docs")


def validate_ownership(
    repo: Path,
    identifier: str,
    value: dict[str, object],
    cross_claims: dict[str, tuple[str, str, str]] | None = None,
) -> dict[str, object]:
    """Validate all optional roles without reordering their declared entries."""
    for field in OWNERSHIP_FIELDS:
        if field not in ROLE_EXTENSIONS:
            fail("ownership-role-undefined", f"no role policy exists for {field}", f"/{field}")
    report: dict[str, object] = {
        "id": identifier,
        "module_root": f"src/modules/{identifier}",
        "ownership": {},
        "result": "PASS",
    }
    ownership = report["ownership"]
    assert isinstance(ownership, dict)
    claimed: dict[str, str] = {}
    for field in OWNERSHIP_FIELDS:
        raw_entries = value.get(field, [])
        if not isinstance(raw_entries, list):
            fail("ownership-field-type", f"{field} must be an array", f"/{field}")
        seen: set[str] = set()
        entries: list[dict[str, str]] = []
        for index, raw in enumerate(raw_entries):
            pointer = f"/{field}/{index}"
            if not isinstance(raw, str):
                fail("ownership-path-type", "ownership path must be a string", pointer)
            if raw in seen:
                fail("ownership-duplicate", f"duplicate {field} path: {raw}", pointer)
            if raw in claimed:
                fail("ownership-cross-role", f"{raw} is already declared in {claimed[raw]}",
                     pointer)
            if cross_claims is not None and raw in cross_claims:
                prior_id, prior_field, prior_pointer = cross_claims[raw]
                fail(
                    "ownership-cross-descriptor",
                    f"{raw} is already declared by {prior_id} {prior_field} at {prior_pointer}",
                    pointer,
                )
            seen.add(raw)
            claimed[raw] = field
            entries.append(validate_owned_path(repo, identifier, field, raw, pointer))
            if cross_claims is not None:
                cross_claims[raw] = (identifier, field, pointer)
        ownership[field] = entries
    if value.get("ownership_complete") is True:
        validate_complete_ownership(repo, identifier, value)
    return report


def discover(root: Path) -> list[Path]:
    if not root.is_dir():
        fail("root", f"descriptor root is not a directory: {root}")
    return sorted(root.rglob("module.yaml"))


def _normalized_cycle(cycle: list[str]) -> list[str]:
    open_cycle = cycle[:-1]
    rotations = [open_cycle[index:] + open_cycle[:index] for index in range(len(open_cycle))]
    result = min(rotations)
    return result + [result[0]]


def validate_graph(descriptors: dict[str, tuple[Path, list[str]]],
                   required: set[str], optional: set[str]) -> None:
    for identifier, (path, dependencies) in sorted(descriptors.items()):
        if identifier in required:
            for index, dependency in enumerate(dependencies):
                if dependency in optional:
                    fail("core-to-optional", f"required module {identifier!r} depends on optional "
                         f"module {dependency!r} in {path}", f"/dependencies/{index}")

    state: dict[str, int] = {identifier: 0 for identifier in descriptors}
    stack: list[str] = []

    def visit(identifier: str) -> None:
        state[identifier] = 1
        stack.append(identifier)
        for dependency in descriptors[identifier][1]:
            if dependency not in descriptors:
                continue
            if state[dependency] == 0:
                visit(dependency)
            elif state[dependency] == 1:
                start = stack.index(dependency)
                cycle = _normalized_cycle(stack[start:] + [dependency])
                fail("dependency-cycle", " -> ".join(cycle), "/dependencies")
        stack.pop()
        state[identifier] = 2

    for identifier in sorted(descriptors):
        if state[identifier] == 0:
            visit(identifier)


def validate_roots(repo: Path, roots: list[Path],
                   ownership_reports: list[dict[str, object]] | None = None) -> int:
    required, optional = load_inventory(repo)
    production_root = (repo / "src/modules").resolve()
    unresolved_roots = [root if root.is_absolute() else repo / root for root in roots]
    resolved_roots = [root.resolve() for root in unresolved_roots]
    seen: dict[str, Path] = {}
    graph: dict[str, tuple[Path, list[str]]] = {}
    cross_claims: dict[str, tuple[str, str, str]] = {}
    count = 0
    for root, resolved in zip(roots, resolved_roots, strict=True):
        files = discover(resolved)
        if not files:
            fail("no-descriptors-found", f"no module.yaml files under {root}")
        for path in files:
            value = load_json(path)
            try:
                identifier = validate_descriptor(value, required, optional)
            except DescriptorError as exc:
                raise DescriptorError(f"{path}: {exc}") from exc
            if identifier in seen:
                fail(
                    "module-duplicate",
                    f"module {identifier!r} also declared by {seen[identifier]}",
                )
            seen[identifier] = path
            graph[identifier] = (path, list(value["dependencies"]))
            ownership = validate_ownership(repo, identifier, value, cross_claims)
            if ownership_reports is not None:
                ownership_reports.append(ownership)
            count += 1
    if resolved_roots == [production_root]:
        expected = required | optional
        actual = set(seen)
        if actual != expected:
            fail("production-coverage", f"missing={sorted(expected-actual)}, "
                 f"extra={sorted(actual-expected)}")
        casefold_paths: dict[str, Path] = {}
        for identifier, path in sorted(seen.items()):
            relative = path.relative_to(production_root)
            expected_relative = Path(identifier) / "module.yaml"
            if relative != expected_relative:
                fail("production-path", f"{path} must be {production_root / expected_relative}")
            folded = relative.as_posix().casefold()
            if folded in casefold_paths and casefold_paths[folded] != path:
                fail("production-case-collision", f"{path} collides with {casefold_paths[folded]}")
            casefold_paths[folded] = path
            value = load_json(path)
            runtime_supported = value["runtime_toggle"]["supported"]
            if identifier in required:
                expected_default = None
                expected_toggle = False
            else:
                expected_default = identifier in DEFAULT_ON
                expected_toggle = identifier in DEFAULT_ON
            if identifier in optional and value["enabled_by_default"] is not expected_default:
                fail("production-default", f"{identifier} enabled_by_default must be "
                     f"{str(expected_default).lower()}", "/enabled_by_default")
            if runtime_supported is not expected_toggle:
                fail("production-runtime-toggle", f"{identifier} runtime_toggle.supported must be "
                     f"{str(expected_toggle).lower()}", "/runtime_toggle/supported")
        validate_graph(graph, required, optional)
    return count


def ownership_report(repo: Path, roots: list[Path]) -> dict[str, object]:
    reports: list[dict[str, object]] = []
    validate_roots(repo, roots, reports)
    reports.sort(key=lambda report: str(report["id"]))
    return {"schema_version": 1, "descriptors": reports, "result": "PASS"}


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("roots", nargs="*", type=Path)
    parser.add_argument("--config-root", type=Path)
    parser.add_argument("--check-schema", action="store_true")
    parser.add_argument("--emit-schema", action="store_true")
    parser.add_argument("--emit-ownership", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.emit_schema and args.emit_ownership:
            fail("cli-flag-conflict", "--emit-schema and --emit-ownership are mutually exclusive",
                 "/args")
        if args.emit_schema:
            sys.stdout.buffer.write(schema_bytes())
            return 0
        repo = Path(os.path.realpath(args.config_root or REPO_ROOT))
        if args.check_schema:
            check_schema(repo)
        if not args.roots:
            fail("root", "at least one descriptor root is required")
        if args.emit_ownership:
            print(json.dumps(ownership_report(repo, args.roots), indent=2, ensure_ascii=False))
            return 0
        count = validate_roots(repo, args.roots)
    except (DescriptorError, OSError, UnicodeError, ValueError) as exc:
        print(f"validate_module_descriptors: error: {exc}", file=sys.stderr)
        return 1
    print(f"validate_module_descriptors: ok ({count} descriptor(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
