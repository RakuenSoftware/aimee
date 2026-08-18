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
CLIENT_HEADER = Path("src/modules/db2/include/aimee/db2/client.h")
CLIENT_SOURCE = Path("src/modules/db2/client/generated.c")
GO_CONTRACT = Path("server-go/db2/contract_generated.go")
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
ENVELOPE_REQUEST_MAGIC = 0x51523244  # "D2RQ", little-endian
ENVELOPE_REPLY_MAGIC = 0x52523244  # "D2RR", little-endian
ENVELOPE_HEADER_LEN = 24
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
        "body_envelope", "result_codes", "operations",
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
        fail("catalog-complete", "the partial catalog cannot claim declaration completeness")
    envelope = _keys(
        catalog["body_envelope"], {"request_magic", "reply_magic", "header_len"},
        "body_envelope",
    )
    if (_integer(envelope["request_magic"], "body_envelope.request_magic", 0, 0xffffffff) !=
            ENVELOPE_REQUEST_MAGIC or
            _integer(envelope["reply_magic"], "body_envelope.reply_magic", 0, 0xffffffff) !=
            ENVELOPE_REPLY_MAGIC or
            _integer(envelope["header_len"], "body_envelope.header_len", 1, 0xffff) !=
            ENVELOPE_HEADER_LEN):
        fail("body-envelope", "body envelope magic or header length differs from version 1")

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
            fail("family-active", "only lifecycle may be active in the partial catalog")
        families[expected_name] = family

    if catalog["result_codes"] != list(RESULT_CODES):
        fail("result-codes", "result_codes must equal the closed version-1 result set")
    raw_operations = catalog["operations"]
    if not isinstance(raw_operations, list) or not raw_operations:
        fail("operations", "operations must be a nonempty array")
    seen_ids: set[tuple[str, int]] = set()
    seen_names: set[str] = set()
    seen_c_symbols: set[str] = set()
    order: list[tuple[int, int]] = []
    for index, raw in enumerate(raw_operations):
        operation = _keys(raw, {
            "family", "id", "name", "wire_format", "scope", "transaction", "idempotency",
            "results", "db3_placement", "db3_reason", "c_symbols", "request", "reply",
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
        expected_transaction = "single-statement" if name == "reembed_clear" else "none"
        if operation["scope"] != "none" or operation["transaction"] != expected_transaction or \
                operation["idempotency"] != "safe":
            fail("operation-semantics",
                 f"{name} must be unscoped, {expected_transaction}, and safe")
        if operation["db3_placement"] != "retained-db2":
            fail("db3-placement", f"{name} must remain in DB2")
        _string(operation["db3_reason"], f"{name}.db3_reason", 256)
        if not isinstance(operation["c_symbols"], list) or not operation["c_symbols"] or not all(
                isinstance(symbol, str) and NAME.fullmatch(symbol)
                for symbol in operation["c_symbols"]):
            fail("operation-c-symbols", f"{name} must name canonical C backend symbols")
        for symbol in operation["c_symbols"]:
            if symbol in seen_c_symbols:
                fail("operation-c-symbols", f"C backend symbol {symbol!r} is duplicated")
            seen_c_symbols.add(symbol)
        if key == ("lifecycle", 1) and name == "health" and \
                operation["wire_format"] == "db2-health-v1":
            if operation["c_symbols"] != [
                    "db2_health_probe", "db2_is_initialized", "db2_kb_health_probe"]:
                fail("operation-c-symbols", "health C symbols differ from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "health results must equal ['ok']")
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
        elif key == ("lifecycle", 2) and name == "embedding_dimension" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            if operation["c_symbols"] != ["db2_embedding_dim"]:
                fail("operation-c-symbols",
                     "embedding_dimension C symbol differs from the reviewed backend")
            if operation["results"] != ["ok", "invalid_state"]:
                fail("operation-results",
                     "embedding_dimension results must equal ['ok', 'invalid_state']")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "embedding_dimension.request")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "embedding_dimension.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "embedding_dimension.reply.field")
            if request != {"encoded_size": ENVELOPE_HEADER_LEN, "payload": "none"}:
                fail("embedding-dimension-request", "request must be an empty version-1 envelope")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "dimension", "type": "u32", "minimum": 1,
                              "maximum": 4000}):
                fail("embedding-dimension-reply", "reply must contain one bounded u32 on success")
        elif key == ("lifecycle", 3) and name == "pool_status" and \
                operation["wire_format"] == "db2-envelope-pool-status-v1":
            if operation["c_symbols"] != ["db2_pool_stats"]:
                fail("operation-c-symbols", "pool_status C symbol differs from the reviewed backend")
            if operation["results"] != ["ok", "invalid_state"]:
                fail("operation-results", "pool_status results must equal ['ok', 'invalid_state']")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "pool_status.request")
            reply = _keys(operation["reply"], {"encoded_size_ok", "encoded_size_error", "fields"},
                          "pool_status.reply")
            expected_fields = [
                {"name": "size", "type": "u32", "minimum": 1, "maximum": 256},
                {"name": "in_use", "type": "u32", "minimum": 0, "maximum": 256},
                {"name": "waiters", "type": "u32", "minimum": 0, "maximum": 0xffffffff},
                {"name": "lease_grants", "type": "u64", "minimum": 0},
                {"name": "lease_timeouts", "type": "u64", "minimum": 0},
                {"name": "stuck", "type": "u64", "minimum": 0},
                {"name": "poisoned", "type": "u64", "minimum": 0},
            ]
            if request != {"encoded_size": ENVELOPE_HEADER_LEN, "payload": "none"}:
                fail("pool-status-request", "request must be an empty version-1 envelope")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 44 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != expected_fields):
                fail("pool-status-reply", "reply must contain the canonical bounded pool snapshot")
        elif key == ("lifecycle", 4) and name == "embedding_refusals" and \
                operation["wire_format"] == "db2-envelope-embedding-refusals-v1":
            if operation["c_symbols"] != ["db2_embedding_dim_last_offered",
                                           "db2_embedding_dim_refused_count"]:
                fail("operation-c-symbols",
                     "embedding_refusals C symbols differ from the reviewed backend")
            if operation["results"] != ["ok", "invalid_state"]:
                fail("operation-results",
                     "embedding_refusals results must equal ['ok', 'invalid_state']")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "embedding_refusals.request")
            reply = _keys(operation["reply"], {"encoded_size_ok", "encoded_size_error", "fields"},
                          "embedding_refusals.reply")
            expected_fields = [
                {"name": "refused_count", "type": "u64", "minimum": 0},
                {"name": "last_offered", "type": "u32", "minimum": 0,
                 "maximum": 0x7fffffff},
            ]
            if request != {"encoded_size": ENVELOPE_HEADER_LEN, "payload": "none"}:
                fail("embedding-refusals-request", "request must be an empty version-1 envelope")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 12 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != expected_fields):
                fail("embedding-refusals-reply",
                     "reply must contain the canonical bounded refusal snapshot")
        elif key == ("lifecycle", 5) and name == "postgres_status" and \
                operation["wire_format"] == "db2-envelope-postgres-status-v1":
            if operation["c_symbols"] != ["db2_pg_stat_summary"]:
                fail("operation-c-symbols",
                     "postgres_status C symbols differ from the reviewed backend")
            if operation["results"] != ["ok", "invalid_state"]:
                fail("operation-results",
                     "postgres_status results must equal ['ok', 'invalid_state']")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "postgres_status.request")
            reply = _keys(operation["reply"], {"encoded_size_ok", "encoded_size_error", "fields"},
                          "postgres_status.reply")
            expected_fields = [
                {"name": "available", "type": "u32", "minimum": 0, "maximum": 15},
                {"name": "active_connections", "type": "u32", "minimum": 0,
                 "maximum": 0x7fffffff},
                {"name": "max_connections", "type": "u32", "minimum": 0,
                 "maximum": 0x7fffffff},
                {"name": "is_replica", "type": "u32", "minimum": 0, "maximum": 1},
                {"name": "replica_lag_bytes", "type": "u64", "minimum": 0},
            ]
            if request != {"encoded_size": ENVELOPE_HEADER_LEN, "payload": "none"}:
                fail("postgres-status-request", "request must be an empty version-1 envelope")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 24 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != expected_fields):
                fail("postgres-status-reply",
                     "reply must contain the canonical bounded PostgreSQL status")
        elif key == ("lifecycle", 6) and name == "reembed_status" and \
                operation["wire_format"] == "db2-envelope-reembed-status-v1":
            if operation["c_symbols"] != ["db2_reembed_in_progress_get"]:
                fail("operation-c-symbols",
                     "reembed_status C symbols differ from the reviewed backend")
            if operation["results"] != ["ok", "not_found", "invalid_state"]:
                fail("operation-results",
                     "reembed_status results must equal ['ok', 'not_found', 'invalid_state']")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "reembed_status.request")
            reply = _keys(operation["reply"], {"encoded_size_ok", "encoded_size_error", "fields"},
                          "reembed_status.reply")
            expected_fields = [
                {"name": "target_dimension", "type": "u32", "minimum": 1, "maximum": 4000},
                {"name": "started_epoch", "type": "u64", "minimum": 1},
            ]
            if request != {"encoded_size": ENVELOPE_HEADER_LEN, "payload": "none"}:
                fail("reembed-status-request", "request must be an empty version-1 envelope")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 12 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != expected_fields):
                fail("reembed-status-reply",
                     "reply must contain the canonical bounded maintenance marker")
        elif key == ("lifecycle", 7) and name == "reembed_clear" and \
                operation["wire_format"] == "db2-envelope-reembed-clear-v1":
            if operation["c_symbols"] != ["db2_reembed_in_progress_clear"]:
                fail("operation-c-symbols",
                     "reembed_clear C symbols differ from the reviewed backend")
            if operation["results"] != ["ok", "invalid_state"]:
                fail("operation-results",
                     "reembed_clear results must equal ['ok', 'invalid_state']")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "reembed_clear.request")
            reply = _keys(operation["reply"], {"encoded_size_ok", "encoded_size_error", "fields"},
                          "reembed_clear.reply")
            if request != {"encoded_size": ENVELOPE_HEADER_LEN, "payload": "none"}:
                fail("reembed-clear-request", "request must be an empty version-1 envelope")
            if reply != {"encoded_size_ok": ENVELOPE_HEADER_LEN,
                         "encoded_size_error": ENVELOPE_HEADER_LEN, "fields": []}:
                fail("reembed-clear-reply", "reply must be an empty closed-result envelope")
        else:
            fail("unsupported-operation", f"unsupported operation {key!r}/{name!r}")
    if len(raw_operations) != 7 or [item["name"] for item in raw_operations] != [
            "health", "embedding_dimension", "pool_status", "embedding_refusals",
            "postgres_status", "reembed_status", "reembed_clear"]:
        fail("unsupported-operation",
             "the partial generator requires the seven supported lifecycle operations exactly once")
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
            type(review.get("declarations_complete")) is not bool or
            not isinstance(review.get("reviews"), list)):
        fail("declaration-review", f"{DECLARATION_REVIEW} has no completeness boolean")
    if (not isinstance(ledger, dict) or
            type(ledger.get("declarations_complete")) is not bool or
            not isinstance(ledger.get("summary"), dict) or
            type(ledger["summary"].get("audit_pending")) is not int):
        fail("declaration-ledger", f"{DECLARATION_LEDGER} has no typed completeness summary")
    if review["declarations_complete"] != ledger["declarations_complete"]:
        fail("declaration-completeness-drift", "review and generated ledger disagree")

    expected: dict[str, tuple[str, str]] = {}
    operations = catalog["operations"]
    assert isinstance(operations, list)
    for operation in operations:
        assert isinstance(operation, dict)
        symbols = operation["c_symbols"]
        assert isinstance(symbols, list)
        for symbol in symbols:
            expected[str(symbol)] = (str(operation["family"]), str(operation["db3_placement"]))
    actual: dict[str, tuple[str, str]] = {}
    for index, row in enumerate(review["reviews"]):
        if not isinstance(row, dict):
            fail("declaration-review", f"review row {index} must be an object")
        if row.get("disposition") != "wire-operation":
            continue
        symbol = row.get("symbol")
        family = row.get("family")
        placement = row.get("db3_placement")
        if not all(isinstance(value, str) for value in (symbol, family, placement)):
            fail("declaration-review", f"wire review row {index} has invalid fields")
        actual[str(symbol)] = (str(family), str(placement))
    if actual != expected:
        fail("declaration-operation-binding",
             f"wire reviews {sorted(actual)} differ from catalog C symbols {sorted(expected)}")
    for symbol, binding in expected.items():
        if actual[symbol] != binding:
            fail("declaration-operation-binding",
                 f"wire review for {symbol} differs from its catalog family or placement")
    if catalog["catalog_complete"] and (
            not review["declarations_complete"] or ledger["summary"]["audit_pending"] != 0):
        fail("catalog-declaration-gate", "catalog completeness requires a closed declaration audit")


def catalog_fingerprint(catalog: dict[str, object]) -> str:
    canonical = json.dumps(catalog, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def _put_u32(value: int) -> bytes:
    return value.to_bytes(4, "little")


def _put_u16(value: int) -> bytes:
    return value.to_bytes(2, "little")


def _put_u64(value: int) -> bytes:
    return value.to_bytes(8, "little")


def _envelope(
        catalog: dict[str, object], magic: int, operation: int, code: int, payload: bytes) -> bytes:
    return (
        _put_u32(magic) + _put_u16(int(catalog["wire_version"])) +
        _put_u16(ENVELOPE_HEADER_LEN) + _put_u32(operation) + _put_u32(code) +
        _put_u32(len(payload)) + _put_u32(0) + payload
    )


def baseline_bytes(catalog: dict[str, object]) -> bytes:
    health = catalog["operations"][0]
    embedding_dimension = catalog["operations"][1]
    pool_status = catalog["operations"][2]
    embedding_refusals = catalog["operations"][3]
    postgres_status = catalog["operations"][4]
    reembed_status = catalog["operations"][5]
    reembed_clear = catalog["operations"][6]
    request = _put_u32(health["request"]["magic"]) + _put_u32(catalog["wire_version"])
    replies = []
    for flags in range(8):
        body = (_put_u32(health["reply"]["magic"]) + _put_u32(catalog["wire_version"]) +
                _put_u32(flags) + _put_u32(0))
        replies.append({"flags": flags, "hex": body.hex()})
    response = bytes.fromhex(replies[0]["hex"])
    envelope_payload = bytes.fromhex("aabbcc")
    envelope_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, 0x01020304, 5, envelope_payload,
    )
    envelope_replies = [
        {
            "result": index,
            "hex": _envelope(
                catalog, ENVELOPE_REPLY_MAGIC, 0x01020304, index, envelope_payload,
            ).hex(),
        }
        for index in range(len(RESULT_CODES))
    ]
    envelope_reply = bytes.fromhex(envelope_replies[0]["hex"])

    def mutate_u32(frame: bytes, offset: int, value: int) -> bytes:
        return frame[:offset] + _put_u32(value) + frame[offset + 4:]

    dimension_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(embedding_dimension["id"]), 0, b"",
    )
    dimension_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(embedding_dimension["id"]), 0, _put_u32(384),
    )
    dimension_invalid = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(embedding_dimension["id"]), 5, b"",
    )
    pool_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(pool_status["id"]), 0, b"",
    )
    pool_payload = (_put_u32(16) + _put_u32(2) + _put_u32(1) + _put_u64(10) +
                    _put_u64(3) + _put_u64(4) + _put_u64(5))
    pool_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(pool_status["id"]), 0, pool_payload,
    )
    pool_invalid = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(pool_status["id"]), 5, b"",
    )
    refusals_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(embedding_refusals["id"]), 0, b"",
    )
    refusals_payload = _put_u64(7) + _put_u32(768)
    refusals_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(embedding_refusals["id"]), 0, refusals_payload,
    )
    refusals_invalid = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(embedding_refusals["id"]), 5, b"",
    )
    postgres_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(postgres_status["id"]), 0, b"",
    )
    postgres_payload = (_put_u32(15) + _put_u32(12) + _put_u32(100) + _put_u32(1) +
                        _put_u64(1048576))
    postgres_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(postgres_status["id"]), 0, postgres_payload,
    )
    postgres_partial_payload = (_put_u32(3) + _put_u32(12) + _put_u32(100) +
                                _put_u32(0) + _put_u64(0))
    postgres_partial = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(postgres_status["id"]), 0,
        postgres_partial_payload,
    )
    postgres_invalid = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(postgres_status["id"]), 5, b"",
    )
    reembed_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(reembed_status["id"]), 0, b"",
    )
    reembed_payload = _put_u32(384) + _put_u64(1700000000)
    reembed_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(reembed_status["id"]), 0, reembed_payload,
    )
    reembed_absent = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(reembed_status["id"]), 1, b"",
    )
    reembed_invalid = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(reembed_status["id"]), 5, b"",
    )
    reembed_clear_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(reembed_clear["id"]), 0, b"",
    )
    reembed_clear_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(reembed_clear["id"]), 0, b"",
    )
    reembed_clear_invalid = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(reembed_clear["id"]), 5, b"",
    )

    value = {
        "schema_version": 1,
        "catalog_sha256": catalog_fingerprint(catalog),
        "wire_version": catalog["wire_version"],
        "families": catalog["families"],
        "result_codes": [
            {"id": index, "name": name} for index, name in enumerate(catalog["result_codes"])
        ],
        "body_envelope": {
            "header_len": ENVELOPE_HEADER_LEN,
            "request": {
                "positive": envelope_request.hex(),
                "negative": [
                    {"mutation": "bad_magic", "hex":
                     (bytes([envelope_request[0] ^ 1]) + envelope_request[1:]).hex()},
                    {"mutation": "bad_version", "hex":
                     (envelope_request[:4] + bytes([envelope_request[4] ^ 1]) +
                      envelope_request[5:]).hex()},
                    {"mutation": "bad_header_len", "hex":
                     (envelope_request[:6] + bytes([envelope_request[6] ^ 1]) +
                      envelope_request[7:]).hex()},
                    {"mutation": "zero_operation", "hex":
                     mutate_u32(envelope_request, 8, 0).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(envelope_request, 16, len(envelope_payload) + 1).hex()},
                    {"mutation": "reserved", "hex":
                     mutate_u32(envelope_request, 20, 1).hex()},
                    {"mutation": "short", "hex": envelope_request[:-1].hex()},
                    {"mutation": "long", "hex": (envelope_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": envelope_replies,
                "negative": [
                    {"mutation": "bad_magic", "hex":
                     (bytes([envelope_reply[0] ^ 1]) + envelope_reply[1:]).hex()},
                    {"mutation": "bad_version", "hex":
                     (envelope_reply[:4] + bytes([envelope_reply[4] ^ 1]) +
                      envelope_reply[5:]).hex()},
                    {"mutation": "bad_header_len", "hex":
                     (envelope_reply[:6] + bytes([envelope_reply[6] ^ 1]) +
                      envelope_reply[7:]).hex()},
                    {"mutation": "zero_operation", "hex":
                     mutate_u32(envelope_reply, 8, 0).hex()},
                    {"mutation": "unknown_result", "hex":
                     mutate_u32(envelope_reply, 12, len(RESULT_CODES)).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(envelope_reply, 16, len(envelope_payload) + 1).hex()},
                    {"mutation": "reserved", "hex":
                     mutate_u32(envelope_reply, 20, 1).hex()},
                    {"mutation": "short", "hex": envelope_reply[:-1].hex()},
                    {"mutation": "long", "hex": (envelope_reply + b"\0").hex()},
                ],
            },
        },
        "operations": [{
            "family": health["family"],
            "id": health["id"],
            "name": health["name"],
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
        }, {
            "family": embedding_dimension["family"],
            "id": embedding_dimension["id"],
            "name": embedding_dimension["name"],
            "request": {
                "positive": dimension_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(dimension_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(dimension_request, 16, 1).hex()},
                    {"mutation": "short", "hex": dimension_request[:-1].hex()},
                    {"mutation": "long", "hex": (dimension_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "dimension": 384, "hex": dimension_ok.hex()},
                    {"result": 5, "dimension": 0, "hex": dimension_invalid.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(dimension_ok, 8, 1).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(dimension_ok, 12, 1).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(embedding_dimension["id"]), 0, b"").hex()},
                    {"mutation": "error_with_payload", "hex":
                     mutate_u32(dimension_ok, 12, 5).hex()},
                    {"mutation": "zero_dimension", "hex":
                     (dimension_ok[:-4] + _put_u32(0)).hex()},
                    {"mutation": "dimension_too_large", "hex":
                     (dimension_ok[:-4] + _put_u32(4001)).hex()},
                    {"mutation": "short", "hex": dimension_ok[:-1].hex()},
                    {"mutation": "long", "hex": (dimension_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": pool_status["family"],
            "id": pool_status["id"],
            "name": pool_status["name"],
            "request": {
                "positive": pool_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex": mutate_u32(pool_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(pool_request, 16, 1).hex()},
                    {"mutation": "short", "hex": pool_request[:-1].hex()},
                    {"mutation": "long", "hex": (pool_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "size": 16, "in_use": 2, "waiters": 1,
                     "lease_grants": 10, "lease_timeouts": 3, "stuck": 4, "poisoned": 5,
                     "hex": pool_ok.hex()},
                    {"result": 5, "size": 0, "in_use": 0, "waiters": 0,
                     "lease_grants": 0, "lease_timeouts": 0, "stuck": 0, "poisoned": 0,
                     "hex": pool_invalid.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex": mutate_u32(pool_ok, 8, 2).hex()},
                    {"mutation": "unsupported_result", "hex": mutate_u32(pool_ok, 12, 1).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(pool_status["id"]), 0, b"").hex()},
                    {"mutation": "error_with_payload", "hex":
                     mutate_u32(pool_ok, 12, 5).hex()},
                    {"mutation": "zero_size", "hex":
                     mutate_u32(pool_ok, ENVELOPE_HEADER_LEN, 0).hex()},
                    {"mutation": "size_too_large", "hex":
                     mutate_u32(pool_ok, ENVELOPE_HEADER_LEN, 257).hex()},
                    {"mutation": "in_use_too_large", "hex":
                     mutate_u32(pool_ok, ENVELOPE_HEADER_LEN + 4, 17).hex()},
                    {"mutation": "short", "hex": pool_ok[:-1].hex()},
                    {"mutation": "long", "hex": (pool_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": embedding_refusals["family"],
            "id": embedding_refusals["id"],
            "name": embedding_refusals["name"],
            "request": {
                "positive": refusals_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(refusals_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(refusals_request, 16, 1).hex()},
                    {"mutation": "short", "hex": refusals_request[:-1].hex()},
                    {"mutation": "long", "hex": (refusals_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "refused_count": 7, "last_offered": 768,
                     "hex": refusals_ok.hex()},
                    {"result": 5, "refused_count": 0, "last_offered": 0,
                     "hex": refusals_invalid.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(refusals_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(refusals_ok, 12, 1).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(embedding_refusals["id"]), 0, b"").hex()},
                    {"mutation": "error_with_payload", "hex":
                     mutate_u32(refusals_ok, 12, 5).hex()},
                    {"mutation": "count_without_dimension", "hex":
                     mutate_u32(refusals_ok, ENVELOPE_HEADER_LEN + 8, 0).hex()},
                    {"mutation": "dimension_without_count", "hex":
                     (refusals_ok[:ENVELOPE_HEADER_LEN] + _put_u64(0) + _put_u32(768)).hex()},
                    {"mutation": "offered_too_large", "hex":
                     mutate_u32(refusals_ok, ENVELOPE_HEADER_LEN + 8, 0x80000000).hex()},
                    {"mutation": "short", "hex": refusals_ok[:-1].hex()},
                    {"mutation": "long", "hex": (refusals_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": postgres_status["family"],
            "id": postgres_status["id"],
            "name": postgres_status["name"],
            "request": {
                "positive": postgres_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(postgres_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(postgres_request, 16, 1).hex()},
                    {"mutation": "short", "hex": postgres_request[:-1].hex()},
                    {"mutation": "long", "hex": (postgres_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "available": 15, "active_connections": 12,
                     "max_connections": 100, "is_replica": 1,
                     "replica_lag_bytes": 1048576, "hex": postgres_ok.hex()},
                    {"result": 0, "available": 3, "active_connections": 12,
                     "max_connections": 100, "is_replica": 0,
                     "replica_lag_bytes": 0, "hex": postgres_partial.hex()},
                    {"result": 5, "available": 0, "active_connections": 0,
                     "max_connections": 0, "is_replica": 0,
                     "replica_lag_bytes": 0, "hex": postgres_invalid.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(postgres_ok, 8, 4).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(postgres_ok, 12, 1).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(postgres_status["id"]), 0, b"").hex()},
                    {"mutation": "error_with_payload", "hex":
                     mutate_u32(postgres_ok, 12, 5).hex()},
                    {"mutation": "unknown_availability", "hex":
                     mutate_u32(postgres_ok, ENVELOPE_HEADER_LEN, 16).hex()},
                    {"mutation": "active_without_availability", "hex":
                     mutate_u32(postgres_ok, ENVELOPE_HEADER_LEN, 14).hex()},
                    {"mutation": "max_without_availability", "hex":
                     mutate_u32(postgres_ok, ENVELOPE_HEADER_LEN, 13).hex()},
                    {"mutation": "role_without_availability", "hex":
                     mutate_u32(postgres_ok, ENVELOPE_HEADER_LEN, 11).hex()},
                    {"mutation": "lag_without_availability", "hex":
                     mutate_u32(postgres_ok, ENVELOPE_HEADER_LEN, 7).hex()},
                    {"mutation": "lag_on_primary", "hex":
                     mutate_u32(postgres_ok, ENVELOPE_HEADER_LEN + 12, 0).hex()},
                    {"mutation": "invalid_replica_role", "hex":
                     mutate_u32(postgres_ok, ENVELOPE_HEADER_LEN + 12, 2).hex()},
                    {"mutation": "short", "hex": postgres_ok[:-1].hex()},
                    {"mutation": "long", "hex": (postgres_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": reembed_status["family"],
            "id": reembed_status["id"],
            "name": reembed_status["name"],
            "request": {
                "positive": reembed_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(reembed_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(reembed_request, 16, 1).hex()},
                    {"mutation": "short", "hex": reembed_request[:-1].hex()},
                    {"mutation": "long", "hex": (reembed_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "target_dimension": 384, "started_epoch": 1700000000,
                     "hex": reembed_ok.hex()},
                    {"result": 1, "target_dimension": 0, "started_epoch": 0,
                     "hex": reembed_absent.hex()},
                    {"result": 5, "target_dimension": 0, "started_epoch": 0,
                     "hex": reembed_invalid.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(reembed_ok, 8, 5).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(reembed_ok, 12, 2).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(reembed_status["id"]), 0, b"").hex()},
                    {"mutation": "error_with_payload", "hex":
                     mutate_u32(reembed_ok, 12, 1).hex()},
                    {"mutation": "zero_dimension", "hex":
                     mutate_u32(reembed_ok, ENVELOPE_HEADER_LEN, 0).hex()},
                    {"mutation": "dimension_too_large", "hex":
                     mutate_u32(reembed_ok, ENVELOPE_HEADER_LEN, 4001).hex()},
                    {"mutation": "zero_epoch", "hex":
                     (reembed_ok[:ENVELOPE_HEADER_LEN + 4] + _put_u64(0)).hex()},
                    {"mutation": "short", "hex": reembed_ok[:-1].hex()},
                    {"mutation": "long", "hex": (reembed_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": reembed_clear["family"],
            "id": reembed_clear["id"],
            "name": reembed_clear["name"],
            "request": {
                "positive": reembed_clear_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(reembed_clear_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(reembed_clear_request, 16, 1).hex()},
                    {"mutation": "short", "hex": reembed_clear_request[:-1].hex()},
                    {"mutation": "long", "hex": (reembed_clear_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": reembed_clear_ok.hex()},
                    {"result": 5, "hex": reembed_clear_invalid.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(reembed_clear_ok, 8, 6).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(reembed_clear_ok, 12, 1).hex()},
                    {"mutation": "ok_with_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(reembed_clear["id"]), 0, b"\0").hex()},
                    {"mutation": "error_with_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(reembed_clear["id"]), 5, b"\0").hex()},
                    {"mutation": "short", "hex": reembed_clear_ok[:-1].hex()},
                    {"mutation": "long", "hex": (reembed_clear_ok + b"\0").hex()},
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
    health = catalog["operations"][0]
    embedding_dimension = catalog["operations"][1]
    pool_status = catalog["operations"][2]
    embedding_refusals = catalog["operations"][3]
    postgres_status = catalog["operations"][4]
    reembed_status = catalog["operations"][5]
    reembed_clear = catalog["operations"][6]
    flags = health["reply"]["flags"]
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
        ("AIMEE_DB2_OPERATION_HEALTH", f"{health['id']}u"),
        ("AIMEE_DB2_REQUEST_MAGIC",
         f"0x{health['request']['magic']:08x}u /* \"D2HQ\", little-endian */"),
        ("AIMEE_DB2_RESPONSE_MAGIC",
         f"0x{health['reply']['magic']:08x}u /* \"D2HR\", little-endian */"),
        ("AIMEE_DB2_REQUEST_LEN", f"{health['request']['encoded_size']}u"),
        ("AIMEE_DB2_RESPONSE_LEN", f"{health['reply']['encoded_size']}u"),
        ("AIMEE_DB2_EVENT_EMBEDDING_DIMENSION", "AIMEE_DB2_EVENT_LIFECYCLE"),
        ("AIMEE_DB2_STAGE_EMBEDDING_DIMENSION", "AIMEE_DB2_FAMILY_LIFECYCLE"),
        ("AIMEE_DB2_OPERATION_EMBEDDING_DIMENSION", f"{embedding_dimension['id']}u"),
        ("AIMEE_DB2_EMBEDDING_DIMENSION_REQUEST_LEN",
         f"{embedding_dimension['request']['encoded_size']}u"),
        ("AIMEE_DB2_EMBEDDING_DIMENSION_RESPONSE_LEN",
         f"{embedding_dimension['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_EMBEDDING_DIMENSION_ERROR_LEN",
         f"{embedding_dimension['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_EMBEDDING_DIMENSION_MIN",
         f"{embedding_dimension['reply']['field']['minimum']}u"),
        ("AIMEE_DB2_EMBEDDING_DIMENSION_MAX",
         f"{embedding_dimension['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_POOL_STATUS", "AIMEE_DB2_EVENT_LIFECYCLE"),
        ("AIMEE_DB2_STAGE_POOL_STATUS", "AIMEE_DB2_FAMILY_LIFECYCLE"),
        ("AIMEE_DB2_OPERATION_POOL_STATUS", f"{pool_status['id']}u"),
        ("AIMEE_DB2_POOL_STATUS_REQUEST_LEN", f"{pool_status['request']['encoded_size']}u"),
        ("AIMEE_DB2_POOL_STATUS_RESPONSE_LEN", f"{pool_status['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_POOL_STATUS_ERROR_LEN", f"{pool_status['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_POOL_SIZE_MAX", "256u"),
        ("AIMEE_DB2_EVENT_EMBEDDING_REFUSALS", "AIMEE_DB2_EVENT_LIFECYCLE"),
        ("AIMEE_DB2_STAGE_EMBEDDING_REFUSALS", "AIMEE_DB2_FAMILY_LIFECYCLE"),
        ("AIMEE_DB2_OPERATION_EMBEDDING_REFUSALS", f"{embedding_refusals['id']}u"),
        ("AIMEE_DB2_EMBEDDING_REFUSALS_REQUEST_LEN",
         f"{embedding_refusals['request']['encoded_size']}u"),
        ("AIMEE_DB2_EMBEDDING_REFUSALS_RESPONSE_LEN",
         f"{embedding_refusals['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_EMBEDDING_REFUSALS_ERROR_LEN",
         f"{embedding_refusals['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_EMBEDDING_OFFERED_MAX", "2147483647u"),
        ("AIMEE_DB2_EVENT_POSTGRES_STATUS", "AIMEE_DB2_EVENT_LIFECYCLE"),
        ("AIMEE_DB2_STAGE_POSTGRES_STATUS", "AIMEE_DB2_FAMILY_LIFECYCLE"),
        ("AIMEE_DB2_OPERATION_POSTGRES_STATUS", f"{postgres_status['id']}u"),
        ("AIMEE_DB2_POSTGRES_STATUS_REQUEST_LEN",
         f"{postgres_status['request']['encoded_size']}u"),
        ("AIMEE_DB2_POSTGRES_STATUS_RESPONSE_LEN",
         f"{postgres_status['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_POSTGRES_STATUS_ERROR_LEN",
         f"{postgres_status['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_POSTGRES_AVAILABLE_ACTIVE", "0x1u"),
        ("AIMEE_DB2_POSTGRES_AVAILABLE_MAX", "0x2u"),
        ("AIMEE_DB2_POSTGRES_AVAILABLE_ROLE", "0x4u"),
        ("AIMEE_DB2_POSTGRES_AVAILABLE_LAG", "0x8u"),
        ("AIMEE_DB2_POSTGRES_AVAILABLE_ALL", "0xfu"),
        ("AIMEE_DB2_POSTGRES_COUNT_MAX", "2147483647u"),
        ("AIMEE_DB2_EVENT_REEMBED_STATUS", "AIMEE_DB2_EVENT_LIFECYCLE"),
        ("AIMEE_DB2_STAGE_REEMBED_STATUS", "AIMEE_DB2_FAMILY_LIFECYCLE"),
        ("AIMEE_DB2_OPERATION_REEMBED_STATUS", f"{reembed_status['id']}u"),
        ("AIMEE_DB2_REEMBED_STATUS_REQUEST_LEN",
         f"{reembed_status['request']['encoded_size']}u"),
        ("AIMEE_DB2_REEMBED_STATUS_RESPONSE_LEN",
         f"{reembed_status['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_REEMBED_STATUS_ERROR_LEN",
         f"{reembed_status['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_REEMBED_DIMENSION_MIN", "1u"),
        ("AIMEE_DB2_REEMBED_DIMENSION_MAX", "4000u"),
        ("AIMEE_DB2_EVENT_REEMBED_CLEAR", "AIMEE_DB2_EVENT_LIFECYCLE"),
        ("AIMEE_DB2_STAGE_REEMBED_CLEAR", "AIMEE_DB2_FAMILY_LIFECYCLE"),
        ("AIMEE_DB2_OPERATION_REEMBED_CLEAR", f"{reembed_clear['id']}u"),
        ("AIMEE_DB2_REEMBED_CLEAR_REQUEST_LEN",
         f"{reembed_clear['request']['encoded_size']}u"),
        ("AIMEE_DB2_REEMBED_CLEAR_RESPONSE_LEN",
         f"{reembed_clear['reply']['encoded_size_ok']}u"),
    ])
    envelope_macros = macros([
        ("AIMEE_DB2_ENVELOPE_REQUEST_MAGIC",
         f"0x{ENVELOPE_REQUEST_MAGIC:08x}u /* \"D2RQ\", little-endian */"),
        ("AIMEE_DB2_ENVELOPE_REPLY_MAGIC",
         f"0x{ENVELOPE_REPLY_MAGIC:08x}u /* \"D2RR\", little-endian */"),
        ("AIMEE_DB2_ENVELOPE_HEADER_LEN", f"{ENVELOPE_HEADER_LEN}u"),
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

{envelope_macros}

{flag_macros}

typedef struct
{{
   uint32_t operation;
   uint32_t flags;
   uint32_t payload_len;
}} aimee_db2_request_header_t;

typedef struct
{{
   uint32_t operation;
   uint32_t result;
   uint32_t payload_len;
}} aimee_db2_reply_header_t;

typedef struct
{{
   uint32_t size;
   uint32_t in_use;
   uint32_t waiters;
   uint64_t lease_grants;
   uint64_t lease_timeouts;
   uint64_t stuck;
   uint64_t poisoned;
}} aimee_db2_pool_status_t;

typedef struct
{{
   uint64_t refused_count;
   uint32_t last_offered;
}} aimee_db2_embedding_refusals_t;

typedef struct
{{
   uint32_t available;
   uint32_t active_connections;
   uint32_t max_connections;
   uint32_t is_replica;
   uint64_t replica_lag_bytes;
}} aimee_db2_postgres_status_t;

typedef struct
{{
   uint32_t target_dimension;
   uint64_t started_epoch;
}} aimee_db2_reembed_status_t;

static inline void aimee_db2_put_u16(uint8_t *output, uint16_t value)
{{
   output[0] = (uint8_t)value;
   output[1] = (uint8_t)(value >> 8u);
}}

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

static inline uint16_t aimee_db2_get_u16(const uint8_t *input)
{{
   return (uint16_t)((uint16_t)input[0] | (uint16_t)((uint16_t)input[1] << 8u));
}}

static inline void aimee_db2_put_u64(uint8_t *output, uint64_t value)
{{
   for (unsigned index = 0; index < 8; ++index)
      output[index] = (uint8_t)(value >> (index * 8u));
}}

static inline uint64_t aimee_db2_get_u64(const uint8_t *input)
{{
   uint64_t value = 0;
   for (unsigned index = 0; index < 8; ++index)
      value |= (uint64_t)input[index] << (index * 8u);
   return value;
}}

static inline int aimee_db2_request_header_encode(uint32_t operation, uint32_t flags,
                                                  uint32_t payload_len, uint8_t *output,
                                                  size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN || operation == 0)
      return -1;
   aimee_db2_put_u32(output, AIMEE_DB2_ENVELOPE_REQUEST_MAGIC);
   aimee_db2_put_u16(output + 4, (uint16_t)AIMEE_DB2_WIRE_VERSION);
   aimee_db2_put_u16(output + 6, (uint16_t)AIMEE_DB2_ENVELOPE_HEADER_LEN);
   aimee_db2_put_u32(output + 8, operation);
   aimee_db2_put_u32(output + 12, flags);
   aimee_db2_put_u32(output + 16, payload_len);
   aimee_db2_put_u32(output + 20, 0u);
   return 0;
}}

static inline int aimee_db2_request_header_decode(const uint8_t *input, size_t input_len,
                                                  aimee_db2_request_header_t *header)
{{
   if (header)
      *header = (aimee_db2_request_header_t){{0}};
   if (!input || !header || input_len < AIMEE_DB2_ENVELOPE_HEADER_LEN ||
       aimee_db2_get_u32(input) != AIMEE_DB2_ENVELOPE_REQUEST_MAGIC ||
       aimee_db2_get_u16(input + 4) != AIMEE_DB2_WIRE_VERSION ||
       aimee_db2_get_u16(input + 6) != AIMEE_DB2_ENVELOPE_HEADER_LEN ||
       aimee_db2_get_u32(input + 8) == 0 || aimee_db2_get_u32(input + 20) != 0u ||
       (size_t)aimee_db2_get_u32(input + 16) !=
           input_len - AIMEE_DB2_ENVELOPE_HEADER_LEN)
      return -1;
   header->operation = aimee_db2_get_u32(input + 8);
   header->flags = aimee_db2_get_u32(input + 12);
   header->payload_len = aimee_db2_get_u32(input + 16);
   return 0;
}}

static inline int aimee_db2_reply_header_encode(uint32_t operation, uint32_t result,
                                                uint32_t payload_len, uint8_t *output,
                                                size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN || operation == 0 ||
       result > AIMEE_DB2_RESULT_INVALID_STATE)
      return -1;
   aimee_db2_put_u32(output, AIMEE_DB2_ENVELOPE_REPLY_MAGIC);
   aimee_db2_put_u16(output + 4, (uint16_t)AIMEE_DB2_WIRE_VERSION);
   aimee_db2_put_u16(output + 6, (uint16_t)AIMEE_DB2_ENVELOPE_HEADER_LEN);
   aimee_db2_put_u32(output + 8, operation);
   aimee_db2_put_u32(output + 12, result);
   aimee_db2_put_u32(output + 16, payload_len);
   aimee_db2_put_u32(output + 20, 0u);
   return 0;
}}

static inline int aimee_db2_reply_header_decode(const uint8_t *input, size_t input_len,
                                                aimee_db2_reply_header_t *header)
{{
   if (header)
      *header = (aimee_db2_reply_header_t){{0}};
   if (!input || !header || input_len < AIMEE_DB2_ENVELOPE_HEADER_LEN ||
       aimee_db2_get_u32(input) != AIMEE_DB2_ENVELOPE_REPLY_MAGIC ||
       aimee_db2_get_u16(input + 4) != AIMEE_DB2_WIRE_VERSION ||
       aimee_db2_get_u16(input + 6) != AIMEE_DB2_ENVELOPE_HEADER_LEN ||
       aimee_db2_get_u32(input + 8) == 0 ||
       aimee_db2_get_u32(input + 12) > AIMEE_DB2_RESULT_INVALID_STATE ||
       aimee_db2_get_u32(input + 20) != 0u ||
       (size_t)aimee_db2_get_u32(input + 16) !=
           input_len - AIMEE_DB2_ENVELOPE_HEADER_LEN)
      return -1;
   header->operation = aimee_db2_get_u32(input + 8);
   header->result = aimee_db2_get_u32(input + 12);
   header->payload_len = aimee_db2_get_u32(input + 16);
   return 0;
}}

static inline int aimee_db2_embedding_dimension_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_EMBEDDING_DIMENSION, 0u, 0u,
                                           output, capacity);
}}

static inline int aimee_db2_embedding_dimension_request_decode(const uint8_t *input,
                                                               size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_EMBEDDING_DIMENSION_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_EMBEDDING_DIMENSION &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_embedding_dimension_reply_encode(uint32_t result,
                                                             uint32_t dimension,
                                                             uint8_t *output, size_t capacity,
                                                             uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0;
   if (!output || !output_len)
      return -1;
   uint32_t payload_len = 0;
   if (result == AIMEE_DB2_RESULT_OK)
   {{
      if (dimension < AIMEE_DB2_EMBEDDING_DIMENSION_MIN ||
          dimension > AIMEE_DB2_EMBEDDING_DIMENSION_MAX ||
          capacity < AIMEE_DB2_EMBEDDING_DIMENSION_RESPONSE_LEN)
         return -1;
      payload_len = 4u;
   }}
   else if (result != AIMEE_DB2_RESULT_INVALID_STATE || dimension != 0u ||
            capacity < AIMEE_DB2_EMBEDDING_DIMENSION_ERROR_LEN)
      return -1;
   if (aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_EMBEDDING_DIMENSION, result,
                                     payload_len, output, capacity) != 0)
      return -1;
   if (payload_len != 0u)
      aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, dimension);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_embedding_dimension_reply_decode(const uint8_t *input,
                                                             size_t input_len,
                                                             uint32_t *result,
                                                             uint32_t *dimension)
{{
   if (result)
      *result = 0u;
   if (dimension)
      *dimension = 0u;
   if (!result || !dimension)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_EMBEDDING_DIMENSION)
      return -1;
   if (header.result == AIMEE_DB2_RESULT_INVALID_STATE && header.payload_len == 0u)
   {{
      *result = header.result;
      return 0;
   }}
   if (header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded < AIMEE_DB2_EMBEDDING_DIMENSION_MIN ||
       decoded > AIMEE_DB2_EMBEDDING_DIMENSION_MAX)
      return -1;
   *result = header.result;
   *dimension = decoded;
   return 0;
}}

static inline int aimee_db2_pool_status_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_POOL_STATUS, 0u, 0u, output,
                                           capacity);
}}

static inline int aimee_db2_pool_status_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_POOL_STATUS_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_POOL_STATUS && header.flags == 0u &&
                  header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_pool_status_reply_encode(uint32_t result,
                                                     const aimee_db2_pool_status_t *status,
                                                     uint8_t *output, size_t capacity,
                                                     uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0;
   if (!output || !output_len)
      return -1;
   uint32_t payload_len = 0u;
   if (result == AIMEE_DB2_RESULT_OK)
   {{
      if (!status || status->size == 0u || status->size > AIMEE_DB2_POOL_SIZE_MAX ||
          status->in_use > status->size || capacity < AIMEE_DB2_POOL_STATUS_RESPONSE_LEN)
         return -1;
      payload_len = 44u;
   }}
   else if (result != AIMEE_DB2_RESULT_INVALID_STATE || status ||
            capacity < AIMEE_DB2_POOL_STATUS_ERROR_LEN)
      return -1;
   if (aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_POOL_STATUS, result, payload_len, output,
                                     capacity) != 0)
      return -1;
   if (status)
   {{
      uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
      aimee_db2_put_u32(payload, status->size);
      aimee_db2_put_u32(payload + 4, status->in_use);
      aimee_db2_put_u32(payload + 8, status->waiters);
      aimee_db2_put_u64(payload + 12, status->lease_grants);
      aimee_db2_put_u64(payload + 20, status->lease_timeouts);
      aimee_db2_put_u64(payload + 28, status->stuck);
      aimee_db2_put_u64(payload + 36, status->poisoned);
   }}
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_pool_status_reply_decode(const uint8_t *input, size_t input_len,
                                                     uint32_t *result,
                                                     aimee_db2_pool_status_t *status)
{{
   if (result)
      *result = 0u;
   if (status)
      *status = (aimee_db2_pool_status_t){{0}};
   if (!result || !status)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_POOL_STATUS)
      return -1;
   if (header.result == AIMEE_DB2_RESULT_INVALID_STATE && header.payload_len == 0u)
   {{
      *result = header.result;
      return 0;
   }}
   if (header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 44u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_pool_status_t decoded = {{
       .size = aimee_db2_get_u32(payload),
       .in_use = aimee_db2_get_u32(payload + 4),
       .waiters = aimee_db2_get_u32(payload + 8),
       .lease_grants = aimee_db2_get_u64(payload + 12),
       .lease_timeouts = aimee_db2_get_u64(payload + 20),
       .stuck = aimee_db2_get_u64(payload + 28),
       .poisoned = aimee_db2_get_u64(payload + 36),
   }};
   if (decoded.size == 0u || decoded.size > AIMEE_DB2_POOL_SIZE_MAX ||
       decoded.in_use > decoded.size)
      return -1;
   *result = header.result;
   *status = decoded;
   return 0;
}}

static inline int aimee_db2_embedding_refusals_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_EMBEDDING_REFUSALS, 0u, 0u,
                                           output, capacity);
}}

static inline int aimee_db2_embedding_refusals_request_decode(const uint8_t *input,
                                                              size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_EMBEDDING_REFUSALS_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_EMBEDDING_REFUSALS &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_embedding_refusals_reply_encode(
    uint32_t result, const aimee_db2_embedding_refusals_t *status, uint8_t *output,
    size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0;
   if (!output || !output_len)
      return -1;
   uint32_t payload_len = 0u;
   if (result == AIMEE_DB2_RESULT_OK)
   {{
      if (!status || status->last_offered > AIMEE_DB2_EMBEDDING_OFFERED_MAX ||
          ((status->refused_count == 0u) != (status->last_offered == 0u)) ||
          capacity < AIMEE_DB2_EMBEDDING_REFUSALS_RESPONSE_LEN)
         return -1;
      payload_len = 12u;
   }}
   else if (result != AIMEE_DB2_RESULT_INVALID_STATE || status ||
            capacity < AIMEE_DB2_EMBEDDING_REFUSALS_ERROR_LEN)
      return -1;
   if (aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_EMBEDDING_REFUSALS, result, payload_len,
                                     output, capacity) != 0)
      return -1;
   if (status)
   {{
      aimee_db2_put_u64(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, status->refused_count);
      aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN + 8, status->last_offered);
   }}
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_embedding_refusals_reply_decode(
    const uint8_t *input, size_t input_len, uint32_t *result,
    aimee_db2_embedding_refusals_t *status)
{{
   if (result)
      *result = 0u;
   if (status)
      *status = (aimee_db2_embedding_refusals_t){{0}};
   if (!result || !status)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_EMBEDDING_REFUSALS)
      return -1;
   if (header.result == AIMEE_DB2_RESULT_INVALID_STATE && header.payload_len == 0u)
   {{
      *result = header.result;
      return 0;
   }}
   if (header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 12u)
      return -1;
   aimee_db2_embedding_refusals_t decoded = {{
       .refused_count = aimee_db2_get_u64(input + AIMEE_DB2_ENVELOPE_HEADER_LEN),
       .last_offered = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN + 8),
   }};
   if (decoded.last_offered > AIMEE_DB2_EMBEDDING_OFFERED_MAX ||
       ((decoded.refused_count == 0u) != (decoded.last_offered == 0u)))
      return -1;
   *result = header.result;
   *status = decoded;
   return 0;
}}

static inline int aimee_db2_postgres_status_valid(const aimee_db2_postgres_status_t *status)
{{
   if (!status || (status->available & ~AIMEE_DB2_POSTGRES_AVAILABLE_ALL) != 0u ||
       status->active_connections > AIMEE_DB2_POSTGRES_COUNT_MAX ||
       status->max_connections > AIMEE_DB2_POSTGRES_COUNT_MAX)
      return 0;
   if ((status->available & AIMEE_DB2_POSTGRES_AVAILABLE_ACTIVE) == 0u &&
       status->active_connections != 0u)
      return 0;
   if ((status->available & AIMEE_DB2_POSTGRES_AVAILABLE_MAX) == 0u &&
       status->max_connections != 0u)
      return 0;
   if ((status->available & AIMEE_DB2_POSTGRES_AVAILABLE_ROLE) == 0u)
   {{
      if (status->is_replica != 0u)
         return 0;
   }}
   else if (status->is_replica > 1u)
      return 0;
   if ((status->available & AIMEE_DB2_POSTGRES_AVAILABLE_LAG) == 0u)
      return status->replica_lag_bytes == 0u;
   return (status->available & AIMEE_DB2_POSTGRES_AVAILABLE_ROLE) != 0u &&
          status->is_replica == 1u;
}}

static inline int aimee_db2_postgres_status_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_POSTGRES_STATUS, 0u, 0u, output,
                                           capacity);
}}

static inline int aimee_db2_postgres_status_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_POSTGRES_STATUS_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_POSTGRES_STATUS && header.flags == 0u &&
                  header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_postgres_status_reply_encode(
    uint32_t result, const aimee_db2_postgres_status_t *status, uint8_t *output, size_t capacity,
    uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0;
   if (!output || !output_len)
      return -1;
   uint32_t payload_len = 0u;
   if (result == AIMEE_DB2_RESULT_OK)
   {{
      if (!aimee_db2_postgres_status_valid(status) ||
          capacity < AIMEE_DB2_POSTGRES_STATUS_RESPONSE_LEN)
         return -1;
      payload_len = 24u;
   }}
   else if (result != AIMEE_DB2_RESULT_INVALID_STATE || status ||
            capacity < AIMEE_DB2_POSTGRES_STATUS_ERROR_LEN)
      return -1;
   if (aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_POSTGRES_STATUS, result, payload_len,
                                     output, capacity) != 0)
      return -1;
   if (status)
   {{
      uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
      aimee_db2_put_u32(payload, status->available);
      aimee_db2_put_u32(payload + 4, status->active_connections);
      aimee_db2_put_u32(payload + 8, status->max_connections);
      aimee_db2_put_u32(payload + 12, status->is_replica);
      aimee_db2_put_u64(payload + 16, status->replica_lag_bytes);
   }}
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_postgres_status_reply_decode(
    const uint8_t *input, size_t input_len, uint32_t *result,
    aimee_db2_postgres_status_t *status)
{{
   if (result)
      *result = 0u;
   if (status)
      *status = (aimee_db2_postgres_status_t){{0}};
   if (!result || !status)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_POSTGRES_STATUS)
      return -1;
   if (header.result == AIMEE_DB2_RESULT_INVALID_STATE && header.payload_len == 0u)
   {{
      *result = header.result;
      return 0;
   }}
   if (header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 24u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_postgres_status_t decoded = {{
       .available = aimee_db2_get_u32(payload),
       .active_connections = aimee_db2_get_u32(payload + 4),
       .max_connections = aimee_db2_get_u32(payload + 8),
       .is_replica = aimee_db2_get_u32(payload + 12),
       .replica_lag_bytes = aimee_db2_get_u64(payload + 16),
   }};
   if (!aimee_db2_postgres_status_valid(&decoded))
      return -1;
   *result = header.result;
   *status = decoded;
   return 0;
}}

static inline int aimee_db2_reembed_status_valid(const aimee_db2_reembed_status_t *status)
{{
   return status && status->target_dimension >= AIMEE_DB2_REEMBED_DIMENSION_MIN &&
          status->target_dimension <= AIMEE_DB2_REEMBED_DIMENSION_MAX && status->started_epoch > 0u;
}}

static inline int aimee_db2_reembed_status_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_REEMBED_STATUS, 0u, 0u, output,
                                           capacity);
}}

static inline int aimee_db2_reembed_status_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_REEMBED_STATUS_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_REEMBED_STATUS && header.flags == 0u &&
                  header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_reembed_status_reply_encode(
    uint32_t result, const aimee_db2_reembed_status_t *status, uint8_t *output, size_t capacity,
    uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0;
   if (!output || !output_len)
      return -1;
   uint32_t payload_len = 0u;
   if (result == AIMEE_DB2_RESULT_OK)
   {{
      if (!aimee_db2_reembed_status_valid(status) ||
          capacity < AIMEE_DB2_REEMBED_STATUS_RESPONSE_LEN)
         return -1;
      payload_len = 12u;
   }}
   else if ((result != AIMEE_DB2_RESULT_NOT_FOUND &&
             result != AIMEE_DB2_RESULT_INVALID_STATE) ||
            status || capacity < AIMEE_DB2_REEMBED_STATUS_ERROR_LEN)
      return -1;
   if (aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_REEMBED_STATUS, result, payload_len,
                                     output, capacity) != 0)
      return -1;
   if (status)
   {{
      uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
      aimee_db2_put_u32(payload, status->target_dimension);
      aimee_db2_put_u64(payload + 4, status->started_epoch);
   }}
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_reembed_status_reply_decode(
    const uint8_t *input, size_t input_len, uint32_t *result, aimee_db2_reembed_status_t *status)
{{
   if (result)
      *result = 0u;
   if (status)
      *status = (aimee_db2_reembed_status_t){{0}};
   if (!result || !status)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_REEMBED_STATUS)
      return -1;
   if ((header.result == AIMEE_DB2_RESULT_NOT_FOUND ||
        header.result == AIMEE_DB2_RESULT_INVALID_STATE) &&
       header.payload_len == 0u)
   {{
      *result = header.result;
      return 0;
   }}
   if (header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 12u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_reembed_status_t decoded = {{
       .target_dimension = aimee_db2_get_u32(payload),
       .started_epoch = aimee_db2_get_u64(payload + 4),
   }};
   if (!aimee_db2_reembed_status_valid(&decoded))
      return -1;
   *result = header.result;
   *status = decoded;
   return 0;
}}

static inline int aimee_db2_reembed_clear_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_REEMBED_CLEAR, 0u, 0u, output,
                                           capacity);
}}

static inline int aimee_db2_reembed_clear_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_REEMBED_CLEAR_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_REEMBED_CLEAR && header.flags == 0u &&
                  header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_reembed_clear_reply_encode(uint32_t result, uint8_t *output,
                                                       size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0;
   if (!output || !output_len || capacity < AIMEE_DB2_REEMBED_CLEAR_RESPONSE_LEN ||
       (result != AIMEE_DB2_RESULT_OK && result != AIMEE_DB2_RESULT_INVALID_STATE))
      return -1;
   if (aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_REEMBED_CLEAR, result, 0u, output,
                                     capacity) != 0)
      return -1;
   *output_len = AIMEE_DB2_REEMBED_CLEAR_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_reembed_clear_reply_decode(const uint8_t *input, size_t input_len,
                                                       uint32_t *result)
{{
   if (result)
      *result = 0u;
   if (!result)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_REEMBED_CLEAR || header.payload_len != 0u ||
       (header.result != AIMEE_DB2_RESULT_OK && header.result != AIMEE_DB2_RESULT_INVALID_STATE))
      return -1;
   *result = header.result;
   return 0;
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


def client_header_bytes() -> bytes:
    text = '''/* Generated by scripts/gen_db2_contract.py; do not edit. */
#ifndef AIMEE_DB2_CLIENT_H
#define AIMEE_DB2_CLIENT_H 1

#include <stdint.h>

#include <aimee/core/event_bus/module_client.h>
#include <aimee/db2/module_api.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef aimee_module_call_result_t (*aimee_db2_call_fn)(
       void *context, uint32_t event_kind, uint32_t stage_id, uint64_t trace_id,
       uint64_t deadline_ns, const void *request_body, uint32_t request_len,
       void *response_body, uint32_t response_capacity, uint32_t *response_len,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_health_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       int *schema_ok, int *have_pg_trgm, int *kb_tables_ok,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_embedding_dimension_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *domain_result, uint32_t *dimension,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_pool_status_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *domain_result, aimee_db2_pool_status_t *status,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_embedding_refusals_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *domain_result, aimee_db2_embedding_refusals_t *status,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_postgres_status_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *domain_result, aimee_db2_postgres_status_t *status,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_reembed_status_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *domain_result, aimee_db2_reembed_status_t *status,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_reembed_clear_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *domain_result, aimee_module_cancelled_fn cancelled, void *cancel_context);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_CLIENT_H */
'''
    return text.encode("utf-8")


def client_source_bytes() -> bytes:
    text = '''/* Generated by scripts/gen_db2_contract.py; do not edit. */
#include <aimee/db2/client.h>
#include <aimee/db2/module_api.h>

aimee_module_call_result_t
aimee_db2_health_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                      uint64_t deadline_ns, int *schema_ok, int *have_pg_trgm, int *kb_tables_ok,
                      aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (schema_ok)
      *schema_ok = 0;
   if (have_pg_trgm)
      *have_pg_trgm = 0;
   if (kb_tables_ok)
      *kb_tables_ok = 0;
   if (!call)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (aimee_db2_health_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;

   aimee_module_call_result_t result = call(
       call_context, AIMEE_DB2_EVENT_HEALTH, AIMEE_DB2_STAGE_HEALTH, trace_id, deadline_ns, request,
       sizeof(request), response, sizeof(response), &response_len, cancelled, cancel_context);
   if (result != AIMEE_MODULE_CALL_OK)
      return result;
   if (aimee_db2_health_response_decode(response, response_len, schema_ok, have_pg_trgm,
                                        kb_tables_ok) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_embedding_dimension_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                   uint64_t deadline_ns, uint32_t *domain_result,
                                   uint32_t *dimension, aimee_module_cancelled_fn cancelled,
                                   void *cancel_context)
{
   if (domain_result)
      *domain_result = 0u;
   if (dimension)
      *dimension = 0u;
   if (!call || !domain_result || !dimension)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_EMBEDDING_DIMENSION_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EMBEDDING_DIMENSION_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (aimee_db2_embedding_dimension_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;

   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_EMBEDDING_DIMENSION, AIMEE_DB2_STAGE_EMBEDDING_DIMENSION,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_embedding_dimension_reply_decode(response, response_len, domain_result,
                                                  dimension) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_pool_status_call(aimee_db2_call_fn call, void *call_context,
                                                      uint64_t trace_id, uint64_t deadline_ns,
                                                      uint32_t *domain_result,
                                                      aimee_db2_pool_status_t *status,
                                                      aimee_module_cancelled_fn cancelled,
                                                      void *cancel_context)
{
   if (domain_result)
      *domain_result = 0u;
   if (status)
      *status = (aimee_db2_pool_status_t){0};
   if (!call || !domain_result || !status)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_POOL_STATUS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_POOL_STATUS_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (aimee_db2_pool_status_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_POOL_STATUS, AIMEE_DB2_STAGE_POOL_STATUS, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_pool_status_reply_decode(response, response_len, domain_result, status) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_embedding_refusals_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                  uint64_t deadline_ns, uint32_t *domain_result,
                                  aimee_db2_embedding_refusals_t *status,
                                  aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (domain_result)
      *domain_result = 0u;
   if (status)
      *status = (aimee_db2_embedding_refusals_t){0};
   if (!call || !domain_result || !status)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   uint8_t request[AIMEE_DB2_EMBEDDING_REFUSALS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EMBEDDING_REFUSALS_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (aimee_db2_embedding_refusals_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_EMBEDDING_REFUSALS, AIMEE_DB2_STAGE_EMBEDDING_REFUSALS,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_embedding_refusals_reply_decode(response, response_len, domain_result, status) !=
       0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_postgres_status_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                               uint64_t deadline_ns, uint32_t *domain_result,
                               aimee_db2_postgres_status_t *status,
                               aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (domain_result)
      *domain_result = 0u;
   if (status)
      *status = (aimee_db2_postgres_status_t){0};
   if (!call || !domain_result || !status)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   uint8_t request[AIMEE_DB2_POSTGRES_STATUS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_POSTGRES_STATUS_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (aimee_db2_postgres_status_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_POSTGRES_STATUS, AIMEE_DB2_STAGE_POSTGRES_STATUS,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_postgres_status_reply_decode(response, response_len, domain_result, status) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_reembed_status_call(aimee_db2_call_fn call, void *call_context,
                                                         uint64_t trace_id, uint64_t deadline_ns,
                                                         uint32_t *domain_result,
                                                         aimee_db2_reembed_status_t *status,
                                                         aimee_module_cancelled_fn cancelled,
                                                         void *cancel_context)
{
   if (domain_result)
      *domain_result = 0u;
   if (status)
      *status = (aimee_db2_reembed_status_t){0};
   if (!call || !domain_result || !status)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   uint8_t request[AIMEE_DB2_REEMBED_STATUS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REEMBED_STATUS_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (aimee_db2_reembed_status_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_REEMBED_STATUS, AIMEE_DB2_STAGE_REEMBED_STATUS, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_reembed_status_reply_decode(response, response_len, domain_result, status) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_reembed_clear_call(aimee_db2_call_fn call, void *call_context,
                                                        uint64_t trace_id, uint64_t deadline_ns,
                                                        uint32_t *domain_result,
                                                        aimee_module_cancelled_fn cancelled,
                                                        void *cancel_context)
{
   if (domain_result)
      *domain_result = 0u;
   if (!call || !domain_result)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   uint8_t request[AIMEE_DB2_REEMBED_CLEAR_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REEMBED_CLEAR_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (aimee_db2_reembed_clear_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_REEMBED_CLEAR, AIMEE_DB2_STAGE_REEMBED_CLEAR, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_reembed_clear_reply_decode(response, response_len, domain_result) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}
'''
    return text.encode("utf-8")


def go_contract_bytes(catalog: dict[str, object]) -> bytes:
    def go_name(value: str) -> str:
        initialisms = {"id": "ID", "kb": "KB", "ok": "OK", "pg": "PG", "url": "URL"}
        return "".join(initialisms.get(part, part.title()) for part in value.split("_"))

    fingerprint = catalog_fingerprint(catalog)
    families = catalog["families"]
    health = catalog["operations"][0]
    embedding_dimension = catalog["operations"][1]
    pool_status = catalog["operations"][2]
    embedding_refusals = catalog["operations"][3]
    postgres_status = catalog["operations"][4]
    reembed_status = catalog["operations"][5]
    reembed_clear = catalog["operations"][6]
    flags = health["reply"]["flags"]
    result_lines = "\n".join(
        f"const Result{go_name(name)} uint32 = {index}"
        for index, name in enumerate(catalog["result_codes"])
    )
    family_lines = "\n".join(
        f"const Family{go_name(item['name'])} uint32 = {item['id']}"
        for item in families
    )
    event_lines = "\n".join(
        f"const Event{go_name(item['name'])} uint32 = {item['event_kind']}"
        for item in families
    )
    flag_lines = "\n".join(
        f"const HealthFlag{go_name(item['name'])} uint32 = 1 << {item['bit']}"
        for item in flags
    )
    all_flags = sum(1 << item["bit"] for item in flags)
    text = f'''// Code generated by scripts/gen_db2_contract.py; DO NOT EDIT.

// Package db2 is the shared Go caller-side contract for the DB2 module.
package db2

import (
\t"encoding/binary"
\t"errors"
)

const ContractSHA256 = "{fingerprint}"
const WireVersion uint32 = {catalog['wire_version']}

{family_lines}

{event_lines}

{result_lines}

const EventHealth = EventLifecycle
const StageHealth = FamilyLifecycle
const OperationHealth uint32 = {health['id']}
const EventEmbeddingDimension = EventLifecycle
const StageEmbeddingDimension = FamilyLifecycle
const OperationEmbeddingDimension uint32 = {embedding_dimension['id']}
const EmbeddingDimensionMin uint32 = {embedding_dimension['reply']['field']['minimum']}
const EmbeddingDimensionMax uint32 = {embedding_dimension['reply']['field']['maximum']}
const EventPoolStatus = EventLifecycle
const StagePoolStatus = FamilyLifecycle
const OperationPoolStatus uint32 = {pool_status['id']}
const PoolSizeMax uint32 = 256
const EventEmbeddingRefusals = EventLifecycle
const StageEmbeddingRefusals = FamilyLifecycle
const OperationEmbeddingRefusals uint32 = {embedding_refusals['id']}
const EmbeddingOfferedMax uint32 = 2147483647
const EventPostgresStatus = EventLifecycle
const StagePostgresStatus = FamilyLifecycle
const OperationPostgresStatus uint32 = {postgres_status['id']}
const PostgresAvailableActive uint32 = 1 << 0
const PostgresAvailableMax uint32 = 1 << 1
const PostgresAvailableRole uint32 = 1 << 2
const PostgresAvailableLag uint32 = 1 << 3
const PostgresAvailableAll uint32 = 0xf
const PostgresCountMax uint32 = 2147483647
const EventReembedStatus = EventLifecycle
const StageReembedStatus = FamilyLifecycle
const OperationReembedStatus uint32 = {reembed_status['id']}
const ReembedDimensionMin uint32 = 1
const ReembedDimensionMax uint32 = 4000
const EventReembedClear = EventLifecycle
const StageReembedClear = FamilyLifecycle
const OperationReembedClear uint32 = {reembed_clear['id']}

const EnvelopeHeaderLen = {ENVELOPE_HEADER_LEN}
const envelopeRequestMagic uint32 = 0x{ENVELOPE_REQUEST_MAGIC:08x}
const envelopeReplyMagic uint32 = 0x{ENVELOPE_REPLY_MAGIC:08x}

const healthRequestMagic uint32 = 0x{health['request']['magic']:08x}
const healthResponseMagic uint32 = 0x{health['reply']['magic']:08x}
const healthRequestLen = {health['request']['encoded_size']}
const healthResponseLen = {health['reply']['encoded_size']}

{flag_lines}
const healthFlagAll uint32 = 0x{all_flags:x}

var ErrMalformedHealth = errors.New("db2: malformed lifecycle health frame")
var ErrMalformedEnvelope = errors.New("db2: malformed operation envelope")

// RequestHeader is the fixed-width prefix for every post-bootstrap DB2
// operation request. Payload contains the exact declared operation body.
type RequestHeader struct {{
	Operation  uint32
	Flags      uint32
	PayloadLen uint32
}}

// ReplyHeader is the fixed-width prefix for every post-bootstrap DB2 operation
// reply. Result is one of the closed Result* values above.
type ReplyHeader struct {{
	Operation  uint32
	Result     uint32
	PayloadLen uint32
}}

func encodeEnvelopeHeader(magic, operation, code, payloadLen uint32) ([]byte, error) {{
	if operation == 0 || magic == envelopeReplyMagic && code > ResultInvalidState {{
		return nil, ErrMalformedEnvelope
	}}
	header := make([]byte, EnvelopeHeaderLen)
	binary.LittleEndian.PutUint32(header[0:4], magic)
	binary.LittleEndian.PutUint16(header[4:6], uint16(WireVersion))
	binary.LittleEndian.PutUint16(header[6:8], uint16(EnvelopeHeaderLen))
	binary.LittleEndian.PutUint32(header[8:12], operation)
	binary.LittleEndian.PutUint32(header[12:16], code)
	binary.LittleEndian.PutUint32(header[16:20], payloadLen)
	return header, nil
}}

func decodeEnvelopeHeader(frame []byte, magic uint32, reply bool) (uint32, uint32, uint32, error) {{
	if len(frame) < EnvelopeHeaderLen ||
		binary.LittleEndian.Uint32(frame[0:4]) != magic ||
		binary.LittleEndian.Uint16(frame[4:6]) != uint16(WireVersion) ||
		binary.LittleEndian.Uint16(frame[6:8]) != uint16(EnvelopeHeaderLen) ||
		binary.LittleEndian.Uint32(frame[8:12]) == 0 ||
		binary.LittleEndian.Uint32(frame[20:24]) != 0 {{
		return 0, 0, 0, ErrMalformedEnvelope
	}}
	operation := binary.LittleEndian.Uint32(frame[8:12])
	code := binary.LittleEndian.Uint32(frame[12:16])
	payloadLen := binary.LittleEndian.Uint32(frame[16:20])
	if uint64(payloadLen) != uint64(len(frame)-EnvelopeHeaderLen) ||
		reply && code > ResultInvalidState {{
		return 0, 0, 0, ErrMalformedEnvelope
	}}
	return operation, code, payloadLen, nil
}}

// EncodeRequestHeader emits the header to which the caller appends payloadLen
// canonical payload bytes.
func EncodeRequestHeader(operation, flags, payloadLen uint32) ([]byte, error) {{
	return encodeEnvelopeHeader(envelopeRequestMagic, operation, flags, payloadLen)
}}

// DecodeRequestHeader validates a complete header-plus-payload frame.
func DecodeRequestHeader(frame []byte) (RequestHeader, error) {{
	operation, flags, payloadLen, err := decodeEnvelopeHeader(frame, envelopeRequestMagic, false)
	if err != nil {{
		return RequestHeader{{}}, err
	}}
	return RequestHeader{{Operation: operation, Flags: flags, PayloadLen: payloadLen}}, nil
}}

// EncodeReplyHeader emits the header to which the provider appends payloadLen
// canonical payload bytes.
func EncodeReplyHeader(operation, result, payloadLen uint32) ([]byte, error) {{
	return encodeEnvelopeHeader(envelopeReplyMagic, operation, result, payloadLen)
}}

// DecodeReplyHeader validates a complete header-plus-payload frame and the
// closed result-code domain.
func DecodeReplyHeader(frame []byte) (ReplyHeader, error) {{
	operation, result, payloadLen, err := decodeEnvelopeHeader(frame, envelopeReplyMagic, true)
	if err != nil {{
		return ReplyHeader{{}}, err
	}}
	return ReplyHeader{{Operation: operation, Result: result, PayloadLen: payloadLen}}, nil
}}

// EncodeEmbeddingDimensionRequest emits the empty request envelope for the
// effective DB2 pgvector schema width.
func EncodeEmbeddingDimensionRequest() []byte {{
	header, err := EncodeRequestHeader(OperationEmbeddingDimension, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeEmbeddingDimensionRequest validates the exact operation envelope.
func DecodeEmbeddingDimensionRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationEmbeddingDimension ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeEmbeddingDimensionReply emits either a bounded u32 success payload or
// an empty invalid-state result.
func EncodeEmbeddingDimensionReply(result, dimension uint32) ([]byte, error) {{
	var payloadLen uint32
	if result == ResultOK {{
		if dimension < EmbeddingDimensionMin || dimension > EmbeddingDimensionMax {{
			return nil, ErrMalformedEnvelope
		}}
		payloadLen = 4
	}} else if result != ResultInvalidState || dimension != 0 {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationEmbeddingDimension, result, payloadLen)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	if payloadLen == 0 {{
		return header, nil
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], dimension)
	return reply, nil
}}

// DecodeEmbeddingDimensionReply validates the operation, closed result subset,
// payload shape, and dimension domain before exposing either field.
func DecodeEmbeddingDimensionReply(reply []byte) (uint32, uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationEmbeddingDimension {{
		return 0, 0, ErrMalformedEnvelope
	}}
	if header.Result == ResultInvalidState && header.PayloadLen == 0 {{
		return header.Result, 0, nil
	}}
	if header.Result != ResultOK || header.PayloadLen != 4 {{
		return 0, 0, ErrMalformedEnvelope
	}}
	dimension := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if dimension < EmbeddingDimensionMin || dimension > EmbeddingDimensionMax {{
		return 0, 0, ErrMalformedEnvelope
	}}
	return header.Result, dimension, nil
}}

// PoolStatus is a bounded snapshot of the DB2 PostgreSQL connection pool.
type PoolStatus struct {{
	Size          uint32
	InUse         uint32
	Waiters       uint32
	LeaseGrants   uint64
	LeaseTimeouts uint64
	Stuck         uint64
	Poisoned      uint64
}}

func EncodePoolStatusRequest() []byte {{
	header, err := EncodeRequestHeader(OperationPoolStatus, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

func DecodePoolStatusRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationPoolStatus || header.Flags != 0 ||
		header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

func EncodePoolStatusReply(result uint32, status PoolStatus) ([]byte, error) {{
	var payloadLen uint32
	if result == ResultOK {{
		if status.Size == 0 || status.Size > PoolSizeMax || status.InUse > status.Size {{
			return nil, ErrMalformedEnvelope
		}}
		payloadLen = 44
	}} else if result != ResultInvalidState || status != (PoolStatus{{}}) {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationPoolStatus, result, payloadLen)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	if payloadLen == 0 {{
		return header, nil
	}}
	reply := append(header, make([]byte, payloadLen)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload[0:4], status.Size)
	binary.LittleEndian.PutUint32(payload[4:8], status.InUse)
	binary.LittleEndian.PutUint32(payload[8:12], status.Waiters)
	binary.LittleEndian.PutUint64(payload[12:20], status.LeaseGrants)
	binary.LittleEndian.PutUint64(payload[20:28], status.LeaseTimeouts)
	binary.LittleEndian.PutUint64(payload[28:36], status.Stuck)
	binary.LittleEndian.PutUint64(payload[36:44], status.Poisoned)
	return reply, nil
}}

func DecodePoolStatusReply(reply []byte) (uint32, PoolStatus, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationPoolStatus {{
		return 0, PoolStatus{{}}, ErrMalformedEnvelope
	}}
	if header.Result == ResultInvalidState && header.PayloadLen == 0 {{
		return header.Result, PoolStatus{{}}, nil
	}}
	if header.Result != ResultOK || header.PayloadLen != 44 {{
		return 0, PoolStatus{{}}, ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	status := PoolStatus{{
		Size:          binary.LittleEndian.Uint32(payload[0:4]),
		InUse:         binary.LittleEndian.Uint32(payload[4:8]),
		Waiters:       binary.LittleEndian.Uint32(payload[8:12]),
		LeaseGrants:   binary.LittleEndian.Uint64(payload[12:20]),
		LeaseTimeouts: binary.LittleEndian.Uint64(payload[20:28]),
		Stuck:         binary.LittleEndian.Uint64(payload[28:36]),
		Poisoned:      binary.LittleEndian.Uint64(payload[36:44]),
	}}
	if status.Size == 0 || status.Size > PoolSizeMax || status.InUse > status.Size {{
		return 0, PoolStatus{{}}, ErrMalformedEnvelope
	}}
	return header.Result, status, nil
}}

type EmbeddingRefusals struct {{
	RefusedCount uint64
	LastOffered  uint32
}}

func EncodeEmbeddingRefusalsRequest() []byte {{
	header, err := EncodeRequestHeader(OperationEmbeddingRefusals, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

func DecodeEmbeddingRefusalsRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationEmbeddingRefusals || header.Flags != 0 ||
		header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

func EncodeEmbeddingRefusalsReply(result uint32, status EmbeddingRefusals) ([]byte, error) {{
	var payloadLen uint32
	if result == ResultOK {{
		if status.LastOffered > EmbeddingOfferedMax ||
			(status.RefusedCount == 0) != (status.LastOffered == 0) {{
			return nil, ErrMalformedEnvelope
		}}
		payloadLen = 12
	}} else if result != ResultInvalidState || status != (EmbeddingRefusals{{}}) {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationEmbeddingRefusals, result, payloadLen)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	if payloadLen == 0 {{
		return header, nil
	}}
	reply := append(header, make([]byte, payloadLen)...)
	binary.LittleEndian.PutUint64(reply[EnvelopeHeaderLen:EnvelopeHeaderLen+8], status.RefusedCount)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen+8:], status.LastOffered)
	return reply, nil
}}

func DecodeEmbeddingRefusalsReply(reply []byte) (uint32, EmbeddingRefusals, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationEmbeddingRefusals {{
		return 0, EmbeddingRefusals{{}}, ErrMalformedEnvelope
	}}
	if header.Result == ResultInvalidState && header.PayloadLen == 0 {{
		return header.Result, EmbeddingRefusals{{}}, nil
	}}
	if header.Result != ResultOK || header.PayloadLen != 12 {{
		return 0, EmbeddingRefusals{{}}, ErrMalformedEnvelope
	}}
	status := EmbeddingRefusals{{
		RefusedCount: binary.LittleEndian.Uint64(reply[EnvelopeHeaderLen : EnvelopeHeaderLen+8]),
		LastOffered:  binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen+8:]),
	}}
	if status.LastOffered > EmbeddingOfferedMax ||
		(status.RefusedCount == 0) != (status.LastOffered == 0) {{
		return 0, EmbeddingRefusals{{}}, ErrMalformedEnvelope
	}}
	return header.Result, status, nil
}}

type PostgresStatus struct {{
	Available         uint32
	ActiveConnections uint32
	MaxConnections    uint32
	IsReplica         uint32
	ReplicaLagBytes   uint64
}}

func validPostgresStatus(status PostgresStatus) bool {{
	if status.Available & ^PostgresAvailableAll != 0 || status.ActiveConnections > PostgresCountMax ||
		status.MaxConnections > PostgresCountMax {{
		return false
	}}
	if status.Available&PostgresAvailableActive == 0 && status.ActiveConnections != 0 {{
		return false
	}}
	if status.Available&PostgresAvailableMax == 0 && status.MaxConnections != 0 {{
		return false
	}}
	if status.Available&PostgresAvailableRole == 0 {{
		if status.IsReplica != 0 {{
			return false
		}}
	}} else if status.IsReplica > 1 {{
		return false
	}}
	if status.Available&PostgresAvailableLag == 0 {{
		return status.ReplicaLagBytes == 0
	}}
	return status.Available&PostgresAvailableRole != 0 && status.IsReplica == 1
}}

func EncodePostgresStatusRequest() []byte {{
	header, err := EncodeRequestHeader(OperationPostgresStatus, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

func DecodePostgresStatusRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationPostgresStatus || header.Flags != 0 ||
		header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

func EncodePostgresStatusReply(result uint32, status PostgresStatus) ([]byte, error) {{
	var payloadLen uint32
	if result == ResultOK {{
		if !validPostgresStatus(status) {{
			return nil, ErrMalformedEnvelope
		}}
		payloadLen = 24
	}} else if result != ResultInvalidState || status != (PostgresStatus{{}}) {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationPostgresStatus, result, payloadLen)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	if payloadLen == 0 {{
		return header, nil
	}}
	reply := append(header, make([]byte, payloadLen)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload[0:4], status.Available)
	binary.LittleEndian.PutUint32(payload[4:8], status.ActiveConnections)
	binary.LittleEndian.PutUint32(payload[8:12], status.MaxConnections)
	binary.LittleEndian.PutUint32(payload[12:16], status.IsReplica)
	binary.LittleEndian.PutUint64(payload[16:24], status.ReplicaLagBytes)
	return reply, nil
}}

func DecodePostgresStatusReply(reply []byte) (uint32, PostgresStatus, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationPostgresStatus {{
		return 0, PostgresStatus{{}}, ErrMalformedEnvelope
	}}
	if header.Result == ResultInvalidState && header.PayloadLen == 0 {{
		return header.Result, PostgresStatus{{}}, nil
	}}
	if header.Result != ResultOK || header.PayloadLen != 24 {{
		return 0, PostgresStatus{{}}, ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	status := PostgresStatus{{
		Available:         binary.LittleEndian.Uint32(payload[0:4]),
		ActiveConnections: binary.LittleEndian.Uint32(payload[4:8]),
		MaxConnections:    binary.LittleEndian.Uint32(payload[8:12]),
		IsReplica:         binary.LittleEndian.Uint32(payload[12:16]),
		ReplicaLagBytes:   binary.LittleEndian.Uint64(payload[16:24]),
	}}
	if !validPostgresStatus(status) {{
		return 0, PostgresStatus{{}}, ErrMalformedEnvelope
	}}
	return header.Result, status, nil
}}

type ReembedStatus struct {{
	TargetDimension uint32
	StartedEpoch    uint64
}}

func validReembedStatus(status ReembedStatus) bool {{
	return status.TargetDimension >= ReembedDimensionMin &&
		status.TargetDimension <= ReembedDimensionMax && status.StartedEpoch > 0
}}

func EncodeReembedStatusRequest() []byte {{
	header, err := EncodeRequestHeader(OperationReembedStatus, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

func DecodeReembedStatusRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationReembedStatus || header.Flags != 0 ||
		header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

func EncodeReembedStatusReply(result uint32, status ReembedStatus) ([]byte, error) {{
	var payloadLen uint32
	if result == ResultOK {{
		if !validReembedStatus(status) {{
			return nil, ErrMalformedEnvelope
		}}
		payloadLen = 12
	}} else if (result != ResultNotFound && result != ResultInvalidState) ||
		status != (ReembedStatus{{}}) {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationReembedStatus, result, payloadLen)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	if payloadLen == 0 {{
		return header, nil
	}}
	reply := append(header, make([]byte, payloadLen)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:EnvelopeHeaderLen+4], status.TargetDimension)
	binary.LittleEndian.PutUint64(reply[EnvelopeHeaderLen+4:], status.StartedEpoch)
	return reply, nil
}}

func DecodeReembedStatusReply(reply []byte) (uint32, ReembedStatus, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationReembedStatus {{
		return 0, ReembedStatus{{}}, ErrMalformedEnvelope
	}}
	if (header.Result == ResultNotFound || header.Result == ResultInvalidState) &&
		header.PayloadLen == 0 {{
		return header.Result, ReembedStatus{{}}, nil
	}}
	if header.Result != ResultOK || header.PayloadLen != 12 {{
		return 0, ReembedStatus{{}}, ErrMalformedEnvelope
	}}
	status := ReembedStatus{{
		TargetDimension: binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen : EnvelopeHeaderLen+4]),
		StartedEpoch:    binary.LittleEndian.Uint64(reply[EnvelopeHeaderLen+4:]),
	}}
	if !validReembedStatus(status) {{
		return 0, ReembedStatus{{}}, ErrMalformedEnvelope
	}}
	return header.Result, status, nil
}}

func EncodeReembedClearRequest() []byte {{
	header, err := EncodeRequestHeader(OperationReembedClear, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

func DecodeReembedClearRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationReembedClear || header.Flags != 0 ||
		header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

func EncodeReembedClearReply(result uint32) ([]byte, error) {{
	if result != ResultOK && result != ResultInvalidState {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationReembedClear, result, 0)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	return header, nil
}}

func DecodeReembedClearReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationReembedClear || header.PayloadLen != 0 ||
		(header.Result != ResultOK && header.Result != ResultInvalidState) {{
		return 0, ErrMalformedEnvelope
	}}
	return header.Result, nil
}}

// HealthEvidence is DB2-owned PostgreSQL readiness evidence. It intentionally
// contains no DSN, SQL, provider identity, or implementation-specific detail.
type HealthEvidence struct {{
\tSchemaOK   bool
\tHavePGTrgm bool
\tKBTablesOK bool
}}

// EncodeHealthRequest emits the exact fixed-width request served by the C DB2
// module and, later, by the Go provider.
func EncodeHealthRequest() []byte {{
\trequest := make([]byte, healthRequestLen)
\tbinary.LittleEndian.PutUint32(request[0:4], healthRequestMagic)
\tbinary.LittleEndian.PutUint32(request[4:8], WireVersion)
\treturn request
}}

// DecodeHealthRequest validates the complete frame, including exact length.
func DecodeHealthRequest(request []byte) error {{
\tif len(request) != healthRequestLen ||
\t\tbinary.LittleEndian.Uint32(request[0:4]) != healthRequestMagic ||
\t\tbinary.LittleEndian.Uint32(request[4:8]) != WireVersion {{
\t\treturn ErrMalformedHealth
\t}}
\treturn nil
}}

// EncodeHealthResponse emits the exact fixed-width response used by the C
// implementation. Boolean fields make unknown flag bits unrepresentable.
func EncodeHealthResponse(evidence HealthEvidence) []byte {{
\tresponse := make([]byte, healthResponseLen)
\tbinary.LittleEndian.PutUint32(response[0:4], healthResponseMagic)
\tbinary.LittleEndian.PutUint32(response[4:8], WireVersion)
\tvar flags uint32
\tif evidence.SchemaOK {{
\t\tflags |= HealthFlagSchema
\t}}
\tif evidence.HavePGTrgm {{
\t\tflags |= HealthFlagPGTrgm
\t}}
\tif evidence.KBTablesOK {{
\t\tflags |= HealthFlagKBTables
\t}}
\tbinary.LittleEndian.PutUint32(response[8:12], flags)
\treturn response
}}

// DecodeHealthResponse rejects unknown flags, nonzero reserved bytes, and any
// short or trailing data before exposing readiness evidence.
func DecodeHealthResponse(response []byte) (HealthEvidence, error) {{
\tif len(response) != healthResponseLen ||
\t\tbinary.LittleEndian.Uint32(response[0:4]) != healthResponseMagic ||
\t\tbinary.LittleEndian.Uint32(response[4:8]) != WireVersion ||
\t\tbinary.LittleEndian.Uint32(response[12:16]) != 0 {{
\t\treturn HealthEvidence{{}}, ErrMalformedHealth
\t}}
\tflags := binary.LittleEndian.Uint32(response[8:12])
\tif flags & ^healthFlagAll != 0 {{
\t\treturn HealthEvidence{{}}, ErrMalformedHealth
\t}}
\tevidence := HealthEvidence{{}}
\tevidence.SchemaOK = flags&HealthFlagSchema != 0
\tevidence.HavePGTrgm = flags&HealthFlagPGTrgm != 0
\tevidence.KBTablesOK = flags&HealthFlagKBTables != 0
\treturn evidence, nil
}}
'''
    return text.encode("utf-8")


def generated(root: Path) -> tuple[bytes, bytes, bytes, bytes, bytes]:
    catalog = validate_catalog(load_json(root / CATALOG))
    _validate_repository_bindings(root, catalog)
    _validate_declaration_gate(root, catalog)
    return (header_bytes(catalog), client_header_bytes(), client_source_bytes(),
            go_contract_bytes(catalog), baseline_bytes(catalog))


def _write(path: Path, content: bytes) -> None:
    if path.is_symlink():
        fail("output-symlink", f"refusing to overwrite symlink {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def run(root: Path, write: bool) -> None:
    header, client_header, client_source, go_contract, baseline = generated(root)
    outputs = (
        (HEADER, header),
        (CLIENT_HEADER, client_header),
        (CLIENT_SOURCE, client_source),
        (GO_CONTRACT, go_contract),
        (BASELINE, baseline),
    )
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
    print(
        f"gen_db2_contract: {action} "
        f"({HEADER}, {CLIENT_HEADER}, {CLIENT_SOURCE}, {GO_CONTRACT}, {BASELINE})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
