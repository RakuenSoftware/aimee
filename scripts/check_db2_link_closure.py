#!/usr/bin/env python3
"""Measure and ratchet the legacy DB2 C link-closure gap.

The probe compiles every C translation unit in the module-owned DB2 boundary,
combines only those objects with a relocatable link, and records the remaining
undefined symbols.  It deliberately supplies no helper objects or libraries:
the result is evidence of work still required, not a claim that DB2 links as a
standalone process.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
from typing import NoReturn


ROOT = Path(__file__).resolve().parent.parent
DESCRIPTOR = Path("src/modules/db2/module.yaml")
BOUNDARY = Path("src/modules/db2/c")
CONTRACT = Path("src/modules/db2/eventcontract/link-closure-v1.json")
SCHEMA_VERSION = 1
DISPOSITIONS = {
    "portable-core-promotion",
    "descriptor-owned-copy/generated-input",
    "injected-module-contract",
    "system-link",
    "private-implementation",
    "remove/dead",
}
SYMBOL = re.compile(r"^[A-Za-z_][A-Za-z0-9_.$@]*$")
REVISION = re.compile(r"^[0-9a-f]{40}$")
# Measure source-level ABI dependencies, not distro-specific compiler hardening
# thunks. Production builds retain their normal fortify and stack-protector policy.
PROBE_FLAGS = (
    "-fno-lto -fno-common -fno-stack-protector "
    "-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"
)
SYSTEM_PREFIXES = ("PQ", "EVP_", "OPENSSL_", "RAND_", "SHA", "CRYPTO_")
INJECTED_PREFIXES = (
    "anti_pattern_", "api_", "audit_", "code_", "cochange_", "config_", "css_",
    "db1_", "kb_", "learning_", "memory_", "module_", "vault_", "wfe_",
)
SYSTEM_SYMBOLS = {
    "_GLOBAL_OFFSET_TABLE_", "_exit", "abort", "accept", "atoi", "atoll", "bind",
    "calloc", "clock_gettime", "close", "connect", "dlclose", "dlopen", "dlsym",
    "difftime", "exp", "fclose", "fcntl", "fflush", "fgets", "fopen", "fprintf", "fputc",
    "fputs", "fread",
    "free", "fseek", "ftell", "fwrite", "getenv", "getpid", "gmtime", "gmtime_r",
    "htonl", "htons", "inet_ntop", "inet_pton", "listen", "log10", "malloc", "memchr", "memcmp",
    "memcpy", "memmove", "memset", "mktime", "nanosleep", "ntohl", "ntohs", "open",
    "poll", "pthread_cond_broadcast", "pthread_cond_destroy", "pthread_cond_init",
    "pthread_cond_signal", "pthread_cond_timedwait", "pthread_cond_wait", "pthread_create",
    "pthread_equal", "pthread_getspecific", "pthread_join", "pthread_key_create",
    "pthread_mutex_destroy", "pthread_mutex_init", "pthread_mutex_lock",
    "pthread_mutex_trylock", "pthread_mutex_unlock", "pthread_once", "pthread_self",
    "pthread_setspecific", "qsort",
    "read", "realloc", "realpath", "recv", "select", "send", "setsockopt", "shutdown", "sleep",
    "snprintf", "socket", "sqlite3_close", "sqlite3_errmsg", "sqlite3_exec", "sqlite3_free",
    "sqlite3_open", "stat", "stderr", "strcasecmp", "strcasestr", "strchr", "strcmp", "strcpy",
    "strcspn", "strdup", "strerror",
    "strftime", "strlen", "strncasecmp", "strncat", "strncmp", "strncpy", "strnlen",
    "strrchr", "strstr", "strtod", "strtok_r", "strtol", "strtoll", "time", "timegm",
    "tolower", "toupper", "unlink", "usleep", "vsnprintf", "write",
}


class ClosureError(ValueError):
    """A fail-closed link-closure invariant."""


def fail(rule: str, message: str) -> NoReturn:
    raise ClosureError(f"rule={rule}: {message}")


def _loads(raw: bytes, label: str) -> object:
    def unique(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                fail("json-duplicate-key", f"{label}: duplicate key {key!r}")
            result[key] = value
        return result

    def reject_constant(token: str) -> NoReturn:
        fail("json-number-domain", f"{label}: forbidden number {token!r}")

    if raw.startswith(b"\xef\xbb\xbf"):
        fail("json-bom", f"{label} begins with a UTF-8 BOM")
    try:
        return json.loads(
            raw.decode("utf-8", "strict"),
            object_pairs_hook=unique,
            parse_constant=reject_constant,
        )
    except (UnicodeError, json.JSONDecodeError) as exc:
        fail("json-parse", f"cannot parse {label}: {exc}")


def _load(root: Path, relative: Path) -> object:
    path = root / relative
    try:
        if path.is_symlink() or not path.is_file():
            fail("input", f"{relative} must be a regular non-symlink file")
        return _loads(path.read_bytes(), str(relative))
    except OSError as exc:
        fail("input", f"cannot read {relative}: {exc}")


def _safe_file(root: Path, raw: str, boundary: Path = BOUNDARY) -> Path:
    pure = PurePosixPath(raw)
    if (not raw or "\\" in raw or pure.is_absolute() or "." in pure.parts or
            ".." in pure.parts or pure.as_posix() != raw):
        fail("source-path", f"invalid repository-relative source path {raw!r}")
    expected = PurePosixPath(boundary.as_posix())
    try:
        pure.relative_to(expected)
    except ValueError:
        fail("source-boundary", f"source is outside {boundary}: {raw}")
    path = root.joinpath(*pure.parts)
    try:
        resolved = path.resolve()
        resolved.relative_to(root.resolve())
    except (OSError, ValueError):
        fail("source-path", f"source escapes repository: {raw}")
    if path.is_symlink() or not path.is_file() or resolved != path.absolute():
        fail("source-file", f"source must be a regular non-symlink file: {raw}")
    return path


def discover_sources(root: Path) -> list[str]:
    boundary = root / BOUNDARY
    if boundary.is_symlink() or not boundary.is_dir():
        fail("source-boundary", f"{BOUNDARY} must be a real directory")
    result: list[str] = []
    for path in sorted(boundary.iterdir()):
        if path.suffix != ".c":
            continue
        relative = path.relative_to(root).as_posix()
        _safe_file(root, relative)
        result.append(relative)
    if not result:
        fail("source-empty", "DB2 C boundary contains no translation units")
    return result


def source_fingerprint(root: Path, sources: list[str]) -> str:
    digest = hashlib.sha256()
    for raw in sources:
        path = _safe_file(root, raw)
        digest.update(raw.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def _run(command: list[str], cwd: Path) -> str:
    try:
        completed = subprocess.run(
            command, cwd=cwd, check=False, text=True, encoding="utf-8",
            errors="strict", stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    except (OSError, UnicodeError) as exc:
        fail("probe-exec", f"cannot execute {command[0]}: {exc}")
    if completed.returncode:
        detail = (completed.stderr or completed.stdout).strip()
        fail("probe-command", f"{command[0]} exited {completed.returncode}: {detail}")
    return completed.stdout


def _nm_undefined(output: str) -> set[str]:
    result: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if not fields:
            continue
        candidate = fields[0]
        if candidate.endswith(":") or not SYMBOL.fullmatch(candidate):
            fail("probe-nm", f"unexpected nm undefined-symbol row {line!r}")
        result.add(candidate)
    return result


def probe(root: Path, sources: list[str]) -> dict[str, list[str]]:
    """Compile DB2 objects and return truly external symbols with their users."""
    src_root = root / "src"
    with tempfile.TemporaryDirectory(prefix="db2-link-closure-") as raw_tmp:
        tmp = Path(raw_tmp)
        targets = [str(tmp / "db2" / (PurePosixPath(item).stem + ".o")) for item in sources]
        command = [
            "make", "-s", f"OBJDIR={tmp}", f"EXTRA_C_FLAGS={PROBE_FLAGS}", *targets,
        ]
        _run(command, src_root)
        objects = [tmp / "db2" / (PurePosixPath(item).stem + ".o") for item in sources]
        missing = [str(path) for path in objects if not path.is_file() or path.is_symlink()]
        if missing:
            fail("probe-object", f"compiler did not create expected objects: {missing[:3]}")

        aggregate = tmp / "db2-link-closure.o"
        # A relocatable link resolves DB2-to-DB2 references but accepts external
        # references.  No archive, shared object, helper stub, or weak definition
        # is supplied, so dependencies cannot disappear transitively.
        _run(["cc", "-r", "-o", str(aggregate), *map(str, objects)], src_root)
        external = _nm_undefined(_run(["nm", "-u", "--format=posix", str(aggregate)], src_root))
        references: dict[str, list[str]] = {symbol: [] for symbol in external}
        for raw, obj in zip(sources, objects, strict=True):
            for symbol in _nm_undefined(
                    _run(["nm", "-u", "--format=posix", str(obj)], src_root)):
                if symbol in references:
                    references[symbol].append(raw)
        missing_references = sorted(symbol for symbol, rows in references.items() if not rows)
        if missing_references:
            fail("probe-reference", f"symbols have no referencing unit: {missing_references}")
        return {symbol: sorted(rows) for symbol, rows in sorted(references.items())}


def classify(symbol: str) -> tuple[str, str]:
    """Return a conservative reviewed starting disposition and its rationale."""
    if symbol.startswith("__") or symbol in SYSTEM_SYMBOLS or symbol.startswith(SYSTEM_PREFIXES):
        return (
            "system-link",
            "C/POSIX, libpq, SQLite, libm, pthread, or OpenSSL ABI symbol; retain only as an "
            "explicit descriptor system dependency.",
        )
    if symbol.startswith("cJSON_"):
        return (
            "descriptor-owned-copy/generated-input",
            "Implemented by src/vendor/cJSON.c; the standalone DB2 bundle must package the pinned "
            "vendored source rather than inherit a monolithic-core object.",
        )
    if symbol.startswith(INJECTED_PREFIXES):
        return (
            "injected-module-contract",
            "The symbol belongs to a KB or sibling-module surface; replace the direct call with an "
            "injected bounded contract before standalone linking.",
        )
    return (
        "portable-core-promotion",
        "Legacy project-owned support symbol outside the DB2 boundary; promote a minimal portable "
        "API or reclassify with stronger owner evidence before closure.",
    )


def build_contract(root: Path) -> dict[str, object]:
    sources = discover_sources(root)
    unresolved = probe(root, sources)
    rows: list[dict[str, object]] = []
    counts = {name: 0 for name in sorted(DISPOSITIONS)}
    for symbol, references in unresolved.items():
        disposition, evidence = classify(symbol)
        counts[disposition] += 1
        rows.append({
            "symbol": symbol,
            "references": references,
            "disposition": disposition,
            "evidence": evidence,
        })
    revision = _run(["git", "rev-parse", "HEAD"], root).strip()
    if not REVISION.fullmatch(revision):
        fail("contract-revision", "git did not return a lowercase 40-hex revision")
    result: dict[str, object] = {
        "schema_version": SCHEMA_VERSION,
        "module": "db2",
        "source_revision": revision,
        "source_fingerprint": source_fingerprint(root, sources),
        "probe": {
            "compile_driver": "src/Makefile",
            "extra_c_flags": PROBE_FLAGS,
            "link_mode": "relocatable-no-libraries",
            "helper_objects": [],
            "libraries": [],
        },
        "translation_units": sources,
        "unresolved": rows,
        "summary": {
            "translation_units": len(sources),
            "unresolved_symbols": len(rows),
            "dispositions": counts,
        },
    }
    encoded = json.dumps(
        result, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
    ).encode("utf-8")
    result["fingerprint"] = hashlib.sha256(encoded).hexdigest()
    return result


def _string_list(value: object, label: str, *, symbols: bool = False) -> list[str]:
    if not isinstance(value, list) or not all(isinstance(item, str) and item for item in value):
        fail("contract-shape", f"{label} must be a non-empty string array")
    result = list(value)
    if result != sorted(set(result)):
        fail("contract-order", f"{label} must be sorted and unique")
    if symbols and any(not SYMBOL.fullmatch(item) for item in result):
        fail("contract-symbol", f"{label} contains an invalid symbol")
    return result


def validate_contract(
    root: Path, value: object, *, check_files: bool = True
) -> tuple[list[str], dict[str, dict[str, object]]]:
    if not isinstance(value, dict):
        fail("contract-shape", "closure contract must be an object")
    required = {
        "schema_version", "module", "source_revision", "source_fingerprint",
        "probe", "translation_units", "unresolved", "summary", "fingerprint",
    }
    if set(value) != required:
        fail("contract-keys", f"keys mismatch: expected={sorted(required)}, actual={sorted(value)}")
    if value["schema_version"] != SCHEMA_VERSION or value["module"] != "db2":
        fail("contract-version", "closure contract must be schema v1 for db2")
    if (not isinstance(value["source_revision"], str) or
            not REVISION.fullmatch(value["source_revision"])):
        fail("contract-revision", "source_revision must be a lowercase 40-hex commit")
    if not isinstance(value["source_fingerprint"], str) or not re.fullmatch(
            r"[0-9a-f]{64}", value["source_fingerprint"]):
        fail("contract-fingerprint", "source_fingerprint must be lowercase SHA-256")
    probe_value = value["probe"]
    expected_probe = {
        "compile_driver": "src/Makefile",
        "extra_c_flags": PROBE_FLAGS,
        "link_mode": "relocatable-no-libraries",
        "helper_objects": [],
        "libraries": [],
    }
    if probe_value != expected_probe:
        fail("probe-policy", "probe must use the frozen no-library/no-helper policy")
    units = _string_list(value["translation_units"], "translation_units")
    if check_files:
        for item in units:
            _safe_file(root, item)

    unresolved_value = value["unresolved"]
    if not isinstance(unresolved_value, list) or not unresolved_value:
        fail("contract-shape", "unresolved must be a non-empty array until closure is complete")
    rows: dict[str, dict[str, object]] = {}
    previous = ""
    for index, row in enumerate(unresolved_value):
        if not isinstance(row, dict) or set(row) != {
                "symbol", "references", "disposition", "evidence"}:
            fail("unresolved-shape", f"unresolved[{index}] has invalid keys")
        symbol = row["symbol"]
        if not isinstance(symbol, str) or not SYMBOL.fullmatch(symbol):
            fail("contract-symbol", f"unresolved[{index}] has invalid symbol")
        if symbol <= previous:
            fail("unresolved-order", "unresolved rows must be sorted and unique")
        previous = symbol
        references = _string_list(row["references"], f"unresolved[{index}].references")
        if any(item not in units for item in references):
            fail("unresolved-reference", f"{symbol} references an undeclared translation unit")
        if row["disposition"] not in DISPOSITIONS:
            fail("unresolved-disposition", f"{symbol} has invalid disposition")
        evidence = row["evidence"]
        if not isinstance(evidence, str) or len(evidence.strip()) < 12:
            fail("unresolved-evidence", f"{symbol} requires concise reviewable evidence")
        rows[symbol] = row

    summary = value["summary"]
    if not isinstance(summary, dict) or set(summary) != {
            "translation_units", "unresolved_symbols", "dispositions"}:
        fail("summary-shape", "summary has invalid keys")
    counts = {name: 0 for name in sorted(DISPOSITIONS)}
    for row in rows.values():
        counts[str(row["disposition"])] += 1
    expected_summary = {
        "translation_units": len(units),
        "unresolved_symbols": len(rows),
        "dispositions": counts,
    }
    if summary != expected_summary:
        fail("summary-drift", f"summary mismatch: expected={expected_summary}")

    fingerprint_payload = dict(value)
    fingerprint = fingerprint_payload.pop("fingerprint")
    if not isinstance(fingerprint, str) or not re.fullmatch(r"[0-9a-f]{64}", fingerprint):
        fail("contract-fingerprint", "fingerprint must be lowercase SHA-256")
    encoded = json.dumps(
        fingerprint_payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
    ).encode("utf-8")
    expected_fingerprint = hashlib.sha256(encoded).hexdigest()
    if fingerprint != expected_fingerprint:
        fail("contract-fingerprint", "contract content fingerprint does not match")
    return units, rows


def check(root: Path, *, run_probe: bool = True) -> None:
    descriptor = _load(root, DESCRIPTOR)
    if not isinstance(descriptor, dict) or not isinstance(descriptor.get("contracts"), list):
        fail("descriptor", "DB2 descriptor has no contracts array")
    if CONTRACT.as_posix() not in descriptor["contracts"]:
        fail("descriptor-contract", f"DB2 descriptor does not own {CONTRACT}")
    contract = _load(root, CONTRACT)
    units, rows = validate_contract(root, contract)
    discovered = discover_sources(root)
    if units != discovered:
        missing = sorted(set(discovered) - set(units))
        extra = sorted(set(units) - set(discovered))
        fail("source-closure", f"translation-unit mismatch; missing={missing}, extra={extra}")
    assert isinstance(contract, dict)
    actual_source_fingerprint = source_fingerprint(root, units)
    if contract["source_fingerprint"] != actual_source_fingerprint:
        fail(
            "source-fingerprint",
            "DB2 translation-unit content changed; regenerate and review closure",
        )
    if run_probe:
        actual = probe(root, units)
        expected = {symbol: list(row["references"]) for symbol, row in rows.items()}
        if actual != expected:
            added = sorted(set(actual) - set(expected))
            removed = sorted(set(expected) - set(actual))
            changed = sorted(symbol for symbol in set(actual) & set(expected)
                             if actual[symbol] != expected[symbol])
            fail("unresolved-drift",
                 f"closure changed; added={added}, removed={removed}, references_changed={changed}")


def compare_contracts(root: Path, previous: object, current: object) -> None:
    previous_units, previous_rows = validate_contract(root, previous, check_files=False)
    current_units, current_rows = validate_contract(root, current)
    added_units = sorted(set(current_units) - set(previous_units))
    if added_units:
        fail("previous-source-growth", f"new DB2 translation units are forbidden: {added_units}")
    added_symbols = sorted(set(current_rows) - set(previous_rows))
    if added_symbols:
        fail("previous-symbol-growth", f"new unresolved symbols are forbidden: {added_symbols}")
    expanded: list[str] = []
    for symbol in sorted(set(current_rows) & set(previous_rows)):
        before = set(previous_rows[symbol]["references"])
        after = set(current_rows[symbol]["references"])
        if not after <= before:
            expanded.append(symbol)
    if expanded:
        fail("previous-reference-growth", f"unresolved reference sets grew: {expanded}")


def check_previous(root: Path, ref: str, current: object) -> None:
    """Reject closure-debt growth even when the checked contract was regenerated."""
    if not ref or ref.startswith("-") or any(char.isspace() for char in ref):
        fail("previous-ref", f"invalid previous ref {ref!r}")
    _run(["git", "rev-parse", "--verify", f"{ref}^{{commit}}"], root)
    previous_paths = _run(
        ["git", "ls-tree", "--name-only", ref, "--", CONTRACT.as_posix()], root
    ).splitlines()
    if not previous_paths:
        # The first contract-introduction PR has no predecessor to compare.
        return
    if previous_paths != [CONTRACT.as_posix()]:
        fail("previous-contract", f"unexpected contract paths at {ref}: {previous_paths}")
    raw = _run(["git", "show", f"{ref}:{CONTRACT.as_posix()}"], root).encode("utf-8")
    compare_contracts(root, _loads(raw, f"{ref}:{CONTRACT}"), current)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--no-probe", action="store_true", help="validate metadata only")
    parser.add_argument("--write-contract", action="store_true",
                        help="regenerate the reviewed closure contract")
    parser.add_argument("--previous-ref", help="reject debt growth relative to a git ref")
    args = parser.parse_args()
    try:
        if args.write_contract:
            value = build_contract(ROOT)
            path = ROOT / CONTRACT
            path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n",
                            encoding="utf-8")
            print(f"check_db2_link_closure: wrote {CONTRACT}")
            return 0
        check(ROOT, run_probe=not args.no_probe)
        if args.previous_ref:
            check_previous(ROOT, args.previous_ref, _load(ROOT, CONTRACT))
    except ClosureError as exc:
        print(f"check_db2_link_closure: error: {exc}", file=sys.stderr)
        return 1
    print("check_db2_link_closure: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
