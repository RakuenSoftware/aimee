#!/usr/bin/env python3
"""Validate the DB2 operation catalog and generate its version-1 wire artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import NoReturn


ROOT = Path(__file__).resolve().parent.parent
CATALOG = Path("src/modules/db2/eventcontract/operations.json")
DESCRIPTOR = Path("src/modules/db2/module.yaml")
PROCESS_CONTRACTS = Path("src/modules/process-contracts.json")
HEADER = Path("src/modules/db2/include/aimee/db2/module_api.h")
BASELINE = Path("tests/baselines/modules/db2-wire-v1.json")
DECLARATION_REVIEW = Path("src/modules/db2/eventcontract/declaration-review.json")
DECLARATION_LEDGER = Path("tests/baselines/db2/declarations-v1.json")
MAX_BYTES = 1_048_576
MAX_LEDGER_BYTES = 2_097_152
MAX_DEPTH = 32
MAX_ARRAY = 4096
FAMILIES = (
    "lifecycle", "tenancy", "memory", "index", "learning", "organization", "custody",
    "maintenance",
)
RESULT_CODES = ("ok", "not_found", "conflict", "denied", "retryable", "invalid_state")
NAME = re.compile(r"^[a-z][a-z0-9]*(?:_[a-z0-9]+)*$")


class ContractError(ValueError):
    """A fail-closed catalog or generated-artifact error."""


def fail(rule: str, message: str) -> NoReturn:
    raise ContractError(f"rule={rule}: {message}")


def _duplicates(label: str):
    def reject(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                fail("json-duplicate-key", f"{label}: duplicate key {key!r}")
            result[key] = value
        return result
    return reject


def _reject_number(value: str) -> NoReturn:
    fail("json-number-domain", f"forbidden number {value!r}")


def _domain(value: object, label: str, depth: int = 0) -> None:
    if depth > MAX_DEPTH:
        fail("json-depth", f"{label}: nesting exceeds {MAX_DEPTH}")
    if isinstance(value, str):
        if any(0xD800 <= ord(char) <= 0xDFFF for char in value):
            fail("json-surrogate", f"{label}: surrogate code point is forbidden")
    elif isinstance(value, list):
        if len(value) > MAX_ARRAY:
            fail("json-array-size", f"{label}: array exceeds {MAX_ARRAY} items")
        for item in value:
            _domain(item, label, depth + 1)
    elif isinstance(value, dict):
        for key, item in value.items():
            _domain(key, label, depth + 1)
            _domain(item, label, depth + 1)


def load_json(path: Path, maximum: int = MAX_BYTES) -> object:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        fail("input", f"cannot read {path}: {exc}")
    if len(raw) > maximum:
        fail("input-size", f"{path} exceeds {maximum} bytes")
    if raw.startswith(b"\xef\xbb\xbf"):
        fail("json-bom", f"{path} begins with a UTF-8 BOM")
    try:
        text = raw.decode("utf-8", "strict")
        value = json.loads(
            text,
            object_pairs_hook=_duplicates(str(path)),
            parse_float=_reject_number,
            parse_constant=_reject_number,
        )
    except UnicodeDecodeError as exc:
        fail("json-encoding", f"{path}: {exc}")
    except json.JSONDecodeError as exc:
        fail("json-parse", f"{path}: {exc.msg} at {exc.lineno}:{exc.colno}")
    _domain(value, str(path))
    return value


def _keys(value: object, expected: set[str], label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        fail("shape", f"{label} must be an object")
    if set(value) != expected:
        fail("keys", f"{label} keys differ; expected={sorted(expected)}, actual={sorted(value)}")
    return value


def _integer(value: object, label: str, low: int, high: int) -> int:
    if type(value) is not int or not low <= value <= high:
        fail("integer", f"{label} must be an integer in [{low}, {high}]")
    return value


def _string(value: object, label: str, maximum: int = 128) -> str:
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > maximum:
        fail("string", f"{label} must be a nonempty string of at most {maximum} UTF-8 bytes")
    return value


def validate_catalog(value: object) -> dict[str, object]:
    catalog = _keys(value, {
        "schema_version", "module", "wire_version", "catalog_complete", "families",
        "result_codes", "operations",
    }, "catalog")
    if type(catalog["schema_version"]) is not int or catalog["schema_version"] != 1:
        fail("schema-version", "schema_version must equal 1")
    if catalog["module"] != "db2":
        fail("module", "module must equal 'db2'")
    if type(catalog["wire_version"]) is not int or catalog["wire_version"] != 1:
        fail("wire-version", "wire_version must equal 1")
    if type(catalog["catalog_complete"]) is not bool:
        fail("catalog-complete-type", "catalog_complete must be boolean")
    if catalog["catalog_complete"]:
        fail("catalog-complete", "the health-only catalog cannot claim declaration completeness")

    raw_families = catalog["families"]
    if not isinstance(raw_families, list) or len(raw_families) != len(FAMILIES):
        fail("families", f"families must contain the {len(FAMILIES)} canonical entries")
    seen_kinds: set[int] = set()
    families: dict[str, dict[str, object]] = {}
    for index, (raw, expected_name) in enumerate(zip(raw_families, FAMILIES, strict=True), 1):
        family = _keys(raw, {"id", "name", "event_kind", "active"}, f"families[{index-1}]")
        if family["name"] != expected_name:
            fail("family-order", f"family {index} must be {expected_name!r}")
        if _integer(family["id"], f"{expected_name}.id", 1, 255) != index:
            fail("family-id", f"{expected_name} id must equal {index}")
        kind = _integer(family["event_kind"], f"{expected_name}.event_kind", 1, 65535)
        if kind != 11520 + index:
            fail("family-event-kind", f"{expected_name} event_kind must equal {11520+index}")
        if kind in seen_kinds:
            fail("family-event-duplicate", f"event kind {kind} is duplicated")
        seen_kinds.add(kind)
        expected_active = index == 1
        if type(family["active"]) is not bool or family["active"] is not expected_active:
            fail("family-active", "only lifecycle may be active in the health-only catalog")
        families[expected_name] = family

    if catalog["result_codes"] != list(RESULT_CODES):
        fail("result-codes", "result_codes must equal the closed version-1 result set")
    raw_operations = catalog["operations"]
    if not isinstance(raw_operations, list) or not raw_operations:
        fail("operations", "operations must be a nonempty array")
    seen_ids: set[tuple[str, int]] = set()
    seen_names: set[str] = set()
    order: list[tuple[int, int]] = []
    for index, raw in enumerate(raw_operations):
        operation = _keys(raw, {
            "family", "id", "name", "wire_format", "scope", "transaction", "idempotency",
            "results", "db3_placement", "db3_reason", "request", "reply",
        }, f"operations[{index}]")
        family_name = operation["family"]
        if family_name not in families:
            fail("operation-family", f"operation {index} names unknown family {family_name!r}")
        identifier = _integer(operation["id"], f"operations[{index}].id", 1, 0xffffffff)
        name = _string(operation["name"], f"operations[{index}].name")
        if not NAME.fullmatch(name):
            fail("operation-name", f"invalid operation name {name!r}")
        key = (str(family_name), identifier)
        if key in seen_ids or name in seen_names:
            fail("operation-duplicate", f"duplicate operation {key!r}/{name!r}")
        seen_ids.add(key)
        seen_names.add(name)
        position = (_integer(families[str(family_name)]["id"], "family.id", 1, 255), identifier)
        if order and position <= order[-1]:
            fail("operation-order", "operations must be sorted by family id then operation id")
        order.append(position)
        if operation["wire_format"] != "db2-health-v1" or name != "health" or key != ("lifecycle", 1):
            fail("unsupported-operation", "the bootstrap generator supports only lifecycle.health")
        if operation["scope"] != "none" or operation["transaction"] != "none" or \
                operation["idempotency"] != "safe":
            fail("operation-semantics", "health must be unscoped, transaction-free, and safe")
        if operation["results"] != ["ok"]:
            fail("operation-results", "health results must equal ['ok']")
        if operation["db3_placement"] != "retained-db2":
            fail("db3-placement", "health must remain in DB2")
        _string(operation["db3_reason"], "health.db3_reason", 256)
        request = _keys(operation["request"], {"magic", "encoded_size"}, "health.request")
        reply = _keys(operation["reply"], {"magic", "encoded_size", "flags"}, "health.reply")
        if _integer(request["magic"], "health.request.magic", 0, 0xffffffff) != 0x51483244 or \
                request["encoded_size"] != 8:
            fail("health-request", "health request magic/size differ from version 1")
        if _integer(reply["magic"], "health.reply.magic", 0, 0xffffffff) != 0x52483244 or \
                reply["encoded_size"] != 16:
            fail("health-reply", "health reply magic/size differ from version 1")
        expected_flags = ((0, "schema"), (1, "pg_trgm"), (2, "kb_tables"))
        if not isinstance(reply["flags"], list) or len(reply["flags"]) != len(expected_flags):
            fail("health-flags", "health reply flags differ from version 1")
        for flag_index, (raw_flag, expected_flag) in enumerate(
                zip(reply["flags"], expected_flags, strict=True)):
            flag = _keys(raw_flag, {"bit", "name"}, f"health.reply.flags[{flag_index}]")
            bit, flag_name = expected_flag
            if _integer(flag["bit"], f"health.reply.flags[{flag_index}].bit", 0, 31) != bit or \
                    flag["name"] != flag_name:
                fail("health-flags", "health reply flags differ from version 1")
    if len(raw_operations) != 1:
        fail("unsupported-operation", "the bootstrap generator requires exactly one health operation")
    return catalog


def _validate_repository_bindings(root: Path, catalog: dict[str, object]) -> None:
    raw_descriptor = load_json(root / DESCRIPTOR)
    if not isinstance(raw_descriptor, dict):
        fail("descriptor-ownership", f"{DESCRIPTOR} must be an object")
    descriptor = raw_descriptor
    contracts = descriptor.get("contracts")
    if (not isinstance(contracts, list) or contracts.count(CATALOG.as_posix()) != 1 or
            not all(isinstance(item, str) for item in contracts)):
        fail("descriptor-ownership", f"{DESCRIPTOR} must own {CATALOG} exactly once")
    process = load_json(root / PROCESS_CONTRACTS)
    if not isinstance(process, dict) or not isinstance(process.get("components"), list):
        fail("process-contracts", f"{PROCESS_CONTRACTS} has no components array")
    families = catalog["families"]
    assert isinstance(families, list)
    reserved = {int(item["event_kind"]): bool(item["active"]) for item in families}
    db2 = None
    for component in process["components"]:
        if not isinstance(component, dict):
            fail("process-contracts", "component must be an object")
        if component.get("id") == "db2":
            db2 = component
        elif isinstance(component.get("stages"), list):
            for stage in component["stages"]:
                if isinstance(stage, dict) and stage.get("event_kind") in reserved:
                    fail("event-kind-collision", f"{component.get('id')} uses a DB2 reserved event kind")
    if not isinstance(db2, dict) or not isinstance(db2.get("stages"), list):
        fail("process-contracts", "DB2 process contract is missing stages")
    actual = {stage.get("event_kind") for stage in db2["stages"] if isinstance(stage, dict)}
    active = {kind for kind, enabled in reserved.items() if enabled}
    if actual != active:
        fail("process-activation", f"DB2 granted kinds {sorted(actual)} differ from active {sorted(active)}")


def _validate_declaration_gate(root: Path, catalog: dict[str, object]) -> None:
    review = load_json(root / DECLARATION_REVIEW)
    ledger = load_json(root / DECLARATION_LEDGER, MAX_LEDGER_BYTES)
    if (not isinstance(review, dict) or
            type(review.get("declarations_complete")) is not bool):
        fail("declaration-review", f"{DECLARATION_REVIEW} has no completeness boolean")
    if (not isinstance(ledger, dict) or
            type(ledger.get("declarations_complete")) is not bool or
            not isinstance(ledger.get("summary"), dict) or
            type(ledger["summary"].get("audit_pending")) is not int):
        fail("declaration-ledger", f"{DECLARATION_LEDGER} has no typed completeness summary")
    if review["declarations_complete"] != ledger["declarations_complete"]:
        fail("declaration-completeness-drift", "review and generated ledger disagree")
    if catalog["catalog_complete"] and (
            not review["declarations_complete"] or ledger["summary"]["audit_pending"] != 0):
        fail("catalog-declaration-gate", "catalog completeness requires a closed declaration audit")


def catalog_fingerprint(catalog: dict[str, object]) -> str:
    canonical = json.dumps(catalog, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def _put_u32(value: int) -> bytes:
    return value.to_bytes(4, "little")


def baseline_bytes(catalog: dict[str, object]) -> bytes:
    operation = catalog["operations"][0]
    request = _put_u32(operation["request"]["magic"]) + _put_u32(catalog["wire_version"])
    replies = []
    for flags in range(8):
        body = (_put_u32(operation["reply"]["magic"]) + _put_u32(catalog["wire_version"]) +
                _put_u32(flags) + _put_u32(0))
        replies.append({"flags": flags, "hex": body.hex()})
    response = bytes.fromhex(replies[0]["hex"])
    value = {
        "schema_version": 1,
        "catalog_sha256": catalog_fingerprint(catalog),
        "wire_version": catalog["wire_version"],
        "families": catalog["families"],
        "result_codes": [
            {"id": index, "name": name} for index, name in enumerate(catalog["result_codes"])
        ],
        "operations": [{
            "family": operation["family"],
            "id": operation["id"],
            "name": operation["name"],
            "request": {
                "positive": request.hex(),
                "negative": [
                    {"mutation": "bad_magic", "hex": (bytes([request[0] ^ 1]) + request[1:]).hex()},
                    {"mutation": "bad_version", "hex": (request[:4] + bytes([request[4] ^ 1]) + request[5:]).hex()},
                    {"mutation": "short", "hex": request[:-1].hex()},
                    {"mutation": "long", "hex": (request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": replies,
                "negative": [
                    {"mutation": "bad_magic", "hex": (bytes([response[0] ^ 1]) + response[1:]).hex()},
                    {"mutation": "bad_version", "hex": (response[:4] + bytes([response[4] ^ 1]) + response[5:]).hex()},
                    {"mutation": "unknown_flags", "hex": (response[:8] + _put_u32(8) + response[12:]).hex()},
                    {"mutation": "reserved", "hex": (response[:12] + _put_u32(1)).hex()},
                    {"mutation": "short", "hex": response[:-1].hex()},
                    {"mutation": "long", "hex": (response + b"\0").hex()},
                ],
            },
        }],
    }
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def header_bytes(catalog: dict[str, object]) -> bytes:
    def macros(rows: list[tuple[str, str]]) -> str:
        width = max(len(name) for name, _ in rows)
        return "\n".join(f"#define {name:<{width}} {value}" for name, value in rows)

    fingerprint = catalog_fingerprint(catalog)
    families = catalog["families"]
    operation = catalog["operations"][0]
    flags = operation["reply"]["flags"]
    version_macros = macros([
        ("AIMEE_DB2_CONTRACT_SHA256", f'"{fingerprint}"'),
        ("AIMEE_DB2_WIRE_VERSION", f"{catalog['wire_version']}u"),
    ])
    family_macros = macros([
        (f"AIMEE_DB2_FAMILY_{item['name'].upper()}", f"{item['id']}u") for item in families
    ])
    event_macros = macros([
        (f"AIMEE_DB2_EVENT_{item['name'].upper()}", f"{item['event_kind']}u")
        for item in families
    ])
    result_macros = macros([
        (f"AIMEE_DB2_RESULT_{name.upper()}", f"{index}u")
        for index, name in enumerate(catalog["result_codes"])
    ])
    operation_macros = macros([
        ("AIMEE_DB2_EVENT_HEALTH", "AIMEE_DB2_EVENT_LIFECYCLE"),
        ("AIMEE_DB2_STAGE_HEALTH", "AIMEE_DB2_FAMILY_LIFECYCLE"),
        ("AIMEE_DB2_OPERATION_HEALTH", f"{operation['id']}u"),
        ("AIMEE_DB2_REQUEST_MAGIC",
         f"0x{operation['request']['magic']:08x}u /* \"D2HQ\", little-endian */"),
        ("AIMEE_DB2_RESPONSE_MAGIC",
         f"0x{operation['reply']['magic']:08x}u /* \"D2HR\", little-endian */"),
        ("AIMEE_DB2_REQUEST_LEN", f"{operation['request']['encoded_size']}u"),
        ("AIMEE_DB2_RESPONSE_LEN", f"{operation['reply']['encoded_size']}u"),
    ])
    all_flags = sum(1 << item["bit"] for item in flags)
    flag_macros = macros([
        *((f"AIMEE_DB2_FLAG_{item['name'].upper()}", f"0x{1 << item['bit']:x}u")
          for item in flags),
        ("AIMEE_DB2_FLAG_ALL", f"0x{all_flags:x}u"),
    ])
    text = f'''/* Generated by scripts/gen_db2_contract.py; do not edit. */
#ifndef AIMEE_DB2_MODULE_API_H
#define AIMEE_DB2_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>

{version_macros}

{family_macros}

{event_macros}

{result_macros}

{operation_macros}

{flag_macros}

static inline void aimee_db2_put_u32(uint8_t *output, uint32_t value)
{{
   for (unsigned index = 0; index < 4; ++index)
      output[index] = (uint8_t)(value >> (index * 8u));
}}

static inline uint32_t aimee_db2_get_u32(const uint8_t *input)
{{
   uint32_t value = 0;
   for (unsigned index = 0; index < 4; ++index)
      value |= (uint32_t)input[index] << (index * 8u);
   return value;
}}

static inline int aimee_db2_health_request_encode(uint8_t *output, size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_REQUEST_LEN)
      return -1;
   aimee_db2_put_u32(output, AIMEE_DB2_REQUEST_MAGIC);
   aimee_db2_put_u32(output + 4, AIMEE_DB2_WIRE_VERSION);
   return 0;
}}

static inline int aimee_db2_health_request_decode(const uint8_t *input, size_t input_len)
{{
   return input && input_len == AIMEE_DB2_REQUEST_LEN &&
                  aimee_db2_get_u32(input) == AIMEE_DB2_REQUEST_MAGIC &&
                  aimee_db2_get_u32(input + 4) == AIMEE_DB2_WIRE_VERSION
              ? 0
              : -1;
}}

static inline int aimee_db2_health_response_encode(uint32_t flags, uint8_t *output, size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_RESPONSE_LEN || (flags & ~AIMEE_DB2_FLAG_ALL) != 0)
      return -1;
   aimee_db2_put_u32(output, AIMEE_DB2_RESPONSE_MAGIC);
   aimee_db2_put_u32(output + 4, AIMEE_DB2_WIRE_VERSION);
   aimee_db2_put_u32(output + 8, flags);
   aimee_db2_put_u32(output + 12, 0u);
   return 0;
}}

static inline int aimee_db2_health_response_decode(const uint8_t *input, size_t input_len,
                                                   int *schema_ok, int *have_pg_trgm,
                                                   int *kb_tables_ok)
{{
   if (schema_ok)
      *schema_ok = 0;
   if (have_pg_trgm)
      *have_pg_trgm = 0;
   if (kb_tables_ok)
      *kb_tables_ok = 0;
   if (!input || input_len != AIMEE_DB2_RESPONSE_LEN ||
       aimee_db2_get_u32(input) != AIMEE_DB2_RESPONSE_MAGIC ||
       aimee_db2_get_u32(input + 4) != AIMEE_DB2_WIRE_VERSION ||
       aimee_db2_get_u32(input + 12) != 0u)
      return -1;
   uint32_t flags = aimee_db2_get_u32(input + 8);
   if ((flags & ~AIMEE_DB2_FLAG_ALL) != 0)
      return -1;
   if (schema_ok)
      *schema_ok = (flags & AIMEE_DB2_FLAG_SCHEMA) != 0;
   if (have_pg_trgm)
      *have_pg_trgm = (flags & AIMEE_DB2_FLAG_PG_TRGM) != 0;
   if (kb_tables_ok)
      *kb_tables_ok = (flags & AIMEE_DB2_FLAG_KB_TABLES) != 0;
   return 0;
}}

#endif /* AIMEE_DB2_MODULE_API_H */
'''
    return text.encode("utf-8")


def generated(root: Path) -> tuple[bytes, bytes]:
    catalog = validate_catalog(load_json(root / CATALOG))
    _validate_repository_bindings(root, catalog)
    _validate_declaration_gate(root, catalog)
    return header_bytes(catalog), baseline_bytes(catalog)


def _write(path: Path, content: bytes) -> None:
    if path.is_symlink():
        fail("output-symlink", f"refusing to overwrite symlink {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def run(root: Path, write: bool) -> None:
    header, baseline = generated(root)
    outputs = ((HEADER, header), (BASELINE, baseline))
    if write:
        for relative, content in outputs:
            _write(root / relative, content)
        return
    for relative, expected in outputs:
        try:
            actual = (root / relative).read_bytes()
        except OSError as exc:
            fail("generated-input", f"cannot read {relative}: {exc}")
        if actual != expected:
            fail("generated-drift", f"{relative} is not generated from {CATALOG}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)
    try:
        run(args.root.resolve(), args.write)
    except (ContractError, OSError, UnicodeError, ValueError) as exc:
        print(f"gen_db2_contract: error: {exc}", file=sys.stderr)
        return 1
    action = "wrote" if args.write else "ok"
    print(f"gen_db2_contract: {action} ({HEADER}, {BASELINE})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
