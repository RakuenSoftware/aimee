#!/usr/bin/env python3
"""Freeze DB2's module-owned C boundary and classify both sides of it.

The phase-one DB2 process migration needs two different kinds of evidence:

* the implementation unit being moved (C, header, and SQL files); and
* every direct include that must eventually become a generated client or remain
  a private implementation test; and
* every project header imported by the DB2 implementation that must be removed,
  promoted to a portable core API, or declared as a module dependency.

The checked-in manifest is exact for boundary files. Both inbound and outbound
project includes are shrink-only: removing one is progress, while adding a
consumer, dependency, header, or duplicate directive fails until the reviewed
manifest is deliberately regenerated. This checker does not claim that an
include is already a wire operation. It makes the portability debt measurable
before the C tree is packaged as an independent process.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
from typing import NoReturn


ROOT = Path(__file__).resolve().parent.parent
BASELINE = Path("tests/baselines/db2/source-boundary-v2.json")
BOUNDARY = PurePosixPath("src/modules/db2/c")
MODULE_ROOT = PurePosixPath("src/modules/db2")
CONTRACTS = Path("src/modules/process-contracts.json")
SCHEMA_VERSION = 2
SOURCE_KINDS = {"c": ".c", "headers": ".h", "sql": ".sql"}
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
INCLUDE_DETAIL = re.compile(r'^\s*#\s*include\s*([<"])([^">]+)[">]')
DB2_BASENAME = re.compile(r"db2[^/]*\.h$")
REVISION = re.compile(r"^[0-9a-f]{40}$")
DEPENDENCY_CLASSES = {
    "generated-schema-input",
    "host-api",
    "kb-authority-leak",
    "module-private-api",
    "module-public-api",
    "portable-core-api",
    "unresolved-project-include",
    "vendored-system-api",
}
HOST_ADAPTER_REHOMES = {
    "src/modules/db2/c/kb_service_backend_agent.c":
        "src/kb/db2_adapters/kb_service_backend_agent.c",
    "src/modules/db2/c/kb_service_backend_export.c":
        "src/kb/db2_adapters/kb_service_backend_export.c",
    "src/modules/db2/c/kb_service_backend_export.h":
        "src/kb/db2_adapters/kb_service_backend_export.h",
    "src/modules/db2/c/kb_service_backend_memory.c":
        "src/kb/db2_adapters/kb_service_backend_memory.c",
}
ADMITTED_SUPPORT_TEST_INCLUDES = {
    (
        "src/tests/test_kb.c",
        "../modules/db2/c/db2_tenant.h",
    ),
    (
        "src/tests/test_db2_extractor_support.c",
        "../modules/db2/support/db2_extractors.h",
    ),
    (
        "src/tests/test_db2_log_support.c",
        "../modules/db2/support/db2_log.h",
    ),
    (
        "src/tests/test_db2_rel_seed_support.c",
        "../modules/db2/support/db2_rel_seed.h",
    ),
    (
        "src/tests/test_db2_runtime_config_support.c",
        "../modules/db2/support/db2_runtime_config.h",
    ),
    (
        "src/tests/vault_witness_provider_fixture.h",
        "modules/db2/c/db2_vault_witness_provider.h",
    ),
}
ADMITTED_HOST_ADAPTER_INCLUDES = {
    (
        "src/kb/db2_adapters/kb_service_backend_runtime.c",
        "modules/db2/c/kb_service_backend.h",
    ),
}
# Reviewed S2 dependency inversions may introduce only these exact private
# contract imports. They replace broader host/vault/CSS implementation edges;
# pinning source, spelling, resolution, class, and count keeps this exception
# from becoming a general dependency-growth escape hatch.
ADMITTED_OUTBOUND_DEPENDENCIES = {
    (
        "src/modules/db2/c/canonical_index.h",
        "css_analyze.h",
        "src/modules/css/css_analyze.h",
    ): (1, "module-private-api"),
    (
        "src/modules/db2/c/db_schema.c",
        "schema_data.h",
        "src/schema_data.h",
    ): (1, "generated-schema-input"),
    (
        "src/modules/db2/c/db2_vault_witness_provider.h",
        "modules/vault/vault_witness_checkpoint.h",
        "src/modules/vault/vault_witness_checkpoint.h",
    ): (1, "module-private-api"),
    (
        "src/modules/db2/c/db2_vault_witness_provider.h",
        "modules/vault/vault_witness_merkle.h",
        "src/modules/vault/vault_witness_merkle.h",
    ): (1, "module-private-api"),
    (
        "src/modules/db2/c/db2_vault_witness_provider.h",
        "modules/vault/vault_witness_record.h",
        "src/modules/vault/vault_witness_record.h",
    ): (1, "module-private-api"),
    (
        "src/modules/db2/c/db2_witness_checkpoint.c",
        "modules/vault/vault_witness_export.h",
        "src/modules/vault/vault_witness_export.h",
    ): (1, "module-private-api"),
}


class BoundaryError(ValueError):
    """A fail-closed inventory error with a stable rule name."""


def fail(rule: str, message: str) -> NoReturn:
    raise BoundaryError(f"rule={rule}: {message}")


def _loads(raw: bytes, label: str) -> object:
    def no_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
        value: dict[str, object] = {}
        for key, item in pairs:
            if key in value:
                fail("json-duplicate-key", f"{label}: duplicate key {key!r}")
            value[key] = item
        return value

    if raw.startswith(b"\xef\xbb\xbf"):
        fail("json-bom", f"{label} begins with a UTF-8 BOM")
    try:
        return json.loads(raw.decode("utf-8", "strict"), object_pairs_hook=no_duplicates)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        fail("json-parse", f"cannot parse {label}: {exc}")


def _load(path: Path) -> object:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        fail("input", f"cannot read {path}: {exc}")
    return _loads(raw, str(path))


def _repo_path(root: Path, relative: PurePosixPath | Path) -> Path:
    candidate = root.joinpath(*PurePosixPath(relative).parts)
    try:
        candidate.resolve().relative_to(root.resolve())
    except (OSError, ValueError):
        fail("path-containment", f"{relative} escapes the repository root")
    return candidate


def _source_files(root: Path) -> dict[str, list[str]]:
    legacy_boundary = _repo_path(root, PurePosixPath("src/db2"))
    if legacy_boundary.exists() or legacy_boundary.is_symlink():
        fail("legacy-boundary", "src/db2 must not coexist with the module-owned C boundary")
    boundary = _repo_path(root, BOUNDARY)
    if not boundary.is_dir() or boundary.is_symlink():
        fail("boundary", f"{BOUNDARY} must be a real directory")
    result: dict[str, list[str]] = {}
    for kind, suffix in SOURCE_KINDS.items():
        result[kind] = sorted(
            path.relative_to(root).as_posix()
            for path in boundary.iterdir()
            if path.is_file() and not path.is_symlink() and path.suffix == suffix
        )
    return result


def _process_placements(root: Path) -> dict[str, set[str]]:
    value = _load(_repo_path(root, CONTRACTS))
    if not isinstance(value, dict) or not isinstance(value.get("components"), list):
        fail("process-contracts", f"{CONTRACTS} has no components array")
    result: dict[str, set[str]] = {}
    for index, component in enumerate(value["components"]):
        if not isinstance(component, dict):
            fail("process-contracts", f"component {index} is not an object")
        identifier, placements = component.get("id"), component.get("placements")
        if (not isinstance(identifier, str) or not isinstance(placements, list) or
                not all(item in {"server", "kb"} for item in placements)):
            fail("process-contracts", f"component {index} has invalid id/placements")
        result[identifier] = set(placements)
    return result


def _is_db2_include(target: str) -> bool:
    # This is the replacement process contract, not a direct implementation
    # dependency. New callers may adopt it while the old private-header
    # inventory remains shrink-only.
    if target.startswith("aimee/db2/"):
        return False
    path = PurePosixPath(target)
    parts = path.parts
    return "db2" in parts[:-1] or bool(DB2_BASENAME.search(path.name))


def _classification(path: PurePosixPath, placements: dict[str, set[str]]) -> str:
    parts = path.parts
    if len(parts) >= 2 and parts[:2] == ("src", "tests"):
        return "private-implementation-test"
    if len(parts) >= 2 and parts[:2] == ("src", "server"):
        return "server-kb-contract"
    if len(parts) >= 3 and parts[:2] == ("src", "modules"):
        module_id = parts[2]
        if module_id not in placements:
            # Some legacy source directories (currently guardrails and roadmap)
            # predate descriptor ownership. Keeping them visible as an explicit
            # audit class is safer than guessing a bus placement from a path.
            return "module-placement-audit"
        if "kb" in placements[module_id]:
            return "kb-generated-client"
        return "module-kb-contract"
    if len(parts) >= 2 and parts[:2] == ("src", "kb"):
        return "kb-generated-client"
    return "host-generated-client"


def _consumers(root: Path) -> list[dict[str, object]]:
    source_root = _repo_path(root, Path("src"))
    placements = _process_placements(root)
    rows: list[dict[str, object]] = []
    for path in sorted(source_root.rglob("*")):
        if (not path.is_file() or path.is_symlink() or path.suffix not in {".c", ".h"} or
                path.is_relative_to(_repo_path(root, MODULE_ROOT))):
            continue
        counts: Counter[str] = Counter()
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeError) as exc:
            fail("source-input", f"cannot read {path.relative_to(root)}: {exc}")
        for line in lines:
            match = INCLUDE.match(line)
            if match and _is_db2_include(match.group(1)):
                counts[match.group(1)] += 1
        if not counts:
            continue
        relative = PurePosixPath(path.relative_to(root).as_posix())
        rows.append({
            "path": relative.as_posix(),
            "classification": _classification(relative, placements),
            "includes": [
                {"header": header, "count": count}
                for header, count in sorted(counts.items())
            ],
        })
    return rows


def _header_index(root: Path) -> dict[str, list[str]]:
    """Index repository headers by the spellings accepted by project builds."""
    source_root = _repo_path(root, Path("src"))
    candidates: dict[str, set[str]] = {}
    for path in sorted(source_root.rglob("*.h")):
        if not path.is_file() or path.is_symlink():
            continue
        relative = PurePosixPath(path.relative_to(root).as_posix())
        source_relative = PurePosixPath(path.relative_to(source_root).as_posix())
        spellings = {source_relative.as_posix(), relative.name}
        if "include" in source_relative.parts:
            index = source_relative.parts.index("include")
            suffix = PurePosixPath(*source_relative.parts[index + 1:]).as_posix()
            if suffix:
                spellings.add(suffix)
        for spelling in spellings:
            candidates.setdefault(spelling, set()).add(relative.as_posix())
    return {key: sorted(values) for key, values in sorted(candidates.items())}


def _resolve_project_header(
    root: Path,
    source: Path,
    target: str,
    index: dict[str, list[str]],
) -> str | None:
    """Resolve an include to a repository header, or return None for system headers."""
    direct = source.parent.joinpath(*PurePosixPath(target).parts)
    try:
        resolved = direct.resolve(strict=True)
        resolved.relative_to(root.resolve())
    except (OSError, ValueError):
        pass
    else:
        if resolved.is_file() and resolved.suffix == ".h":
            return resolved.relative_to(root).as_posix()

    # The standalone DB2 process compiles its private C closure with the
    # descriptor-owned support root ahead of repository-wide include roots.
    # Mirror that exact lookup here so an owned copy such as cJSON.h wins over
    # the same basename in src/vendor instead of becoming falsely ambiguous.
    target_path = PurePosixPath(target)
    if not target_path.is_absolute() and ".." not in target_path.parts:
        support = root / "src/modules/db2/support" / target_path
        try:
            support_resolved = support.resolve(strict=True)
            support_resolved.relative_to((root / "src/modules/db2/support").resolve())
        except (OSError, ValueError):
            pass
        else:
            if support_resolved.is_file() and support_resolved.suffix == ".h":
                return support_resolved.relative_to(root).as_posix()

    # Historical DB2 sources use ../headers and ../kb spellings. Project
    # include roots made these resolve before the directory relocation, so
    # normalize only leading parent components and then consult the exact
    # repository index. Embedded '..' components remain unresolved/fail-safe.
    parts = list(PurePosixPath(target).parts)
    while parts and parts[0] == "..":
        parts.pop(0)
    # schema_data.h is a deterministic build output, not a tracked source
    # header. Account for the dependency even in a clean checkout where the
    # generator has not run yet.
    if parts == ["schema_data.h"]:
        return "src/schema_data.h"
    spellings = [target]
    if parts:
        spellings.append(PurePosixPath(*parts).as_posix())
    spellings.append(PurePosixPath(target).name)
    for spelling in dict.fromkeys(spellings):
        matches = index.get(spelling, [])
        if len(matches) == 1:
            return matches[0]
    return None


def _dependency_classification(resolved: PurePosixPath) -> str:
    parts = resolved.parts
    if parts[:2] == ("src", "core"):
        return "portable-core-api"
    if parts[:2] == ("src", "vendor"):
        return "vendored-system-api"
    if parts[:2] == ("src", "kb"):
        return "kb-authority-leak"
    if len(parts) >= 3 and parts[:2] == ("src", "modules"):
        if "include" in parts[3:]:
            return "module-public-api"
        return "module-private-api"
    if parts[:2] == ("src", "headers"):
        return "host-api"
    if resolved.as_posix() == "src/schema_data.h":
        return "generated-schema-input"
    return "host-api"


def _outbound_dependencies(root: Path) -> list[dict[str, object]]:
    boundary = _repo_path(root, BOUNDARY)
    index = _header_index(root)
    rows: list[dict[str, object]] = []
    for path in sorted(boundary.iterdir()):
        if not path.is_file() or path.is_symlink() or path.suffix not in {".c", ".h"}:
            continue
        counts: Counter[tuple[str, str, str]] = Counter()
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeError) as exc:
            fail("source-input", f"cannot read {path.relative_to(root)}: {exc}")
        for line in lines:
            match = INCLUDE_DETAIL.match(line)
            if not match:
                continue
            delimiter, target = match.groups()
            resolved = _resolve_project_header(root, path, target, index)
            if resolved is None:
                if delimiter == '"':
                    counts[(
                        target,
                        f"unresolved:{target}",
                        "unresolved-project-include",
                    )] += 1
                continue
            resolved_path = PurePosixPath(resolved)
            if resolved_path.is_relative_to(BOUNDARY):
                continue
            classification = _dependency_classification(resolved_path)
            counts[(target, resolved, classification)] += 1
        relative = path.relative_to(root).as_posix()
        for (target, resolved, classification), count in sorted(counts.items()):
            rows.append({
                "source": relative,
                "header": target,
                "resolved": resolved,
                "classification": classification,
                "count": count,
            })
    return rows


def _payload_fingerprint(
    source_files: object,
    consumers: object,
    outbound_dependencies: object,
) -> str:
    canonical = json.dumps(
        {
            "source_files": source_files,
            "consumers": consumers,
            "outbound_dependencies": outbound_dependencies,
        },
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("ascii")
    return hashlib.sha256(canonical).hexdigest()


def _summary(
    source_files: dict[str, list[str]],
    consumers: list[dict[str, object]],
    outbound_dependencies: list[dict[str, object]],
) -> dict[str, int]:
    include_count = sum(
        include["count"]
        for row in consumers
        for include in row["includes"]  # type: ignore[index]
    )
    test_count = sum(
        row["classification"] == "private-implementation-test" for row in consumers
    )
    return {
        "c_files": len(source_files["c"]),
        "headers": len(source_files["headers"]),
        "sql_files": len(source_files["sql"]),
        "consumer_files": len(consumers),
        "production_consumers": len(consumers) - test_count,
        "test_consumers": test_count,
        "include_directives": include_count,
        "outbound_dependency_rows": len(outbound_dependencies),
        "outbound_include_directives": sum(
            row["count"] for row in outbound_dependencies  # type: ignore[misc]
        ),
    }


def build_inventory(root: Path, source_revision: str) -> dict[str, object]:
    if not REVISION.fullmatch(source_revision):
        fail("source-revision", "source revision must be a full lowercase SHA-1")
    source_files = _source_files(root)
    consumers = _consumers(root)
    outbound_dependencies = _outbound_dependencies(root)
    summary = _summary(source_files, consumers, outbound_dependencies)
    return {
        "schema_version": SCHEMA_VERSION,
        "source_revision": source_revision,
        "source_boundary": BOUNDARY.as_posix(),
        "summary": summary,
        "source_files": source_files,
        "consumers": consumers,
        "outbound_dependencies": outbound_dependencies,
        "fingerprint": _payload_fingerprint(
            source_files, consumers, outbound_dependencies
        ),
    }


def _baseline_rows(value: object) -> dict[tuple[str, str], tuple[int, str]]:
    if not isinstance(value, list):
        fail("baseline-shape", "consumers must be an array")
    result: dict[tuple[str, str], tuple[int, str]] = {}
    previous_path = ""
    for index, row in enumerate(value):
        if not isinstance(row, dict) or set(row) != {"path", "classification", "includes"}:
            fail("baseline-shape", f"consumer {index} has invalid keys")
        path, classification, includes = row["path"], row["classification"], row["includes"]
        if (not isinstance(path, str) or path <= previous_path or
                not isinstance(classification, str) or not isinstance(includes, list)):
            fail("baseline-order", f"consumer {index} is invalid or not path-sorted")
        previous_path = path
        previous_header = ""
        for include in includes:
            if not isinstance(include, dict) or set(include) != {"header", "count"}:
                fail("baseline-shape", f"{path}: invalid include row")
            header, count = include["header"], include["count"]
            if (not isinstance(header, str) or header <= previous_header or
                    type(count) is not int or count < 1):
                fail("baseline-order", f"{path}: includes are invalid or not sorted")
            previous_header = header
            key = (path, header)
            if key in result:
                fail("baseline-duplicate", f"duplicate consumer/include pair {key}")
            result[key] = (count, classification)
    return result


def _dependency_rows(value: object) -> dict[tuple[str, str, str], tuple[int, str]]:
    if not isinstance(value, list):
        fail("baseline-shape", "outbound_dependencies must be an array")
    result: dict[tuple[str, str, str], tuple[int, str]] = {}
    previous: tuple[str, str, str] | None = None
    for index, row in enumerate(value):
        if not isinstance(row, dict) or set(row) != {
            "source", "header", "resolved", "classification", "count"
        }:
            fail("baseline-shape", f"outbound dependency {index} has invalid keys")
        source = row["source"]
        header = row["header"]
        resolved = row["resolved"]
        classification = row["classification"]
        count = row["count"]
        if not all(isinstance(item, str) for item in (
            source, header, resolved, classification
        )) or type(count) is not int or count < 1:
            fail("baseline-order", f"outbound dependency {index} is invalid")
        source_path = PurePosixPath(source)
        if (source_path.is_absolute() or ".." in source_path.parts or
                not source_path.is_relative_to(BOUNDARY) or
                source_path.suffix not in {".c", ".h"}):
            fail("baseline-path", f"outbound dependency {index} has unsafe source {source!r}")
        header_path = PurePosixPath(header)
        if (not header or "\\" in header or "\x00" in header or header_path.is_absolute()):
            fail("baseline-path", f"outbound dependency {index} has unsafe header {header!r}")
        if resolved.startswith("unresolved:"):
            if resolved != f"unresolved:{header}":
                fail("baseline-path", f"outbound dependency {index} has invalid unresolved path")
        else:
            resolved_path = PurePosixPath(resolved)
            if (resolved_path.is_absolute() or ".." in resolved_path.parts or
                    not resolved_path.parts or resolved_path.parts[0] != "src" or
                    resolved_path.suffix != ".h"):
                fail(
                    "baseline-path",
                    f"outbound dependency {index} has unsafe resolved path {resolved!r}",
                )
        if classification not in DEPENDENCY_CLASSES:
            fail(
                "baseline-classification",
                f"outbound dependency {index} has unknown class {classification!r}",
            )
        key = (source, header, resolved)
        if previous is not None and key <= previous:
            fail("baseline-order", "outbound dependencies are not sorted and unique")
        previous = key
        result[key] = (count, classification)
    return result


def enforce_shrink_only(previous: object, current: object) -> None:
    """Reject compatibility-include allowlist growth across a reviewed PR."""
    if not isinstance(previous, dict) or not isinstance(current, dict):
        fail("baseline-shape", "previous/current manifests must be objects")
    previous_rows = _baseline_rows(previous.get("consumers"))
    current_rows = _baseline_rows(current.get("consumers"))
    previous_sources = previous.get("source_files")
    previous_source_paths = {
        str(path)
        for paths in previous_sources.values()
        if isinstance(paths, list)
        for path in paths
    } if isinstance(previous_sources, dict) else set()
    rehome_origins = {new: old for old, new in HOST_ADAPTER_REHOMES.items()}
    for key, (count, classification) in current_rows.items():
        prior = previous_rows.get(key)
        if prior is None:
            if (key in ADMITTED_SUPPORT_TEST_INCLUDES and count <= 1 and
                    classification == "private-implementation-test"):
                continue
            if (key in ADMITTED_HOST_ADAPTER_INCLUDES and count <= 1 and
                    classification == "kb-generated-client"):
                continue
            old_source = rehome_origins.get(key[0])
            if old_source in previous_source_paths:
                continue
            fail("baseline-growth", f"new allowlist entry {key[1]!r} in {key[0]}")
        if count > prior[0]:
            fail(
                "baseline-growth",
                f"allowlist entry {key[1]!r} in {key[0]} grew from {prior[0]} to {count}",
            )
        if classification != prior[1]:
            fail(
                "baseline-classification",
                f"allowlist entry {key[1]!r} in {key[0]} changed from "
                f"{prior[1]} to {classification}",
            )
    # Schema v2 seeds outbound accounting. During that one upgrade, the v1
    # base still constrains inbound consumers; only the new outbound half has
    # no predecessor. Every subsequent comparison has both arrays.
    if "outbound_dependencies" not in previous:
        return
    previous_dependencies = _dependency_rows(previous.get("outbound_dependencies"))
    current_dependencies = _dependency_rows(current.get("outbound_dependencies"))
    for key, (count, classification) in current_dependencies.items():
        prior = previous_dependencies.get(key)
        if prior is None:
            source, header, resolved = key
            admitted = ADMITTED_OUTBOUND_DEPENDENCIES.get(key)
            if admitted is not None and count <= admitted[0] and classification == admitted[1]:
                continue
            # A private KB contract may move to the host's public header boundary
            # without becoming new dependency debt. Require the same source and
            # basename, a directional src/kb -> src/headers move, a narrower
            # classification, and no count growth. All other retargeting remains
            # fail-closed.
            promoted = next(
                (
                    value
                    for (old_source, old_header, old_resolved), value
                    in previous_dependencies.items()
                    if old_source == source
                    and PurePosixPath(old_header).name == PurePosixPath(header).name
                    and PurePosixPath(old_resolved).name == PurePosixPath(resolved).name
                    and old_resolved.startswith("src/kb/")
                    and resolved.startswith("src/headers/")
                    and value[1] == "kb-authority-leak"
                    and classification == "host-api"
                ),
                None,
            )
            if promoted is not None and count <= promoted[0]:
                continue
            # DB2 may internalize its pinned cJSON input under the exact
            # descriptor support boundary. This is a directional ownership
            # reduction, not dependency growth: require the same source/include
            # spelling, exact old/new paths and classes, and no count increase.
            localized = next(
                (
                    value
                    for (old_source, old_header, old_resolved), value
                    in previous_dependencies.items()
                    if old_source == source
                    and old_header == header == "cJSON.h"
                    and old_resolved == "src/vendor/headers/cJSON.h"
                    and resolved == "src/modules/db2/support/cJSON.h"
                    and value[1] == "vendored-system-api"
                    and classification == "module-private-api"
                ),
                None,
            )
            if localized is not None and count <= localized[0]:
                continue
            # The DB2 process consumes one immutable, descriptor-owned runtime
            # config snapshot instead of importing the host config module.
            config_localized = next(
                (
                    value
                    for (old_source, old_header, old_resolved), value
                    in previous_dependencies.items()
                    if old_source == source
                    and old_header == "config.h"
                    and old_resolved == "src/modules/config/config.h"
                    and header == "../support/db2_runtime_config.h"
                    and resolved == "src/modules/db2/support/db2_runtime_config.h"
                    and value[1] == classification == "module-private-api"
                ),
                None,
            )
            if config_localized is not None and count <= config_localized[0]:
                continue
            # DB2 process logging is a startup-installed module-private sink;
            # replacing the host logger include is directional debt removal.
            log_localized = next(
                (
                    value
                    for (old_source, old_header, old_resolved), value
                    in previous_dependencies.items()
                    if old_source == source
                    and PurePosixPath(old_header).name == "log.h"
                    and old_resolved == "src/headers/log.h"
                    and header == "../support/db2_log.h"
                    and resolved == "src/modules/db2/support/db2_log.h"
                    and value[1] == "host-api"
                    and classification == "module-private-api"
                ),
                None,
            )
            if log_localized is not None and count <= log_localized[0]:
                continue
            fail(
                "baseline-growth",
                f"new outbound dependency {key[1]!r} in {key[0]} resolves to {key[2]}",
            )
        if count > prior[0]:
            fail(
                "baseline-growth",
                f"outbound dependency {key[1]!r} in {key[0]} grew from "
                f"{prior[0]} to {count}",
            )
        if classification != prior[1]:
            fail(
                "baseline-classification",
                f"outbound dependency {key[1]!r} in {key[0]} changed from "
                f"{prior[1]} to {classification}",
            )


def check_previous_ref(root: Path, baseline_path: Path, previous_ref: str) -> bool:
    """Compare the checked manifest with a Git base; return false for first seed."""
    verify = subprocess.run(
        ["git", "rev-parse", "--verify", f"{previous_ref}^{{commit}}"], cwd=root,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if verify.returncode != 0:
        fail("previous-ref", f"cannot resolve {previous_ref!r} as a commit")
    relative = PurePosixPath(baseline_path)
    if relative.is_absolute() or ".." in relative.parts:
        fail("path-containment", f"{baseline_path} is not repository-relative")
    prior = subprocess.run(
        ["git", "show", f"{previous_ref}:{relative.as_posix()}"], cwd=root,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if prior.returncode != 0:
        legacy = relative.as_posix().replace("source-boundary-v2.json", "source-boundary-v1.json")
        if legacy != relative.as_posix():
            prior = subprocess.run(
                ["git", "show", f"{previous_ref}:{legacy}"], cwd=root,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            )
        if prior.returncode != 0:
            # The merge that introduces the first manifest has no previous
            # payload. Every later base contains v1 or v2, so this exception
            # cannot be reused to expand an established allowlist.
            return False
    previous = _loads(prior.stdout, f"{previous_ref}:{relative.as_posix()}")
    current = _load(_repo_path(root, baseline_path))
    enforce_shrink_only(previous, current)
    return True


def check(root: Path, baseline_path: Path = BASELINE) -> dict[str, int]:
    raw = _load(_repo_path(root, baseline_path))
    if not isinstance(raw, dict) or set(raw) != {
        "schema_version", "source_revision", "source_boundary", "summary",
        "source_files", "consumers", "outbound_dependencies", "fingerprint",
    }:
        fail("baseline-shape", "top-level keys differ from source-boundary v2")
    if raw["schema_version"] != SCHEMA_VERSION or raw["source_boundary"] != BOUNDARY.as_posix():
        fail("baseline-version", "schema version or source boundary differs from v2")
    if not isinstance(raw["source_revision"], str) or not REVISION.fullmatch(raw["source_revision"]):
        fail("source-revision", "baseline source revision is not a full lowercase SHA-1")
    if raw["fingerprint"] != _payload_fingerprint(
        raw["source_files"], raw["consumers"], raw["outbound_dependencies"]
    ):
        fail("baseline-fingerprint", "baseline payload fingerprint does not match")
    if not isinstance(raw["source_files"], dict) or set(raw["source_files"]) != set(SOURCE_KINDS):
        fail("baseline-shape", "source_files keys differ from v2")
    baseline_source_files: dict[str, list[str]] = {}
    for kind in SOURCE_KINDS:
        values = raw["source_files"].get(kind)
        if (not isinstance(values, list) or not all(isinstance(item, str) for item in values) or
                values != sorted(set(values))):
            fail("baseline-order", f"source_files.{kind} must be a sorted unique string array")
        baseline_source_files[kind] = values
    baseline_consumers = raw["consumers"]
    _baseline_rows(baseline_consumers)
    baseline_dependencies = raw["outbound_dependencies"]
    baseline_dependency_rows = _dependency_rows(baseline_dependencies)
    if raw["summary"] != _summary(
        baseline_source_files, baseline_consumers, baseline_dependencies
    ):
        fail("summary-drift", "baseline summary differs from its payload")
    actual_sources = _source_files(root)
    if raw["source_files"] != actual_sources:
        fail("source-drift", "DB2 C/header/SQL file inventory differs from the baseline")
    baseline_rows = _baseline_rows(raw["consumers"])
    actual_consumers = _consumers(root)
    actual_rows = _baseline_rows(actual_consumers)
    for key, (count, classification) in actual_rows.items():
        expected = baseline_rows.get(key)
        if expected is None:
            fail("include-growth", f"new DB2 include {key[1]!r} in {key[0]}")
        expected_count, expected_classification = expected
        if count > expected_count:
            fail(
                "include-growth",
                f"{key[0]} includes {key[1]!r} {count} times; baseline permits {expected_count}",
            )
        if classification != expected_classification:
            fail(
                "consumer-classification",
                f"{key[0]} changed from {expected_classification} to {classification}",
            )
    actual_dependencies = _outbound_dependencies(root)
    actual_dependency_rows = _dependency_rows(actual_dependencies)
    authority_leaks = [
        row for row in actual_dependencies if row["classification"] == "kb-authority-leak"
    ]
    if authority_leaks:
        first = authority_leaks[0]
        fail(
            "kb-authority-import",
            f"{first['source']} imports private KB header {first['resolved']}",
        )
    for key, (count, classification) in actual_dependency_rows.items():
        expected = baseline_dependency_rows.get(key)
        if expected is None:
            fail(
                "dependency-growth",
                f"new outbound dependency {key[1]!r} in {key[0]} resolves to {key[2]}",
            )
        expected_count, expected_classification = expected
        if count > expected_count:
            fail(
                "dependency-growth",
                f"{key[0]} includes outbound {key[1]!r} {count} times; "
                f"baseline permits {expected_count}",
            )
        if classification != expected_classification:
            fail(
                "dependency-classification",
                f"{key[0]} dependency {key[1]!r} changed from "
                f"{expected_classification} to {classification}",
            )
    return {
        "source_files": sum(len(items) for items in actual_sources.values()),
        "consumer_files": len(actual_consumers),
        "include_directives": sum(count for count, _ in actual_rows.values()),
        "outbound_dependencies": len(actual_dependencies),
        "outbound_include_directives": sum(
            count for count, _ in actual_dependency_rows.values()
        ),
    }


def _head_revision(root: Path) -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=root, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    revision = result.stdout.strip()
    if result.returncode != 0 or not REVISION.fullmatch(revision):
        fail("source-revision", result.stderr.strip() or "cannot resolve HEAD")
    return revision


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-root", type=Path, default=ROOT)
    parser.add_argument("--baseline", type=Path, default=BASELINE)
    parser.add_argument("--previous-ref")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    root = args.config_root.resolve()
    try:
        if args.write:
            inventory = build_inventory(root, _head_revision(root))
            target = _repo_path(root, args.baseline)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(inventory, indent=2) + "\n", encoding="utf-8")
            print(f"check_db2_source_boundary: wrote {args.baseline}")
        else:
            result = check(root, args.baseline)
            if args.previous_ref:
                compared = check_previous_ref(root, args.baseline, args.previous_ref)
                suffix = "previous allowlist compared" if compared else "initial allowlist seeded"
            else:
                suffix = "current manifest checked"
            print(
                "check_db2_source_boundary: ok "
                f"({result['source_files']} boundary files, "
                f"{result['consumer_files']} consumers, "
                f"{result['include_directives']} inbound includes, "
                f"{result['outbound_include_directives']} outbound includes remain; {suffix})"
            )
    except (OSError, BoundaryError) as exc:
        print(f"check_db2_source_boundary: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
