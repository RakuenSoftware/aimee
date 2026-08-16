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
SUPPORT_BOUNDARY = Path("src/modules/db2/support")
CONTRACT = Path("src/modules/db2/eventcontract/link-closure-v1.json")
SCHEMA_VERSION = 2
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
SUPPORT_COMPILE_FLAGS = (
    "-std=c11 -Os -Wall -Wextra -Werror " + PROBE_FLAGS
)
SUPPORT_INCLUDE_ROOTS = ["src/modules/db2/support"]
SUPPORT_UNITS: list[dict[str, object]] = [{
    "path": "src/modules/db2/support/dstr_primitives.c",
    "source_sha256": "ae448e0ae6e0464922042536b77ab396ea230d5ba16ec977e003ecd614cf22ab",
    "header": "src/modules/db2/support/db2_dstr.h",
    "header_sha256": "47d3f825ea79b187a1f500e7bf58beda95dc472a4573ebff7128212019185b7c",
    "defines": ["dstr_appendf", "dstr_init", "dstr_steal"],
    "resolves": ["dstr_appendf", "dstr_init", "dstr_steal"],
    "allowed_includes": ["db2_dstr.h", "stdarg.h", "stdio.h", "stdlib.h"],
    "allowed_header_includes": ["stddef.h"],
    "allowed_undefined": ["realloc", "vsnprintf"],
    "base_references": {
        "dstr_appendf": ["src/modules/db2/c/collab_rules.c"],
        "dstr_init": ["src/modules/db2/c/collab_rules.c"],
        "dstr_steal": ["src/modules/db2/c/collab_rules.c"],
    },
    "provenance": "Definitions and required static helpers promoted from src/dstr.c; all DB2 "
                  "calls audited in src/modules/db2/c/collab_rules.c.",
    "evidence": "Three deterministic dynamic-string lifecycle functions with only realloc and "
                "vsnprintf imports; no DB2, event-bus, provider, I/O, or platform dependency.",
}, {
    "path": "src/modules/db2/support/management_read_primitives.c",
    "source_sha256": "2b1799442b2d57c6088eaa8fbcff744d6422bd3011b816bf78f3faefde4b8058",
    "header": "src/modules/db2/support/db2_management_read.h",
    "header_sha256": "b95b4714b891a71689bc2ad95bae3f693d6694cde83a3fef481d7212c4c2f318",
    "defines": ["server_mgmt_read_selector_name"],
    "resolves": ["server_mgmt_read_selector_name"],
    "allowed_includes": ["db2_management_read.h"],
    "allowed_header_includes": [],
    "allowed_undefined": [],
    "base_references": {
        "server_mgmt_read_selector_name": [
            "src/modules/db2/c/management_read_journal.c",
        ],
    },
    "provenance": "Definition promoted from src/shared/management_read.c; the sole DB2 call was "
                  "audited in management_read_journal.c.",
    "evidence": "Deterministic two-value selector mapping with no imports, allocation, I/O, DB, "
                "event-bus, provider, platform, pgvector, or DB3 dependency; ABI parity tested.",
}, {
    "path": "src/modules/db2/support/sketch_primitives.c",
    "source_sha256": "20318d4f9c92894892ef9475f95c1542602f3a7d2b4d9fe6df2b8d63b4986280",
    "header": "src/modules/db2/support/sketch.h",
    "header_sha256": "bab2cf2066fc50e583d4490c109a79496ef0f19a430b28cbb850e2b2a74b647d",
    "defines": [
        "sketch_bloom_init",
        "sketch_count_min_init",
        "sketch_fnv1a",
        "sketch_hll_add_hash",
        "sketch_hll_init",
        "sketch_lsh_band_hash",
        "sketch_minhash_init",
    ],
    "resolves": [
        "sketch_bloom_init",
        "sketch_count_min_init",
        "sketch_fnv1a",
        "sketch_hll_add_hash",
        "sketch_hll_init",
        "sketch_lsh_band_hash",
        "sketch_minhash_init",
    ],
    "allowed_includes": ["sketch.h", "string.h"],
    "allowed_header_includes": ["stddef.h", "stdint.h"],
    "allowed_undefined": ["memset"],
    "base_references": {
        "sketch_bloom_init": ["src/modules/db2/c/sketch.c"],
        "sketch_count_min_init": ["src/modules/db2/c/sketch.c"],
        "sketch_fnv1a": ["src/modules/db2/c/kb_payload.c"],
        "sketch_hll_add_hash": ["src/modules/db2/c/kb_payload.c"],
        "sketch_hll_init": [
            "src/modules/db2/c/kb_payload.c", "src/modules/db2/c/sketch.c",
        ],
        "sketch_lsh_band_hash": ["src/modules/db2/c/sketch.c"],
        "sketch_minhash_init": ["src/modules/db2/c/sketch.c"],
    },
    "provenance": "Definitions promoted from src/sketch.c and calls audited in "
                  "src/modules/db2/c/sketch.c and src/modules/db2/c/kb_payload.c.",
    "evidence": "Seven deterministic, database-free sketch primitives with one allowed libc "
                "dependency (memset); no DB2, event-bus, provider, I/O, or heap dependency.",
}, {
    "path": "src/modules/db2/support/text_primitives.c",
    "source_sha256": "2bbdb09370052759967f53557d0904398c55c118c964699a97a74a9abad02e78",
    "header": "src/modules/db2/support/db2_text.h",
    "header_sha256": "748a028371661444d450f1d365dca5e6988f0ba02730b38bfeb32078e67311b2",
    "defines": ["text_sanitize_utf8"],
    "resolves": ["text_sanitize_utf8"],
    "allowed_includes": ["db2_text.h"],
    "allowed_header_includes": ["stddef.h"],
    "allowed_undefined": [],
    "base_references": {
        "text_sanitize_utf8": [
            "src/modules/db2/c/canonical_index.c",
            "src/modules/db2/c/code_index.c",
            "src/modules/db2/c/kb_payload.c",
        ],
    },
    "provenance": "Definition promoted from src/text.c; all DB2 calls audited in "
                  "canonical_index.c, code_index.c, and kb_payload.c.",
    "evidence": "Deterministic in-place UTF-8 repair with no imports, allocation, I/O, DB, "
                "event-bus, provider, or platform dependency; exhaustive parity tested.",
}]
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
        boundary = (SUPPORT_BOUNDARY if raw.startswith(SUPPORT_BOUNDARY.as_posix() + "/")
                    else BOUNDARY)
        path = _safe_file(root, raw, boundary)
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


def _nm_global_definitions(output: str) -> tuple[set[str], set[str]]:
    definitions: set[str] = set()
    weak: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 2 or not SYMBOL.fullmatch(fields[0]) or len(fields[1]) != 1:
            fail("probe-nm", f"unexpected nm definition row {line!r}")
        definitions.add(fields[0])
        if fields[1] in {"W", "w", "V", "v"}:
            weak.add(fields[0])
    return definitions, weak


def _support_includes(root: Path, raw: str) -> list[str]:
    path = _safe_file(root, raw, SUPPORT_BOUNDARY)
    try:
        text = path.read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError) as exc:
        fail("support-source", f"cannot read {raw}: {exc}")
    result: list[str] = []
    include = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]\s*$')
    directive = re.compile(r"^\s*#\s*include\b")
    for number, line in enumerate(text.splitlines(), 1):
        if not directive.match(line):
            continue
        match = include.match(line)
        if not match:
            fail("support-include", f"malformed include at {raw}:{number}")
        result.append(match.group(1))
    if result != sorted(set(result)):
        fail("support-include", f"includes must be sorted and unique in {raw}: {result}")
    return result


def probe(
    root: Path, sources: list[str], support_units: list[dict[str, object]] | None = None
) -> dict[str, list[str]]:
    """Compile DB2 objects and return truly external symbols with their users."""
    support_units = support_units or []
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

        support_objects: list[Path] = []
        for unit in support_units:
            raw = str(unit["path"])
            source = _safe_file(root, raw, SUPPORT_BOUNDARY)
            obj = tmp / "support" / f"{PurePosixPath(raw).stem}.o"
            obj.parent.mkdir(parents=True, exist_ok=True)
            _run([
                "cc", *SUPPORT_COMPILE_FLAGS.split(),
                *(f"-I{root / item}" for item in SUPPORT_INCLUDE_ROOTS),
                "-c", str(source), "-o", str(obj),
            ], root)
            definitions, weak = _nm_global_definitions(_run(
                ["nm", "-g", "--defined-only", "--format=posix", str(obj)], root
            ))
            expected_definitions = set(unit["defines"])
            if definitions != expected_definitions:
                fail("support-exports", f"{raw}: expected={sorted(expected_definitions)}, "
                     f"actual={sorted(definitions)}")
            if weak:
                fail("support-weak", f"{raw} defines weak symbols: {sorted(weak)}")
            undefined = _nm_undefined(
                _run(["nm", "-u", "--format=posix", str(obj)], root)
            )
            allowed_undefined = set(unit["allowed_undefined"])
            if not undefined <= allowed_undefined:
                fail("support-undefined", f"{raw}: forbidden undefined symbols "
                     f"{sorted(undefined - allowed_undefined)}")
            support_objects.append(obj)

        aggregate = tmp / "db2-link-closure.o"
        # A relocatable link resolves DB2-to-DB2 references but accepts external
        # references.  No archive, shared object, helper stub, or weak definition
        # is supplied, so dependencies cannot disappear transitively.
        all_objects = [*objects, *support_objects]
        _run(["cc", "-r", "-o", str(aggregate), *map(str, all_objects)], src_root)
        external = _nm_undefined(_run(["nm", "-u", "--format=posix", str(aggregate)], src_root))
        references: dict[str, list[str]] = {symbol: [] for symbol in external}
        all_sources = [*sources, *(str(unit["path"]) for unit in support_units)]
        for raw, obj in zip(all_sources, all_objects, strict=True):
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


def descriptor_support_policy(root: Path, descriptor: object) -> list[dict[str, object]]:
    if not isinstance(descriptor, dict) or not isinstance(descriptor.get("sources"), list):
        fail("descriptor", "DB2 descriptor has no sources array")
    descriptor_sources = descriptor["sources"]
    assert isinstance(descriptor_sources, list)
    support_paths = [str(unit["path"]) for unit in SUPPORT_UNITS]
    actual_support = sorted(
        item for item in descriptor_sources
        if isinstance(item, str) and item.startswith(SUPPORT_BOUNDARY.as_posix() + "/")
    )
    if actual_support != support_paths:
        fail("support-descriptor", f"expected={support_paths}, actual={actual_support}")
    if not support_paths:
        return []
    boundary = root / SUPPORT_BOUNDARY
    if boundary.is_symlink() or not boundary.is_dir():
        fail("support-boundary", f"{SUPPORT_BOUNDARY} must be a real directory")
    disk_support = sorted(
        path.relative_to(root).as_posix() for path in boundary.iterdir()
        if path.suffix == ".c"
    )
    if disk_support != support_paths:
        fail("support-source-closure", f"expected={support_paths}, actual={disk_support}")
    expected_headers = sorted(str(unit["header"]) for unit in SUPPORT_UNITS)
    private_headers = descriptor.get("private_headers")
    if not isinstance(private_headers, list):
        fail("support-descriptor", "DB2 descriptor has no private_headers array")
    actual_headers = sorted(
        item for item in private_headers
        if isinstance(item, str) and item.startswith(SUPPORT_BOUNDARY.as_posix() + "/")
    )
    if actual_headers != expected_headers:
        fail("support-descriptor", f"expected headers={expected_headers}, actual={actual_headers}")
    disk_headers = sorted(
        path.relative_to(root).as_posix() for path in boundary.iterdir()
        if path.suffix == ".h"
    )
    if disk_headers != expected_headers:
        fail("support-header-closure", f"expected={expected_headers}, actual={disk_headers}")
    c_build = descriptor.get("c_build")
    if not isinstance(c_build, dict) or not isinstance(c_build.get("include_roots"), list):
        fail("support-build", "DB2 descriptor has no C include-root policy")
    missing_roots = sorted(set(SUPPORT_INCLUDE_ROOTS) - set(c_build["include_roots"]))
    if missing_roots:
        fail("support-build", f"descriptor omits support include roots: {missing_roots}")
    policy = json.loads(json.dumps(SUPPORT_UNITS))
    assert isinstance(policy, list)
    for unit in policy:
        assert isinstance(unit, dict)
        raw = str(unit["path"])
        support_source = _safe_file(root, raw, SUPPORT_BOUNDARY)
        actual_sha256 = hashlib.sha256(support_source.read_bytes()).hexdigest()
        if actual_sha256 != unit["source_sha256"]:
            fail("support-source-hash", f"{raw}: reviewed source content changed")
        header_raw = str(unit["header"])
        support_header = _safe_file(root, header_raw, SUPPORT_BOUNDARY)
        actual_header_sha256 = hashlib.sha256(support_header.read_bytes()).hexdigest()
        if actual_header_sha256 != unit["header_sha256"]:
            fail("support-header-hash", f"{header_raw}: reviewed header content changed")
        includes = _support_includes(root, raw)
        if includes != unit["allowed_includes"]:
            fail("support-include", f"{raw}: expected={unit['allowed_includes']}, "
                 f"actual={includes}")
        header_includes = _support_includes(root, header_raw)
        if header_includes != unit["allowed_header_includes"]:
            fail("support-header-include", f"{header_raw}: "
                 f"expected={unit['allowed_header_includes']}, actual={header_includes}")
    return policy


def build_contract(root: Path) -> dict[str, object]:
    sources = discover_sources(root)
    descriptor = _load(root, DESCRIPTOR)
    support_units = descriptor_support_policy(root, descriptor)
    unresolved = probe(root, sources, support_units)
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
        "source_fingerprint": source_fingerprint(
            root, [*sources, *(str(unit["path"]) for unit in support_units),
                   *(str(unit["header"]) for unit in support_units)]
        ),
        "probe": {
            "compile_driver": "src/Makefile",
            "extra_c_flags": PROBE_FLAGS,
            "link_mode": "relocatable-no-libraries",
            "helper_objects": [],
            "libraries": [],
            "support_compile_flags": SUPPORT_COMPILE_FLAGS,
            "support_include_roots": SUPPORT_INCLUDE_ROOTS,
        },
        "translation_units": sources,
        "descriptor_support_units": support_units,
        "unresolved": rows,
        "summary": {
            "translation_units": len(sources),
            "descriptor_support_units": len(support_units),
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


def _validate_support_units(
    root: Path, value: object, legacy_units: list[str], *, check_files: bool
) -> list[dict[str, object]]:
    if not isinstance(value, list):
        fail("support-shape", "descriptor_support_units must be an array")
    result: list[dict[str, object]] = []
    previous = ""
    required = {
        "path", "header", "defines", "resolves", "allowed_includes",
        "allowed_header_includes", "allowed_undefined", "base_references",
        "provenance", "evidence", "source_sha256", "header_sha256",
    }
    for index, unit in enumerate(value):
        if not isinstance(unit, dict) or set(unit) != required:
            fail("support-shape", f"descriptor_support_units[{index}] has invalid keys")
        path = unit["path"]
        if not isinstance(path, str) or path <= previous:
            fail("support-order", "support paths must be sorted and unique")
        previous = path
        if check_files:
            _safe_file(root, path, SUPPORT_BOUNDARY)
        header = unit["header"]
        if not isinstance(header, str):
            fail("support-header", f"{path}: header must be a path")
        if check_files:
            _safe_file(root, header, SUPPORT_BOUNDARY)
        source_sha256 = unit["source_sha256"]
        if not isinstance(source_sha256, str) or not re.fullmatch(r"[0-9a-f]{64}", source_sha256):
            fail("support-source-hash", f"{path}: source_sha256 must be lowercase SHA-256")
        header_sha256 = unit["header_sha256"]
        if not isinstance(header_sha256, str) or not re.fullmatch(r"[0-9a-f]{64}", header_sha256):
            fail("support-header-hash", f"{header}: header_sha256 must be lowercase SHA-256")
        defines = _string_list(unit["defines"], f"support[{index}].defines", symbols=True)
        resolves = _string_list(unit["resolves"], f"support[{index}].resolves", symbols=True)
        includes = _string_list(unit["allowed_includes"],
                                f"support[{index}].allowed_includes")
        header_includes = _string_list(
            unit["allowed_header_includes"],
            f"support[{index}].allowed_header_includes",
        )
        allowed_undefined = _string_list(
            unit["allowed_undefined"], f"support[{index}].allowed_undefined", symbols=True
        )
        if defines != resolves:
            fail("support-resolves", f"{path}: global definitions must exactly equal resolves")
        if any("/" in item or "\\" in item for item in [*includes, *header_includes]):
            fail("support-include", f"{path}: include names must not contain paths")
        base_references = unit["base_references"]
        if not isinstance(base_references, dict) or sorted(base_references) != resolves:
            fail("support-provenance", f"{path}: base_references must cover every resolve")
        for symbol, references in base_references.items():
            checked = _string_list(references, f"support[{index}].base_references.{symbol}")
            if any(item not in legacy_units for item in checked):
                fail("support-provenance", f"{path}: {symbol} names a non-legacy reference")
        for field in ("provenance", "evidence"):
            text = unit[field]
            if not isinstance(text, str) or len(text.strip()) < 24:
                fail("support-evidence", f"{path}: {field} requires reviewable evidence")
        result.append(unit)
    return result


def validate_contract(
    root: Path, value: object, *, check_files: bool = True
) -> tuple[list[str], list[dict[str, object]], dict[str, dict[str, object]]]:
    if not isinstance(value, dict):
        fail("contract-shape", "closure contract must be an object")
    version = value.get("schema_version")
    if version not in {1, SCHEMA_VERSION}:
        fail("contract-version", "closure contract must be schema v1 or v2 for db2")
    required = {
        "schema_version", "module", "source_revision", "source_fingerprint",
        "probe", "translation_units", "unresolved", "summary", "fingerprint",
    }
    if version == SCHEMA_VERSION:
        required.add("descriptor_support_units")
    if set(value) != required:
        fail("contract-keys", f"keys mismatch: expected={sorted(required)}, actual={sorted(value)}")
    if value["module"] != "db2":
        fail("contract-version", "closure contract must belong to db2")
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
    if version == SCHEMA_VERSION:
        expected_probe.update({
            "support_compile_flags": SUPPORT_COMPILE_FLAGS,
            "support_include_roots": SUPPORT_INCLUDE_ROOTS,
        })
    if probe_value != expected_probe:
        fail("probe-policy", "probe must use the frozen no-library/no-helper policy")
    units = _string_list(value["translation_units"], "translation_units")
    if check_files:
        for item in units:
            _safe_file(root, item)
    support_units = _validate_support_units(
        root, value.get("descriptor_support_units", []), units, check_files=check_files
    )
    all_units = [*units, *(str(unit["path"]) for unit in support_units)]

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
        if any(item not in all_units for item in references):
            fail("unresolved-reference", f"{symbol} references an undeclared translation unit")
        if row["disposition"] not in DISPOSITIONS:
            fail("unresolved-disposition", f"{symbol} has invalid disposition")
        evidence = row["evidence"]
        if not isinstance(evidence, str) or len(evidence.strip()) < 12:
            fail("unresolved-evidence", f"{symbol} requires concise reviewable evidence")
        rows[symbol] = row

    summary = value["summary"]
    summary_keys = {"translation_units", "unresolved_symbols", "dispositions"}
    if version == SCHEMA_VERSION:
        summary_keys.add("descriptor_support_units")
    if not isinstance(summary, dict) or set(summary) != summary_keys:
        fail("summary-shape", "summary has invalid keys")
    counts = {name: 0 for name in sorted(DISPOSITIONS)}
    for row in rows.values():
        counts[str(row["disposition"])] += 1
    expected_summary = {
        "translation_units": len(units),
        "unresolved_symbols": len(rows),
        "dispositions": counts,
    }
    if version == SCHEMA_VERSION:
        expected_summary["descriptor_support_units"] = len(support_units)
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
    return units, support_units, rows


def check(root: Path, *, run_probe: bool = True) -> None:
    descriptor = _load(root, DESCRIPTOR)
    if not isinstance(descriptor, dict) or not isinstance(descriptor.get("contracts"), list):
        fail("descriptor", "DB2 descriptor has no contracts array")
    if CONTRACT.as_posix() not in descriptor["contracts"]:
        fail("descriptor-contract", f"DB2 descriptor does not own {CONTRACT}")
    contract = _load(root, CONTRACT)
    units, support_units, rows = validate_contract(root, contract)
    assert isinstance(contract, dict)
    if contract["schema_version"] != SCHEMA_VERSION:
        fail("contract-version", f"current closure contract must be schema v{SCHEMA_VERSION}")
    expected_support = descriptor_support_policy(root, descriptor)
    if support_units != expected_support:
        fail("support-policy", "contract support units differ from reviewed checker policy")
    discovered = discover_sources(root)
    if units != discovered:
        missing = sorted(set(discovered) - set(units))
        extra = sorted(set(units) - set(discovered))
        fail("source-closure", f"translation-unit mismatch; missing={missing}, extra={extra}")
    all_sources = [
        *units,
        *(str(unit["path"]) for unit in support_units),
        *(str(unit["header"]) for unit in support_units),
    ]
    actual_source_fingerprint = source_fingerprint(root, all_sources)
    if contract["source_fingerprint"] != actual_source_fingerprint:
        fail(
            "source-fingerprint",
            "DB2 translation-unit content changed; regenerate and review closure",
        )
    if run_probe:
        actual = probe(root, units, support_units)
        expected = {symbol: list(row["references"]) for symbol, row in rows.items()}
        if actual != expected:
            added = sorted(set(actual) - set(expected))
            removed = sorted(set(expected) - set(actual))
            changed = sorted(symbol for symbol in set(actual) & set(expected)
                             if actual[symbol] != expected[symbol])
            fail("unresolved-drift",
                 f"closure changed; added={added}, removed={removed}, references_changed={changed}")


def compare_contracts(root: Path, previous: object, current: object) -> None:
    previous_units, previous_support, previous_rows = validate_contract(
        root, previous, check_files=False
    )
    current_units, current_support, current_rows = validate_contract(root, current)
    added_units = sorted(set(current_units) - set(previous_units))
    if added_units:
        fail("previous-source-growth", f"new DB2 translation units are forbidden: {added_units}")
    removed_units = sorted(set(previous_units) - set(current_units))
    if removed_units:
        fail("previous-source-removal", f"legacy DB2 translation units disappeared: {removed_units}")
    previous_support_by_path = {str(unit["path"]): unit for unit in previous_support}
    current_support_by_path = {str(unit["path"]): unit for unit in current_support}
    removed_support = sorted(set(previous_support_by_path) - set(current_support_by_path))
    if removed_support:
        fail("previous-support-removal", f"descriptor support units disappeared: {removed_support}")
    changed_support = sorted(
        path for path in set(previous_support_by_path) & set(current_support_by_path)
        if previous_support_by_path[path] != current_support_by_path[path]
    )
    if changed_support:
        fail("previous-support-change", f"reviewed support policy changed: {changed_support}")
    added_support = [
        current_support_by_path[path]
        for path in sorted(set(current_support_by_path) - set(previous_support_by_path))
    ]
    for unit in added_support:
        base_references = unit["base_references"]
        assert isinstance(base_references, dict)
        for symbol in unit["resolves"]:
            before = previous_rows.get(str(symbol))
            if (before is None or before["disposition"] != "portable-core-promotion" or
                    before["references"] != base_references[symbol]):
                fail("previous-support-admission",
                     f"{unit['path']}: {symbol} lacks exact portable-core base evidence")
            if symbol in current_rows:
                fail("previous-support-resolution",
                     f"{unit['path']}: declared resolution {symbol} remains unresolved")
    added_symbols = sorted(set(current_rows) - set(previous_rows))
    if added_symbols:
        fail("previous-symbol-growth", f"new unresolved symbols are forbidden: {added_symbols}")
    expanded: list[str] = []
    for symbol in sorted(set(current_rows) & set(previous_rows)):
        before = set(previous_rows[symbol]["references"])
        after = set(current_rows[symbol]["references"])
        growth = after - before
        permitted_paths = {
            str(unit["path"]) for unit in added_support
            if symbol in unit["allowed_undefined"]
        }
        if growth and not (
                previous_rows[symbol]["disposition"] == "system-link" and
                growth <= permitted_paths):
            expanded.append(symbol)
    if expanded:
        fail("previous-reference-growth", f"unresolved reference sets grew: {expanded}")
    if added_support and len(current_rows) >= len(previous_rows):
        fail("previous-support-shrink", "support admission must strictly shrink unresolved debt")


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
