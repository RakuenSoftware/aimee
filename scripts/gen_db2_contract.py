#!/usr/bin/env python3
"""Validate the DB2 operation catalog and generate its version-1 wire artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
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
# The rolling health-window aggregate, in the order the reviewed struct declares
# it. Wire order is part of the contract, so it lives here rather than being
# re-listed at each use.
HEALTH_COUNTERS = (
    "cycles", "total_contradictions", "total_promotions", "total_demotions",
    "total_expirations", "new_memories", "l1_eligible", "l2_total", "l2_stale_30_days",
)
# Corpus breakdown buckets, in wire order. The labels are the canonical tier and
# kind strings the backend groups by, so a bucket can never be silently dropped.
MEMORY_TIERS = ("L0", "L1", "L2", "L3", "L4", "L5")
MEMORY_KINDS = (
    "fact", "preference", "decision", "episode", "task",
    "scratch", "procedure", "policy", "workflow", "opinion",
)


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
        expected_transaction = ("single-statement" if name in ("reembed_clear",
                                                                "effectiveness_update",
                                                                "effectiveness_demote",
                                                                "health_record",
                                                                "promote_stable",
                                                                "reclassify_directives",
                                                                "record_l4_approval",
                                                                "prune_orphaned_l0",
                                                                "lifecycle_sweep_expired",
                                                                "demote_id",
                                                                "delete_row",
                                                                "touch",
                                                                "link_delete",
                                                                "reject",
                                                                "update_content",
                                                                "decay_confidence",
                                                                "workspace_tag_insert",
                                                                "set_cognified_kind",
                                                                "set_source_session",
                                                                "negation_tokens_update",
                                                                "entity_edge_prune_orphans",
                                                                "entity_edge_normalize_weights",
                                                                "purge_hidden_pollution",
                                                                "requeue_drifted",
                                                                "prospective_sweep_expired",
                                                                "directive_sweep_expired") else
                                "single" if name in ("reembed_clear_maintenance",
                                                     "dimension_reset") else "none")
        # A health-cycle snapshot appends a row per call, so replaying it is not
        # observationally neutral the way every other operation here is.
        expected_idempotency = ("unsafe"
                                if name in ("health_record", "demote_id", "touch", "reject",
                                            "decay_confidence")
                                else "safe")
        if operation["scope"] != "none" or operation["transaction"] != expected_transaction or \
                operation["idempotency"] != expected_idempotency:
            fail("operation-semantics",
                 f"{name} must be unscoped, {expected_transaction}, and {expected_idempotency}")
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
        elif key == ("lifecycle", 8) and name == "reembed_clear_maintenance" and \
                operation["wire_format"] == "db2-envelope-reembed-clear-maintenance-v1":
            if operation["c_symbols"] != ["db2_reembed_clear_maintenance"]:
                fail("operation-c-symbols",
                     "reembed_clear_maintenance C symbol differs from the reviewed backend")
            if operation["results"] != ["ok", "conflict", "invalid_state"]:
                fail("operation-results",
                     "reembed_clear_maintenance results must equal "
                     "['ok', 'conflict', 'invalid_state']")
            request = _keys(operation["request"], {"encoded_size", "field"},
                            "reembed_clear_maintenance.request")
            request_field = _keys(request["field"], {"name", "type", "minimum", "maximum"},
                                  "reembed_clear_maintenance.request.field")
            reply = _keys(operation["reply"],
                          {"encoded_size_payload", "encoded_size_error", "payload_results",
                           "fields"}, "reembed_clear_maintenance.reply")
            expected_fields = [
                {"name": "was_in_progress", "type": "u32", "minimum": 0, "maximum": 1},
                {"name": "recorded_dimension", "type": "u32", "minimum": 0, "maximum": 4000},
                {"name": "running_dimension", "type": "u32", "minimum": 1, "maximum": 4000},
            ]
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 4 or
                    request_field != {"name": "force", "type": "u32", "minimum": 0,
                                      "maximum": 1}):
                fail("reembed-clear-maintenance-request",
                     "request must contain one canonical boolean u32")
            if (reply["encoded_size_payload"] != ENVELOPE_HEADER_LEN + 12 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["payload_results"] != ["ok", "conflict"] or
                    reply["fields"] != expected_fields):
                fail("reembed-clear-maintenance-reply",
                     "ok/conflict replies must contain the canonical consistency snapshot")
        elif key == ("lifecycle", 9) and name == "embedder_serving_id" and \
                operation["wire_format"] == "db2-envelope-embedder-serving-id-v1":
            if operation["c_symbols"] != ["db2_embedder_serving_id"]:
                fail("operation-c-symbols",
                     "embedder_serving_id C symbol differs from the reviewed backend")
            if operation["results"] != ["ok", "invalid_state"]:
                fail("operation-results",
                     "embedder_serving_id results must equal ['ok', 'invalid_state']")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "embedder_serving_id.request")
            reply = _keys(operation["reply"],
                          {"encoded_size_min_ok", "encoded_size_max_ok",
                           "encoded_size_error", "field"},
                          "embedder_serving_id.reply")
            field = _keys(reply["field"],
                          {"name", "type", "minimum_bytes", "maximum_bytes"},
                          "embedder_serving_id.reply.field")
            if request != {"encoded_size": ENVELOPE_HEADER_LEN, "payload": "none"}:
                fail("embedder-serving-id-request",
                     "request must be an empty version-1 envelope")
            if (reply["encoded_size_min_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_max_ok"] != ENVELOPE_HEADER_LEN + 4 + 159 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "serving_id", "type": "opaque-string",
                              "minimum_bytes": 0, "maximum_bytes": 159}):
                fail("embedder-serving-id-reply",
                     "success must contain one length-prefixed bounded opaque string")
        elif key == ("lifecycle", 10) and name == "dimension_reset" and \
                operation["wire_format"] == "db2-envelope-dimension-reset-v1":
            if operation["c_symbols"] != ["db2_dim_change_reset"]:
                fail("operation-c-symbols",
                     "dimension_reset C symbol differs from the reviewed backend")
            if operation["results"] != ["ok", "conflict", "denied", "invalid_state"]:
                fail("operation-results",
                     "dimension_reset results must equal "
                     "['ok', 'conflict', 'denied', 'invalid_state']")
            request = _keys(operation["request"], {"encoded_size", "fields"},
                            "dimension_reset.request")
            reply = _keys(operation["reply"],
                          {"encoded_size_payload", "encoded_size_error", "payload_results",
                           "fields"}, "dimension_reset.reply")
            expected_request_fields = [
                {"name": "target_dimension", "type": "u32", "minimum": 1, "maximum": 4000},
                {"name": "force", "type": "u32", "minimum": 0, "maximum": 1},
                {"name": "dry_run", "type": "u32", "minimum": 0, "maximum": 1},
            ]
            expected_reply_fields = [
                {"name": "recorded_dimension", "type": "u32", "minimum": 0,
                 "maximum": 4000},
                {"name": "target_dimension", "type": "u32", "minimum": 1,
                 "maximum": 4000},
                {"name": "tables_discovered", "type": "u32", "minimum": 0,
                 "maximum": 16},
                {"name": "tables_dropped", "type": "u32", "minimum": 0, "maximum": 16},
                {"name": "rows_cleared", "type": "u64", "minimum": 0},
                {"name": "curator_requeued", "type": "i32", "minimum": -1,
                 "maximum": 0x7fffffff},
                {"name": "evidence_requeued", "type": "i32", "minimum": -1,
                 "maximum": 0x7fffffff},
            ]
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 12 or
                    request["fields"] != expected_request_fields):
                fail("dimension-reset-request",
                     "request must contain target dimension and canonical force/dry-run booleans")
            if (reply["encoded_size_payload"] != ENVELOPE_HEADER_LEN + 32 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["payload_results"] != ["ok", "conflict", "denied"] or
                    reply["fields"] != expected_reply_fields):
                fail("dimension-reset-reply",
                     "actionable results must contain the bounded reset-plan snapshot")
        elif key == ("memory", 1) and name == "level3_count" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            if operation["c_symbols"] != ["db2_memory_count_l3"]:
                fail("operation-c-symbols",
                     "level3_count C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "level3_count results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "level3_count.request")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "level3_count.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "level3_count.reply.field")
            if request != {"encoded_size": ENVELOPE_HEADER_LEN, "payload": "none"}:
                fail("level3-count-request", "request must be an empty version-1 envelope")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "count", "type": "u32", "minimum": 0,
                              "maximum": 0x7fffffff}):
                fail("level3-count-reply", "reply must contain one bounded u32 count")
        elif key == ("memory", 2) and name == "level2_count" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            if operation["c_symbols"] != ["db2_memory_count_l2"]:
                fail("operation-c-symbols",
                     "level2_count C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "level2_count results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "level2_count.request")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "level2_count.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "level2_count.reply.field")
            if request != {"encoded_size": ENVELOPE_HEADER_LEN, "payload": "none"}:
                fail("level2-count-request", "request must be an empty version-1 envelope")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "count", "type": "u32", "minimum": 0,
                              "maximum": 0x7fffffff}):
                fail("level2-count-reply", "reply must contain one bounded u32 count")
        elif key == ("memory", 3) and name == "orphaned_l0_count" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            if operation["c_symbols"] != ["db2_memory_count_orphaned_l0"]:
                fail("operation-c-symbols",
                     "orphaned_l0_count C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "orphaned_l0_count results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "orphaned_l0_count.request")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "orphaned_l0_count.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "orphaned_l0_count.reply.field")
            if request != {"encoded_size": ENVELOPE_HEADER_LEN, "payload": "none"}:
                fail("orphaned-l0-count-request",
                     "request must be an empty version-1 envelope")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "count", "type": "u32", "minimum": 0,
                              "maximum": 0x7fffffff}):
                fail("orphaned-l0-count-reply", "reply must contain one bounded u32 count")
        elif key == ("memory", 4) and name == "total_count" and \
                operation["wire_format"] == "db2-envelope-u64-v1":
            if operation["c_symbols"] != ["db2_memory_count"]:
                fail("operation-c-symbols",
                     "total_count C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "total_count results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "total_count.request")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "total_count.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "total_count.reply.field")
            if request != {"encoded_size": ENVELOPE_HEADER_LEN, "payload": "none"}:
                fail("total-count-request", "request must be an empty version-1 envelope")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 8 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "count", "type": "u64", "minimum": 0,
                              "maximum": 0x7fffffffffffffff}):
                fail("total-count-reply", "reply must contain one bounded u64 count")
        elif key == ("memory", 5) and name == "session_l2_count" and \
                operation["wire_format"] == "db2-envelope-string-u32-v1":
            if operation["c_symbols"] != ["db2_memory_count_l2_for_session"]:
                fail("operation-c-symbols",
                     "session_l2_count C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "session_l2_count results must equal ['ok']")
            request = _keys(operation["request"],
                            {"encoded_size_min", "encoded_size_max", "field"},
                            "session_l2_count.request")
            request_field = _keys(request["field"],
                                  {"name", "type", "minimum_bytes", "maximum_bytes"},
                                  "session_l2_count.request.field")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "session_l2_count.reply")
            reply_field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                                "session_l2_count.reply.field")
            if (request["encoded_size_min"] != ENVELOPE_HEADER_LEN + 5 or
                    request["encoded_size_max"] != ENVELOPE_HEADER_LEN + 4 + 127 or
                    request_field != {"name": "source_session", "type": "utf8",
                                      "minimum_bytes": 1, "maximum_bytes": 127}):
                fail("session-l2-count-request",
                     "request must contain one non-empty bounded session identifier")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply_field != {"name": "count", "type": "u32", "minimum": 0,
                                    "maximum": 0x7fffffff}):
                fail("session-l2-count-reply", "reply must contain one bounded u32 count")
        elif key == ("memory", 6) and name == "key_exists" and \
                operation["wire_format"] == "db2-envelope-string-u32-v1":
            if operation["c_symbols"] != ["db2_memory_key_exists"]:
                fail("operation-c-symbols", "key_exists C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "key_exists results must equal ['ok']")
            request = _keys(operation["request"],
                            {"encoded_size_min", "encoded_size_max", "field"},
                            "key_exists.request")
            request_field = _keys(request["field"],
                                  {"name", "type", "minimum_bytes", "maximum_bytes"},
                                  "key_exists.request.field")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "key_exists.reply")
            reply_field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                                "key_exists.reply.field")
            if (request["encoded_size_min"] != ENVELOPE_HEADER_LEN + 5 or
                    request["encoded_size_max"] != ENVELOPE_HEADER_LEN + 4 + 511 or
                    request_field != {"name": "key", "type": "utf8",
                                      "minimum_bytes": 1, "maximum_bytes": 511}):
                fail("key-exists-request", "request must contain one non-empty bounded memory key")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply_field != {"name": "exists", "type": "u32", "minimum": 0,
                                    "maximum": 1}):
                fail("key-exists-reply", "reply must contain one boolean u32 value")
        elif key == ("memory", 7) and name == "find_id_by_key_kind" and \
                operation["wire_format"] == "db2-envelope-string-pair-u32-u64-v1":
            if operation["c_symbols"] != ["db2_memory_find_id_by_key_kind"]:
                fail("operation-c-symbols",
                     "find_id_by_key_kind C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "find_id_by_key_kind results must equal ['ok']")
            request = _keys(operation["request"],
                            {"encoded_size_min", "encoded_size_max", "fields"},
                            "find_id_by_key_kind.request")
            request_fields = request["fields"]
            if not isinstance(request_fields, list) or len(request_fields) != 2:
                fail("find-id-by-key-kind-request", "request must contain exactly two fields")
            request_key = _keys(request_fields[0],
                                {"name", "type", "minimum_bytes", "maximum_bytes"},
                                "find_id_by_key_kind.request.fields[0]")
            request_kind = _keys(request_fields[1],
                                 {"name", "type", "minimum_bytes", "maximum_bytes"},
                                 "find_id_by_key_kind.request.fields[1]")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "find_id_by_key_kind.reply")
            reply_fields = reply["fields"]
            if not isinstance(reply_fields, list) or len(reply_fields) != 2:
                fail("find-id-by-key-kind-reply", "reply must contain exactly two fields")
            reply_found = _keys(reply_fields[0], {"name", "type", "minimum", "maximum"},
                                "find_id_by_key_kind.reply.fields[0]")
            reply_id = _keys(reply_fields[1], {"name", "type", "minimum", "maximum"},
                             "find_id_by_key_kind.reply.fields[1]")
            if (request["encoded_size_min"] != ENVELOPE_HEADER_LEN + 10 or
                    request["encoded_size_max"] != ENVELOPE_HEADER_LEN + 8 + 511 + 15 or
                    request_key != {"name": "key", "type": "utf8", "minimum_bytes": 1,
                                    "maximum_bytes": 511} or
                    request_kind != {"name": "kind", "type": "utf8", "minimum_bytes": 1,
                                     "maximum_bytes": 15}):
                fail("find-id-by-key-kind-request",
                     "request must contain bounded non-empty canonical key and kind fields")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 12 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply_found != {"name": "found", "type": "u32", "minimum": 0,
                                    "maximum": 1} or
                    reply_id != {"name": "id", "type": "u64", "minimum": 0,
                                 "maximum": 0x7fffffffffffffff}):
                fail("find-id-by-key-kind-reply",
                     "reply must contain consistent found and bounded identifier fields")
        elif key == ("memory", 8) and name == "key_exists_in_tier_pair" and \
                operation["wire_format"] == "db2-envelope-string-triple-u32-v1":
            if operation["c_symbols"] != ["db2_memory_key_exists_in_tier_pair"]:
                fail("operation-c-symbols",
                     "key_exists_in_tier_pair C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "key_exists_in_tier_pair results must equal ['ok']")
            request = _keys(operation["request"],
                            {"encoded_size_min", "encoded_size_max", "fields"},
                            "key_exists_in_tier_pair.request")
            request_fields = request["fields"]
            if not isinstance(request_fields, list) or len(request_fields) != 3:
                fail("key-exists-in-tier-pair-request",
                     "request must contain exactly three fields")
            request_key = _keys(request_fields[0],
                                {"name", "type", "minimum_bytes", "maximum_bytes"},
                                "key_exists_in_tier_pair.request.fields[0]")
            request_tier_a = _keys(request_fields[1],
                                   {"name", "type", "minimum_bytes", "maximum_bytes"},
                                   "key_exists_in_tier_pair.request.fields[1]")
            request_tier_b = _keys(request_fields[2],
                                   {"name", "type", "minimum_bytes", "maximum_bytes"},
                                   "key_exists_in_tier_pair.request.fields[2]")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "key_exists_in_tier_pair.reply")
            reply_field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                                "key_exists_in_tier_pair.reply.field")
            if (request["encoded_size_min"] != ENVELOPE_HEADER_LEN + 15 or
                    request["encoded_size_max"] != ENVELOPE_HEADER_LEN + 12 + 511 + 15 + 15 or
                    request_key != {"name": "key", "type": "utf8", "minimum_bytes": 1,
                                    "maximum_bytes": 511} or
                    request_tier_a != {"name": "tier_a", "type": "utf8", "minimum_bytes": 1,
                                       "maximum_bytes": 15} or
                    request_tier_b != {"name": "tier_b", "type": "utf8", "minimum_bytes": 1,
                                       "maximum_bytes": 15}):
                fail("key-exists-in-tier-pair-request",
                     "request must contain bounded non-empty canonical key and tier fields")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply_field != {"name": "exists", "type": "u32", "minimum": 0,
                                    "maximum": 1}):
                fail("key-exists-in-tier-pair-reply",
                     "reply must contain one boolean u32 value")
        elif key == ("memory", 9) and name == "effectiveness_update" and \
                operation["wire_format"] == "db2-envelope-u64-u32-f64-v1":
            if operation["c_symbols"] != ["db2_memory_health_clear_effectiveness",
                                           "db2_memory_health_set_effectiveness"]:
                fail("operation-c-symbols",
                     "effectiveness_update C symbols differ from the reviewed backend")
            if operation["results"] != ["ok", "invalid_state"]:
                fail("operation-results",
                     "effectiveness_update results must equal ['ok', 'invalid_state']")
            request = _keys(operation["request"], {"encoded_size", "fields", "consistency"},
                            "effectiveness_update.request")
            request_fields = request["fields"]
            if not isinstance(request_fields, list) or len(request_fields) != 3:
                fail("effectiveness-update-request", "request must contain exactly three fields")
            memory_id = _keys(request_fields[0], {"name", "type", "minimum", "maximum"},
                              "effectiveness_update.request.fields[0]")
            has_value = _keys(request_fields[1], {"name", "type", "minimum", "maximum"},
                              "effectiveness_update.request.fields[1]")
            value = _keys(request_fields[2], {"name", "type", "encoding"},
                          "effectiveness_update.request.fields[2]")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "effectiveness_update.reply")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 20 or
                    memory_id != {"name": "memory_id", "type": "u64", "minimum": 1,
                                  "maximum": 0x7fffffffffffffff} or
                    has_value != {"name": "has_value", "type": "u32", "minimum": 0,
                                  "maximum": 1} or
                    value != {"name": "value", "type": "f64",
                              "encoding": "ieee754-binary64-le"} or
                    request["consistency"] !=
                    "has_value=0 requires value bits to be positive zero"):
                fail("effectiveness-update-request",
                     "request must contain a positive identifier and canonical nullable binary64")
            if reply != {"encoded_size_ok": ENVELOPE_HEADER_LEN,
                         "encoded_size_error": ENVELOPE_HEADER_LEN, "fields": []}:
                fail("effectiveness-update-reply", "reply must be an empty closed-result envelope")
        elif key == ("memory", 10) and name == "retention_enforce" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            if operation["c_symbols"] != ["db2_memory_health_delete_by_sensitivity"]:
                fail("operation-c-symbols",
                     "retention_enforce C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "retention_enforce results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "retention_enforce.request")
            policy = request["policy"]
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    policy != [
                        {"sensitivity": "restricted", "retention_days": 7},
                        {"sensitivity": "sensitive", "retention_days": 90},
                    ]):
                fail("retention-enforce-request",
                     "request must carry no payload and use the fixed canonical retention policy")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "retention_enforce.reply")
            reply_field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                                "retention_enforce.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply_field != {"name": "deleted_count", "type": "u32", "minimum": 0,
                                    "maximum": 0x7fffffff}):
                fail("retention-enforce-reply", "reply must contain one bounded deletion count")
        elif key == ("memory", 11) and name == "effectiveness_demote" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            if operation["c_symbols"] != ["db2_memory_health_demote_low_effectiveness"]:
                fail("operation-c-symbols",
                     "effectiveness_demote C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "effectiveness_demote results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "effectiveness_demote.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"threshold_binary64_bits": 0x3fd3333333333333}):
                fail("effectiveness-demote-request",
                     "request must carry no payload and use the fixed canonical threshold")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "effectiveness_demote.reply")
            reply_field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                                "effectiveness_demote.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply_field != {"name": "demoted_count", "type": "u32", "minimum": 0,
                                    "maximum": 0x7fffffff}):
                fail("effectiveness-demote-reply", "reply must contain one bounded demotion count")
        elif key == ("memory", 12) and name == "effectiveness_stats" and \
                operation["wire_format"] == "db2-envelope-f64-u32-pair-v1":
            if operation["c_symbols"] != ["db2_memory_health_effectiveness_stats"]:
                fail("operation-c-symbols",
                     "effectiveness_stats C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "effectiveness_stats results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "effectiveness_stats.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"low_threshold_binary64_bits": 0x3fd3333333333333}):
                fail("effectiveness-stats-request",
                     "request must carry no payload and use the fixed canonical low threshold")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "effectiveness_stats.reply")
            reply_fields = reply["fields"]
            if not isinstance(reply_fields, list) or len(reply_fields) != 3:
                fail("effectiveness-stats-reply", "reply must contain exactly three fields")
            average = _keys(reply_fields[0],
                            {"name", "type", "encoding", "minimum_binary64_bits",
                             "maximum_binary64_bits"},
                            "effectiveness_stats.reply.fields[0]")
            low = _keys(reply_fields[1], {"name", "type", "minimum", "maximum"},
                        "effectiveness_stats.reply.fields[1]")
            high = _keys(reply_fields[2], {"name", "type", "minimum", "maximum"},
                         "effectiveness_stats.reply.fields[2]")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 16 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    average != {"name": "avg_effectiveness", "type": "f64",
                                "encoding": "ieee754-binary64-le",
                                "minimum_binary64_bits": 0,
                                "maximum_binary64_bits": 0x3ff0000000000000} or
                    low != {"name": "low_effectiveness_count", "type": "u32", "minimum": 0,
                            "maximum": 0x7fffffff} or
                    high != {"name": "high_impact_count", "type": "u32", "minimum": 0,
                             "maximum": 0x7fffffff}):
                fail("effectiveness-stats-reply",
                     "reply must contain the bounded average and the two bounded counts")
        elif key == ("memory", 13) and name == "l2_memory_ids" and \
                operation["wire_format"] == "db2-envelope-u64-list-v1":
            if operation["c_symbols"] != ["db2_memory_health_list_l2_memory_ids"]:
                fail("operation-c-symbols",
                     "l2_memory_ids C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "l2_memory_ids results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "l2_memory_ids.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"maximum_ids": 2048}):
                fail("l2-memory-ids-request",
                     "request must carry no payload and use the fixed canonical identifier bound")
            reply = _keys(operation["reply"],
                          {"encoded_size_min_ok", "encoded_size_max_ok", "encoded_size_error",
                           "field"},
                          "l2_memory_ids.reply")
            reply_field = _keys(reply["field"],
                                {"name", "type", "minimum_items", "maximum_items",
                                 "item_minimum", "item_maximum"},
                                "l2_memory_ids.reply.field")
            maximum_ids = request["policy"]["maximum_ids"]
            if (reply["encoded_size_min_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_max_ok"] != ENVELOPE_HEADER_LEN + 4 + maximum_ids * 8 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply_field != {"name": "memory_ids", "type": "u64-list",
                                    "minimum_items": 0, "maximum_items": maximum_ids,
                                    "item_minimum": 1, "item_maximum": 0x7fffffffffffffff}):
                fail("l2-memory-ids-reply",
                     "reply must be a counted identifier list bounded by the request policy")
        elif key == ("memory", 14) and name == "health_record" and \
                operation["wire_format"] == "db2-envelope-u32-triple-v1":
            # The corpus total and the conflict count stay private-db2: the handler
            # composes them behind this operation rather than naming them on the wire.
            if operation["c_symbols"] != ["db2_memory_health_record"]:
                fail("operation-c-symbols",
                     "health_record C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "health_record results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "policy", "fields"},
                            "health_record.request")
            request_fields = request["fields"]
            if not isinstance(request_fields, list) or len(request_fields) != 3:
                fail("health-record-request", "request must contain exactly three counters")
            counters = [_keys(field, {"name", "type", "minimum", "maximum"},
                              f"health_record.request.fields[{index}]")
                        for index, field in enumerate(request_fields)]
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 12 or
                    request["policy"] != {"conflict_window_days": 1} or
                    counters != [{"name": counter, "type": "u32", "minimum": 0,
                                  "maximum": 0x7fffffff}
                                 for counter in ("promotions", "demotions", "expirations")]):
                fail("health-record-request",
                     "request must carry the three bounded counters and the fixed conflict window")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "health_record.reply")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != []):
                fail("health-record-reply", "reply must acknowledge the cycle without a payload")
        elif key == ("memory", 15) and name == "health_retention" and \
                operation["wire_format"] == "db2-envelope-u32-pair-v1":
            if operation["c_symbols"] != ["db2_memory_health_prune_old",
                                          "db2_memory_health_prune_old_contradictions"]:
                fail("operation-c-symbols",
                     "health_retention C symbols differ from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "health_retention results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "health_retention.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"snapshot_retention_days": 90,
                                          "contradiction_retention_days": 90}):
                fail("health-retention-request",
                     "request must carry no payload and use the complete fixed retention policy")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "health_retention.reply")
            reply_fields = reply["fields"]
            if not isinstance(reply_fields, list) or len(reply_fields) != 2:
                fail("health-retention-reply", "reply must report both halves of the action")
            counts = [_keys(field, {"name", "type", "minimum", "maximum"},
                            f"health_retention.reply.fields[{index}]")
                      for index, field in enumerate(reply_fields)]
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 8 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    counts != [{"name": name_, "type": "u32", "minimum": 0,
                                "maximum": 0x7fffffff}
                               for name_ in ("snapshots_deleted", "contradictions_deleted")]):
                fail("health-retention-reply",
                     "reply must contain the two bounded deletion counts")
        elif key == ("memory", 16) and name == "health_counters" and \
                operation["wire_format"] == "db2-envelope-health-counters-v1":
            if operation["c_symbols"] != ["db2_memory_health_query_counters"]:
                fail("operation-c-symbols",
                     "health_counters C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "health_counters results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "health_counters.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"promote_use_count": 3,
                                          "promote_confidence_binary64_bits":
                                              0x3feccccccccccccd}):
                fail("health-counters-request",
                     "request must carry no payload and use the fixed canonical promotion policy")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "health_counters.reply")
            reply_fields = reply["fields"]
            if not isinstance(reply_fields, list) or len(reply_fields) != len(HEALTH_COUNTERS):
                fail("health-counters-reply",
                     "reply must contain the complete rolling-window aggregate")
            counters = [_keys(field, {"name", "type", "minimum", "maximum"},
                              f"health_counters.reply.fields[{index}]")
                        for index, field in enumerate(reply_fields)]
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 * len(HEALTH_COUNTERS) or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    counters != [{"name": counter, "type": "u32", "minimum": 0,
                                  "maximum": 0x7fffffff} for counter in HEALTH_COUNTERS]):
                fail("health-counters-reply",
                     "reply must contain every bounded counter in the reviewed order")
        elif key == ("memory", 17) and name == "stats_counts" and \
                operation["wire_format"] == "db2-envelope-memory-stats-v1":
            if operation["c_symbols"] != ["db2_memory_stats_counts"]:
                fail("operation-c-symbols",
                     "stats_counts C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "stats_counts results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "stats_counts.request")
            if request["encoded_size"] != ENVELOPE_HEADER_LEN or request["payload"] != "none":
                fail("stats-counts-request", "request must carry no payload")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "stats_counts.reply")
            reply_fields = reply["fields"]
            if not isinstance(reply_fields, list) or len(reply_fields) != 4:
                fail("stats-counts-reply",
                     "reply must contain both breakdowns, the total, and the conflict count")
            arrays = [_keys(reply_fields[index],
                            {"name", "type", "items", "item_minimum", "item_maximum", "labels"},
                            f"stats_counts.reply.fields[{index}]") for index in (0, 1)]
            scalars = [_keys(reply_fields[index], {"name", "type", "minimum", "maximum"},
                             f"stats_counts.reply.fields[{index}]") for index in (2, 3)]
            expected_arrays = [
                {"name": "tier_counts", "type": "u32-array", "items": len(MEMORY_TIERS),
                 "item_minimum": 0, "item_maximum": 0x7fffffff, "labels": list(MEMORY_TIERS)},
                {"name": "kind_counts", "type": "u32-array", "items": len(MEMORY_KINDS),
                 "item_minimum": 0, "item_maximum": 0x7fffffff, "labels": list(MEMORY_KINDS)},
            ]
            buckets = len(MEMORY_TIERS) + len(MEMORY_KINDS) + 2
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 * buckets or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    arrays != expected_arrays or
                    scalars != [{"name": scalar, "type": "u32", "minimum": 0,
                                 "maximum": 0x7fffffff} for scalar in ("total", "conflicts")]):
                fail("stats-counts-reply",
                     "reply must declare every labelled bucket in the reviewed order")
        elif key == ("memory", 18) and name == "expire" and \
                operation["wire_format"] == "db2-envelope-u32-pair-v1":
            # Each row delete is paired with its provenance delete; the private
            # kind and lifecycle lookups stay behind the handler.
            if operation["c_symbols"] != ["db2_memory_promotion_delete_l0_provenance",
                                          "db2_memory_promotion_delete_l0",
                                          "db2_memory_promotion_delete_stale_l1_provenance",
                                          "db2_memory_promotion_delete_stale_l1"]:
                fail("operation-c-symbols",
                     "expire C symbols differ from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "expire results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "expire.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"stale_l1_tier": "L1", "maximum_kinds": 16}):
                fail("expire-request",
                     "request must carry no payload and use the fixed tier and kind bound")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"}, "expire.reply")
            reply_fields = reply["fields"]
            if not isinstance(reply_fields, list) or len(reply_fields) != 2:
                fail("expire-reply", "reply must report both expiry stages")
            counts = [_keys(field, {"name", "type", "minimum", "maximum"},
                            f"expire.reply.fields[{index}]")
                      for index, field in enumerate(reply_fields)]
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 8 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    counts != [{"name": name_, "type": "u32", "minimum": 0,
                                "maximum": 0x7fffffff}
                               for name_ in ("level0_deleted", "stale_level1_deleted")]):
                fail("expire-reply", "reply must contain both bounded deletion counts")
        elif key == ("memory", 19) and name == "demote" and \
                operation["wire_format"] == "db2-envelope-u32-pair-v1":
            if operation["c_symbols"] != ["db2_memory_promotion_demote_kind",
                                          "db2_memory_promotion_demote_cascade"]:
                fail("operation-c-symbols",
                     "demote C symbols differ from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "demote results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "demote.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"demote_tier": "L2", "maximum_kinds": 16}):
                fail("demote-request",
                     "request must carry no payload and use the fixed tier and kind bound")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields", "consistency"},
                          "demote.reply")
            reply_fields = reply["fields"]
            if not isinstance(reply_fields, list) or len(reply_fields) != 2:
                fail("demote-reply", "reply must report the demotion and its cascade")
            counts = [_keys(field, {"name", "type", "minimum", "maximum"},
                            f"demote.reply.fields[{index}]")
                      for index, field in enumerate(reply_fields)]
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 8 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["consistency"] != "demoted_count=0 requires cascaded_count=0" or
                    counts != [{"name": name_, "type": "u32", "minimum": 0,
                                "maximum": 0x7fffffff}
                               for name_ in ("demoted_count", "cascaded_count")]):
                fail("demote-reply",
                     "reply must contain both bounded counts and the cascade invariant")
        elif key == ("memory", 20) and name == "promote_stable" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            if operation["c_symbols"] != ["db2_memory_promotion_promote_stable_l2_to_l3"]:
                fail("operation-c-symbols",
                     "promote_stable C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "promote_stable results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "promote_stable.request")
            policy = _keys(request["policy"],
                           {"source_tier", "target_tier", "kinds",
                            "minimum_confidence_binary64_bits", "minimum_use_count",
                            "stable_days"},
                           "promote_stable.request.policy")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    policy != {"source_tier": "L2", "target_tier": "L3",
                               "kinds": ["fact", "preference"],
                               "minimum_confidence_binary64_bits": 0x3fee666666666666,
                               "minimum_use_count": 5, "stable_days": 30}):
                fail("promote-stable-request",
                     "request must carry no payload and use the complete fixed stability policy")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "promote_stable.reply")
            reply_field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                                "promote_stable.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply_field != {"name": "promoted_count", "type": "u32", "minimum": 0,
                                    "maximum": 0x7fffffff}):
                fail("promote-stable-reply", "reply must contain one bounded promotion count")
        elif key == ("memory", 21) and name == "reclassify_directives" and \
                operation["wire_format"] == "db2-envelope-flag-u32-v1":
            if operation["c_symbols"] != ["db2_memory_promotion_reclassify_directives"]:
                fail("operation-c-symbols",
                     "reclassify_directives C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "reclassify_directives results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "policy", "field"},
                            "reclassify_directives.request")
            policy = _keys(request["policy"],
                           {"source_tier", "target_tier", "kinds", "gated_kind"},
                           "reclassify_directives.request.policy")
            request_field = _keys(request["field"], {"name", "type", "minimum", "maximum"},
                                  "reclassify_directives.request.field")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 4 or
                    policy != {"source_tier": "L3", "target_tier": "L4",
                               "kinds": ["policy", "workflow"], "gated_kind": "policy"} or
                    request_field != {"name": "require_approval", "type": "u32", "minimum": 0,
                                      "maximum": 1}):
                fail("reclassify-directives-request",
                     "request must fix the tiers and kinds and carry only the approval gate")
            # The gate only narrows the gated kind; the other directive kind is
            # always eligible, so a gated_kind outside the promotable set would
            # make the gate meaningless.
            if policy["gated_kind"] not in policy["kinds"]:
                fail("reclassify-directives-request",
                     "the gated kind must be one of the promotable directive kinds")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "reclassify_directives.reply")
            reply_field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                                "reclassify_directives.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply_field != {"name": "reclassified_count", "type": "u32", "minimum": 0,
                                    "maximum": 0x7fffffff}):
                fail("reclassify-directives-reply",
                     "reply must contain one bounded reclassification count")
        elif key == ("memory", 22) and name == "record_l4_approval" and \
                operation["wire_format"] == "db2-envelope-u64-string-pair-v1":
            if operation["c_symbols"] != ["db2_memory_promotion_record_l4_approval"]:
                fail("operation-c-symbols",
                     "record_l4_approval C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "record_l4_approval results must equal ['ok']")
            request = _keys(operation["request"],
                            {"encoded_size_min", "encoded_size_max", "policy", "fields"},
                            "record_l4_approval.request")
            request_fields = request["fields"]
            if not isinstance(request_fields, list) or len(request_fields) != 3:
                fail("record-l4-approval-request",
                     "request must carry the memory, the approver, and the note")
            memory_field = _keys(request_fields[0], {"name", "type", "minimum", "maximum"},
                                 "record_l4_approval.request.fields[0]")
            approver = _keys(request_fields[1],
                             {"name", "type", "minimum_bytes", "maximum_bytes"},
                             "record_l4_approval.request.fields[1]")
            note = _keys(request_fields[2], {"name", "type", "minimum_bytes", "maximum_bytes"},
                         "record_l4_approval.request.fields[2]")
            # The approver is who is accountable for the promotion, so it is
            # required; the note is optional colour.
            if (request["policy"] != {"target_tier": "L4"} or
                    memory_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                     "maximum": 0x7fffffffffffffff} or
                    approver != {"name": "approver", "type": "utf8", "minimum_bytes": 1,
                                 "maximum_bytes": 63} or
                    note != {"name": "note", "type": "utf8", "minimum_bytes": 0,
                             "maximum_bytes": 511}):
                fail("record-l4-approval-request",
                     "request must fix the approved tier and bound the approver and note")
            if (request["encoded_size_min"] !=
                    ENVELOPE_HEADER_LEN + 8 + 4 + approver["minimum_bytes"] + 4 +
                    note["minimum_bytes"] or
                    request["encoded_size_max"] !=
                    ENVELOPE_HEADER_LEN + 8 + 4 + approver["maximum_bytes"] + 4 +
                    note["maximum_bytes"]):
                fail("record-l4-approval-request",
                     "encoded sizes must follow from the declared field bounds")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "record_l4_approval.reply")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != []):
                fail("record-l4-approval-reply",
                     "reply must acknowledge the approval without a payload")
        elif key == ("memory", 23) and name == "prune_orphaned_l0" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            # The retention window and tier are fixed policy, so the request
            # carries no payload: an operator cannot widen the delete over the
            # bus. The reply is the affected-row count of the single DELETE.
            if operation["c_symbols"] != ["db2_memory_prune_orphaned_l0"]:
                fail("operation-c-symbols",
                     "prune_orphaned_l0 C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "prune_orphaned_l0 results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "prune_orphaned_l0.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"tier": "L0", "maximum_age": "-7 days"}):
                fail("prune-orphaned-l0-request",
                     "request must carry no payload and use the fixed tier and age bound")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "prune_orphaned_l0.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "prune_orphaned_l0.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "deleted_count", "type": "u32", "minimum": 0,
                              "maximum": 0x7fffffff}):
                fail("prune-orphaned-l0-reply",
                     "reply must contain one bounded u32 deletion count")
        elif key == ("memory", 24) and name == "lifecycle_sweep_expired" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            # The states and the archive reason are fixed policy, so the request
            # carries no payload: a caller cannot archive a different set of
            # rows or relabel why they were archived.
            if operation["c_symbols"] != ["db2_memory_lifecycle_sweep_expired"]:
                fail("operation-c-symbols",
                     "lifecycle_sweep_expired C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "lifecycle_sweep_expired results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "lifecycle_sweep_expired.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"source_state": "pending",
                                          "target_state": "archived",
                                          "archive_reason": "pending_ttl_expired"}):
                fail("lifecycle-sweep-expired-request",
                     "request must carry no payload and use the fixed lifecycle states")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "lifecycle_sweep_expired.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "lifecycle_sweep_expired.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "archived_count", "type": "u32", "minimum": 0,
                              "maximum": 0x7fffffff}):
                fail("lifecycle-sweep-expired-reply",
                     "reply must contain one bounded u32 archived count")
        elif key == ("memory", 25) and name == "demote_id" and \
                operation["wire_format"] == "db2-envelope-u64-u32-v1":
            # The caller names the row; the decay multiplier and the floor it
            # stops at are fixed policy. Both are pinned as binary64 bit
            # patterns because the catalog loader refuses float literals.
            if operation["c_symbols"] != ["db2_memory_promotion_demote_id"]:
                fail("operation-c-symbols",
                     "demote_id C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "demote_id results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "policy", "field"},
                            "demote_id.request")
            request_field = _keys(request["field"],
                                  {"name", "type", "minimum", "maximum"},
                                  "demote_id.request.field")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 8 or
                    request["policy"] != {
                        "confidence_multiplier_binary64_bits": 4606281698874543309,
                        "minimum_confidence_binary64_bits": 4599075939470750515} or
                    request_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                      "maximum": 0x7fffffffffffffff}):
                fail("demote-id-request",
                     "request must name one positive memory and use the fixed decay policy")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "demote_id.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "demote_id.reply.field")
            # The predicate is an equality on the primary key, so it can touch
            # at most one row; a wider count would mean the statement changed.
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "demoted_count", "type": "u32", "minimum": 0,
                              "maximum": 1}):
                fail("demote-id-reply",
                     "reply must contain the single-row demotion count")
        elif key == ("memory", 26) and name == "has_workspace_tag" and \
                operation["wire_format"] == "db2-envelope-u64-u32-v1":
            # A pure existence probe: the caller names the memory and learns
            # only whether any attribution row exists, never which workspace.
            if operation["c_symbols"] != ["db2_memory_has_any_workspace_tag"]:
                fail("operation-c-symbols",
                     "has_workspace_tag C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "has_workspace_tag results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "field"},
                            "has_workspace_tag.request")
            request_field = _keys(request["field"],
                                  {"name", "type", "minimum", "maximum"},
                                  "has_workspace_tag.request.field")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 8 or
                    request_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                      "maximum": 0x7fffffffffffffff}):
                fail("has-workspace-tag-request",
                     "request must name one positive memory and carry nothing else")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "has_workspace_tag.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "has_workspace_tag.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "tagged", "type": "u32", "minimum": 0, "maximum": 1}):
                fail("has-workspace-tag-reply",
                     "reply must contain one Boolean attribution flag")
        elif key == ("memory", 27) and name == "delete_row" and \
                operation["wire_format"] == "db2-envelope-u64-u32-v1":
            if operation["c_symbols"] != ["db2_memory_delete_row"]:
                fail("operation-c-symbols",
                     "delete_row C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "delete_row results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "field"},
                            "delete_row.request")
            request_field = _keys(request["field"],
                                  {"name", "type", "minimum", "maximum"},
                                  "delete_row.request.field")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 8 or
                    request_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                      "maximum": 0x7fffffffffffffff}):
                fail("delete-row-request",
                     "request must name one positive memory and carry nothing else")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "delete_row.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "delete_row.reply.field")
            # Equality on the primary key: the row existed or it did not.
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "deleted_rows", "type": "u32", "minimum": 0,
                              "maximum": 1}):
                fail("delete-row-reply",
                     "reply must contain the single-row deletion count")
        elif key == ("memory", 28) and name == "touch" and \
                operation["wire_format"] == "db2-envelope-u64-ack-v1":
            # The caller records that a memory was used. How much the count
            # moves and what stamp is written are fixed, so retrieval evidence
            # cannot be inflated from outside DB2.
            if operation["c_symbols"] != ["db2_memory_touch"]:
                fail("operation-c-symbols", "touch C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "touch results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "field"}, "touch.request")
            request_field = _keys(request["field"], {"name", "type", "minimum", "maximum"},
                                  "touch.request.field")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 8 or
                    request_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                      "maximum": 0x7fffffffffffffff}):
                fail("touch-request",
                     "request must name one positive memory and carry nothing else")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"}, "touch.reply")
            # An acknowledgement only. The backend distinguishes a missing row
            # from a fault, but reports both as failure, so there is no count
            # here that could be mistaken for evidence the row existed.
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != []):
                fail("touch-reply", "reply must acknowledge the bump without a payload")
        elif key == ("memory", 29) and name == "link_delete" and \
                operation["wire_format"] == "db2-envelope-u64-ack-v1":
            if operation["c_symbols"] != ["db2_memory_link_delete"]:
                fail("operation-c-symbols",
                     "link_delete C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "link_delete results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "field"},
                            "link_delete.request")
            request_field = _keys(request["field"], {"name", "type", "minimum", "maximum"},
                                  "link_delete.request.field")
            # A link, not a memory: deleting a relation must not be reachable
            # by naming one of its endpoints.
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 8 or
                    request_field != {"name": "link_id", "type": "u64", "minimum": 1,
                                      "maximum": 0x7fffffffffffffff}):
                fail("link-delete-request",
                     "request must name one positive link and carry nothing else")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "link_delete.reply")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != []):
                fail("link-delete-reply",
                     "reply must acknowledge the delete without a payload")
        elif key == ("memory", 30) and name == "valid_at" and \
                operation["wire_format"] == "db2-envelope-u64-string-u32-v1":
            # The only operation so far whose backend distinguishes a false
            # answer from an unanswerable one, and the only one carrying
            # invalid_state for that reason rather than for an unusable module.
            if operation["c_symbols"] != ["db2_memory_valid_at"]:
                fail("operation-c-symbols", "valid_at C symbol differs from the reviewed backend")
            if operation["results"] != ["ok", "invalid_state"]:
                fail("operation-results",
                     "valid_at must be able to report that it could not evaluate the bounds")
            request = _keys(operation["request"],
                            {"encoded_size_min", "encoded_size_max", "fields"},
                            "valid_at.request")
            request_fields = request["fields"]
            if not isinstance(request_fields, list) or len(request_fields) != 2:
                fail("valid-at-request", "request must carry the memory and the instant")
            memory_field = _keys(request_fields[0], {"name", "type", "minimum", "maximum"},
                                 "valid_at.request.fields[0]")
            as_of_field = _keys(request_fields[1],
                                {"name", "type", "minimum_bytes", "maximum_bytes"},
                                "valid_at.request.fields[1]")
            if (request["encoded_size_min"] != ENVELOPE_HEADER_LEN + 13 or
                    request["encoded_size_max"] != ENVELOPE_HEADER_LEN + 12 + 63 or
                    memory_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                     "maximum": 0x7fffffffffffffff} or
                    as_of_field != {"name": "as_of", "type": "utf8", "minimum_bytes": 1,
                                    "maximum_bytes": 63}):
                fail("valid-at-request",
                     "request must name one positive memory and one non-empty bounded instant")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"}, "valid_at.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "valid_at.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "in_force", "type": "u32", "minimum": 0, "maximum": 1}):
                fail("valid-at-reply", "reply must contain one Boolean validity verdict")
        elif key == ("memory", 31) and name == "has_scope_type" and \
                operation["wire_format"] == "db2-envelope-u64-string-u32-v1":
            if operation["c_symbols"] != ["db2_memory_has_scope_type"]:
                fail("operation-c-symbols",
                     "has_scope_type C symbol differs from the reviewed backend")
            # Unlike valid_at, which shares this format, the backend here folds
            # a fault into a miss, so there is no unevaluated state to report.
            if operation["results"] != ["ok"]:
                fail("operation-results", "has_scope_type results must equal ['ok']")
            request = _keys(operation["request"],
                            {"encoded_size_min", "encoded_size_max", "fields"},
                            "has_scope_type.request")
            request_fields = request["fields"]
            if not isinstance(request_fields, list) or len(request_fields) != 2:
                fail("has-scope-type-request", "request must carry the memory and the scope kind")
            memory_field = _keys(request_fields[0], {"name", "type", "minimum", "maximum"},
                                 "has_scope_type.request.fields[0]")
            scope_field = _keys(request_fields[1],
                                {"name", "type", "minimum_bytes", "maximum_bytes"},
                                "has_scope_type.request.fields[1]")
            if (request["encoded_size_min"] != ENVELOPE_HEADER_LEN + 13 or
                    request["encoded_size_max"] != ENVELOPE_HEADER_LEN + 12 + 63 or
                    memory_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                     "maximum": 0x7fffffffffffffff} or
                    scope_field != {"name": "scope_type", "type": "utf8", "minimum_bytes": 1,
                                    "maximum_bytes": 63}):
                fail("has-scope-type-request",
                     "request must name one positive memory and one non-empty bounded scope kind")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "has_scope_type.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "has_scope_type.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "present", "type": "u32", "minimum": 0, "maximum": 1}):
                fail("has-scope-type-reply", "reply must contain one Boolean attribution flag")
        elif key == ("memory", 32) and name == "reject" and \
                operation["wire_format"] == "db2-envelope-u64-ack-v1":
            # The backend takes a reason and discards it, and the audit trail
            # above it does not record one either. The wire operation therefore
            # carries only the memory: putting a reason on the bus would imply
            # a rationale is stored somewhere, and none is.
            if operation["c_symbols"] != ["db2_memory_reject"]:
                fail("operation-c-symbols", "reject C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "reject results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "policy", "field"},
                            "reject.request")
            request_field = _keys(request["field"], {"name", "type", "minimum", "maximum"},
                                  "reject.request.field")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 8 or
                    request["policy"] != {
                        "confidence_penalty_binary64_bits": 4591870180066957722,
                        "confidence_floor_binary64_bits": 0} or
                    request_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                      "maximum": 0x7fffffffffffffff}):
                fail("reject-request",
                     "request must name one positive memory and use the fixed penalty policy")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"}, "reject.reply")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != []):
                fail("reject-reply", "reply must acknowledge the penalty without a payload")
        elif key == ("memory", 33) and name == "update_content" and \
                operation["wire_format"] == "db2-envelope-u64-string-u32-v1":
            if operation["c_symbols"] != ["db2_memory_update_content"]:
                fail("operation-c-symbols",
                     "update_content C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "update_content results must equal ['ok']")
            request = _keys(operation["request"],
                            {"encoded_size_min", "encoded_size_max", "fields"},
                            "update_content.request")
            request_fields = request["fields"]
            if not isinstance(request_fields, list) or len(request_fields) != 2:
                fail("update-content-request", "request must carry the memory and the text")
            memory_field = _keys(request_fields[0], {"name", "type", "minimum", "maximum"},
                                 "update_content.request.fields[0]")
            content_field = _keys(request_fields[1],
                                  {"name", "type", "minimum_bytes", "maximum_bytes"},
                                  "update_content.request.fields[1]")
            # Bounded to the memory record's own content width: a longer value
            # would be accepted here and then truncated by whatever reads it
            # back into a memory_t, which is a silent corruption rather than a
            # rejected write.
            if (request["encoded_size_min"] != ENVELOPE_HEADER_LEN + 13 or
                    request["encoded_size_max"] != ENVELOPE_HEADER_LEN + 12 + 2047 or
                    memory_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                     "maximum": 0x7fffffffffffffff} or
                    content_field != {"name": "content", "type": "utf8", "minimum_bytes": 1,
                                      "maximum_bytes": 2047}):
                fail("update-content-request",
                     "request must name one positive memory and one non-empty bounded text")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "update_content.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "update_content.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "updated_rows", "type": "u32", "minimum": 0,
                              "maximum": 1}):
                fail("update-content-reply", "reply must contain the single-row update count")
        elif key == ("memory", 34) and name == "decay_confidence" and \
                operation["wire_format"] == "db2-envelope-u64-ack-v1":
            # The backend returns void, so it cannot say whether the row
            # existed. An acknowledgement is therefore the only honest reply:
            # a count here would have to be invented.
            if operation["c_symbols"] != ["db2_memory_decay_confidence"]:
                fail("operation-c-symbols",
                     "decay_confidence C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "decay_confidence results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "policy", "field"},
                            "decay_confidence.request")
            request_field = _keys(request["field"], {"name", "type", "minimum", "maximum"},
                                  "decay_confidence.request.field")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 8 or
                    request["policy"] != {
                        "confidence_multiplier_binary64_bits": 4604480259023595110} or
                    request_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                      "maximum": 0x7fffffffffffffff}):
                fail("decay-confidence-request",
                     "request must name one positive memory and use the fixed multiplier")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "decay_confidence.reply")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != []):
                fail("decay-confidence-reply",
                     "reply must acknowledge the decay without a payload")
        elif key == ("memory", 35) and name == "workspace_tag_insert" and \
                operation["wire_format"] == "db2-envelope-u64-string-ack-v1":
            if operation["c_symbols"] != ["db2_memory_workspace_tag_insert"]:
                fail("operation-c-symbols",
                     "workspace_tag_insert C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "workspace_tag_insert results must equal ['ok']")
            request = _keys(operation["request"],
                            {"encoded_size_min", "encoded_size_max", "fields"},
                            "workspace_tag_insert.request")
            request_fields = request["fields"]
            if not isinstance(request_fields, list) or len(request_fields) != 2:
                fail("workspace-tag-insert-request",
                     "request must carry the memory and the workspace")
            memory_field = _keys(request_fields[0], {"name", "type", "minimum", "maximum"},
                                 "workspace_tag_insert.request.fields[0]")
            workspace_field = _keys(request_fields[1],
                                    {"name", "type", "minimum_bytes", "maximum_bytes"},
                                    "workspace_tag_insert.request.fields[1]")
            # Bounded to DB2's own workspace identifier width, so a value that
            # survives the wire also survives being read back through the scope
            # context rather than being truncated into a different workspace.
            if (request["encoded_size_min"] != ENVELOPE_HEADER_LEN + 13 or
                    request["encoded_size_max"] != ENVELOPE_HEADER_LEN + 12 + 511 or
                    memory_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                     "maximum": 0x7fffffffffffffff} or
                    workspace_field != {"name": "workspace", "type": "utf8",
                                        "minimum_bytes": 1, "maximum_bytes": 511}):
                fail("workspace-tag-insert-request",
                     "request must name one positive memory and one non-empty bounded workspace")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "workspace_tag_insert.reply")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != []):
                fail("workspace-tag-insert-reply",
                     "reply must acknowledge the attribution without a payload")
        elif key == ("memory", 36) and name == "set_cognified_kind" and \
                operation["wire_format"] == "db2-envelope-u64-string-ack-v1":
            if operation["c_symbols"] != ["db2_memory_set_cognified_kind"]:
                fail("operation-c-symbols",
                     "set_cognified_kind C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "set_cognified_kind results must equal ['ok']")
            request = _keys(operation["request"],
                            {"encoded_size_min", "encoded_size_max", "fields"},
                            "set_cognified_kind.request")
            request_fields = request["fields"]
            if not isinstance(request_fields, list) or len(request_fields) != 2:
                fail("set-cognified-kind-request",
                     "request must carry the memory and the kind")
            memory_field = _keys(request_fields[0], {"name", "type", "minimum", "maximum"},
                                 "set_cognified_kind.request.fields[0]")
            kind_field = _keys(request_fields[1],
                               {"name", "type", "minimum_bytes", "maximum_bytes"},
                               "set_cognified_kind.request.fields[1]")
            # The backend refuses an empty kind, so the wire refuses it too --
            # unlike the two setters that follow, where empty clears the column.
            if (request["encoded_size_min"] != ENVELOPE_HEADER_LEN + 13 or
                    request["encoded_size_max"] != ENVELOPE_HEADER_LEN + 12 + 15 or
                    memory_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                     "maximum": 0x7fffffffffffffff} or
                    kind_field != {"name": "kind", "type": "utf8", "minimum_bytes": 1,
                                   "maximum_bytes": 15}):
                fail("set-cognified-kind-request",
                     "request must name one positive memory and one non-empty bounded kind")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "set_cognified_kind.reply")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != []):
                fail("set-cognified-kind-reply",
                     "reply must acknowledge the write without a payload")
        elif key == ("memory", 37) and name == "set_source_session" and \
                operation["wire_format"] == "db2-envelope-u64-string-ack-v1":
            if operation["c_symbols"] != ["db2_memory_set_source_session"]:
                fail("operation-c-symbols",
                     "set_source_session C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "set_source_session results must equal ['ok']")
            request = _keys(operation["request"],
                            {"encoded_size_min", "encoded_size_max", "fields"},
                            "set_source_session.request")
            request_fields = request["fields"]
            if not isinstance(request_fields, list) or len(request_fields) != 2:
                fail("set-source-session-request",
                     "request must carry the memory and the session")
            memory_field = _keys(request_fields[0], {"name", "type", "minimum", "maximum"},
                                 "set_source_session.request.fields[0]")
            session_field = _keys(request_fields[1],
                                  {"name", "type", "minimum_bytes", "maximum_bytes"},
                                  "set_source_session.request.fields[1]")
            # Minimum zero, unlike set_cognified_kind on the same wire format:
            # the backend accepts an empty session and that clears the column.
            # Refusing it here would silently remove the ability to unset.
            if (request["encoded_size_min"] != ENVELOPE_HEADER_LEN + 12 or
                    request["encoded_size_max"] != ENVELOPE_HEADER_LEN + 12 + 127 or
                    memory_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                     "maximum": 0x7fffffffffffffff} or
                    session_field != {"name": "session_id", "type": "utf8",
                                      "minimum_bytes": 0, "maximum_bytes": 127}):
                fail("set-source-session-request",
                     "request must name one positive memory and one clearable bounded session")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "set_source_session.reply")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != []):
                fail("set-source-session-reply",
                     "reply must acknowledge the write without a payload")
        elif key == ("memory", 38) and name == "negation_tokens_update" and \
                operation["wire_format"] == "db2-envelope-u64-string-ack-v1":
            if operation["c_symbols"] != ["db2_memory_negation_tokens_update"]:
                fail("operation-c-symbols",
                     "negation_tokens_update C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "negation_tokens_update results must equal ['ok']")
            request = _keys(operation["request"],
                            {"encoded_size_min", "encoded_size_max", "fields"},
                            "negation_tokens_update.request")
            request_fields = request["fields"]
            if not isinstance(request_fields, list) or len(request_fields) != 2:
                fail("negation-tokens-update-request",
                     "request must carry the memory and the tokens")
            memory_field = _keys(request_fields[0], {"name", "type", "minimum", "maximum"},
                                 "negation_tokens_update.request.fields[0]")
            tokens_field = _keys(request_fields[1],
                                 {"name", "type", "minimum_bytes", "maximum_bytes"},
                                 "negation_tokens_update.request.fields[1]")
            # Minimum zero, like set_source_session: the backend maps a null to
            # an empty string and stores it, and the extractor legitimately
            # produces nothing when a memory contains no negations.
            if (request["encoded_size_min"] != ENVELOPE_HEADER_LEN + 12 or
                    request["encoded_size_max"] != ENVELOPE_HEADER_LEN + 12 + 2047 or
                    memory_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                     "maximum": 0x7fffffffffffffff} or
                    tokens_field != {"name": "tokens", "type": "utf8", "minimum_bytes": 0,
                                     "maximum_bytes": 2047}):
                fail("negation-tokens-update-request",
                     "request must name one positive memory and one clearable bounded token set")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "fields"},
                          "negation_tokens_update.reply")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    reply["fields"] != []):
                fail("negation-tokens-update-reply",
                     "reply must acknowledge the write without a payload")
        elif key == ("memory", 39) and name == "get_content" and \
                operation["wire_format"] == "db2-envelope-u64-string-reply-v1":
            # The first read-back operation on this bus, and the first to use
            # not_found. A stored row may hold an empty string, so an empty
            # reply cannot double as "no such memory": the two are reported as
            # different results rather than the same zero-length payload.
            if operation["c_symbols"] != ["db2_memory_get_content"]:
                fail("operation-c-symbols",
                     "get_content C symbol differs from the reviewed backend")
            if operation["results"] != ["ok", "not_found"]:
                fail("operation-results",
                     "get_content must distinguish a missing memory from empty content")
            request = _keys(operation["request"], {"encoded_size", "field"},
                            "get_content.request")
            request_field = _keys(request["field"], {"name", "type", "minimum", "maximum"},
                                  "get_content.request.field")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 8 or
                    request_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                      "maximum": 0x7fffffffffffffff}):
                fail("get-content-request",
                     "request must name one positive memory and carry nothing else")
            reply = _keys(operation["reply"],
                          {"encoded_size_min_ok", "encoded_size_max_ok", "encoded_size_error",
                           "field"}, "get_content.reply")
            field = _keys(reply["field"],
                          {"name", "type", "minimum_bytes", "maximum_bytes"},
                          "get_content.reply.field")
            # Same width as memory.update_content writes: a row this operation
            # cannot return is a row that operation should not have stored.
            if (reply["encoded_size_min_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_max_ok"] != ENVELOPE_HEADER_LEN + 4 + 2047 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "content", "type": "utf8", "minimum_bytes": 0,
                              "maximum_bytes": 2047}):
                fail("get-content-reply",
                     "reply must carry the bounded content at the width update_content accepts")
        elif key == ("memory", 40) and name == "get_source_session" and \
                operation["wire_format"] == "db2-envelope-u64-string-reply-v1":
            if operation["c_symbols"] != ["db2_memory_get_source_session"]:
                fail("operation-c-symbols",
                     "get_source_session C symbol differs from the reviewed backend")
            if operation["results"] != ["ok", "not_found"]:
                fail("operation-results",
                     "get_source_session must be able to report that no session is set")
            request = _keys(operation["request"], {"encoded_size", "field"},
                            "get_source_session.request")
            request_field = _keys(request["field"], {"name", "type", "minimum", "maximum"},
                                  "get_source_session.request.field")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 8 or
                    request_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                      "maximum": 0x7fffffffffffffff}):
                fail("get-source-session-request",
                     "request must name one positive memory and carry nothing else")
            reply = _keys(operation["reply"],
                          {"encoded_size_min_ok", "encoded_size_max_ok", "encoded_size_error",
                           "field"}, "get_source_session.reply")
            field = _keys(reply["field"],
                          {"name", "type", "minimum_bytes", "maximum_bytes"},
                          "get_source_session.reply.field")
            # Minimum one, unlike get_content on the same wire format. This
            # backend succeeds only for a non-empty session and collapses an
            # absent memory and a blank column into the same failure, so an
            # empty ok would be a distinction the backend cannot actually make.
            if (reply["encoded_size_min_ok"] != ENVELOPE_HEADER_LEN + 5 or
                    reply["encoded_size_max_ok"] != ENVELOPE_HEADER_LEN + 4 + 127 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "session_id", "type": "utf8", "minimum_bytes": 1,
                              "maximum_bytes": 127}):
                fail("get-source-session-reply",
                     "reply must carry a non-empty bounded session at the set width")
        elif key == ("memory", 41) and name == "pick_first_temporal_ref" and \
                operation["wire_format"] == "db2-envelope-u64-string-reply-v1":
            if operation["c_symbols"] != ["db2_memory_pick_first_temporal_ref"]:
                fail("operation-c-symbols",
                     "pick_first_temporal_ref C symbol differs from the reviewed backend")
            if operation["results"] != ["ok", "not_found"]:
                fail("operation-results",
                     "pick_first_temporal_ref must be able to report that no reference exists")
            request = _keys(operation["request"], {"encoded_size", "field"},
                            "pick_first_temporal_ref.request")
            request_field = _keys(request["field"], {"name", "type", "minimum", "maximum"},
                                  "pick_first_temporal_ref.request.field")
            # The caller names the memory only. Which reference wins is decided
            # by the statement's own ordering, so no ranking input crosses the
            # bus and a caller cannot steer the answer.
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN + 8 or
                    request_field != {"name": "memory_id", "type": "u64", "minimum": 1,
                                      "maximum": 0x7fffffffffffffff}):
                fail("pick-first-temporal-ref-request",
                     "request must name one positive memory and carry no ranking input")
            reply = _keys(operation["reply"],
                          {"encoded_size_min_ok", "encoded_size_max_ok", "encoded_size_error",
                           "field"}, "pick_first_temporal_ref.reply")
            field = _keys(reply["field"],
                          {"name", "type", "minimum_bytes", "maximum_bytes"},
                          "pick_first_temporal_ref.reply.field")
            # Minimum one, like get_source_session: the backend succeeds only
            # for a non-empty key, so an empty ok would be unreachable state.
            if (reply["encoded_size_min_ok"] != ENVELOPE_HEADER_LEN + 5 or
                    reply["encoded_size_max_ok"] != ENVELOPE_HEADER_LEN + 4 + 127 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "ref_key", "type": "utf8", "minimum_bytes": 1,
                              "maximum_bytes": 127}):
                fail("pick-first-temporal-ref-reply",
                     "reply must carry a non-empty bounded reference key")
        elif key == ("memory", 42) and name == "count_and_max_updated" and \
                operation["wire_format"] == "db2-envelope-u32-string-reply-v1":
            if operation["c_symbols"] != ["db2_memory_count_and_max_updated"]:
                fail("operation-c-symbols",
                     "count_and_max_updated C symbol differs from the reviewed backend")
            # The aggregate always produces a row, so a failure to produce one
            # means the statement did not run -- invalid_state, not not_found.
            if operation["results"] != ["ok", "invalid_state"]:
                fail("operation-results",
                     "count_and_max_updated must report an aggregate it could not compute")
            request = _keys(operation["request"], {"encoded_size", "payload"},
                            "count_and_max_updated.request")
            if request != {"encoded_size": ENVELOPE_HEADER_LEN, "payload": "none"}:
                fail("count-and-max-updated-request",
                     "request must be an empty version-1 envelope")
            reply = _keys(operation["reply"],
                          {"encoded_size_min_ok", "encoded_size_max_ok", "encoded_size_error",
                           "fields"}, "count_and_max_updated.reply")
            reply_fields = reply["fields"]
            if not isinstance(reply_fields, list) or len(reply_fields) != 2:
                fail("count-and-max-updated-reply",
                     "reply must carry the count and the stamp together")
            count_field = _keys(reply_fields[0], {"name", "type", "minimum", "maximum"},
                                "count_and_max_updated.reply.fields[0]")
            stamp_field = _keys(reply_fields[1],
                                {"name", "type", "minimum_bytes", "maximum_bytes"},
                                "count_and_max_updated.reply.fields[1]")
            # An empty corpus has a count of zero and no latest stamp at all, so
            # the stamp is zero-minimum: an empty value is the honest answer
            # rather than a missing one.
            if (reply["encoded_size_min_ok"] != ENVELOPE_HEADER_LEN + 8 or
                    reply["encoded_size_max_ok"] != ENVELOPE_HEADER_LEN + 8 + 31 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    count_field != {"name": "count", "type": "u32", "minimum": 0,
                                    "maximum": 0x7fffffff} or
                    stamp_field != {"name": "max_updated_at", "type": "utf8",
                                    "minimum_bytes": 0, "maximum_bytes": 31}):
                fail("count-and-max-updated-reply",
                     "reply must carry a bounded count and a clearable bounded stamp")
        elif key == ("index", 1) and name == "entity_edge_prune_orphans" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            # First operation of the index family. The tiers that count as a
            # surviving reference are fixed policy: a caller that could name
            # them could delete the whole graph by naming none.
            if operation["c_symbols"] != ["db2_entity_edge_prune_orphans"]:
                fail("operation-c-symbols",
                     "entity_edge_prune_orphans C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results",
                     "entity_edge_prune_orphans results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "entity_edge_prune_orphans.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"referencing_tiers": ["L1", "L2"]}):
                fail("entity-edge-prune-orphans-request",
                     "request must carry no payload and use the fixed referencing tiers")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "entity_edge_prune_orphans.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "entity_edge_prune_orphans.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "pruned_count", "type": "u32", "minimum": 0,
                              "maximum": 0x7fffffff}):
                fail("entity-edge-prune-orphans-reply",
                     "reply must contain one bounded u32 prune count")
        elif key == ("index", 2) and name == "entity_edge_normalize_weights" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            if operation["c_symbols"] != ["db2_entity_edge_normalize_weights"]:
                fail("operation-c-symbols",
                     "entity_edge_normalize_weights C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results",
                     "entity_edge_normalize_weights results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "entity_edge_normalize_weights.request")
            # converged_rows_skipped records that the statement excludes rows
            # already holding their normalised value. Without that the pass
            # rewrites every edge on every maintenance cycle, burning WAL and
            # moving updated_at on an idle graph.
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"scale": 100, "converged_rows_skipped": True}):
                fail("entity-edge-normalize-weights-request",
                     "request must carry no payload and record the fixed scale and skip")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "entity_edge_normalize_weights.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "entity_edge_normalize_weights.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "normalized_count", "type": "u32", "minimum": 0,
                              "maximum": 0x7fffffff}):
                fail("entity-edge-normalize-weights-reply",
                     "reply must contain one bounded u32 rescale count")
        elif key == ("index", 3) and name == "project_count" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            # Only current projects count. Detached ones still exist as rows, so
            # a caller able to choose the lifecycle state could inflate the
            # tally with projects that were deliberately retired.
            if operation["c_symbols"] != ["db2_code_index_project_count"]:
                fail("operation-c-symbols",
                     "project_count C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "project_count results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "project_count.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"lifecycle_state": "current"}):
                fail("project-count-request",
                     "request must carry no payload and use the fixed lifecycle state")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "project_count.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "project_count.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "project_count", "type": "u32", "minimum": 0,
                              "maximum": 0x7fffffff}):
                fail("project-count-reply",
                     "reply must contain one bounded u32 project count")
        elif key == ("index", 4) and name == "purge_hidden_pollution" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            # A deletion sweep whose reach is entirely policy. The hidden-path
            # rule and the .gitmodules exemption both stay here because the
            # ingest path admits submodule declarations: a caller able to send
            # either one could delete files the indexer is supposed to keep.
            if operation["c_symbols"] != ["db2_code_index_purge_hidden_pollution"]:
                fail("operation-c-symbols",
                     "purge_hidden_pollution C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results",
                     "purge_hidden_pollution results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "purge_hidden_pollution.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"lifecycle_state": "current",
                                          "generation": "current",
                                          "manifest_exemption": ".gitmodules"}):
                fail("purge-hidden-pollution-request",
                     "request must carry no payload and fix the sweep's reach")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "purge_hidden_pollution.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "purge_hidden_pollution.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "purged_count", "type": "u32", "minimum": 0,
                              "maximum": 0x7fffffff}):
                fail("purge-hidden-pollution-reply",
                     "reply must contain one bounded u32 purge count")
        elif key == ("index", 5) and name == "requeue_drifted" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            # The dedup is what makes a repeated sweep harmless. If a caller
            # could send it, or send a different status, the same drifted
            # project would be enqueued once per maintenance cycle and the
            # ingest drain would do the same work over and over.
            if operation["c_symbols"] != ["db2_code_index_requeue_drifted"]:
                fail("operation-c-symbols",
                     "requeue_drifted C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "requeue_drifted results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "requeue_drifted.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"force": True, "enqueued_status": "pending",
                                          "dedup_against": ["pending", "running"]}):
                fail("requeue-drifted-request",
                     "request must carry no payload and fix the enqueue and dedup rule")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "requeue_drifted.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "requeue_drifted.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "requeued_count", "type": "u32", "minimum": 0,
                              "maximum": 0x7fffffff}):
                fail("requeue-drifted-reply",
                     "reply must contain one bounded u32 requeue count")
        elif key == ("maintenance", 1) and name == "prospective_sweep_expired" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            # The clock is the database's, not the caller's. A caller-supplied
            # instant would let one host with a wrong clock retire prospective
            # memories that are still inside their window everywhere else.
            if operation["c_symbols"] != ["db2_prospective_sweep_expired"]:
                fail("operation-c-symbols",
                     "prospective_sweep_expired C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results",
                     "prospective_sweep_expired results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "prospective_sweep_expired.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"from_state": "armed", "to_state": "expired",
                                          "clock": "database",
                                          "requires_valid_until": True}):
                fail("prospective-sweep-expired-request",
                     "request must carry no payload and fix the states and the clock")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "prospective_sweep_expired.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "prospective_sweep_expired.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "expired_count", "type": "u32", "minimum": 0,
                              "maximum": 0x7fffffff}):
                fail("prospective-sweep-expired-reply",
                     "reply must contain one bounded u32 expiry count")
        elif key == ("maintenance", 2) and name == "directive_sweep_expired" and \
                operation["wire_format"] == "db2-envelope-u32-v1":
            # Same fixed policy as the sweep beside it, and for a sharper
            # reason: a directive is a constraint the system holds itself to,
            # so retiring one early would quietly drop a rule still in force.
            if operation["c_symbols"] != ["db2_directive_sweep_expired"]:
                fail("operation-c-symbols",
                     "directive_sweep_expired C symbol differs from the reviewed backend")
            if operation["results"] != ["ok"]:
                fail("operation-results", "directive_sweep_expired results must equal ['ok']")
            request = _keys(operation["request"], {"encoded_size", "payload", "policy"},
                            "directive_sweep_expired.request")
            if (request["encoded_size"] != ENVELOPE_HEADER_LEN or
                    request["payload"] != "none" or
                    request["policy"] != {"from_state": "open", "to_state": "expired",
                                          "clock": "database",
                                          "requires_valid_until": True}):
                fail("directive-sweep-expired-request",
                     "request must carry no payload and fix the states and the clock")
            reply = _keys(operation["reply"],
                          {"encoded_size_ok", "encoded_size_error", "field"},
                          "directive_sweep_expired.reply")
            field = _keys(reply["field"], {"name", "type", "minimum", "maximum"},
                          "directive_sweep_expired.reply.field")
            if (reply["encoded_size_ok"] != ENVELOPE_HEADER_LEN + 4 or
                    reply["encoded_size_error"] != ENVELOPE_HEADER_LEN or
                    field != {"name": "directives_expired", "type": "u32", "minimum": 0,
                              "maximum": 0x7fffffff}):
                fail("directive-sweep-expired-reply",
                     "reply must contain one bounded u32 directive expiry count")
        else:
            fail("unsupported-operation", f"unsupported operation {key!r}/{name!r}")
    if len(raw_operations) != 59 or [item["name"] for item in raw_operations] != [
            "health", "embedding_dimension", "pool_status", "embedding_refusals",
            "postgres_status", "reembed_status", "reembed_clear",
            "reembed_clear_maintenance", "embedder_serving_id", "dimension_reset",
            "level3_count", "level2_count", "orphaned_l0_count", "total_count",
            "session_l2_count", "key_exists", "find_id_by_key_kind",
            "key_exists_in_tier_pair", "effectiveness_update", "retention_enforce",
            "effectiveness_demote", "effectiveness_stats", "l2_memory_ids",
            "health_record", "health_retention", "health_counters", "stats_counts",
            "expire", "demote", "promote_stable", "reclassify_directives",
            "record_l4_approval", "prune_orphaned_l0", "lifecycle_sweep_expired",
            "demote_id", "has_workspace_tag", "delete_row", "touch", "link_delete",
            "valid_at", "has_scope_type", "reject", "update_content", "decay_confidence",
            "workspace_tag_insert", "set_cognified_kind", "set_source_session",
            "negation_tokens_update", "get_content", "get_source_session",
            "pick_first_temporal_ref", "count_and_max_updated",
            "entity_edge_prune_orphans", "entity_edge_normalize_weights", "project_count",
            "purge_hidden_pollution", "requeue_drifted", "prospective_sweep_expired",
            "directive_sweep_expired"]:
        fail("unsupported-operation",
             "the partial generator requires the fifty-nine supported operations exactly once")
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


def _binary64_literal(bits: int) -> str:
    """Render a catalog binary64 bit pattern as a finite C and Go floating literal."""
    if not isinstance(bits, int) or isinstance(bits, bool) or not 0 <= bits <= 0xFFFFFFFFFFFFFFFF:
        fail("binary64-literal", "binary64 bound must be an unsigned 64-bit bit pattern")
    value = struct.unpack("<d", _put_u64(bits))[0]
    if value != value or value in (float("inf"), float("-inf")):
        fail("binary64-literal", "binary64 bound must be finite")
    text = repr(value)
    return text if ("." in text or "e" in text or "E" in text) else text + ".0"


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
    reembed_clear_maintenance = catalog["operations"][7]
    embedder_serving_id = catalog["operations"][8]
    dimension_reset = catalog["operations"][9]
    level3_count = catalog["operations"][10]
    level2_count = catalog["operations"][11]
    orphaned_l0_count = catalog["operations"][12]
    total_count = catalog["operations"][13]
    session_l2_count = catalog["operations"][14]
    key_exists = catalog["operations"][15]
    find_id_by_key_kind = catalog["operations"][16]
    key_exists_in_tier_pair = catalog["operations"][17]
    effectiveness_update = catalog["operations"][18]
    retention_enforce = catalog["operations"][19]
    effectiveness_demote = catalog["operations"][20]
    effectiveness_stats = catalog["operations"][21]
    l2_memory_ids = catalog["operations"][22]
    health_record = catalog["operations"][23]
    health_retention = catalog["operations"][24]
    health_counters = catalog["operations"][25]
    stats_counts = catalog["operations"][26]
    expire = catalog["operations"][27]
    demote = catalog["operations"][28]
    promote_stable = catalog["operations"][29]
    reclassify_directives = catalog["operations"][30]
    record_l4_approval = catalog["operations"][31]
    prune_orphaned_l0 = catalog["operations"][32]
    lifecycle_sweep_expired = catalog["operations"][33]
    demote_id = catalog["operations"][34]
    has_workspace_tag = catalog["operations"][35]
    delete_row = catalog["operations"][36]
    touch = catalog["operations"][37]
    link_delete = catalog["operations"][38]
    valid_at = catalog["operations"][39]
    has_scope_type = catalog["operations"][40]
    reject = catalog["operations"][41]
    update_content = catalog["operations"][42]
    decay_confidence = catalog["operations"][43]
    workspace_tag_insert = catalog["operations"][44]
    set_cognified_kind = catalog["operations"][45]
    set_source_session = catalog["operations"][46]
    negation_tokens_update = catalog["operations"][47]
    get_content = catalog["operations"][48]
    get_source_session = catalog["operations"][49]
    pick_first_temporal_ref = catalog["operations"][50]
    count_and_max_updated = catalog["operations"][51]
    entity_edge_prune_orphans = catalog["operations"][52]
    entity_edge_normalize_weights = catalog["operations"][53]
    project_count = catalog["operations"][54]
    purge_hidden_pollution = catalog["operations"][55]
    requeue_drifted = catalog["operations"][56]
    prospective_sweep_expired = catalog["operations"][57]
    directive_sweep_expired = catalog["operations"][58]
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
    maintenance_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(reembed_clear_maintenance["id"]), 0, _put_u32(0),
    )
    maintenance_ok_payload = _put_u32(1) + _put_u32(384) + _put_u32(384)
    maintenance_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(reembed_clear_maintenance["id"]), 0,
        maintenance_ok_payload,
    )
    maintenance_conflict_payload = _put_u32(1) + _put_u32(768) + _put_u32(384)
    maintenance_conflict = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(reembed_clear_maintenance["id"]), 2,
        maintenance_conflict_payload,
    )
    maintenance_invalid = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(reembed_clear_maintenance["id"]), 5, b"",
    )
    serving_id_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(embedder_serving_id["id"]), 0, b"",
    )
    serving_id = b"bekko-a25m/8721341054416418"
    serving_id_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(embedder_serving_id["id"]), 0,
        _put_u32(len(serving_id)) + serving_id,
    )
    serving_id_empty = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(embedder_serving_id["id"]), 0, _put_u32(0),
    )
    serving_id_max_value = b"x" * 159
    serving_id_max = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(embedder_serving_id["id"]), 0,
        _put_u32(len(serving_id_max_value)) + serving_id_max_value,
    )
    serving_id_invalid = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(embedder_serving_id["id"]), 5, b"",
    )
    reset_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(dimension_reset["id"]), 0,
        _put_u32(384) + _put_u32(0) + _put_u32(1),
    )
    reset_payload = (_put_u32(768) + _put_u32(384) + _put_u32(6) + _put_u32(0) +
                     _put_u64(1234) + _put_u32(0) + _put_u32(0))
    reset_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(dimension_reset["id"]), 0, reset_payload,
    )
    reset_conflict = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(dimension_reset["id"]), 2, reset_payload,
    )
    reset_denied = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(dimension_reset["id"]), 3, reset_payload,
    )
    reset_invalid = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(dimension_reset["id"]), 5, b"",
    )
    level3_count_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(level3_count["id"]), 0, b"",
    )
    level3_count_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(level3_count["id"]), 0, _put_u32(42),
    )
    level2_count_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(level2_count["id"]), 0, b"",
    )
    level2_count_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(level2_count["id"]), 0, _put_u32(17),
    )
    orphaned_l0_count_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(orphaned_l0_count["id"]), 0, b"",
    )
    orphaned_l0_count_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(orphaned_l0_count["id"]), 0, _put_u32(5),
    )
    prune_orphaned_l0_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(prune_orphaned_l0["id"]), 0, b"",
    )
    prune_orphaned_l0_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(prune_orphaned_l0["id"]), 0, _put_u32(3),
    )
    lifecycle_sweep_expired_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(lifecycle_sweep_expired["id"]), 0, b"",
    )
    lifecycle_sweep_expired_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(lifecycle_sweep_expired["id"]), 0, _put_u32(4),
    )
    demote_id_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(demote_id["id"]), 0, _put_u64(42),
    )
    demote_id_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(demote_id["id"]), 0, _put_u32(1),
    )
    has_workspace_tag_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(has_workspace_tag["id"]), 0, _put_u64(42),
    )
    has_workspace_tag_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(has_workspace_tag["id"]), 0, _put_u32(1),
    )
    delete_row_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(delete_row["id"]), 0, _put_u64(42),
    )
    delete_row_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(delete_row["id"]), 0, _put_u32(1),
    )
    touch_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(touch["id"]), 0, _put_u64(42),
    )
    touch_ok = _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(touch["id"]), 0, b"")
    link_delete_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(link_delete["id"]), 0, _put_u64(7),
    )
    link_delete_ok = _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(link_delete["id"]), 0, b"")
    valid_at_as_of = b"2026-08-18 12:00:00"
    valid_at_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(valid_at["id"]), 0,
        _put_u64(42) + _put_u32(len(valid_at_as_of)) + valid_at_as_of,
    )
    valid_at_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(valid_at["id"]), 0, _put_u32(1),
    )
    valid_at_unevaluated = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(valid_at["id"]), 5, b"",
    )
    has_scope_type_scope = b"workspace"
    has_scope_type_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(has_scope_type["id"]), 0,
        _put_u64(42) + _put_u32(len(has_scope_type_scope)) + has_scope_type_scope,
    )
    has_scope_type_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(has_scope_type["id"]), 0, _put_u32(1),
    )
    reject_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(reject["id"]), 0, _put_u64(42),
    )
    reject_ok = _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(reject["id"]), 0, b"")
    update_content_text = b"the revised memory text"
    update_content_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(update_content["id"]), 0,
        _put_u64(42) + _put_u32(len(update_content_text)) + update_content_text,
    )
    update_content_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(update_content["id"]), 0, _put_u32(1),
    )
    decay_confidence_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(decay_confidence["id"]), 0, _put_u64(42),
    )
    decay_confidence_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(decay_confidence["id"]), 0, b"",
    )
    workspace_tag_name = b"aimee"
    workspace_tag_insert_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(workspace_tag_insert["id"]), 0,
        _put_u64(42) + _put_u32(len(workspace_tag_name)) + workspace_tag_name,
    )
    workspace_tag_insert_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(workspace_tag_insert["id"]), 0, b"",
    )
    cognified_kind_value = b"preference"
    set_cognified_kind_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(set_cognified_kind["id"]), 0,
        _put_u64(42) + _put_u32(len(cognified_kind_value)) + cognified_kind_value,
    )
    set_cognified_kind_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(set_cognified_kind["id"]), 0, b"",
    )
    source_session_value = b"sess-2026-08-19"
    set_source_session_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(set_source_session["id"]), 0,
        _put_u64(42) + _put_u32(len(source_session_value)) + source_session_value,
    )
    set_source_session_cleared = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(set_source_session["id"]), 0,
        _put_u64(42) + _put_u32(0),
    )
    set_source_session_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(set_source_session["id"]), 0, b"",
    )
    negation_tokens_value = b"not never without"
    negation_tokens_update_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(negation_tokens_update["id"]), 0,
        _put_u64(42) + _put_u32(len(negation_tokens_value)) + negation_tokens_value,
    )
    negation_tokens_update_cleared = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(negation_tokens_update["id"]), 0,
        _put_u64(42) + _put_u32(0),
    )
    negation_tokens_update_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(negation_tokens_update["id"]), 0, b"",
    )
    get_content_text = b"the stored memory text"
    get_content_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(get_content["id"]), 0, _put_u64(42),
    )
    get_content_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(get_content["id"]), 0,
        _put_u32(len(get_content_text)) + get_content_text,
    )
    get_content_empty = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(get_content["id"]), 0, _put_u32(0),
    )
    get_content_missing = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(get_content["id"]), 1, b"",
    )
    source_session_read = b"sess-2026-08-19"
    get_source_session_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(get_source_session["id"]), 0, _put_u64(42),
    )
    get_source_session_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(get_source_session["id"]), 0,
        _put_u32(len(source_session_read)) + source_session_read,
    )
    get_source_session_missing = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(get_source_session["id"]), 1, b"",
    )
    temporal_ref_key = b"2026-08-19"
    pick_first_temporal_ref_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(pick_first_temporal_ref["id"]), 0, _put_u64(42),
    )
    pick_first_temporal_ref_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(pick_first_temporal_ref["id"]), 0,
        _put_u32(len(temporal_ref_key)) + temporal_ref_key,
    )
    pick_first_temporal_ref_missing = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(pick_first_temporal_ref["id"]), 1, b"",
    )
    corpus_stamp = b"2026-08-19 09:00:00"
    count_and_max_updated_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(count_and_max_updated["id"]), 0, b"",
    )
    count_and_max_updated_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(count_and_max_updated["id"]), 0,
        _put_u32(7) + _put_u32(len(corpus_stamp)) + corpus_stamp,
    )
    count_and_max_updated_empty = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(count_and_max_updated["id"]), 0,
        _put_u32(0) + _put_u32(0),
    )
    count_and_max_updated_unavailable = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(count_and_max_updated["id"]), 5, b"",
    )
    entity_edge_prune_orphans_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(entity_edge_prune_orphans["id"]), 0, b"",
    )
    entity_edge_prune_orphans_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(entity_edge_prune_orphans["id"]), 0, _put_u32(2),
    )
    entity_edge_normalize_weights_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(entity_edge_normalize_weights["id"]), 0, b"",
    )
    entity_edge_normalize_weights_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(entity_edge_normalize_weights["id"]), 0, _put_u32(3),
    )
    entity_edge_normalize_weights_converged = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(entity_edge_normalize_weights["id"]), 0, _put_u32(0),
    )
    project_count_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(project_count["id"]), 0, b"",
    )
    project_count_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(project_count["id"]), 0, _put_u32(4),
    )
    purge_hidden_pollution_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(purge_hidden_pollution["id"]), 0, b"",
    )
    purge_hidden_pollution_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(purge_hidden_pollution["id"]), 0, _put_u32(5),
    )
    purge_hidden_pollution_clean = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(purge_hidden_pollution["id"]), 0, _put_u32(0),
    )
    requeue_drifted_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(requeue_drifted["id"]), 0, b"",
    )
    requeue_drifted_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(requeue_drifted["id"]), 0, _put_u32(6),
    )
    requeue_drifted_none = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(requeue_drifted["id"]), 0, _put_u32(0),
    )
    prospective_sweep_expired_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(prospective_sweep_expired["id"]), 0, b"",
    )
    prospective_sweep_expired_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(prospective_sweep_expired["id"]), 0, _put_u32(7),
    )
    prospective_sweep_expired_none = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(prospective_sweep_expired["id"]), 0, _put_u32(0),
    )
    directive_sweep_expired_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(directive_sweep_expired["id"]), 0, b"",
    )
    directive_sweep_expired_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(directive_sweep_expired["id"]), 0, _put_u32(8),
    )
    directive_sweep_expired_none = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(directive_sweep_expired["id"]), 0, _put_u32(0),
    )
    total_count_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(total_count["id"]), 0, b"",
    )
    total_count_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(total_count["id"]), 0, _put_u64(1234567890123),
    )
    source_session = b"session-123"
    session_l2_count_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(session_l2_count["id"]), 0,
        _put_u32(len(source_session)) + source_session,
    )
    session_l2_count_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(session_l2_count["id"]), 0, _put_u32(3),
    )
    memory_key = b"recovery:tool-a->tool-b"
    key_exists_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(key_exists["id"]), 0,
        _put_u32(len(memory_key)) + memory_key,
    )
    key_exists_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(key_exists["id"]), 0, _put_u32(1),
    )
    lookup_key = b"task:deploy-fix"
    lookup_kind = b"task"
    find_id_by_key_kind_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(find_id_by_key_kind["id"]), 0,
        _put_u32(len(lookup_key)) + lookup_key + _put_u32(len(lookup_kind)) + lookup_kind,
    )
    find_id_by_key_kind_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(find_id_by_key_kind["id"]), 0,
        _put_u32(1) + _put_u64(42),
    )
    lookup_tier_a = b"L3"
    lookup_tier_b = b"L4"
    key_exists_in_tier_pair_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(key_exists_in_tier_pair["id"]), 0,
        _put_u32(len(memory_key)) + memory_key +
        _put_u32(len(lookup_tier_a)) + lookup_tier_a +
        _put_u32(len(lookup_tier_b)) + lookup_tier_b,
    )
    key_exists_in_tier_pair_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(key_exists_in_tier_pair["id"]), 0, _put_u32(1),
    )
    effectiveness_value_bits = 0x3fe8000000000000  # 0.75
    effectiveness_update_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(effectiveness_update["id"]), 0,
        _put_u64(42) + _put_u32(1) + _put_u64(effectiveness_value_bits),
    )
    effectiveness_update_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(effectiveness_update["id"]), 0, b"",
    )
    effectiveness_update_invalid = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(effectiveness_update["id"]), 5, b"",
    )
    retention_enforce_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(retention_enforce["id"]), 0, b"",
    )
    retention_enforce_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(retention_enforce["id"]), 0, _put_u32(4),
    )
    effectiveness_demote_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(effectiveness_demote["id"]), 0, b"",
    )
    effectiveness_demote_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(effectiveness_demote["id"]), 0, _put_u32(2),
    )
    effectiveness_stats_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(effectiveness_stats["id"]), 0, b"",
    )
    effectiveness_stats_average_bits = 0x3fe0000000000000
    effectiveness_stats_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(effectiveness_stats["id"]), 0,
        _put_u64(effectiveness_stats_average_bits) + _put_u32(3) + _put_u32(1),
    )
    l2_memory_ids_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(l2_memory_ids["id"]), 0, b"",
    )
    l2_memory_ids_values = (7, 19, 9223372036854775807)
    l2_memory_ids_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(l2_memory_ids["id"]), 0,
        _put_u32(len(l2_memory_ids_values)) + b"".join(
            _put_u64(value) for value in l2_memory_ids_values),
    )
    l2_memory_ids_empty = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(l2_memory_ids["id"]), 0, _put_u32(0),
    )
    health_record_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(health_record["id"]), 0,
        _put_u32(4) + _put_u32(2) + _put_u32(9),
    )
    health_record_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(health_record["id"]), 0, b"",
    )
    health_retention_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(health_retention["id"]), 0, b"",
    )
    health_retention_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(health_retention["id"]), 0,
        _put_u32(11) + _put_u32(3),
    )
    health_counters_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(health_counters["id"]), 0, b"",
    )
    health_counters_values = (7, 13, 5, 2, 4, 21, 9, 30, 6)
    health_counters_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(health_counters["id"]), 0,
        b"".join(_put_u32(value) for value in health_counters_values),
    )
    stats_counts_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(stats_counts["id"]), 0, b"",
    )
    stats_counts_tiers = (3, 12, 30, 8, 2, 1)
    stats_counts_kinds = (14, 5, 6, 9, 4, 3, 2, 1, 7, 5)
    stats_counts_total = sum(stats_counts_tiers)
    stats_counts_conflicts = 4
    stats_counts_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(stats_counts["id"]), 0,
        b"".join(_put_u32(value) for value in stats_counts_tiers) +
        b"".join(_put_u32(value) for value in stats_counts_kinds) +
        _put_u32(stats_counts_total) + _put_u32(stats_counts_conflicts),
    )
    expire_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(expire["id"]), 0, b"",
    )
    expire_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(expire["id"]), 0, _put_u32(9) + _put_u32(17),
    )
    demote_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(demote["id"]), 0, b"",
    )
    demote_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(demote["id"]), 0, _put_u32(6) + _put_u32(2),
    )
    demote_idle = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(demote["id"]), 0, _put_u32(0) + _put_u32(0),
    )
    promote_stable_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(promote_stable["id"]), 0, b"",
    )
    promote_stable_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(promote_stable["id"]), 0, _put_u32(4),
    )
    reclassify_gated_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(reclassify_directives["id"]), 0, _put_u32(1),
    )
    reclassify_open_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(reclassify_directives["id"]), 0, _put_u32(0),
    )
    reclassify_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(reclassify_directives["id"]), 0, _put_u32(3),
    )
    approval_approver = b"operator"
    approval_note = b"reviewed with the platform team"
    approval_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(record_l4_approval["id"]), 0,
        _put_u64(42) + _put_u32(len(approval_approver)) + approval_approver +
        _put_u32(len(approval_note)) + approval_note,
    )
    # An empty note is legal; an empty approver is not.
    approval_bare_request = _envelope(
        catalog, ENVELOPE_REQUEST_MAGIC, int(record_l4_approval["id"]), 0,
        _put_u64(42) + _put_u32(len(approval_approver)) + approval_approver + _put_u32(0),
    )
    approval_ok = _envelope(
        catalog, ENVELOPE_REPLY_MAGIC, int(record_l4_approval["id"]), 0, b"",
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
        }, {
            "family": reembed_clear_maintenance["family"],
            "id": reembed_clear_maintenance["id"],
            "name": reembed_clear_maintenance["name"],
            "request": {
                "positive": maintenance_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(maintenance_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(maintenance_request, 16, 0).hex()},
                    {"mutation": "invalid_force", "hex":
                     mutate_u32(maintenance_request, ENVELOPE_HEADER_LEN, 2).hex()},
                    {"mutation": "short", "hex": maintenance_request[:-1].hex()},
                    {"mutation": "long", "hex": (maintenance_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "was_in_progress": 1, "recorded_dimension": 384,
                     "running_dimension": 384, "hex": maintenance_ok.hex()},
                    {"result": 2, "was_in_progress": 1, "recorded_dimension": 768,
                     "running_dimension": 384, "hex": maintenance_conflict.hex()},
                    {"result": 5, "was_in_progress": 0, "recorded_dimension": 0,
                     "running_dimension": 0, "hex": maintenance_invalid.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(maintenance_ok, 8, 7).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(maintenance_ok, 12, 1).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(reembed_clear_maintenance["id"]), 0, b"").hex()},
                    {"mutation": "conflict_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(reembed_clear_maintenance["id"]), 2, b"").hex()},
                    {"mutation": "error_with_payload", "hex":
                     mutate_u32(maintenance_ok, 12, 5).hex()},
                    {"mutation": "invalid_was_in_progress", "hex":
                     mutate_u32(maintenance_ok, ENVELOPE_HEADER_LEN, 2).hex()},
                    {"mutation": "recorded_too_large", "hex":
                     mutate_u32(maintenance_ok, ENVELOPE_HEADER_LEN + 4, 4001).hex()},
                    {"mutation": "zero_running_dimension", "hex":
                     mutate_u32(maintenance_ok, ENVELOPE_HEADER_LEN + 8, 0).hex()},
                    {"mutation": "conflict_without_mismatch", "hex":
                     mutate_u32(maintenance_conflict, ENVELOPE_HEADER_LEN + 4, 384).hex()},
                    {"mutation": "short", "hex": maintenance_ok[:-1].hex()},
                    {"mutation": "long", "hex": (maintenance_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": embedder_serving_id["family"],
            "id": embedder_serving_id["id"],
            "name": embedder_serving_id["name"],
            "request": {
                "positive": serving_id_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(serving_id_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(serving_id_request, 16, 1).hex()},
                    {"mutation": "short", "hex": serving_id_request[:-1].hex()},
                    {"mutation": "long", "hex": (serving_id_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "serving_id": serving_id.decode(),
                     "hex": serving_id_ok.hex()},
                    {"result": 0, "serving_id": "", "hex": serving_id_empty.hex()},
                    {"result": 0, "serving_id": serving_id_max_value.decode(),
                     "hex": serving_id_max.hex()},
                    {"result": 5, "serving_id": "", "hex": serving_id_invalid.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(serving_id_ok, 8, 8).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(serving_id_ok, 12, 1).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(embedder_serving_id["id"]), 0, b"").hex()},
                    {"mutation": "error_with_payload", "hex":
                     mutate_u32(serving_id_ok, 12, 5).hex()},
                    {"mutation": "length_mismatch", "hex":
                     mutate_u32(serving_id_ok, ENVELOPE_HEADER_LEN,
                                len(serving_id) + 1).hex()},
                    {"mutation": "length_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(embedder_serving_id["id"]), 0,
                               _put_u32(160) + (b"x" * 160)).hex()},
                    {"mutation": "embedded_nul", "hex":
                     (serving_id_ok[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": serving_id_ok[:-1].hex()},
                    {"mutation": "long", "hex": (serving_id_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": dimension_reset["family"],
            "id": dimension_reset["id"],
            "name": dimension_reset["name"],
            "request": {
                "positive": reset_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex": mutate_u32(reset_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(reset_request, 16, 8).hex()},
                    {"mutation": "target_zero", "hex":
                     mutate_u32(reset_request, ENVELOPE_HEADER_LEN, 0).hex()},
                    {"mutation": "target_too_large", "hex":
                     mutate_u32(reset_request, ENVELOPE_HEADER_LEN, 4001).hex()},
                    {"mutation": "invalid_force", "hex":
                     mutate_u32(reset_request, ENVELOPE_HEADER_LEN + 4, 2).hex()},
                    {"mutation": "invalid_dry_run", "hex":
                     mutate_u32(reset_request, ENVELOPE_HEADER_LEN + 8, 2).hex()},
                    {"mutation": "short", "hex": reset_request[:-1].hex()},
                    {"mutation": "long", "hex": (reset_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": reset_ok.hex()},
                    {"result": 2, "hex": reset_conflict.hex()},
                    {"result": 3, "hex": reset_denied.hex()},
                    {"result": 5, "hex": reset_invalid.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(reset_ok, 8, 9).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(reset_ok, 12, 1).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(dimension_reset["id"]), 0, b"").hex()},
                    {"mutation": "error_with_payload", "hex":
                     mutate_u32(reset_ok, 12, 5).hex()},
                    {"mutation": "target_zero", "hex":
                     mutate_u32(reset_ok, ENVELOPE_HEADER_LEN + 4, 0).hex()},
                    {"mutation": "tables_too_large", "hex":
                     mutate_u32(reset_ok, ENVELOPE_HEADER_LEN + 8, 17).hex()},
                    {"mutation": "short", "hex": reset_ok[:-1].hex()},
                    {"mutation": "long", "hex": (reset_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": level3_count["family"],
            "id": level3_count["id"],
            "name": level3_count["name"],
            "request": {
                "positive": level3_count_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(level3_count_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(level3_count_request, 16, 1).hex()},
                    {"mutation": "short", "hex": level3_count_request[:-1].hex()},
                    {"mutation": "long", "hex": (level3_count_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "count": 42, "hex": level3_count_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(level3_count_ok, 8, 2).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(level3_count_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(level3_count["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (level3_count_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": level3_count_ok[:-1].hex()},
                    {"mutation": "long", "hex": (level3_count_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": level2_count["family"],
            "id": level2_count["id"],
            "name": level2_count["name"],
            "request": {
                "positive": level2_count_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(level2_count_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(level2_count_request, 16, 1).hex()},
                    {"mutation": "short", "hex": level2_count_request[:-1].hex()},
                    {"mutation": "long", "hex": (level2_count_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "count": 17, "hex": level2_count_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(level2_count_ok, 8, 1).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(level2_count_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(level2_count["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (level2_count_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": level2_count_ok[:-1].hex()},
                    {"mutation": "long", "hex": (level2_count_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": orphaned_l0_count["family"],
            "id": orphaned_l0_count["id"],
            "name": orphaned_l0_count["name"],
            "request": {
                "positive": orphaned_l0_count_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(orphaned_l0_count_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(orphaned_l0_count_request, 16, 1).hex()},
                    {"mutation": "short", "hex": orphaned_l0_count_request[:-1].hex()},
                    {"mutation": "long", "hex": (orphaned_l0_count_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "count": 5, "hex": orphaned_l0_count_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(orphaned_l0_count_ok, 8, 2).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(orphaned_l0_count_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(orphaned_l0_count["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (orphaned_l0_count_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": orphaned_l0_count_ok[:-1].hex()},
                    {"mutation": "long", "hex": (orphaned_l0_count_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": total_count["family"],
            "id": total_count["id"],
            "name": total_count["name"],
            "request": {
                "positive": total_count_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(total_count_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(total_count_request, 16, 1).hex()},
                    {"mutation": "short", "hex": total_count_request[:-1].hex()},
                    {"mutation": "long", "hex": (total_count_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "count": 1234567890123, "hex": total_count_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(total_count_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(total_count_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(total_count["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (total_count_ok[:-8] + _put_u64(0x8000000000000000)).hex()},
                    {"mutation": "short", "hex": total_count_ok[:-1].hex()},
                    {"mutation": "long", "hex": (total_count_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": session_l2_count["family"],
            "id": session_l2_count["id"],
            "name": session_l2_count["name"],
            "request": {
                "positive": session_l2_count_request.hex(),
                "source_session": source_session.decode(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(session_l2_count_request, 12, 1).hex()},
                    {"mutation": "empty_session", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(session_l2_count["id"]), 0, _put_u32(0)).hex()},
                    {"mutation": "length_mismatch", "hex":
                     mutate_u32(session_l2_count_request, ENVELOPE_HEADER_LEN,
                                len(source_session) + 1).hex()},
                    {"mutation": "session_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(session_l2_count["id"]), 0,
                               _put_u32(128) + b"x" * 128).hex()},
                    {"mutation": "embedded_nul", "hex":
                     (session_l2_count_request[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": session_l2_count_request[:-1].hex()},
                    {"mutation": "long", "hex": (session_l2_count_request + b"x").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "count": 3, "hex": session_l2_count_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(session_l2_count_ok, 8, 4).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(session_l2_count_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(session_l2_count["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (session_l2_count_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": session_l2_count_ok[:-1].hex()},
                    {"mutation": "long", "hex": (session_l2_count_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": key_exists["family"],
            "id": key_exists["id"],
            "name": key_exists["name"],
            "request": {
                "positive": key_exists_request.hex(),
                "key": memory_key.decode(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(key_exists_request, 12, 1).hex()},
                    {"mutation": "empty_key", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(key_exists["id"]), 0, _put_u32(0)).hex()},
                    {"mutation": "length_mismatch", "hex":
                     mutate_u32(key_exists_request, ENVELOPE_HEADER_LEN,
                                len(memory_key) + 1).hex()},
                    {"mutation": "key_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(key_exists["id"]), 0,
                               _put_u32(512) + b"x" * 512).hex()},
                    {"mutation": "embedded_nul", "hex":
                     (key_exists_request[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": key_exists_request[:-1].hex()},
                    {"mutation": "long", "hex": (key_exists_request + b"x").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "exists": 1, "hex": key_exists_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(key_exists_ok, 8, 5).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(key_exists_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(key_exists["id"]), 0, b"").hex()},
                    {"mutation": "exists_too_large", "hex":
                     (key_exists_ok[:-4] + _put_u32(2)).hex()},
                    {"mutation": "short", "hex": key_exists_ok[:-1].hex()},
                    {"mutation": "long", "hex": (key_exists_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": find_id_by_key_kind["family"],
            "id": find_id_by_key_kind["id"],
            "name": find_id_by_key_kind["name"],
            "request": {
                "positive": find_id_by_key_kind_request.hex(),
                "key": lookup_key.decode(),
                "kind": lookup_kind.decode(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(find_id_by_key_kind_request, 12, 1).hex()},
                    {"mutation": "empty_key", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(find_id_by_key_kind["id"]), 0,
                               _put_u32(0) + _put_u32(len(lookup_kind)) + lookup_kind).hex()},
                    {"mutation": "key_length_mismatch", "hex":
                     mutate_u32(find_id_by_key_kind_request, ENVELOPE_HEADER_LEN,
                                len(lookup_key) + 1).hex()},
                    {"mutation": "key_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(find_id_by_key_kind["id"]), 0,
                               _put_u32(512) + b"x" * 512 +
                               _put_u32(len(lookup_kind)) + lookup_kind).hex()},
                    {"mutation": "key_embedded_nul", "hex":
                     (find_id_by_key_kind_request[:ENVELOPE_HEADER_LEN + 4] + b"\0" +
                      find_id_by_key_kind_request[ENVELOPE_HEADER_LEN + 5:]).hex()},
                    {"mutation": "empty_kind", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(find_id_by_key_kind["id"]), 0,
                               _put_u32(len(lookup_key)) + lookup_key + _put_u32(0)).hex()},
                    {"mutation": "kind_length_mismatch", "hex":
                     mutate_u32(find_id_by_key_kind_request,
                                ENVELOPE_HEADER_LEN + 4 + len(lookup_key),
                                len(lookup_kind) + 1).hex()},
                    {"mutation": "kind_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(find_id_by_key_kind["id"]), 0,
                               _put_u32(len(lookup_key)) + lookup_key +
                               _put_u32(16) + b"x" * 16).hex()},
                    {"mutation": "kind_embedded_nul", "hex":
                     (find_id_by_key_kind_request[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": find_id_by_key_kind_request[:-1].hex()},
                    {"mutation": "long", "hex": (find_id_by_key_kind_request + b"x").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "found": 1, "id": 42,
                     "hex": find_id_by_key_kind_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(find_id_by_key_kind_ok, 8, 6).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(find_id_by_key_kind_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(find_id_by_key_kind["id"]), 0, b"").hex()},
                    {"mutation": "found_too_large", "hex":
                     (find_id_by_key_kind_ok[:ENVELOPE_HEADER_LEN] + _put_u32(2) +
                      find_id_by_key_kind_ok[ENVELOPE_HEADER_LEN + 4:]).hex()},
                    {"mutation": "absent_with_id", "hex":
                     (find_id_by_key_kind_ok[:ENVELOPE_HEADER_LEN] + _put_u32(0) +
                      _put_u64(42)).hex()},
                    {"mutation": "present_without_id", "hex":
                     (find_id_by_key_kind_ok[:ENVELOPE_HEADER_LEN + 4] + _put_u64(0)).hex()},
                    {"mutation": "id_too_large", "hex":
                     (find_id_by_key_kind_ok[:ENVELOPE_HEADER_LEN + 4] +
                      _put_u64(0x8000000000000000)).hex()},
                    {"mutation": "short", "hex": find_id_by_key_kind_ok[:-1].hex()},
                    {"mutation": "long", "hex": (find_id_by_key_kind_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": key_exists_in_tier_pair["family"],
            "id": key_exists_in_tier_pair["id"],
            "name": key_exists_in_tier_pair["name"],
            "request": {
                "positive": key_exists_in_tier_pair_request.hex(),
                "key": memory_key.decode(),
                "tier_a": lookup_tier_a.decode(),
                "tier_b": lookup_tier_b.decode(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(key_exists_in_tier_pair_request, 12, 1).hex()},
                    {"mutation": "empty_key", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(key_exists_in_tier_pair["id"]), 0,
                               _put_u32(0) +
                               _put_u32(len(lookup_tier_a)) + lookup_tier_a +
                               _put_u32(len(lookup_tier_b)) + lookup_tier_b).hex()},
                    {"mutation": "key_length_mismatch", "hex":
                     mutate_u32(key_exists_in_tier_pair_request, ENVELOPE_HEADER_LEN,
                                len(memory_key) + 1).hex()},
                    {"mutation": "key_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(key_exists_in_tier_pair["id"]), 0,
                               _put_u32(512) + b"x" * 512 +
                               _put_u32(len(lookup_tier_a)) + lookup_tier_a +
                               _put_u32(len(lookup_tier_b)) + lookup_tier_b).hex()},
                    {"mutation": "key_embedded_nul", "hex":
                     (key_exists_in_tier_pair_request[:ENVELOPE_HEADER_LEN + 4] + b"\0" +
                      key_exists_in_tier_pair_request[ENVELOPE_HEADER_LEN + 5:]).hex()},
                    {"mutation": "empty_tier_a", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(key_exists_in_tier_pair["id"]), 0,
                               _put_u32(len(memory_key)) + memory_key + _put_u32(0) +
                               _put_u32(len(lookup_tier_b)) + lookup_tier_b).hex()},
                    {"mutation": "tier_a_length_mismatch", "hex":
                     mutate_u32(key_exists_in_tier_pair_request,
                                ENVELOPE_HEADER_LEN + 4 + len(memory_key),
                                len(lookup_tier_a) + 1).hex()},
                    {"mutation": "tier_a_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(key_exists_in_tier_pair["id"]), 0,
                               _put_u32(len(memory_key)) + memory_key +
                               _put_u32(16) + b"x" * 16 +
                               _put_u32(len(lookup_tier_b)) + lookup_tier_b).hex()},
                    {"mutation": "tier_a_embedded_nul", "hex":
                     (key_exists_in_tier_pair_request[
                         :ENVELOPE_HEADER_LEN + 8 + len(memory_key)] + b"\0" +
                      key_exists_in_tier_pair_request[
                          ENVELOPE_HEADER_LEN + 9 + len(memory_key):]).hex()},
                    {"mutation": "empty_tier_b", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(key_exists_in_tier_pair["id"]), 0,
                               _put_u32(len(memory_key)) + memory_key +
                               _put_u32(len(lookup_tier_a)) + lookup_tier_a +
                               _put_u32(0)).hex()},
                    {"mutation": "tier_b_length_mismatch", "hex":
                     mutate_u32(key_exists_in_tier_pair_request,
                                ENVELOPE_HEADER_LEN + 8 + len(memory_key) + len(lookup_tier_a),
                                len(lookup_tier_b) + 1).hex()},
                    {"mutation": "tier_b_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(key_exists_in_tier_pair["id"]), 0,
                               _put_u32(len(memory_key)) + memory_key +
                               _put_u32(len(lookup_tier_a)) + lookup_tier_a +
                               _put_u32(16) + b"x" * 16).hex()},
                    {"mutation": "tier_b_embedded_nul", "hex":
                     (key_exists_in_tier_pair_request[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex":
                     key_exists_in_tier_pair_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (key_exists_in_tier_pair_request + b"x").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "exists": 1, "hex": key_exists_in_tier_pair_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(key_exists_in_tier_pair_ok, 8, 7).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(key_exists_in_tier_pair_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(key_exists_in_tier_pair["id"]), 0, b"").hex()},
                    {"mutation": "exists_too_large", "hex":
                     (key_exists_in_tier_pair_ok[:-4] + _put_u32(2)).hex()},
                    {"mutation": "short", "hex": key_exists_in_tier_pair_ok[:-1].hex()},
                    {"mutation": "long", "hex":
                     (key_exists_in_tier_pair_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": effectiveness_update["family"],
            "id": effectiveness_update["id"],
            "name": effectiveness_update["name"],
            "request": {
                "positive": effectiveness_update_request.hex(),
                "memory_id": 42,
                "has_value": 1,
                "value_bits": effectiveness_value_bits,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(effectiveness_update_request, 12, 1).hex()},
                    {"mutation": "zero_memory_id", "hex":
                     (effectiveness_update_request[:ENVELOPE_HEADER_LEN] + _put_u64(0) +
                      effectiveness_update_request[ENVELOPE_HEADER_LEN + 8:]).hex()},
                    {"mutation": "memory_id_too_large", "hex":
                     (effectiveness_update_request[:ENVELOPE_HEADER_LEN] +
                      _put_u64(0x8000000000000000) +
                      effectiveness_update_request[ENVELOPE_HEADER_LEN + 8:]).hex()},
                    {"mutation": "has_value_too_large", "hex":
                     mutate_u32(effectiveness_update_request, ENVELOPE_HEADER_LEN + 8, 2).hex()},
                    {"mutation": "clear_with_value", "hex":
                     mutate_u32(effectiveness_update_request, ENVELOPE_HEADER_LEN + 8, 0).hex()},
                    {"mutation": "short", "hex": effectiveness_update_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (effectiveness_update_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": effectiveness_update_ok.hex()},
                    {"result": 5, "hex": effectiveness_update_invalid.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(effectiveness_update_ok, 8, 8).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(effectiveness_update_ok, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(effectiveness_update["id"]), 0, b"\0").hex()},
                    {"mutation": "short", "hex": effectiveness_update_ok[:-1].hex()},
                    {"mutation": "long", "hex":
                     (effectiveness_update_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": retention_enforce["family"],
            "id": retention_enforce["id"],
            "name": retention_enforce["name"],
            "request": {
                "positive": retention_enforce_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(retention_enforce_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(retention_enforce_request, 16, 1).hex()},
                    {"mutation": "short", "hex": retention_enforce_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (retention_enforce_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "deleted_count": 4, "hex": retention_enforce_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(retention_enforce_ok, 8, 9).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(retention_enforce_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(retention_enforce["id"]), 0, b"").hex()},
                    {"mutation": "deleted_count_too_large", "hex":
                     (retention_enforce_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": retention_enforce_ok[:-1].hex()},
                    {"mutation": "long", "hex":
                     (retention_enforce_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": effectiveness_demote["family"],
            "id": effectiveness_demote["id"],
            "name": effectiveness_demote["name"],
            "request": {
                "positive": effectiveness_demote_request.hex(),
                "threshold_bits": 0x3fd3333333333333,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(effectiveness_demote_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(effectiveness_demote_request, 16, 1).hex()},
                    {"mutation": "short", "hex": effectiveness_demote_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (effectiveness_demote_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "demoted_count": 2, "hex": effectiveness_demote_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(effectiveness_demote_ok, 8, 10).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(effectiveness_demote_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(effectiveness_demote["id"]), 0, b"").hex()},
                    {"mutation": "demoted_count_too_large", "hex":
                     (effectiveness_demote_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": effectiveness_demote_ok[:-1].hex()},
                    {"mutation": "long", "hex":
                     (effectiveness_demote_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": effectiveness_stats["family"],
            "id": effectiveness_stats["id"],
            "name": effectiveness_stats["name"],
            "request": {
                "positive": effectiveness_stats_request.hex(),
                "low_threshold_bits": 0x3fd3333333333333,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(effectiveness_stats_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(effectiveness_stats_request, 16, 1).hex()},
                    {"mutation": "short", "hex": effectiveness_stats_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (effectiveness_stats_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "avg_effectiveness_bits": effectiveness_stats_average_bits,
                     "low_effectiveness_count": 3, "high_impact_count": 1,
                     "hex": effectiveness_stats_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(effectiveness_stats_ok, 8, 11).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(effectiveness_stats_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(effectiveness_stats["id"]), 0, b"").hex()},
                    {"mutation": "average_above_maximum", "hex":
                     (effectiveness_stats_ok[:ENVELOPE_HEADER_LEN] +
                      _put_u64(0x3ff0000000000001) +
                      effectiveness_stats_ok[ENVELOPE_HEADER_LEN + 8:]).hex()},
                    {"mutation": "average_negative", "hex":
                     (effectiveness_stats_ok[:ENVELOPE_HEADER_LEN] +
                      _put_u64(0xbfe0000000000000) +
                      effectiveness_stats_ok[ENVELOPE_HEADER_LEN + 8:]).hex()},
                    {"mutation": "average_not_a_number", "hex":
                     (effectiveness_stats_ok[:ENVELOPE_HEADER_LEN] +
                      _put_u64(0x7ff8000000000000) +
                      effectiveness_stats_ok[ENVELOPE_HEADER_LEN + 8:]).hex()},
                    {"mutation": "low_effectiveness_count_too_large", "hex":
                     (effectiveness_stats_ok[:ENVELOPE_HEADER_LEN + 8] +
                      _put_u32(0x80000000) +
                      effectiveness_stats_ok[ENVELOPE_HEADER_LEN + 12:]).hex()},
                    {"mutation": "high_impact_count_too_large", "hex":
                     (effectiveness_stats_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": effectiveness_stats_ok[:-1].hex()},
                    {"mutation": "long", "hex":
                     (effectiveness_stats_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": l2_memory_ids["family"],
            "id": l2_memory_ids["id"],
            "name": l2_memory_ids["name"],
            "request": {
                "positive": l2_memory_ids_request.hex(),
                "maximum_ids": l2_memory_ids["request"]["policy"]["maximum_ids"],
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(l2_memory_ids_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(l2_memory_ids_request, 16, 1).hex()},
                    {"mutation": "short", "hex": l2_memory_ids_request[:-1].hex()},
                    {"mutation": "long", "hex": (l2_memory_ids_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "memory_ids": list(l2_memory_ids_values),
                     "hex": l2_memory_ids_ok.hex()},
                    {"result": 0, "memory_ids": [], "hex": l2_memory_ids_empty.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(l2_memory_ids_ok, 8, 12).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(l2_memory_ids_ok, 12, 5).hex()},
                    {"mutation": "ok_without_count", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(l2_memory_ids["id"]), 0, b"").hex()},
                    {"mutation": "count_exceeds_payload", "hex":
                     mutate_u32(l2_memory_ids_ok, ENVELOPE_HEADER_LEN, 4).hex()},
                    {"mutation": "count_below_payload", "hex":
                     mutate_u32(l2_memory_ids_ok, ENVELOPE_HEADER_LEN, 2).hex()},
                    {"mutation": "count_above_maximum", "hex":
                     mutate_u32(l2_memory_ids_ok, ENVELOPE_HEADER_LEN, 2049).hex()},
                    {"mutation": "identifier_zero", "hex":
                     (l2_memory_ids_ok[:ENVELOPE_HEADER_LEN + 4] + _put_u64(0) +
                      l2_memory_ids_ok[ENVELOPE_HEADER_LEN + 12:]).hex()},
                    {"mutation": "identifier_above_maximum", "hex":
                     (l2_memory_ids_ok[:-8] + _put_u64(0x8000000000000000)).hex()},
                    {"mutation": "short", "hex": l2_memory_ids_ok[:-1].hex()},
                    {"mutation": "long", "hex": (l2_memory_ids_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": health_record["family"],
            "id": health_record["id"],
            "name": health_record["name"],
            "request": {
                "positive": health_record_request.hex(),
                "promotions": 4,
                "demotions": 2,
                "expirations": 9,
                "conflict_window_days":
                    health_record["request"]["policy"]["conflict_window_days"],
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(health_record_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(health_record_request, 16, 8).hex()},
                    {"mutation": "promotions_too_large", "hex":
                     mutate_u32(health_record_request, ENVELOPE_HEADER_LEN, 0x80000000).hex()},
                    {"mutation": "demotions_too_large", "hex":
                     mutate_u32(health_record_request, ENVELOPE_HEADER_LEN + 4, 0x80000000).hex()},
                    {"mutation": "expirations_too_large", "hex":
                     mutate_u32(health_record_request, ENVELOPE_HEADER_LEN + 8, 0x80000000).hex()},
                    {"mutation": "short", "hex": health_record_request[:-1].hex()},
                    {"mutation": "long", "hex": (health_record_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": health_record_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(health_record_ok, 8, 13).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(health_record_ok, 12, 5).hex()},
                    {"mutation": "unexpected_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(health_record["id"]), 0,
                               _put_u32(0)).hex()},
                    {"mutation": "short", "hex": health_record_ok[:-1].hex()},
                    {"mutation": "long", "hex": (health_record_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": health_retention["family"],
            "id": health_retention["id"],
            "name": health_retention["name"],
            "request": {
                "positive": health_retention_request.hex(),
                "snapshot_retention_days":
                    health_retention["request"]["policy"]["snapshot_retention_days"],
                "contradiction_retention_days":
                    health_retention["request"]["policy"]["contradiction_retention_days"],
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(health_retention_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(health_retention_request, 16, 1).hex()},
                    {"mutation": "short", "hex": health_retention_request[:-1].hex()},
                    {"mutation": "long", "hex": (health_retention_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "snapshots_deleted": 11, "contradictions_deleted": 3,
                     "hex": health_retention_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(health_retention_ok, 8, 14).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(health_retention_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(health_retention["id"]), 0, b"").hex()},
                    {"mutation": "snapshots_deleted_too_large", "hex":
                     mutate_u32(health_retention_ok, ENVELOPE_HEADER_LEN, 0x80000000).hex()},
                    {"mutation": "contradictions_deleted_too_large", "hex":
                     mutate_u32(health_retention_ok, ENVELOPE_HEADER_LEN + 4, 0x80000000).hex()},
                    {"mutation": "short", "hex": health_retention_ok[:-1].hex()},
                    {"mutation": "long", "hex": (health_retention_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": health_counters["family"],
            "id": health_counters["id"],
            "name": health_counters["name"],
            "request": {
                "positive": health_counters_request.hex(),
                "promote_use_count":
                    health_counters["request"]["policy"]["promote_use_count"],
                "promote_confidence_bits":
                    health_counters["request"]["policy"]["promote_confidence_binary64_bits"],
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(health_counters_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(health_counters_request, 16, 1).hex()},
                    {"mutation": "short", "hex": health_counters_request[:-1].hex()},
                    {"mutation": "long", "hex": (health_counters_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0,
                     "counters": dict(zip(HEALTH_COUNTERS, health_counters_values)),
                     "hex": health_counters_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(health_counters_ok, 8, 15).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(health_counters_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(health_counters["id"]), 0, b"").hex()},
                    # Every counter is bounded, including the last one on the wire.
                    {"mutation": "first_counter_too_large", "hex":
                     mutate_u32(health_counters_ok, ENVELOPE_HEADER_LEN, 0x80000000).hex()},
                    {"mutation": "last_counter_too_large", "hex":
                     (health_counters_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": health_counters_ok[:-1].hex()},
                    {"mutation": "long", "hex": (health_counters_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": stats_counts["family"],
            "id": stats_counts["id"],
            "name": stats_counts["name"],
            "request": {
                "positive": stats_counts_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(stats_counts_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(stats_counts_request, 16, 1).hex()},
                    {"mutation": "short", "hex": stats_counts_request[:-1].hex()},
                    {"mutation": "long", "hex": (stats_counts_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0,
                     "tier_counts": list(stats_counts_tiers),
                     "kind_counts": list(stats_counts_kinds),
                     "total": stats_counts_total,
                     "conflicts": stats_counts_conflicts,
                     "hex": stats_counts_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(stats_counts_ok, 8, 16).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(stats_counts_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(stats_counts["id"]), 0, b"").hex()},
                    {"mutation": "first_tier_too_large", "hex":
                     mutate_u32(stats_counts_ok, ENVELOPE_HEADER_LEN, 0x80000000).hex()},
                    # The last kind bucket is the one a short mapping would drop.
                    {"mutation": "last_kind_too_large", "hex":
                     mutate_u32(
                         stats_counts_ok,
                         ENVELOPE_HEADER_LEN + 4 * (len(MEMORY_TIERS) + len(MEMORY_KINDS) - 1),
                         0x80000000).hex()},
                    {"mutation": "conflicts_too_large", "hex":
                     (stats_counts_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": stats_counts_ok[:-1].hex()},
                    {"mutation": "long", "hex": (stats_counts_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": expire["family"],
            "id": expire["id"],
            "name": expire["name"],
            "request": {
                "positive": expire_request.hex(),
                "stale_l1_tier": expire["request"]["policy"]["stale_l1_tier"],
                "maximum_kinds": expire["request"]["policy"]["maximum_kinds"],
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(expire_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(expire_request, 16, 1).hex()},
                    {"mutation": "short", "hex": expire_request[:-1].hex()},
                    {"mutation": "long", "hex": (expire_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "level0_deleted": 9, "stale_level1_deleted": 17,
                     "hex": expire_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(expire_ok, 8, 17).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(expire_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(expire["id"]), 0, b"").hex()},
                    {"mutation": "level0_deleted_too_large", "hex":
                     mutate_u32(expire_ok, ENVELOPE_HEADER_LEN, 0x80000000).hex()},
                    {"mutation": "stale_level1_deleted_too_large", "hex":
                     mutate_u32(expire_ok, ENVELOPE_HEADER_LEN + 4, 0x80000000).hex()},
                    {"mutation": "short", "hex": expire_ok[:-1].hex()},
                    {"mutation": "long", "hex": (expire_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": demote["family"],
            "id": demote["id"],
            "name": demote["name"],
            "request": {
                "positive": demote_request.hex(),
                "demote_tier": demote["request"]["policy"]["demote_tier"],
                "maximum_kinds": demote["request"]["policy"]["maximum_kinds"],
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(demote_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(demote_request, 16, 1).hex()},
                    {"mutation": "short", "hex": demote_request[:-1].hex()},
                    {"mutation": "long", "hex": (demote_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "demoted_count": 6, "cascaded_count": 2,
                     "hex": demote_ok.hex()},
                    {"result": 0, "demoted_count": 0, "cascaded_count": 0,
                     "hex": demote_idle.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(demote_ok, 8, 18).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(demote_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(demote["id"]), 0, b"").hex()},
                    {"mutation": "demoted_count_too_large", "hex":
                     mutate_u32(demote_ok, ENVELOPE_HEADER_LEN, 0x80000000).hex()},
                    {"mutation": "cascaded_count_too_large", "hex":
                     mutate_u32(demote_ok, ENVELOPE_HEADER_LEN + 4, 0x80000000).hex()},
                    # A cascade without a demotion contradicts the invariant.
                    {"mutation": "cascade_without_demotion", "hex":
                     mutate_u32(demote_idle, ENVELOPE_HEADER_LEN + 4, 1).hex()},
                    {"mutation": "short", "hex": demote_ok[:-1].hex()},
                    {"mutation": "long", "hex": (demote_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": promote_stable["family"],
            "id": promote_stable["id"],
            "name": promote_stable["name"],
            "request": {
                "positive": promote_stable_request.hex(),
                "source_tier": promote_stable["request"]["policy"]["source_tier"],
                "target_tier": promote_stable["request"]["policy"]["target_tier"],
                "kinds": list(promote_stable["request"]["policy"]["kinds"]),
                "confidence_bits":
                    promote_stable["request"]["policy"]["minimum_confidence_binary64_bits"],
                "use_count": promote_stable["request"]["policy"]["minimum_use_count"],
                "stable_days": promote_stable["request"]["policy"]["stable_days"],
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(promote_stable_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(promote_stable_request, 16, 1).hex()},
                    {"mutation": "short", "hex": promote_stable_request[:-1].hex()},
                    {"mutation": "long", "hex": (promote_stable_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "promoted_count": 4, "hex": promote_stable_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(promote_stable_ok, 8, 19).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(promote_stable_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(promote_stable["id"]), 0, b"").hex()},
                    {"mutation": "promoted_count_too_large", "hex":
                     (promote_stable_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": promote_stable_ok[:-1].hex()},
                    {"mutation": "long", "hex": (promote_stable_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": reclassify_directives["family"],
            "id": reclassify_directives["id"],
            "name": reclassify_directives["name"],
            "request": {
                # Both gate settings are canonical: the batch path runs open,
                # the operator approval path runs gated.
                "positive": reclassify_gated_request.hex(),
                "require_approval": 1,
                "open_positive": reclassify_open_request.hex(),
                "source_tier": reclassify_directives["request"]["policy"]["source_tier"],
                "target_tier": reclassify_directives["request"]["policy"]["target_tier"],
                "kinds": list(reclassify_directives["request"]["policy"]["kinds"]),
                "gated_kind": reclassify_directives["request"]["policy"]["gated_kind"],
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(reclassify_gated_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(reclassify_gated_request, 16, 0).hex()},
                    {"mutation": "gate_out_of_range", "hex":
                     mutate_u32(reclassify_gated_request, ENVELOPE_HEADER_LEN, 2).hex()},
                    {"mutation": "short", "hex": reclassify_gated_request[:-1].hex()},
                    {"mutation": "long", "hex": (reclassify_gated_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "reclassified_count": 3, "hex": reclassify_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(reclassify_ok, 8, 20).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(reclassify_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(reclassify_directives["id"]), 0, b"").hex()},
                    {"mutation": "reclassified_count_too_large", "hex":
                     (reclassify_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": reclassify_ok[:-1].hex()},
                    {"mutation": "long", "hex": (reclassify_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": record_l4_approval["family"],
            "id": record_l4_approval["id"],
            "name": record_l4_approval["name"],
            "request": {
                "positive": approval_request.hex(),
                "memory_id": 42,
                "approver": approval_approver.decode("ascii"),
                "note": approval_note.decode("ascii"),
                "bare_positive": approval_bare_request.hex(),
                "target_tier": record_l4_approval["request"]["policy"]["target_tier"],
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(approval_request, 12, 1).hex()},
                    {"mutation": "memory_id_zero", "hex":
                     (approval_request[:ENVELOPE_HEADER_LEN] + _put_u64(0) +
                      approval_request[ENVELOPE_HEADER_LEN + 8:]).hex()},
                    {"mutation": "approver_empty", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(record_l4_approval["id"]), 0,
                               _put_u64(42) + _put_u32(0) + _put_u32(0)).hex()},
                    {"mutation": "approver_too_long", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(record_l4_approval["id"]), 0,
                               _put_u64(42) + _put_u32(64) + b"a" * 64 + _put_u32(0)).hex()},
                    {"mutation": "approver_length_overruns_payload", "hex":
                     mutate_u32(approval_request, ENVELOPE_HEADER_LEN + 8, 60).hex()},
                    {"mutation": "approver_embedded_nul", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(record_l4_approval["id"]), 0,
                               _put_u64(42) + _put_u32(8) + b"opera\0tor" [:8] +
                               _put_u32(0)).hex()},
                    {"mutation": "short", "hex": approval_request[:-1].hex()},
                    {"mutation": "long", "hex": (approval_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": approval_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(approval_ok, 8, 21).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(approval_ok, 12, 5).hex()},
                    {"mutation": "unexpected_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(record_l4_approval["id"]), 0,
                               _put_u32(0)).hex()},
                    {"mutation": "short", "hex": approval_ok[:-1].hex()},
                    {"mutation": "long", "hex": (approval_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": prune_orphaned_l0["family"],
            "id": prune_orphaned_l0["id"],
            "name": prune_orphaned_l0["name"],
            "request": {
                "positive": prune_orphaned_l0_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(prune_orphaned_l0_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(prune_orphaned_l0_request, 16, 1).hex()},
                    {"mutation": "short", "hex": prune_orphaned_l0_request[:-1].hex()},
                    {"mutation": "long", "hex": (prune_orphaned_l0_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "deleted_count": 3, "hex": prune_orphaned_l0_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(prune_orphaned_l0_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(prune_orphaned_l0_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(prune_orphaned_l0["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (prune_orphaned_l0_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": prune_orphaned_l0_ok[:-1].hex()},
                    {"mutation": "long", "hex": (prune_orphaned_l0_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": lifecycle_sweep_expired["family"],
            "id": lifecycle_sweep_expired["id"],
            "name": lifecycle_sweep_expired["name"],
            "request": {
                "positive": lifecycle_sweep_expired_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(lifecycle_sweep_expired_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(lifecycle_sweep_expired_request, 16, 1).hex()},
                    {"mutation": "short", "hex": lifecycle_sweep_expired_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (lifecycle_sweep_expired_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "archived_count": 4,
                     "hex": lifecycle_sweep_expired_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(lifecycle_sweep_expired_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(lifecycle_sweep_expired_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(lifecycle_sweep_expired["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (lifecycle_sweep_expired_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": lifecycle_sweep_expired_ok[:-1].hex()},
                    {"mutation": "long", "hex": (lifecycle_sweep_expired_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": demote_id["family"],
            "id": demote_id["id"],
            "name": demote_id["name"],
            "request": {
                "positive": demote_id_request.hex(),
                "memory_id": 42,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(demote_id_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(demote_id_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(demote_id["id"]), 0,
                               _put_u64(0)).hex()},
                    {"mutation": "memory_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(demote_id["id"]), 0,
                               _put_u64(0x8000000000000000)).hex()},
                    {"mutation": "short", "hex": demote_id_request[:-1].hex()},
                    {"mutation": "long", "hex": (demote_id_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "demoted_count": 1, "hex": demote_id_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(demote_id_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(demote_id_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(demote_id["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (demote_id_ok[:-4] + _put_u32(2)).hex()},
                    {"mutation": "short", "hex": demote_id_ok[:-1].hex()},
                    {"mutation": "long", "hex": (demote_id_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": has_workspace_tag["family"],
            "id": has_workspace_tag["id"],
            "name": has_workspace_tag["name"],
            "request": {
                "positive": has_workspace_tag_request.hex(),
                "memory_id": 42,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(has_workspace_tag_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(has_workspace_tag_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(has_workspace_tag["id"]), 0,
                               _put_u64(0)).hex()},
                    {"mutation": "memory_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(has_workspace_tag["id"]), 0,
                               _put_u64(0x8000000000000000)).hex()},
                    {"mutation": "short", "hex": has_workspace_tag_request[:-1].hex()},
                    {"mutation": "long", "hex": (has_workspace_tag_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "tagged": 1, "hex": has_workspace_tag_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(has_workspace_tag_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(has_workspace_tag_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(has_workspace_tag["id"]), 0, b"").hex()},
                    {"mutation": "flag_too_large", "hex":
                     (has_workspace_tag_ok[:-4] + _put_u32(2)).hex()},
                    {"mutation": "short", "hex": has_workspace_tag_ok[:-1].hex()},
                    {"mutation": "long", "hex": (has_workspace_tag_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": delete_row["family"],
            "id": delete_row["id"],
            "name": delete_row["name"],
            "request": {
                "positive": delete_row_request.hex(),
                "memory_id": 42,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(delete_row_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(delete_row_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(delete_row["id"]), 0,
                               _put_u64(0)).hex()},
                    {"mutation": "memory_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(delete_row["id"]), 0,
                               _put_u64(0x8000000000000000)).hex()},
                    {"mutation": "short", "hex": delete_row_request[:-1].hex()},
                    {"mutation": "long", "hex": (delete_row_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "deleted_rows": 1, "hex": delete_row_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(delete_row_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(delete_row_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(delete_row["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (delete_row_ok[:-4] + _put_u32(2)).hex()},
                    {"mutation": "short", "hex": delete_row_ok[:-1].hex()},
                    {"mutation": "long", "hex": (delete_row_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": touch["family"],
            "id": touch["id"],
            "name": touch["name"],
            "request": {
                "positive": touch_request.hex(),
                "memory_id": 42,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(touch_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(touch_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(touch["id"]), 0,
                               _put_u64(0)).hex()},
                    {"mutation": "memory_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(touch["id"]), 0,
                               _put_u64(0x8000000000000000)).hex()},
                    {"mutation": "short", "hex": touch_request[:-1].hex()},
                    {"mutation": "long", "hex": (touch_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": touch_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(touch_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(touch_ok, 12, 5).hex()},
                    {"mutation": "unexpected_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(touch["id"]), 0,
                               _put_u32(0)).hex()},
                    {"mutation": "short", "hex": touch_ok[:-1].hex()},
                    {"mutation": "long", "hex": (touch_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": link_delete["family"],
            "id": link_delete["id"],
            "name": link_delete["name"],
            "request": {
                "positive": link_delete_request.hex(),
                "link_id": 7,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(link_delete_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(link_delete_request, 16, 4).hex()},
                    {"mutation": "zero_link", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(link_delete["id"]), 0,
                               _put_u64(0)).hex()},
                    {"mutation": "link_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(link_delete["id"]), 0,
                               _put_u64(0x8000000000000000)).hex()},
                    {"mutation": "short", "hex": link_delete_request[:-1].hex()},
                    {"mutation": "long", "hex": (link_delete_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": link_delete_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(link_delete_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(link_delete_ok, 12, 5).hex()},
                    {"mutation": "unexpected_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(link_delete["id"]), 0,
                               _put_u32(0)).hex()},
                    {"mutation": "short", "hex": link_delete_ok[:-1].hex()},
                    {"mutation": "long", "hex": (link_delete_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": valid_at["family"],
            "id": valid_at["id"],
            "name": valid_at["name"],
            "request": {
                "positive": valid_at_request.hex(),
                "memory_id": 42,
                "as_of": valid_at_as_of.decode("ascii"),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(valid_at_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(valid_at_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(valid_at["id"]), 0,
                               _put_u64(0) + _put_u32(len(valid_at_as_of)) +
                               valid_at_as_of).hex()},
                    {"mutation": "empty_as_of", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(valid_at["id"]), 0,
                               _put_u64(42) + _put_u32(0)).hex()},
                    {"mutation": "as_of_length_mismatch", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(valid_at["id"]), 0,
                               _put_u64(42) + _put_u32(len(valid_at_as_of) + 1) +
                               valid_at_as_of).hex()},
                    {"mutation": "embedded_nul", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(valid_at["id"]), 0,
                               _put_u64(42) + _put_u32(len(valid_at_as_of)) +
                               valid_at_as_of[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": valid_at_request[:-1].hex()},
                    {"mutation": "long", "hex": (valid_at_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "in_force": 1, "hex": valid_at_ok.hex()},
                    {"result": 5, "in_force": 0, "hex": valid_at_unevaluated.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(valid_at_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(valid_at_ok, 12, 3).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(valid_at["id"]), 0, b"").hex()},
                    {"mutation": "unevaluated_with_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(valid_at["id"]), 5,
                               _put_u32(0)).hex()},
                    {"mutation": "verdict_too_large", "hex":
                     (valid_at_ok[:-4] + _put_u32(2)).hex()},
                    {"mutation": "short", "hex": valid_at_ok[:-1].hex()},
                    {"mutation": "long", "hex": (valid_at_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": has_scope_type["family"],
            "id": has_scope_type["id"],
            "name": has_scope_type["name"],
            "request": {
                "positive": has_scope_type_request.hex(),
                "memory_id": 42,
                "scope_type": has_scope_type_scope.decode("ascii"),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(has_scope_type_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(has_scope_type_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(has_scope_type["id"]), 0,
                               _put_u64(0) + _put_u32(len(has_scope_type_scope)) +
                               has_scope_type_scope).hex()},
                    {"mutation": "empty_scope", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(has_scope_type["id"]), 0,
                               _put_u64(42) + _put_u32(0)).hex()},
                    {"mutation": "scope_length_mismatch", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(has_scope_type["id"]), 0,
                               _put_u64(42) + _put_u32(len(has_scope_type_scope) + 1) +
                               has_scope_type_scope).hex()},
                    {"mutation": "embedded_nul", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(has_scope_type["id"]), 0,
                               _put_u64(42) + _put_u32(len(has_scope_type_scope)) +
                               has_scope_type_scope[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": has_scope_type_request[:-1].hex()},
                    {"mutation": "long", "hex": (has_scope_type_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "present": 1, "hex": has_scope_type_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(has_scope_type_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(has_scope_type_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(has_scope_type["id"]), 0, b"").hex()},
                    {"mutation": "flag_too_large", "hex":
                     (has_scope_type_ok[:-4] + _put_u32(2)).hex()},
                    {"mutation": "short", "hex": has_scope_type_ok[:-1].hex()},
                    {"mutation": "long", "hex": (has_scope_type_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": reject["family"],
            "id": reject["id"],
            "name": reject["name"],
            "request": {
                "positive": reject_request.hex(),
                "memory_id": 42,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(reject_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(reject_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(reject["id"]), 0,
                               _put_u64(0)).hex()},
                    {"mutation": "memory_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(reject["id"]), 0,
                               _put_u64(0x8000000000000000)).hex()},
                    {"mutation": "short", "hex": reject_request[:-1].hex()},
                    {"mutation": "long", "hex": (reject_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": reject_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(reject_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(reject_ok, 12, 5).hex()},
                    {"mutation": "unexpected_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(reject["id"]), 0,
                               _put_u32(0)).hex()},
                    {"mutation": "short", "hex": reject_ok[:-1].hex()},
                    {"mutation": "long", "hex": (reject_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": update_content["family"],
            "id": update_content["id"],
            "name": update_content["name"],
            "request": {
                "positive": update_content_request.hex(),
                "memory_id": 42,
                "content": update_content_text.decode("ascii"),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(update_content_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(update_content_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(update_content["id"]), 0,
                               _put_u64(0) + _put_u32(len(update_content_text)) +
                               update_content_text).hex()},
                    {"mutation": "empty_content", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(update_content["id"]), 0,
                               _put_u64(42) + _put_u32(0)).hex()},
                    {"mutation": "content_length_mismatch", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(update_content["id"]), 0,
                               _put_u64(42) + _put_u32(len(update_content_text) + 1) +
                               update_content_text).hex()},
                    {"mutation": "embedded_nul", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(update_content["id"]), 0,
                               _put_u64(42) + _put_u32(len(update_content_text)) +
                               update_content_text[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": update_content_request[:-1].hex()},
                    {"mutation": "long", "hex": (update_content_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "updated_rows": 1, "hex": update_content_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(update_content_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(update_content_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(update_content["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (update_content_ok[:-4] + _put_u32(2)).hex()},
                    {"mutation": "short", "hex": update_content_ok[:-1].hex()},
                    {"mutation": "long", "hex": (update_content_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": decay_confidence["family"],
            "id": decay_confidence["id"],
            "name": decay_confidence["name"],
            "request": {
                "positive": decay_confidence_request.hex(),
                "memory_id": 42,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(decay_confidence_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(decay_confidence_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(decay_confidence["id"]), 0,
                               _put_u64(0)).hex()},
                    {"mutation": "memory_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(decay_confidence["id"]), 0,
                               _put_u64(0x8000000000000000)).hex()},
                    {"mutation": "short", "hex": decay_confidence_request[:-1].hex()},
                    {"mutation": "long", "hex": (decay_confidence_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": decay_confidence_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(decay_confidence_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(decay_confidence_ok, 12, 5).hex()},
                    {"mutation": "unexpected_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(decay_confidence["id"]), 0,
                               _put_u32(0)).hex()},
                    {"mutation": "short", "hex": decay_confidence_ok[:-1].hex()},
                    {"mutation": "long", "hex": (decay_confidence_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": workspace_tag_insert["family"],
            "id": workspace_tag_insert["id"],
            "name": workspace_tag_insert["name"],
            "request": {
                "positive": workspace_tag_insert_request.hex(),
                "memory_id": 42,
                "workspace": workspace_tag_name.decode("ascii"),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(workspace_tag_insert_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(workspace_tag_insert_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(workspace_tag_insert["id"]), 0,
                               _put_u64(0) + _put_u32(len(workspace_tag_name)) +
                               workspace_tag_name).hex()},
                    {"mutation": "empty_workspace", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(workspace_tag_insert["id"]), 0,
                               _put_u64(42) + _put_u32(0)).hex()},
                    {"mutation": "workspace_length_mismatch", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(workspace_tag_insert["id"]), 0,
                               _put_u64(42) + _put_u32(len(workspace_tag_name) + 1) +
                               workspace_tag_name).hex()},
                    {"mutation": "embedded_nul", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(workspace_tag_insert["id"]), 0,
                               _put_u64(42) + _put_u32(len(workspace_tag_name)) +
                               workspace_tag_name[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": workspace_tag_insert_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (workspace_tag_insert_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": workspace_tag_insert_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(workspace_tag_insert_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(workspace_tag_insert_ok, 12, 5).hex()},
                    {"mutation": "unexpected_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(workspace_tag_insert["id"]), 0, _put_u32(0)).hex()},
                    {"mutation": "short", "hex": workspace_tag_insert_ok[:-1].hex()},
                    {"mutation": "long", "hex": (workspace_tag_insert_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": set_cognified_kind["family"],
            "id": set_cognified_kind["id"],
            "name": set_cognified_kind["name"],
            "request": {
                "positive": set_cognified_kind_request.hex(),
                "memory_id": 42,
                "kind": cognified_kind_value.decode("ascii"),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(set_cognified_kind_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(set_cognified_kind_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(set_cognified_kind["id"]), 0,
                               _put_u64(0) + _put_u32(len(cognified_kind_value)) +
                               cognified_kind_value).hex()},
                    {"mutation": "empty_kind", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(set_cognified_kind["id"]), 0,
                               _put_u64(42) + _put_u32(0)).hex()},
                    {"mutation": "kind_length_mismatch", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(set_cognified_kind["id"]), 0,
                               _put_u64(42) + _put_u32(len(cognified_kind_value) + 1) +
                               cognified_kind_value).hex()},
                    {"mutation": "embedded_nul", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(set_cognified_kind["id"]), 0,
                               _put_u64(42) + _put_u32(len(cognified_kind_value)) +
                               cognified_kind_value[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": set_cognified_kind_request[:-1].hex()},
                    {"mutation": "long", "hex": (set_cognified_kind_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": set_cognified_kind_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(set_cognified_kind_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(set_cognified_kind_ok, 12, 5).hex()},
                    {"mutation": "unexpected_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(set_cognified_kind["id"]), 0, _put_u32(0)).hex()},
                    {"mutation": "short", "hex": set_cognified_kind_ok[:-1].hex()},
                    {"mutation": "long", "hex": (set_cognified_kind_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": set_source_session["family"],
            "id": set_source_session["id"],
            "name": set_source_session["name"],
            "request": {
                "positive": set_source_session_request.hex(),
                "memory_id": 42,
                "session_id": source_session_value.decode("ascii"),
                "cleared": set_source_session_cleared.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(set_source_session_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(set_source_session_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(set_source_session["id"]), 0,
                               _put_u64(0) + _put_u32(len(source_session_value)) +
                               source_session_value).hex()},
                    {"mutation": "session_length_mismatch", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(set_source_session["id"]), 0,
                               _put_u64(42) + _put_u32(len(source_session_value) + 1) +
                               source_session_value).hex()},
                    {"mutation": "embedded_nul", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(set_source_session["id"]), 0,
                               _put_u64(42) + _put_u32(len(source_session_value)) +
                               source_session_value[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": set_source_session_request[:-1].hex()},
                    {"mutation": "long", "hex": (set_source_session_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": set_source_session_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(set_source_session_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(set_source_session_ok, 12, 5).hex()},
                    {"mutation": "unexpected_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(set_source_session["id"]), 0, _put_u32(0)).hex()},
                    {"mutation": "short", "hex": set_source_session_ok[:-1].hex()},
                    {"mutation": "long", "hex": (set_source_session_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": negation_tokens_update["family"],
            "id": negation_tokens_update["id"],
            "name": negation_tokens_update["name"],
            "request": {
                "positive": negation_tokens_update_request.hex(),
                "memory_id": 42,
                "tokens": negation_tokens_value.decode("ascii"),
                "cleared": negation_tokens_update_cleared.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(negation_tokens_update_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(negation_tokens_update_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(negation_tokens_update["id"]), 0,
                               _put_u64(0) + _put_u32(len(negation_tokens_value)) +
                               negation_tokens_value).hex()},
                    {"mutation": "tokens_length_mismatch", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(negation_tokens_update["id"]), 0,
                               _put_u64(42) + _put_u32(len(negation_tokens_value) + 1) +
                               negation_tokens_value).hex()},
                    {"mutation": "embedded_nul", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(negation_tokens_update["id"]), 0,
                               _put_u64(42) + _put_u32(len(negation_tokens_value)) +
                               negation_tokens_value[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": negation_tokens_update_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (negation_tokens_update_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "hex": negation_tokens_update_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(negation_tokens_update_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(negation_tokens_update_ok, 12, 5).hex()},
                    {"mutation": "unexpected_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(negation_tokens_update["id"]), 0, _put_u32(0)).hex()},
                    {"mutation": "short", "hex": negation_tokens_update_ok[:-1].hex()},
                    {"mutation": "long", "hex": (negation_tokens_update_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": get_content["family"],
            "id": get_content["id"],
            "name": get_content["name"],
            "request": {
                "positive": get_content_request.hex(),
                "memory_id": 42,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(get_content_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(get_content_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC, int(get_content["id"]), 0,
                               _put_u64(0)).hex()},
                    {"mutation": "short", "hex": get_content_request[:-1].hex()},
                    {"mutation": "long", "hex": (get_content_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "content": get_content_text.decode("ascii"),
                     "hex": get_content_ok.hex()},
                    {"result": 0, "content": "", "hex": get_content_empty.hex()},
                    {"result": 1, "content": "", "hex": get_content_missing.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(get_content_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(get_content_ok, 12, 3).hex()},
                    {"mutation": "missing_with_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(get_content["id"]), 1,
                               _put_u32(0)).hex()},
                    {"mutation": "ok_without_length", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(get_content["id"]), 0,
                               b"").hex()},
                    {"mutation": "content_length_mismatch", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(get_content["id"]), 0,
                               _put_u32(len(get_content_text) + 1) +
                               get_content_text).hex()},
                    {"mutation": "embedded_nul", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC, int(get_content["id"]), 0,
                               _put_u32(len(get_content_text)) +
                               get_content_text[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": get_content_ok[:-1].hex()},
                    {"mutation": "long", "hex": (get_content_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": get_source_session["family"],
            "id": get_source_session["id"],
            "name": get_source_session["name"],
            "request": {
                "positive": get_source_session_request.hex(),
                "memory_id": 42,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(get_source_session_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(get_source_session_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(get_source_session["id"]), 0, _put_u64(0)).hex()},
                    {"mutation": "short", "hex": get_source_session_request[:-1].hex()},
                    {"mutation": "long", "hex": (get_source_session_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "session_id": source_session_read.decode("ascii"),
                     "hex": get_source_session_ok.hex()},
                    {"result": 1, "session_id": "",
                     "hex": get_source_session_missing.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(get_source_session_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(get_source_session_ok, 12, 3).hex()},
                    {"mutation": "missing_with_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(get_source_session["id"]), 1, _put_u32(0)).hex()},
                    {"mutation": "empty_ok", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(get_source_session["id"]), 0, _put_u32(0)).hex()},
                    {"mutation": "session_length_mismatch", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(get_source_session["id"]), 0,
                               _put_u32(len(source_session_read) + 1) +
                               source_session_read).hex()},
                    {"mutation": "embedded_nul", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(get_source_session["id"]), 0,
                               _put_u32(len(source_session_read)) +
                               source_session_read[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": get_source_session_ok[:-1].hex()},
                    {"mutation": "long", "hex": (get_source_session_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": pick_first_temporal_ref["family"],
            "id": pick_first_temporal_ref["id"],
            "name": pick_first_temporal_ref["name"],
            "request": {
                "positive": pick_first_temporal_ref_request.hex(),
                "memory_id": 42,
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(pick_first_temporal_ref_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(pick_first_temporal_ref_request, 16, 4).hex()},
                    {"mutation": "zero_memory", "hex":
                     _envelope(catalog, ENVELOPE_REQUEST_MAGIC,
                               int(pick_first_temporal_ref["id"]), 0, _put_u64(0)).hex()},
                    {"mutation": "short", "hex": pick_first_temporal_ref_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (pick_first_temporal_ref_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "ref_key": temporal_ref_key.decode("ascii"),
                     "hex": pick_first_temporal_ref_ok.hex()},
                    {"result": 1, "ref_key": "",
                     "hex": pick_first_temporal_ref_missing.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(pick_first_temporal_ref_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(pick_first_temporal_ref_ok, 12, 3).hex()},
                    {"mutation": "missing_with_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(pick_first_temporal_ref["id"]), 1, _put_u32(0)).hex()},
                    {"mutation": "empty_ok", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(pick_first_temporal_ref["id"]), 0, _put_u32(0)).hex()},
                    {"mutation": "key_length_mismatch", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(pick_first_temporal_ref["id"]), 0,
                               _put_u32(len(temporal_ref_key) + 1) +
                               temporal_ref_key).hex()},
                    {"mutation": "embedded_nul", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(pick_first_temporal_ref["id"]), 0,
                               _put_u32(len(temporal_ref_key)) +
                               temporal_ref_key[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": pick_first_temporal_ref_ok[:-1].hex()},
                    {"mutation": "long", "hex":
                     (pick_first_temporal_ref_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": count_and_max_updated["family"],
            "id": count_and_max_updated["id"],
            "name": count_and_max_updated["name"],
            "request": {
                "positive": count_and_max_updated_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(count_and_max_updated_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(count_and_max_updated_request, 16, 1).hex()},
                    {"mutation": "short", "hex": count_and_max_updated_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (count_and_max_updated_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "count": 7,
                     "max_updated_at": corpus_stamp.decode("ascii"),
                     "hex": count_and_max_updated_ok.hex()},
                    {"result": 0, "count": 0, "max_updated_at": "",
                     "hex": count_and_max_updated_empty.hex()},
                    {"result": 5, "count": 0, "max_updated_at": "",
                     "hex": count_and_max_updated_unavailable.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(count_and_max_updated_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(count_and_max_updated_ok, 12, 3).hex()},
                    {"mutation": "unavailable_with_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(count_and_max_updated["id"]), 5,
                               _put_u32(0) + _put_u32(0)).hex()},
                    {"mutation": "ok_without_fields", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(count_and_max_updated["id"]), 0, b"").hex()},
                    {"mutation": "stamp_length_mismatch", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(count_and_max_updated["id"]), 0,
                               _put_u32(7) + _put_u32(len(corpus_stamp) + 1) +
                               corpus_stamp).hex()},
                    {"mutation": "count_too_large", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(count_and_max_updated["id"]), 0,
                               _put_u32(0x80000000) + _put_u32(0)).hex()},
                    {"mutation": "embedded_nul", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(count_and_max_updated["id"]), 0,
                               _put_u32(7) + _put_u32(len(corpus_stamp)) +
                               corpus_stamp[:-1] + b"\0").hex()},
                    {"mutation": "short", "hex": count_and_max_updated_ok[:-1].hex()},
                    {"mutation": "long", "hex": (count_and_max_updated_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": entity_edge_prune_orphans["family"],
            "id": entity_edge_prune_orphans["id"],
            "name": entity_edge_prune_orphans["name"],
            "request": {
                "positive": entity_edge_prune_orphans_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(entity_edge_prune_orphans_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(entity_edge_prune_orphans_request, 16, 1).hex()},
                    {"mutation": "short", "hex":
                     entity_edge_prune_orphans_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (entity_edge_prune_orphans_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "pruned_count": 2,
                     "hex": entity_edge_prune_orphans_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(entity_edge_prune_orphans_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(entity_edge_prune_orphans_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(entity_edge_prune_orphans["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (entity_edge_prune_orphans_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": entity_edge_prune_orphans_ok[:-1].hex()},
                    {"mutation": "long", "hex":
                     (entity_edge_prune_orphans_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": entity_edge_normalize_weights["family"],
            "id": entity_edge_normalize_weights["id"],
            "name": entity_edge_normalize_weights["name"],
            "request": {
                "positive": entity_edge_normalize_weights_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(entity_edge_normalize_weights_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(entity_edge_normalize_weights_request, 16, 1).hex()},
                    {"mutation": "short", "hex":
                     entity_edge_normalize_weights_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (entity_edge_normalize_weights_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "normalized_count": 3,
                     "hex": entity_edge_normalize_weights_ok.hex()},
                    {"result": 0, "normalized_count": 0,
                     "hex": entity_edge_normalize_weights_converged.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(entity_edge_normalize_weights_ok, 8, 3).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(entity_edge_normalize_weights_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(entity_edge_normalize_weights["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (entity_edge_normalize_weights_ok[:-4] +
                      _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex":
                     entity_edge_normalize_weights_ok[:-1].hex()},
                    {"mutation": "long", "hex":
                     (entity_edge_normalize_weights_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": project_count["family"],
            "id": project_count["id"],
            "name": project_count["name"],
            "request": {
                "positive": project_count_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(project_count_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(project_count_request, 16, 1).hex()},
                    {"mutation": "short", "hex": project_count_request[:-1].hex()},
                    {"mutation": "long", "hex": (project_count_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "project_count": 4, "hex": project_count_ok.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(project_count_ok, 8, 9).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(project_count_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(project_count["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (project_count_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": project_count_ok[:-1].hex()},
                    {"mutation": "long", "hex": (project_count_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": purge_hidden_pollution["family"],
            "id": purge_hidden_pollution["id"],
            "name": purge_hidden_pollution["name"],
            "request": {
                "positive": purge_hidden_pollution_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(purge_hidden_pollution_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(purge_hidden_pollution_request, 16, 1).hex()},
                    {"mutation": "short", "hex": purge_hidden_pollution_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (purge_hidden_pollution_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "purged_count": 5, "hex": purge_hidden_pollution_ok.hex()},
                    {"result": 0, "purged_count": 0,
                     "hex": purge_hidden_pollution_clean.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(purge_hidden_pollution_ok, 8, 9).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(purge_hidden_pollution_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(purge_hidden_pollution["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (purge_hidden_pollution_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": purge_hidden_pollution_ok[:-1].hex()},
                    {"mutation": "long", "hex": (purge_hidden_pollution_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": requeue_drifted["family"],
            "id": requeue_drifted["id"],
            "name": requeue_drifted["name"],
            "request": {
                "positive": requeue_drifted_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(requeue_drifted_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(requeue_drifted_request, 16, 1).hex()},
                    {"mutation": "short", "hex": requeue_drifted_request[:-1].hex()},
                    {"mutation": "long", "hex": (requeue_drifted_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "requeued_count": 6, "hex": requeue_drifted_ok.hex()},
                    {"result": 0, "requeued_count": 0, "hex": requeue_drifted_none.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(requeue_drifted_ok, 8, 9).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(requeue_drifted_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(requeue_drifted["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (requeue_drifted_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": requeue_drifted_ok[:-1].hex()},
                    {"mutation": "long", "hex": (requeue_drifted_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": prospective_sweep_expired["family"],
            "id": prospective_sweep_expired["id"],
            "name": prospective_sweep_expired["name"],
            "request": {
                "positive": prospective_sweep_expired_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(prospective_sweep_expired_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(prospective_sweep_expired_request, 16, 1).hex()},
                    {"mutation": "short", "hex":
                     prospective_sweep_expired_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (prospective_sweep_expired_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "expired_count": 7,
                     "hex": prospective_sweep_expired_ok.hex()},
                    {"result": 0, "expired_count": 0,
                     "hex": prospective_sweep_expired_none.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(prospective_sweep_expired_ok, 8, 9).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(prospective_sweep_expired_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(prospective_sweep_expired["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (prospective_sweep_expired_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex":
                     prospective_sweep_expired_ok[:-1].hex()},
                    {"mutation": "long", "hex":
                     (prospective_sweep_expired_ok + b"\0").hex()},
                ],
            },
        }, {
            "family": directive_sweep_expired["family"],
            "id": directive_sweep_expired["id"],
            "name": directive_sweep_expired["name"],
            "request": {
                "positive": directive_sweep_expired_request.hex(),
                "negative": [
                    {"mutation": "bad_flags", "hex":
                     mutate_u32(directive_sweep_expired_request, 12, 1).hex()},
                    {"mutation": "payload_length", "hex":
                     mutate_u32(directive_sweep_expired_request, 16, 1).hex()},
                    {"mutation": "short", "hex": directive_sweep_expired_request[:-1].hex()},
                    {"mutation": "long", "hex":
                     (directive_sweep_expired_request + b"\0").hex()},
                ],
            },
            "reply": {
                "positive": [
                    {"result": 0, "directives_expired": 8,
                     "hex": directive_sweep_expired_ok.hex()},
                    {"result": 0, "directives_expired": 0,
                     "hex": directive_sweep_expired_none.hex()},
                ],
                "negative": [
                    {"mutation": "wrong_operation", "hex":
                     mutate_u32(directive_sweep_expired_ok, 8, 9).hex()},
                    {"mutation": "unsupported_result", "hex":
                     mutate_u32(directive_sweep_expired_ok, 12, 5).hex()},
                    {"mutation": "ok_without_payload", "hex":
                     _envelope(catalog, ENVELOPE_REPLY_MAGIC,
                               int(directive_sweep_expired["id"]), 0, b"").hex()},
                    {"mutation": "count_too_large", "hex":
                     (directive_sweep_expired_ok[:-4] + _put_u32(0x80000000)).hex()},
                    {"mutation": "short", "hex": directive_sweep_expired_ok[:-1].hex()},
                    {"mutation": "long", "hex":
                     (directive_sweep_expired_ok + b"\0").hex()},
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
    reembed_clear_maintenance = catalog["operations"][7]
    embedder_serving_id = catalog["operations"][8]
    dimension_reset = catalog["operations"][9]
    level3_count = catalog["operations"][10]
    level2_count = catalog["operations"][11]
    orphaned_l0_count = catalog["operations"][12]
    total_count = catalog["operations"][13]
    session_l2_count = catalog["operations"][14]
    key_exists = catalog["operations"][15]
    find_id_by_key_kind = catalog["operations"][16]
    key_exists_in_tier_pair = catalog["operations"][17]
    effectiveness_update = catalog["operations"][18]
    retention_enforce = catalog["operations"][19]
    effectiveness_demote = catalog["operations"][20]
    effectiveness_stats = catalog["operations"][21]
    l2_memory_ids = catalog["operations"][22]
    health_record = catalog["operations"][23]
    health_retention = catalog["operations"][24]
    health_counters = catalog["operations"][25]
    stats_counts = catalog["operations"][26]
    expire = catalog["operations"][27]
    demote = catalog["operations"][28]
    promote_stable = catalog["operations"][29]
    reclassify_directives = catalog["operations"][30]
    record_l4_approval = catalog["operations"][31]
    prune_orphaned_l0 = catalog["operations"][32]
    lifecycle_sweep_expired = catalog["operations"][33]
    demote_id = catalog["operations"][34]
    has_workspace_tag = catalog["operations"][35]
    delete_row = catalog["operations"][36]
    touch = catalog["operations"][37]
    link_delete = catalog["operations"][38]
    valid_at = catalog["operations"][39]
    has_scope_type = catalog["operations"][40]
    reject = catalog["operations"][41]
    update_content = catalog["operations"][42]
    decay_confidence = catalog["operations"][43]
    workspace_tag_insert = catalog["operations"][44]
    set_cognified_kind = catalog["operations"][45]
    set_source_session = catalog["operations"][46]
    negation_tokens_update = catalog["operations"][47]
    get_content = catalog["operations"][48]
    get_source_session = catalog["operations"][49]
    pick_first_temporal_ref = catalog["operations"][50]
    count_and_max_updated = catalog["operations"][51]
    entity_edge_prune_orphans = catalog["operations"][52]
    entity_edge_normalize_weights = catalog["operations"][53]
    project_count = catalog["operations"][54]
    purge_hidden_pollution = catalog["operations"][55]
    requeue_drifted = catalog["operations"][56]
    prospective_sweep_expired = catalog["operations"][57]
    directive_sweep_expired = catalog["operations"][58]
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
        ("AIMEE_DB2_EVENT_REEMBED_MAINT_CLEAR", "AIMEE_DB2_EVENT_LIFECYCLE"),
        ("AIMEE_DB2_STAGE_REEMBED_MAINT_CLEAR", "AIMEE_DB2_FAMILY_LIFECYCLE"),
        ("AIMEE_DB2_OPERATION_REEMBED_MAINT_CLEAR",
         f"{reembed_clear_maintenance['id']}u"),
        ("AIMEE_DB2_REEMBED_MAINT_CLEAR_REQUEST_LEN",
         f"{reembed_clear_maintenance['request']['encoded_size']}u"),
        ("AIMEE_DB2_REEMBED_MAINT_CLEAR_RESPONSE_LEN",
         f"{reembed_clear_maintenance['reply']['encoded_size_payload']}u"),
        ("AIMEE_DB2_REEMBED_MAINT_CLEAR_ERROR_LEN",
         f"{reembed_clear_maintenance['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_EVENT_EMBEDDER_SERVING_ID", "AIMEE_DB2_EVENT_LIFECYCLE"),
        ("AIMEE_DB2_STAGE_EMBEDDER_SERVING_ID", "AIMEE_DB2_FAMILY_LIFECYCLE"),
        ("AIMEE_DB2_OPERATION_EMBEDDER_SERVING_ID", f"{embedder_serving_id['id']}u"),
        ("AIMEE_DB2_EMBEDDER_SERVING_ID_REQUEST_LEN",
         f"{embedder_serving_id['request']['encoded_size']}u"),
        ("AIMEE_DB2_EMBEDDER_SERVING_ID_RESPONSE_MIN_LEN",
         f"{embedder_serving_id['reply']['encoded_size_min_ok']}u"),
        ("AIMEE_DB2_EMBEDDER_SERVING_ID_RESPONSE_MAX_LEN",
         f"{embedder_serving_id['reply']['encoded_size_max_ok']}u"),
        ("AIMEE_DB2_EMBEDDER_SERVING_ID_ERROR_LEN",
         f"{embedder_serving_id['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_EMBEDDER_SERVING_ID_MAX",
         f"{embedder_serving_id['reply']['field']['maximum_bytes']}u"),
        ("AIMEE_DB2_EVENT_DIMENSION_RESET", "AIMEE_DB2_EVENT_LIFECYCLE"),
        ("AIMEE_DB2_STAGE_DIMENSION_RESET", "AIMEE_DB2_FAMILY_LIFECYCLE"),
        ("AIMEE_DB2_OPERATION_DIMENSION_RESET", f"{dimension_reset['id']}u"),
        ("AIMEE_DB2_DIMENSION_RESET_REQUEST_LEN",
         f"{dimension_reset['request']['encoded_size']}u"),
        ("AIMEE_DB2_DIMENSION_RESET_RESPONSE_LEN",
         f"{dimension_reset['reply']['encoded_size_payload']}u"),
        ("AIMEE_DB2_DIMENSION_RESET_ERROR_LEN",
         f"{dimension_reset['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_DIMENSION_RESET_TABLES_MAX", "16u"),
        ("AIMEE_DB2_EVENT_LEVEL3_COUNT", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_LEVEL3_COUNT", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_LEVEL3_COUNT", f"{level3_count['id']}u"),
        ("AIMEE_DB2_LEVEL3_COUNT_REQUEST_LEN",
         f"{level3_count['request']['encoded_size']}u"),
        ("AIMEE_DB2_LEVEL3_COUNT_RESPONSE_LEN",
         f"{level3_count['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_LEVEL3_COUNT_ERROR_LEN",
         f"{level3_count['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_LEVEL3_COUNT_MAX",
         f"{level3_count['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_LEVEL2_COUNT", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_LEVEL2_COUNT", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_LEVEL2_COUNT", f"{level2_count['id']}u"),
        ("AIMEE_DB2_LEVEL2_COUNT_REQUEST_LEN",
         f"{level2_count['request']['encoded_size']}u"),
        ("AIMEE_DB2_LEVEL2_COUNT_RESPONSE_LEN",
         f"{level2_count['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_LEVEL2_COUNT_ERROR_LEN",
         f"{level2_count['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_LEVEL2_COUNT_MAX",
         f"{level2_count['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_ORPHANED_L0_COUNT", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_ORPHANED_L0_COUNT", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_ORPHANED_L0_COUNT", f"{orphaned_l0_count['id']}u"),
        ("AIMEE_DB2_ORPHANED_L0_COUNT_REQUEST_LEN",
         f"{orphaned_l0_count['request']['encoded_size']}u"),
        ("AIMEE_DB2_ORPHANED_L0_COUNT_RESPONSE_LEN",
         f"{orphaned_l0_count['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_ORPHANED_L0_COUNT_ERROR_LEN",
         f"{orphaned_l0_count['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_ORPHANED_L0_COUNT_MAX",
         f"{orphaned_l0_count['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_TOTAL_COUNT", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_TOTAL_COUNT", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_TOTAL_COUNT", f"{total_count['id']}u"),
        ("AIMEE_DB2_TOTAL_COUNT_REQUEST_LEN",
         f"{total_count['request']['encoded_size']}u"),
        ("AIMEE_DB2_TOTAL_COUNT_RESPONSE_LEN",
         f"{total_count['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_TOTAL_COUNT_ERROR_LEN",
         f"{total_count['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_TOTAL_COUNT_MAX",
         f"{total_count['reply']['field']['maximum']}ull"),
        ("AIMEE_DB2_EVENT_SESSION_L2_COUNT", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_SESSION_L2_COUNT", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_SESSION_L2_COUNT", f"{session_l2_count['id']}u"),
        ("AIMEE_DB2_SESSION_L2_COUNT_REQUEST_MIN_LEN",
         f"{session_l2_count['request']['encoded_size_min']}u"),
        ("AIMEE_DB2_SESSION_L2_COUNT_REQUEST_MAX_LEN",
         f"{session_l2_count['request']['encoded_size_max']}u"),
        ("AIMEE_DB2_SESSION_L2_COUNT_RESPONSE_LEN",
         f"{session_l2_count['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_SESSION_L2_COUNT_ERROR_LEN",
         f"{session_l2_count['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_SESSION_L2_COUNT_SESSION_MAX",
         f"{session_l2_count['request']['field']['maximum_bytes']}u"),
        ("AIMEE_DB2_SESSION_L2_COUNT_MAX",
         f"{session_l2_count['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_KEY_EXISTS", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_KEY_EXISTS", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_KEY_EXISTS", f"{key_exists['id']}u"),
        ("AIMEE_DB2_KEY_EXISTS_REQUEST_MIN_LEN",
         f"{key_exists['request']['encoded_size_min']}u"),
        ("AIMEE_DB2_KEY_EXISTS_REQUEST_MAX_LEN",
         f"{key_exists['request']['encoded_size_max']}u"),
        ("AIMEE_DB2_KEY_EXISTS_RESPONSE_LEN",
         f"{key_exists['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_KEY_EXISTS_ERROR_LEN",
         f"{key_exists['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_KEY_EXISTS_KEY_MAX",
         f"{key_exists['request']['field']['maximum_bytes']}u"),
        ("AIMEE_DB2_KEY_EXISTS_MAX",
         f"{key_exists['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_FIND_ID_BY_KEY_KIND", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_FIND_ID_BY_KEY_KIND", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_FIND_ID_BY_KEY_KIND", f"{find_id_by_key_kind['id']}u"),
        ("AIMEE_DB2_FIND_ID_BY_KEY_KIND_REQUEST_MIN_LEN",
         f"{find_id_by_key_kind['request']['encoded_size_min']}u"),
        ("AIMEE_DB2_FIND_ID_BY_KEY_KIND_REQUEST_MAX_LEN",
         f"{find_id_by_key_kind['request']['encoded_size_max']}u"),
        ("AIMEE_DB2_FIND_ID_BY_KEY_KIND_RESPONSE_LEN",
         f"{find_id_by_key_kind['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_FIND_ID_BY_KEY_KIND_ERROR_LEN",
         f"{find_id_by_key_kind['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_FIND_ID_BY_KEY_KIND_KEY_MAX",
         f"{find_id_by_key_kind['request']['fields'][0]['maximum_bytes']}u"),
        ("AIMEE_DB2_FIND_ID_BY_KEY_KIND_KIND_MAX",
         f"{find_id_by_key_kind['request']['fields'][1]['maximum_bytes']}u"),
        ("AIMEE_DB2_FIND_ID_BY_KEY_KIND_FOUND_MAX",
         f"{find_id_by_key_kind['reply']['fields'][0]['maximum']}u"),
        ("AIMEE_DB2_FIND_ID_BY_KEY_KIND_ID_MAX",
         f"{find_id_by_key_kind['reply']['fields'][1]['maximum']}ull"),
        ("AIMEE_DB2_EVENT_KEY_EXISTS_IN_TIER_PAIR", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_KEY_EXISTS_IN_TIER_PAIR", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_KEY_EXISTS_IN_TIER_PAIR",
         f"{key_exists_in_tier_pair['id']}u"),
        ("AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_REQUEST_MIN_LEN",
         f"{key_exists_in_tier_pair['request']['encoded_size_min']}u"),
        ("AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_REQUEST_MAX_LEN",
         f"{key_exists_in_tier_pair['request']['encoded_size_max']}u"),
        ("AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_RESPONSE_LEN",
         f"{key_exists_in_tier_pair['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_ERROR_LEN",
         f"{key_exists_in_tier_pair['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_KEY_MAX",
         f"{key_exists_in_tier_pair['request']['fields'][0]['maximum_bytes']}u"),
        ("AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_A_MAX",
         f"{key_exists_in_tier_pair['request']['fields'][1]['maximum_bytes']}u"),
        ("AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_B_MAX",
         f"{key_exists_in_tier_pair['request']['fields'][2]['maximum_bytes']}u"),
        ("AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_MAX",
         f"{key_exists_in_tier_pair['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_EFFECTIVENESS_UPDATE", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_EFFECTIVENESS_UPDATE", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_EFFECTIVENESS_UPDATE", f"{effectiveness_update['id']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_UPDATE_REQUEST_LEN",
         f"{effectiveness_update['request']['encoded_size']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_UPDATE_RESPONSE_LEN",
         f"{effectiveness_update['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_UPDATE_ERROR_LEN",
         f"{effectiveness_update['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_UPDATE_MEMORY_ID_MAX",
         f"{effectiveness_update['request']['fields'][0]['maximum']}ull"),
        ("AIMEE_DB2_EFFECTIVENESS_UPDATE_HAS_VALUE_MAX",
         f"{effectiveness_update['request']['fields'][1]['maximum']}u"),
        ("AIMEE_DB2_EVENT_RETENTION_ENFORCE", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_RETENTION_ENFORCE", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_RETENTION_ENFORCE", f"{retention_enforce['id']}u"),
        ("AIMEE_DB2_RETENTION_ENFORCE_REQUEST_LEN",
         f"{retention_enforce['request']['encoded_size']}u"),
        ("AIMEE_DB2_RETENTION_ENFORCE_RESPONSE_LEN",
         f"{retention_enforce['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_RETENTION_ENFORCE_ERROR_LEN",
         f"{retention_enforce['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_RETENTION_RESTRICTED", '"restricted"'),
        ("AIMEE_DB2_RETENTION_RESTRICTED_DAYS",
         f"{retention_enforce['request']['policy'][0]['retention_days']}u"),
        ("AIMEE_DB2_RETENTION_SENSITIVE", '"sensitive"'),
        ("AIMEE_DB2_RETENTION_SENSITIVE_DAYS",
         f"{retention_enforce['request']['policy'][1]['retention_days']}u"),
        ("AIMEE_DB2_RETENTION_ENFORCE_MAX",
         f"{retention_enforce['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_EFFECTIVENESS_DEMOTE", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_EFFECTIVENESS_DEMOTE", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_EFFECTIVENESS_DEMOTE", f"{effectiveness_demote['id']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_DEMOTE_REQUEST_LEN",
         f"{effectiveness_demote['request']['encoded_size']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_DEMOTE_RESPONSE_LEN",
         f"{effectiveness_demote['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_DEMOTE_ERROR_LEN",
         f"{effectiveness_demote['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_DEMOTE_THRESHOLD", "0.3"),
        ("AIMEE_DB2_EFFECTIVENESS_DEMOTE_MAX",
         f"{effectiveness_demote['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_EFFECTIVENESS_STATS", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_EFFECTIVENESS_STATS", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_EFFECTIVENESS_STATS", f"{effectiveness_stats['id']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_STATS_REQUEST_LEN",
         f"{effectiveness_stats['request']['encoded_size']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_STATS_RESPONSE_LEN",
         f"{effectiveness_stats['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_STATS_ERROR_LEN",
         f"{effectiveness_stats['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_STATS_LOW_THRESHOLD", "0.3"),
        ("AIMEE_DB2_EFFECTIVENESS_STATS_AVG_MIN",
         _binary64_literal(effectiveness_stats["reply"]["fields"][0]["minimum_binary64_bits"])),
        ("AIMEE_DB2_EFFECTIVENESS_STATS_AVG_MAX",
         _binary64_literal(effectiveness_stats["reply"]["fields"][0]["maximum_binary64_bits"])),
        ("AIMEE_DB2_EFFECTIVENESS_STATS_LOW_MAX",
         f"{effectiveness_stats['reply']['fields'][1]['maximum']}u"),
        ("AIMEE_DB2_EFFECTIVENESS_STATS_HIGH_MAX",
         f"{effectiveness_stats['reply']['fields'][2]['maximum']}u"),
        ("AIMEE_DB2_EVENT_L2_MEMORY_IDS", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_L2_MEMORY_IDS", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_L2_MEMORY_IDS", f"{l2_memory_ids['id']}u"),
        ("AIMEE_DB2_L2_MEMORY_IDS_REQUEST_LEN",
         f"{l2_memory_ids['request']['encoded_size']}u"),
        ("AIMEE_DB2_L2_MEMORY_IDS_RESPONSE_MIN_LEN",
         f"{l2_memory_ids['reply']['encoded_size_min_ok']}u"),
        ("AIMEE_DB2_L2_MEMORY_IDS_RESPONSE_MAX_LEN",
         f"{l2_memory_ids['reply']['encoded_size_max_ok']}u"),
        ("AIMEE_DB2_L2_MEMORY_IDS_ERROR_LEN",
         f"{l2_memory_ids['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_L2_MEMORY_IDS_MAX",
         f"{l2_memory_ids['reply']['field']['maximum_items']}u"),
        ("AIMEE_DB2_L2_MEMORY_ID_MIN",
         f"{l2_memory_ids['reply']['field']['item_minimum']}u"),
        ("AIMEE_DB2_L2_MEMORY_ID_MAX",
         f"{l2_memory_ids['reply']['field']['item_maximum']}ull"),
        ("AIMEE_DB2_EVENT_HEALTH_RECORD", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_HEALTH_RECORD", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_HEALTH_RECORD", f"{health_record['id']}u"),
        ("AIMEE_DB2_HEALTH_RECORD_REQUEST_LEN",
         f"{health_record['request']['encoded_size']}u"),
        ("AIMEE_DB2_HEALTH_RECORD_RESPONSE_LEN",
         f"{health_record['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_HEALTH_RECORD_ERROR_LEN",
         f"{health_record['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_HEALTH_RECORD_CONFLICT_WINDOW_DAYS",
         f"{health_record['request']['policy']['conflict_window_days']}"),
        ("AIMEE_DB2_HEALTH_RECORD_COUNTER_MAX",
         f"{health_record['request']['fields'][0]['maximum']}u"),
        ("AIMEE_DB2_EVENT_HEALTH_RETENTION", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_HEALTH_RETENTION", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_HEALTH_RETENTION", f"{health_retention['id']}u"),
        ("AIMEE_DB2_HEALTH_RETENTION_REQUEST_LEN",
         f"{health_retention['request']['encoded_size']}u"),
        ("AIMEE_DB2_HEALTH_RETENTION_RESPONSE_LEN",
         f"{health_retention['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_HEALTH_RETENTION_ERROR_LEN",
         f"{health_retention['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_HEALTH_RETENTION_SNAPSHOT_DAYS",
         f"{health_retention['request']['policy']['snapshot_retention_days']}"),
        ("AIMEE_DB2_HEALTH_RETENTION_CONTRADICTION_DAYS",
         f"{health_retention['request']['policy']['contradiction_retention_days']}"),
        ("AIMEE_DB2_HEALTH_RETENTION_MAX",
         f"{health_retention['reply']['fields'][0]['maximum']}u"),
        ("AIMEE_DB2_EVENT_HEALTH_COUNTERS", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_HEALTH_COUNTERS", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_HEALTH_COUNTERS", f"{health_counters['id']}u"),
        ("AIMEE_DB2_HEALTH_COUNTERS_REQUEST_LEN",
         f"{health_counters['request']['encoded_size']}u"),
        ("AIMEE_DB2_HEALTH_COUNTERS_RESPONSE_LEN",
         f"{health_counters['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_HEALTH_COUNTERS_ERROR_LEN",
         f"{health_counters['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_HEALTH_COUNTERS_PROMOTE_USE_COUNT",
         f"{health_counters['request']['policy']['promote_use_count']}"),
        ("AIMEE_DB2_HEALTH_COUNTERS_PROMOTE_CONFIDENCE",
         _binary64_literal(
             health_counters["request"]["policy"]["promote_confidence_binary64_bits"])),
        ("AIMEE_DB2_HEALTH_COUNTERS_FIELDS", f"{len(HEALTH_COUNTERS)}u"),
        ("AIMEE_DB2_HEALTH_COUNTERS_MAX",
         f"{health_counters['reply']['fields'][0]['maximum']}u"),
        ("AIMEE_DB2_EVENT_STATS_COUNTS", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_STATS_COUNTS", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_STATS_COUNTS", f"{stats_counts['id']}u"),
        ("AIMEE_DB2_STATS_COUNTS_REQUEST_LEN",
         f"{stats_counts['request']['encoded_size']}u"),
        ("AIMEE_DB2_STATS_COUNTS_RESPONSE_LEN",
         f"{stats_counts['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_STATS_COUNTS_ERROR_LEN",
         f"{stats_counts['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_STATS_COUNTS_TIERS", f"{len(MEMORY_TIERS)}u"),
        ("AIMEE_DB2_STATS_COUNTS_KINDS", f"{len(MEMORY_KINDS)}u"),
        ("AIMEE_DB2_STATS_COUNTS_MAX",
         f"{stats_counts['reply']['fields'][2]['maximum']}u"),
        ("AIMEE_DB2_EVENT_EXPIRE", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_EXPIRE", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_EXPIRE", f"{expire['id']}u"),
        ("AIMEE_DB2_EXPIRE_REQUEST_LEN", f"{expire['request']['encoded_size']}u"),
        ("AIMEE_DB2_EXPIRE_RESPONSE_LEN", f"{expire['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_EXPIRE_ERROR_LEN", f"{expire['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_EXPIRE_STALE_TIER", f"\"{expire['request']['policy']['stale_l1_tier']}\""),
        ("AIMEE_DB2_EXPIRE_KINDS_MAX", f"{expire['request']['policy']['maximum_kinds']}u"),
        ("AIMEE_DB2_EXPIRE_MAX", f"{expire['reply']['fields'][0]['maximum']}u"),
        ("AIMEE_DB2_EVENT_DEMOTE", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_DEMOTE", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_DEMOTE", f"{demote['id']}u"),
        ("AIMEE_DB2_DEMOTE_REQUEST_LEN", f"{demote['request']['encoded_size']}u"),
        ("AIMEE_DB2_DEMOTE_RESPONSE_LEN", f"{demote['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_DEMOTE_ERROR_LEN", f"{demote['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_DEMOTE_TIER", f"\"{demote['request']['policy']['demote_tier']}\""),
        ("AIMEE_DB2_DEMOTE_KINDS_MAX", f"{demote['request']['policy']['maximum_kinds']}u"),
        ("AIMEE_DB2_DEMOTE_MAX", f"{demote['reply']['fields'][0]['maximum']}u"),
        ("AIMEE_DB2_EVENT_PROMOTE_STABLE", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_PROMOTE_STABLE", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_PROMOTE_STABLE", f"{promote_stable['id']}u"),
        ("AIMEE_DB2_PROMOTE_STABLE_REQUEST_LEN",
         f"{promote_stable['request']['encoded_size']}u"),
        ("AIMEE_DB2_PROMOTE_STABLE_RESPONSE_LEN",
         f"{promote_stable['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_PROMOTE_STABLE_ERROR_LEN",
         f"{promote_stable['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_PROMOTE_STABLE_SOURCE_TIER",
         f"\"{promote_stable['request']['policy']['source_tier']}\""),
        ("AIMEE_DB2_PROMOTE_STABLE_TARGET_TIER",
         f"\"{promote_stable['request']['policy']['target_tier']}\""),
        ("AIMEE_DB2_PROMOTE_STABLE_CONFIDENCE",
         _binary64_literal(
             promote_stable["request"]["policy"]["minimum_confidence_binary64_bits"])),
        ("AIMEE_DB2_PROMOTE_STABLE_USE_COUNT",
         f"{promote_stable['request']['policy']['minimum_use_count']}u"),
        ("AIMEE_DB2_PROMOTE_STABLE_DAYS",
         f"{promote_stable['request']['policy']['stable_days']}u"),
        ("AIMEE_DB2_PROMOTE_STABLE_MAX",
         f"{promote_stable['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_RECLASSIFY_DIRECTIVES", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_RECLASSIFY_DIRECTIVES", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_RECLASSIFY_DIRECTIVES", f"{reclassify_directives['id']}u"),
        ("AIMEE_DB2_RECLASSIFY_DIRECTIVES_REQUEST_LEN",
         f"{reclassify_directives['request']['encoded_size']}u"),
        ("AIMEE_DB2_RECLASSIFY_DIRECTIVES_RESPONSE_LEN",
         f"{reclassify_directives['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_RECLASSIFY_DIRECTIVES_ERROR_LEN",
         f"{reclassify_directives['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_RECLASSIFY_DIRECTIVES_SOURCE_TIER",
         f"\"{reclassify_directives['request']['policy']['source_tier']}\""),
        ("AIMEE_DB2_RECLASSIFY_DIRECTIVES_TARGET_TIER",
         f"\"{reclassify_directives['request']['policy']['target_tier']}\""),
        ("AIMEE_DB2_RECLASSIFY_DIRECTIVES_GATED_KIND",
         f"\"{reclassify_directives['request']['policy']['gated_kind']}\""),
        ("AIMEE_DB2_RECLASSIFY_DIRECTIVES_GATE_MAX",
         f"{reclassify_directives['request']['field']['maximum']}u"),
        ("AIMEE_DB2_RECLASSIFY_DIRECTIVES_MAX",
         f"{reclassify_directives['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_RECORD_L4_APPROVAL", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_RECORD_L4_APPROVAL", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_RECORD_L4_APPROVAL", f"{record_l4_approval['id']}u"),
        ("AIMEE_DB2_RECORD_L4_APPROVAL_REQUEST_MIN_LEN",
         f"{record_l4_approval['request']['encoded_size_min']}u"),
        ("AIMEE_DB2_RECORD_L4_APPROVAL_REQUEST_MAX_LEN",
         f"{record_l4_approval['request']['encoded_size_max']}u"),
        ("AIMEE_DB2_RECORD_L4_APPROVAL_RESPONSE_LEN",
         f"{record_l4_approval['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_RECORD_L4_APPROVAL_ERROR_LEN",
         f"{record_l4_approval['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_RECORD_L4_APPROVAL_TIER",
         f"\"{record_l4_approval['request']['policy']['target_tier']}\""),
        ("AIMEE_DB2_RECORD_L4_APPROVAL_MEMORY_ID_MAX",
         f"{record_l4_approval['request']['fields'][0]['maximum']}ull"),
        ("AIMEE_DB2_RECORD_L4_APPROVAL_APPROVER_MAX",
         f"{record_l4_approval['request']['fields'][1]['maximum_bytes']}u"),
        ("AIMEE_DB2_RECORD_L4_APPROVAL_NOTE_MAX",
         f"{record_l4_approval['request']['fields'][2]['maximum_bytes']}u"),
        ("AIMEE_DB2_EVENT_PRUNE_ORPHANED_L0", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_PRUNE_ORPHANED_L0", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_PRUNE_ORPHANED_L0", f"{prune_orphaned_l0['id']}u"),
        ("AIMEE_DB2_PRUNE_ORPHANED_L0_REQUEST_LEN",
         f"{prune_orphaned_l0['request']['encoded_size']}u"),
        ("AIMEE_DB2_PRUNE_ORPHANED_L0_RESPONSE_LEN",
         f"{prune_orphaned_l0['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_PRUNE_ORPHANED_L0_ERROR_LEN",
         f"{prune_orphaned_l0['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_PRUNE_ORPHANED_L0_TIER",
         '"' + prune_orphaned_l0['request']['policy']['tier'] + '"'),
        ("AIMEE_DB2_PRUNE_ORPHANED_L0_MAX_AGE",
         '"' + prune_orphaned_l0['request']['policy']['maximum_age'] + '"'),
        ("AIMEE_DB2_PRUNE_ORPHANED_L0_COUNT_MAX",
         f"{prune_orphaned_l0['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_LIFECYCLE_SWEEP_EXPIRED", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_LIFECYCLE_SWEEP_EXPIRED", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_LIFECYCLE_SWEEP_EXPIRED",
         f"{lifecycle_sweep_expired['id']}u"),
        ("AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_REQUEST_LEN",
         f"{lifecycle_sweep_expired['request']['encoded_size']}u"),
        ("AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_RESPONSE_LEN",
         f"{lifecycle_sweep_expired['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_ERROR_LEN",
         f"{lifecycle_sweep_expired['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_SOURCE_STATE",
         '"' + lifecycle_sweep_expired['request']['policy']['source_state'] + '"'),
        ("AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_TARGET_STATE",
         '"' + lifecycle_sweep_expired['request']['policy']['target_state'] + '"'),
        ("AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_REASON",
         '"' + lifecycle_sweep_expired['request']['policy']['archive_reason'] + '"'),
        ("AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_COUNT_MAX",
         f"{lifecycle_sweep_expired['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_DEMOTE_ID", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_DEMOTE_ID", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_DEMOTE_ID", f"{demote_id['id']}u"),
        ("AIMEE_DB2_DEMOTE_ID_REQUEST_LEN",
         f"{demote_id['request']['encoded_size']}u"),
        ("AIMEE_DB2_DEMOTE_ID_RESPONSE_LEN",
         f"{demote_id['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_DEMOTE_ID_ERROR_LEN",
         f"{demote_id['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_DEMOTE_ID_MEMORY_ID_MAX",
         f"{demote_id['request']['field']['maximum']}ull"),
        ("AIMEE_DB2_DEMOTE_ID_COUNT_MAX",
         f"{demote_id['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_DEMOTE_ID_MULTIPLIER_BITS",
         f"{demote_id['request']['policy']['confidence_multiplier_binary64_bits']}ull"),
        ("AIMEE_DB2_DEMOTE_ID_MINIMUM_CONFIDENCE_BITS",
         f"{demote_id['request']['policy']['minimum_confidence_binary64_bits']}ull"),
        ("AIMEE_DB2_EVENT_HAS_WORKSPACE_TAG", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_HAS_WORKSPACE_TAG", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_HAS_WORKSPACE_TAG", f"{has_workspace_tag['id']}u"),
        ("AIMEE_DB2_HAS_WORKSPACE_TAG_REQUEST_LEN",
         f"{has_workspace_tag['request']['encoded_size']}u"),
        ("AIMEE_DB2_HAS_WORKSPACE_TAG_RESPONSE_LEN",
         f"{has_workspace_tag['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_HAS_WORKSPACE_TAG_ERROR_LEN",
         f"{has_workspace_tag['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_HAS_WORKSPACE_TAG_MEMORY_ID_MAX",
         f"{has_workspace_tag['request']['field']['maximum']}ull"),
        ("AIMEE_DB2_HAS_WORKSPACE_TAG_MAX",
         f"{has_workspace_tag['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_DELETE_ROW", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_DELETE_ROW", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_DELETE_ROW", f"{delete_row['id']}u"),
        ("AIMEE_DB2_DELETE_ROW_REQUEST_LEN",
         f"{delete_row['request']['encoded_size']}u"),
        ("AIMEE_DB2_DELETE_ROW_RESPONSE_LEN",
         f"{delete_row['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_DELETE_ROW_ERROR_LEN",
         f"{delete_row['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_DELETE_ROW_MEMORY_ID_MAX",
         f"{delete_row['request']['field']['maximum']}ull"),
        ("AIMEE_DB2_DELETE_ROW_MAX",
         f"{delete_row['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_TOUCH", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_TOUCH", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_TOUCH", f"{touch['id']}u"),
        ("AIMEE_DB2_TOUCH_REQUEST_LEN", f"{touch['request']['encoded_size']}u"),
        ("AIMEE_DB2_TOUCH_RESPONSE_LEN", f"{touch['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_TOUCH_ERROR_LEN", f"{touch['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_TOUCH_MEMORY_ID_MAX",
         f"{touch['request']['field']['maximum']}ull"),
        ("AIMEE_DB2_EVENT_LINK_DELETE", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_LINK_DELETE", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_LINK_DELETE", f"{link_delete['id']}u"),
        ("AIMEE_DB2_LINK_DELETE_REQUEST_LEN",
         f"{link_delete['request']['encoded_size']}u"),
        ("AIMEE_DB2_LINK_DELETE_RESPONSE_LEN",
         f"{link_delete['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_LINK_DELETE_ERROR_LEN",
         f"{link_delete['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_LINK_DELETE_LINK_ID_MAX",
         f"{link_delete['request']['field']['maximum']}ull"),
        ("AIMEE_DB2_EVENT_VALID_AT", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_VALID_AT", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_VALID_AT", f"{valid_at['id']}u"),
        ("AIMEE_DB2_VALID_AT_REQUEST_MIN_LEN",
         f"{valid_at['request']['encoded_size_min']}u"),
        ("AIMEE_DB2_VALID_AT_REQUEST_MAX_LEN",
         f"{valid_at['request']['encoded_size_max']}u"),
        ("AIMEE_DB2_VALID_AT_RESPONSE_LEN",
         f"{valid_at['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_VALID_AT_ERROR_LEN",
         f"{valid_at['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_VALID_AT_MEMORY_ID_MAX",
         f"{valid_at['request']['fields'][0]['maximum']}ull"),
        ("AIMEE_DB2_VALID_AT_AS_OF_MAX",
         f"{valid_at['request']['fields'][1]['maximum_bytes']}u"),
        ("AIMEE_DB2_VALID_AT_MAX",
         f"{valid_at['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_HAS_SCOPE_TYPE", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_HAS_SCOPE_TYPE", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_HAS_SCOPE_TYPE", f"{has_scope_type['id']}u"),
        ("AIMEE_DB2_HAS_SCOPE_TYPE_REQUEST_MIN_LEN",
         f"{has_scope_type['request']['encoded_size_min']}u"),
        ("AIMEE_DB2_HAS_SCOPE_TYPE_REQUEST_MAX_LEN",
         f"{has_scope_type['request']['encoded_size_max']}u"),
        ("AIMEE_DB2_HAS_SCOPE_TYPE_RESPONSE_LEN",
         f"{has_scope_type['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_HAS_SCOPE_TYPE_ERROR_LEN",
         f"{has_scope_type['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_HAS_SCOPE_TYPE_MEMORY_ID_MAX",
         f"{has_scope_type['request']['fields'][0]['maximum']}ull"),
        ("AIMEE_DB2_HAS_SCOPE_TYPE_SCOPE_MAX",
         f"{has_scope_type['request']['fields'][1]['maximum_bytes']}u"),
        ("AIMEE_DB2_HAS_SCOPE_TYPE_MAX",
         f"{has_scope_type['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_REJECT", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_REJECT", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_REJECT", f"{reject['id']}u"),
        ("AIMEE_DB2_REJECT_REQUEST_LEN", f"{reject['request']['encoded_size']}u"),
        ("AIMEE_DB2_REJECT_RESPONSE_LEN", f"{reject['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_REJECT_ERROR_LEN", f"{reject['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_REJECT_MEMORY_ID_MAX",
         f"{reject['request']['field']['maximum']}ull"),
        ("AIMEE_DB2_REJECT_PENALTY_BITS",
         f"{reject['request']['policy']['confidence_penalty_binary64_bits']}ull"),
        ("AIMEE_DB2_REJECT_FLOOR_BITS",
         f"{reject['request']['policy']['confidence_floor_binary64_bits']}ull"),
        ("AIMEE_DB2_EVENT_UPDATE_CONTENT", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_UPDATE_CONTENT", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_UPDATE_CONTENT", f"{update_content['id']}u"),
        ("AIMEE_DB2_UPDATE_CONTENT_REQUEST_MIN_LEN",
         f"{update_content['request']['encoded_size_min']}u"),
        ("AIMEE_DB2_UPDATE_CONTENT_REQUEST_MAX_LEN",
         f"{update_content['request']['encoded_size_max']}u"),
        ("AIMEE_DB2_UPDATE_CONTENT_RESPONSE_LEN",
         f"{update_content['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_UPDATE_CONTENT_ERROR_LEN",
         f"{update_content['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_UPDATE_CONTENT_MEMORY_ID_MAX",
         f"{update_content['request']['fields'][0]['maximum']}ull"),
        ("AIMEE_DB2_UPDATE_CONTENT_CONTENT_MAX",
         f"{update_content['request']['fields'][1]['maximum_bytes']}u"),
        ("AIMEE_DB2_UPDATE_CONTENT_MAX",
         f"{update_content['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_DECAY_CONFIDENCE", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_DECAY_CONFIDENCE", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_DECAY_CONFIDENCE", f"{decay_confidence['id']}u"),
        ("AIMEE_DB2_DECAY_CONFIDENCE_REQUEST_LEN",
         f"{decay_confidence['request']['encoded_size']}u"),
        ("AIMEE_DB2_DECAY_CONFIDENCE_RESPONSE_LEN",
         f"{decay_confidence['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_DECAY_CONFIDENCE_ERROR_LEN",
         f"{decay_confidence['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_DECAY_CONFIDENCE_MEMORY_ID_MAX",
         f"{decay_confidence['request']['field']['maximum']}ull"),
        ("AIMEE_DB2_DECAY_CONFIDENCE_MULTIPLIER_BITS",
         f"{decay_confidence['request']['policy']['confidence_multiplier_binary64_bits']}ull"),
        ("AIMEE_DB2_EVENT_WORKSPACE_TAG_INSERT", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_WORKSPACE_TAG_INSERT", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_WORKSPACE_TAG_INSERT",
         f"{workspace_tag_insert['id']}u"),
        ("AIMEE_DB2_WORKSPACE_TAG_INSERT_REQUEST_MIN_LEN",
         f"{workspace_tag_insert['request']['encoded_size_min']}u"),
        ("AIMEE_DB2_WORKSPACE_TAG_INSERT_REQUEST_MAX_LEN",
         f"{workspace_tag_insert['request']['encoded_size_max']}u"),
        ("AIMEE_DB2_WORKSPACE_TAG_INSERT_RESPONSE_LEN",
         f"{workspace_tag_insert['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_WORKSPACE_TAG_INSERT_ERROR_LEN",
         f"{workspace_tag_insert['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_WORKSPACE_TAG_INSERT_MEMORY_ID_MAX",
         f"{workspace_tag_insert['request']['fields'][0]['maximum']}ull"),
        ("AIMEE_DB2_WORKSPACE_TAG_INSERT_WORKSPACE_MAX",
         f"{workspace_tag_insert['request']['fields'][1]['maximum_bytes']}u"),
        ("AIMEE_DB2_EVENT_SET_COGNIFIED_KIND", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_SET_COGNIFIED_KIND", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_SET_COGNIFIED_KIND", f"{set_cognified_kind['id']}u"),
        ("AIMEE_DB2_SET_COGNIFIED_KIND_REQUEST_MIN_LEN",
         f"{set_cognified_kind['request']['encoded_size_min']}u"),
        ("AIMEE_DB2_SET_COGNIFIED_KIND_REQUEST_MAX_LEN",
         f"{set_cognified_kind['request']['encoded_size_max']}u"),
        ("AIMEE_DB2_SET_COGNIFIED_KIND_RESPONSE_LEN",
         f"{set_cognified_kind['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_SET_COGNIFIED_KIND_ERROR_LEN",
         f"{set_cognified_kind['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_SET_COGNIFIED_KIND_MEMORY_ID_MAX",
         f"{set_cognified_kind['request']['fields'][0]['maximum']}ull"),
        ("AIMEE_DB2_SET_COGNIFIED_KIND_KIND_MAX",
         f"{set_cognified_kind['request']['fields'][1]['maximum_bytes']}u"),
        ("AIMEE_DB2_EVENT_SET_SOURCE_SESSION", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_SET_SOURCE_SESSION", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_SET_SOURCE_SESSION", f"{set_source_session['id']}u"),
        ("AIMEE_DB2_SET_SOURCE_SESSION_REQUEST_MIN_LEN",
         f"{set_source_session['request']['encoded_size_min']}u"),
        ("AIMEE_DB2_SET_SOURCE_SESSION_REQUEST_MAX_LEN",
         f"{set_source_session['request']['encoded_size_max']}u"),
        ("AIMEE_DB2_SET_SOURCE_SESSION_RESPONSE_LEN",
         f"{set_source_session['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_SET_SOURCE_SESSION_ERROR_LEN",
         f"{set_source_session['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_SET_SOURCE_SESSION_MEMORY_ID_MAX",
         f"{set_source_session['request']['fields'][0]['maximum']}ull"),
        ("AIMEE_DB2_SET_SOURCE_SESSION_SESSION_MAX",
         f"{set_source_session['request']['fields'][1]['maximum_bytes']}u"),
        ("AIMEE_DB2_EVENT_NEGATION_TOKENS_UPDATE", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_NEGATION_TOKENS_UPDATE", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_NEGATION_TOKENS_UPDATE",
         f"{negation_tokens_update['id']}u"),
        ("AIMEE_DB2_NEGATION_TOKENS_UPDATE_REQUEST_MIN_LEN",
         f"{negation_tokens_update['request']['encoded_size_min']}u"),
        ("AIMEE_DB2_NEGATION_TOKENS_UPDATE_REQUEST_MAX_LEN",
         f"{negation_tokens_update['request']['encoded_size_max']}u"),
        ("AIMEE_DB2_NEGATION_TOKENS_UPDATE_RESPONSE_LEN",
         f"{negation_tokens_update['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_NEGATION_TOKENS_UPDATE_ERROR_LEN",
         f"{negation_tokens_update['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_NEGATION_TOKENS_UPDATE_MEMORY_ID_MAX",
         f"{negation_tokens_update['request']['fields'][0]['maximum']}ull"),
        ("AIMEE_DB2_NEGATION_TOKENS_UPDATE_TOKENS_MAX",
         f"{negation_tokens_update['request']['fields'][1]['maximum_bytes']}u"),
        ("AIMEE_DB2_EVENT_GET_CONTENT", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_GET_CONTENT", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_GET_CONTENT", f"{get_content['id']}u"),
        ("AIMEE_DB2_GET_CONTENT_REQUEST_LEN",
         f"{get_content['request']['encoded_size']}u"),
        ("AIMEE_DB2_GET_CONTENT_RESPONSE_MIN_LEN",
         f"{get_content['reply']['encoded_size_min_ok']}u"),
        ("AIMEE_DB2_GET_CONTENT_RESPONSE_MAX_LEN",
         f"{get_content['reply']['encoded_size_max_ok']}u"),
        ("AIMEE_DB2_GET_CONTENT_ERROR_LEN",
         f"{get_content['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_GET_CONTENT_MEMORY_ID_MAX",
         f"{get_content['request']['field']['maximum']}ull"),
        ("AIMEE_DB2_GET_CONTENT_CONTENT_MAX",
         f"{get_content['reply']['field']['maximum_bytes']}u"),
        ("AIMEE_DB2_EVENT_GET_SOURCE_SESSION", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_GET_SOURCE_SESSION", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_GET_SOURCE_SESSION", f"{get_source_session['id']}u"),
        ("AIMEE_DB2_GET_SOURCE_SESSION_REQUEST_LEN",
         f"{get_source_session['request']['encoded_size']}u"),
        ("AIMEE_DB2_GET_SOURCE_SESSION_RESPONSE_MIN_LEN",
         f"{get_source_session['reply']['encoded_size_min_ok']}u"),
        ("AIMEE_DB2_GET_SOURCE_SESSION_RESPONSE_MAX_LEN",
         f"{get_source_session['reply']['encoded_size_max_ok']}u"),
        ("AIMEE_DB2_GET_SOURCE_SESSION_ERROR_LEN",
         f"{get_source_session['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_GET_SOURCE_SESSION_MEMORY_ID_MAX",
         f"{get_source_session['request']['field']['maximum']}ull"),
        ("AIMEE_DB2_GET_SOURCE_SESSION_SESSION_MAX",
         f"{get_source_session['reply']['field']['maximum_bytes']}u"),
        ("AIMEE_DB2_EVENT_PICK_FIRST_TEMPORAL_REF", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_PICK_FIRST_TEMPORAL_REF", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_PICK_FIRST_TEMPORAL_REF",
         f"{pick_first_temporal_ref['id']}u"),
        ("AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_REQUEST_LEN",
         f"{pick_first_temporal_ref['request']['encoded_size']}u"),
        ("AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_RESPONSE_MIN_LEN",
         f"{pick_first_temporal_ref['reply']['encoded_size_min_ok']}u"),
        ("AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_RESPONSE_MAX_LEN",
         f"{pick_first_temporal_ref['reply']['encoded_size_max_ok']}u"),
        ("AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_ERROR_LEN",
         f"{pick_first_temporal_ref['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_MEMORY_ID_MAX",
         f"{pick_first_temporal_ref['request']['field']['maximum']}ull"),
        ("AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_KEY_MAX",
         f"{pick_first_temporal_ref['reply']['field']['maximum_bytes']}u"),
        ("AIMEE_DB2_EVENT_COUNT_AND_MAX_UPDATED", "AIMEE_DB2_EVENT_MEMORY"),
        ("AIMEE_DB2_STAGE_COUNT_AND_MAX_UPDATED", "AIMEE_DB2_FAMILY_MEMORY"),
        ("AIMEE_DB2_OPERATION_COUNT_AND_MAX_UPDATED",
         f"{count_and_max_updated['id']}u"),
        ("AIMEE_DB2_COUNT_AND_MAX_UPDATED_REQUEST_LEN",
         f"{count_and_max_updated['request']['encoded_size']}u"),
        ("AIMEE_DB2_COUNT_AND_MAX_UPDATED_RESPONSE_MIN_LEN",
         f"{count_and_max_updated['reply']['encoded_size_min_ok']}u"),
        ("AIMEE_DB2_COUNT_AND_MAX_UPDATED_RESPONSE_MAX_LEN",
         f"{count_and_max_updated['reply']['encoded_size_max_ok']}u"),
        ("AIMEE_DB2_COUNT_AND_MAX_UPDATED_ERROR_LEN",
         f"{count_and_max_updated['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_COUNT_AND_MAX_UPDATED_COUNT_MAX",
         f"{count_and_max_updated['reply']['fields'][0]['maximum']}u"),
        ("AIMEE_DB2_COUNT_AND_MAX_UPDATED_STAMP_MAX",
         f"{count_and_max_updated['reply']['fields'][1]['maximum_bytes']}u"),
        ("AIMEE_DB2_EVENT_ENTITY_EDGE_PRUNE_ORPHANS", "AIMEE_DB2_EVENT_INDEX"),
        ("AIMEE_DB2_STAGE_ENTITY_EDGE_PRUNE_ORPHANS", "AIMEE_DB2_FAMILY_INDEX"),
        ("AIMEE_DB2_OPERATION_ENTITY_EDGE_PRUNE_ORPHANS",
         f"{entity_edge_prune_orphans['id']}u"),
        ("AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_REQUEST_LEN",
         f"{entity_edge_prune_orphans['request']['encoded_size']}u"),
        ("AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_RESPONSE_LEN",
         f"{entity_edge_prune_orphans['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_ERROR_LEN",
         f"{entity_edge_prune_orphans['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_COUNT_MAX",
         f"{entity_edge_prune_orphans['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_ENTITY_EDGE_NORMALIZE_WEIGHTS", "AIMEE_DB2_EVENT_INDEX"),
        ("AIMEE_DB2_STAGE_ENTITY_EDGE_NORMALIZE_WEIGHTS", "AIMEE_DB2_FAMILY_INDEX"),
        ("AIMEE_DB2_OPERATION_ENTITY_EDGE_NORMALIZE_WEIGHTS",
         f"{entity_edge_normalize_weights['id']}u"),
        ("AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_REQUEST_LEN",
         f"{entity_edge_normalize_weights['request']['encoded_size']}u"),
        ("AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_RESPONSE_LEN",
         f"{entity_edge_normalize_weights['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_ERROR_LEN",
         f"{entity_edge_normalize_weights['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_SCALE",
         f"{entity_edge_normalize_weights['request']['policy']['scale']}u"),
        ("AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_COUNT_MAX",
         f"{entity_edge_normalize_weights['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_PROJECT_COUNT", "AIMEE_DB2_EVENT_INDEX"),
        ("AIMEE_DB2_STAGE_PROJECT_COUNT", "AIMEE_DB2_FAMILY_INDEX"),
        ("AIMEE_DB2_OPERATION_PROJECT_COUNT", f"{project_count['id']}u"),
        ("AIMEE_DB2_PROJECT_COUNT_REQUEST_LEN",
         f"{project_count['request']['encoded_size']}u"),
        ("AIMEE_DB2_PROJECT_COUNT_RESPONSE_LEN",
         f"{project_count['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_PROJECT_COUNT_ERROR_LEN",
         f"{project_count['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_PROJECT_COUNT_MAX",
         f"{project_count['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_PURGE_HIDDEN_POLLUTION", "AIMEE_DB2_EVENT_INDEX"),
        ("AIMEE_DB2_STAGE_PURGE_HIDDEN_POLLUTION", "AIMEE_DB2_FAMILY_INDEX"),
        ("AIMEE_DB2_OPERATION_PURGE_HIDDEN_POLLUTION",
         f"{purge_hidden_pollution['id']}u"),
        ("AIMEE_DB2_PURGE_HIDDEN_POLLUTION_REQUEST_LEN",
         f"{purge_hidden_pollution['request']['encoded_size']}u"),
        ("AIMEE_DB2_PURGE_HIDDEN_POLLUTION_RESPONSE_LEN",
         f"{purge_hidden_pollution['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_PURGE_HIDDEN_POLLUTION_ERROR_LEN",
         f"{purge_hidden_pollution['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_PURGE_HIDDEN_POLLUTION_MAX",
         f"{purge_hidden_pollution['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_REQUEUE_DRIFTED", "AIMEE_DB2_EVENT_INDEX"),
        ("AIMEE_DB2_STAGE_REQUEUE_DRIFTED", "AIMEE_DB2_FAMILY_INDEX"),
        ("AIMEE_DB2_OPERATION_REQUEUE_DRIFTED", f"{requeue_drifted['id']}u"),
        ("AIMEE_DB2_REQUEUE_DRIFTED_REQUEST_LEN",
         f"{requeue_drifted['request']['encoded_size']}u"),
        ("AIMEE_DB2_REQUEUE_DRIFTED_RESPONSE_LEN",
         f"{requeue_drifted['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_REQUEUE_DRIFTED_ERROR_LEN",
         f"{requeue_drifted['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_REQUEUE_DRIFTED_MAX",
         f"{requeue_drifted['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_PROSPECTIVE_SWEEP_EXPIRED", "AIMEE_DB2_EVENT_MAINTENANCE"),
        ("AIMEE_DB2_STAGE_PROSPECTIVE_SWEEP_EXPIRED", "AIMEE_DB2_FAMILY_MAINTENANCE"),
        ("AIMEE_DB2_OPERATION_PROSPECTIVE_SWEEP_EXPIRED",
         f"{prospective_sweep_expired['id']}u"),
        ("AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_REQUEST_LEN",
         f"{prospective_sweep_expired['request']['encoded_size']}u"),
        ("AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_RESPONSE_LEN",
         f"{prospective_sweep_expired['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_ERROR_LEN",
         f"{prospective_sweep_expired['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_MAX",
         f"{prospective_sweep_expired['reply']['field']['maximum']}u"),
        ("AIMEE_DB2_EVENT_DIRECTIVE_SWEEP_EXPIRED", "AIMEE_DB2_EVENT_MAINTENANCE"),
        ("AIMEE_DB2_STAGE_DIRECTIVE_SWEEP_EXPIRED", "AIMEE_DB2_FAMILY_MAINTENANCE"),
        ("AIMEE_DB2_OPERATION_DIRECTIVE_SWEEP_EXPIRED",
         f"{directive_sweep_expired['id']}u"),
        ("AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_REQUEST_LEN",
         f"{directive_sweep_expired['request']['encoded_size']}u"),
        ("AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_RESPONSE_LEN",
         f"{directive_sweep_expired['reply']['encoded_size_ok']}u"),
        ("AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_ERROR_LEN",
         f"{directive_sweep_expired['reply']['encoded_size_error']}u"),
        ("AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_MAX",
         f"{directive_sweep_expired['reply']['field']['maximum']}u"),
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
#include <string.h>

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

typedef struct
{{
   uint32_t was_in_progress;
   uint32_t recorded_dimension;
   uint32_t running_dimension;
}} aimee_db2_reembed_clear_maintenance_t;

typedef struct
{{
   uint32_t recorded_dimension;
   uint32_t target_dimension;
   uint32_t tables_discovered;
   uint32_t tables_dropped;
   uint64_t rows_cleared;
   int32_t curator_requeued;
   int32_t evidence_requeued;
}} aimee_db2_dimension_reset_t;

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

static inline int aimee_db2_level3_count_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_LEVEL3_COUNT, 0u, 0u, output,
                                           capacity);
}}

static inline int aimee_db2_level3_count_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_LEVEL3_COUNT_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_LEVEL3_COUNT && header.flags == 0u &&
                  header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_level3_count_reply_encode(uint32_t count, uint8_t *output,
                                                      size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || count > AIMEE_DB2_LEVEL3_COUNT_MAX ||
       capacity < AIMEE_DB2_LEVEL3_COUNT_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_LEVEL3_COUNT, AIMEE_DB2_RESULT_OK, 4u,
                                     output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, count);
   *output_len = AIMEE_DB2_LEVEL3_COUNT_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_level3_count_reply_decode(const uint8_t *input, size_t input_len,
                                                      uint32_t *count)
{{
   if (count)
      *count = 0u;
   if (!count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_LEVEL3_COUNT ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_LEVEL3_COUNT_MAX)
      return -1;
   *count = decoded;
   return 0;
}}

static inline int aimee_db2_level2_count_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_LEVEL2_COUNT, 0u, 0u, output,
                                           capacity);
}}

static inline int aimee_db2_level2_count_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_LEVEL2_COUNT_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_LEVEL2_COUNT && header.flags == 0u &&
                  header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_level2_count_reply_encode(uint32_t count, uint8_t *output,
                                                      size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || count > AIMEE_DB2_LEVEL2_COUNT_MAX ||
       capacity < AIMEE_DB2_LEVEL2_COUNT_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_LEVEL2_COUNT, AIMEE_DB2_RESULT_OK, 4u,
                                     output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, count);
   *output_len = AIMEE_DB2_LEVEL2_COUNT_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_level2_count_reply_decode(const uint8_t *input, size_t input_len,
                                                      uint32_t *count)
{{
   if (count)
      *count = 0u;
   if (!count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_LEVEL2_COUNT ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_LEVEL2_COUNT_MAX)
      return -1;
   *count = decoded;
   return 0;
}}

static inline int aimee_db2_orphaned_l0_count_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_ORPHANED_L0_COUNT, 0u, 0u,
                                           output, capacity);
}}

static inline int aimee_db2_orphaned_l0_count_request_decode(const uint8_t *input,
                                                             size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_ORPHANED_L0_COUNT_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_ORPHANED_L0_COUNT &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_orphaned_l0_count_reply_encode(uint32_t count, uint8_t *output,
                                                           size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || count > AIMEE_DB2_ORPHANED_L0_COUNT_MAX ||
       capacity < AIMEE_DB2_ORPHANED_L0_COUNT_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_ORPHANED_L0_COUNT,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, count);
   *output_len = AIMEE_DB2_ORPHANED_L0_COUNT_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_orphaned_l0_count_reply_decode(const uint8_t *input,
                                                           size_t input_len, uint32_t *count)
{{
   if (count)
      *count = 0u;
   if (!count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_ORPHANED_L0_COUNT ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_ORPHANED_L0_COUNT_MAX)
      return -1;
   *count = decoded;
   return 0;
}}

static inline int aimee_db2_total_count_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_TOTAL_COUNT, 0u, 0u,
                                           output, capacity);
}}

static inline int aimee_db2_total_count_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_TOTAL_COUNT_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_TOTAL_COUNT &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_total_count_reply_encode(uint64_t count, uint8_t *output,
                                                      size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || count > AIMEE_DB2_TOTAL_COUNT_MAX ||
       capacity < AIMEE_DB2_TOTAL_COUNT_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_TOTAL_COUNT,
                                     AIMEE_DB2_RESULT_OK, 8u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u64(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, count);
   *output_len = AIMEE_DB2_TOTAL_COUNT_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_total_count_reply_decode(const uint8_t *input, size_t input_len,
                                                      uint64_t *count)
{{
   if (count)
      *count = 0u;
   if (!count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_TOTAL_COUNT ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 8u)
      return -1;
   uint64_t decoded = aimee_db2_get_u64(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_TOTAL_COUNT_MAX)
      return -1;
   *count = decoded;
   return 0;
}}

static inline int aimee_db2_session_l2_count_request_encode(const char *source_session,
                                                            uint8_t *output, size_t capacity,
                                                            uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!source_session || !output || !output_len)
      return -1;
   size_t session_len = 0u;
   while (session_len <= AIMEE_DB2_SESSION_L2_COUNT_SESSION_MAX &&
          source_session[session_len])
      ++session_len;
   if (session_len == 0u || session_len > AIMEE_DB2_SESSION_L2_COUNT_SESSION_MAX ||
       capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u + session_len ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_SESSION_L2_COUNT, 0u,
                                       4u + (uint32_t)session_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, (uint32_t)session_len);
   memcpy(payload + 4, source_session, session_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u + (uint32_t)session_len;
   return 0;
}}

static inline int aimee_db2_session_l2_count_request_decode(
    const uint8_t *input, size_t input_len, char *source_session, size_t source_session_capacity)
{{
   if (source_session && source_session_capacity)
      source_session[0] = '\\0';
   if (!source_session || source_session_capacity == 0u)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_SESSION_L2_COUNT || header.flags != 0u ||
       input_len < AIMEE_DB2_SESSION_L2_COUNT_REQUEST_MIN_LEN ||
       input_len > AIMEE_DB2_SESSION_L2_COUNT_REQUEST_MAX_LEN || header.payload_len < 5u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t decoded_len = aimee_db2_get_u32(payload);
   if (decoded_len == 0u || decoded_len > AIMEE_DB2_SESSION_L2_COUNT_SESSION_MAX ||
       header.payload_len != 4u + decoded_len || source_session_capacity <= decoded_len ||
       memchr(payload + 4, '\\0', decoded_len) != NULL)
      return -1;
   memcpy(source_session, payload + 4, decoded_len);
   source_session[decoded_len] = '\\0';
   return 0;
}}

static inline int aimee_db2_session_l2_count_reply_encode(uint32_t count, uint8_t *output,
                                                          size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || count > AIMEE_DB2_SESSION_L2_COUNT_MAX ||
       capacity < AIMEE_DB2_SESSION_L2_COUNT_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_SESSION_L2_COUNT,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, count);
   *output_len = AIMEE_DB2_SESSION_L2_COUNT_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_session_l2_count_reply_decode(const uint8_t *input,
                                                          size_t input_len, uint32_t *count)
{{
   if (count)
      *count = 0u;
   if (!count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_SESSION_L2_COUNT ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_SESSION_L2_COUNT_MAX)
      return -1;
   *count = decoded;
   return 0;
}}

static inline int aimee_db2_key_exists_request_encode(const char *key, uint8_t *output,
                                                      size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!key || !output || !output_len)
      return -1;
   size_t key_len = 0u;
   while (key_len <= AIMEE_DB2_KEY_EXISTS_KEY_MAX && key[key_len])
      ++key_len;
   if (key_len == 0u || key_len > AIMEE_DB2_KEY_EXISTS_KEY_MAX ||
       capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u + key_len ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_KEY_EXISTS, 0u,
                                       4u + (uint32_t)key_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, (uint32_t)key_len);
   memcpy(payload + 4, key, key_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u + (uint32_t)key_len;
   return 0;
}}

static inline int aimee_db2_key_exists_request_decode(const uint8_t *input, size_t input_len,
                                                      char *key, size_t key_capacity)
{{
   if (key && key_capacity)
      key[0] = '\\0';
   if (!key || key_capacity == 0u)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_KEY_EXISTS || header.flags != 0u ||
       input_len < AIMEE_DB2_KEY_EXISTS_REQUEST_MIN_LEN ||
       input_len > AIMEE_DB2_KEY_EXISTS_REQUEST_MAX_LEN || header.payload_len < 5u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t decoded_len = aimee_db2_get_u32(payload);
   if (decoded_len == 0u || decoded_len > AIMEE_DB2_KEY_EXISTS_KEY_MAX ||
       header.payload_len != 4u + decoded_len || key_capacity <= decoded_len ||
       memchr(payload + 4, '\\0', decoded_len) != NULL)
      return -1;
   memcpy(key, payload + 4, decoded_len);
   key[decoded_len] = '\\0';
   return 0;
}}

static inline int aimee_db2_key_exists_reply_encode(uint32_t exists, uint8_t *output,
                                                    size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || exists > AIMEE_DB2_KEY_EXISTS_MAX ||
       capacity < AIMEE_DB2_KEY_EXISTS_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_KEY_EXISTS, AIMEE_DB2_RESULT_OK, 4u,
                                     output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, exists);
   *output_len = AIMEE_DB2_KEY_EXISTS_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_key_exists_reply_decode(const uint8_t *input, size_t input_len,
                                                    uint32_t *exists)
{{
   if (exists)
      *exists = 0u;
   if (!exists)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_KEY_EXISTS || header.result != AIMEE_DB2_RESULT_OK ||
       header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_KEY_EXISTS_MAX)
      return -1;
   *exists = decoded;
   return 0;
}}

static inline int aimee_db2_find_id_by_key_kind_request_encode(
    const char *key, const char *kind, uint8_t *output, size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!key || !kind || !output || !output_len)
      return -1;
   size_t key_len = 0u, kind_len = 0u;
   while (key_len <= AIMEE_DB2_FIND_ID_BY_KEY_KIND_KEY_MAX && key[key_len])
      ++key_len;
   while (kind_len <= AIMEE_DB2_FIND_ID_BY_KEY_KIND_KIND_MAX && kind[kind_len])
      ++kind_len;
   size_t payload_len = 8u + key_len + kind_len;
   if (key_len == 0u || key_len > AIMEE_DB2_FIND_ID_BY_KEY_KIND_KEY_MAX ||
       kind_len == 0u || kind_len > AIMEE_DB2_FIND_ID_BY_KEY_KIND_KIND_MAX ||
       capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_FIND_ID_BY_KEY_KIND, 0u,
                                       (uint32_t)payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, (uint32_t)key_len);
   memcpy(payload + 4, key, key_len);
   aimee_db2_put_u32(payload + 4u + key_len, (uint32_t)kind_len);
   memcpy(payload + 8u + key_len, kind, kind_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + (uint32_t)payload_len;
   return 0;
}}

static inline int aimee_db2_find_id_by_key_kind_request_decode(
    const uint8_t *input, size_t input_len, char *key, size_t key_capacity, char *kind,
    size_t kind_capacity)
{{
   if (key && key_capacity)
      key[0] = '\\0';
   if (kind && kind_capacity)
      kind[0] = '\\0';
   if (!key || key_capacity == 0u || !kind || kind_capacity == 0u)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_FIND_ID_BY_KEY_KIND || header.flags != 0u ||
       input_len < AIMEE_DB2_FIND_ID_BY_KEY_KIND_REQUEST_MIN_LEN ||
       input_len > AIMEE_DB2_FIND_ID_BY_KEY_KIND_REQUEST_MAX_LEN || header.payload_len < 10u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t key_len = aimee_db2_get_u32(payload);
   if (key_len == 0u || key_len > AIMEE_DB2_FIND_ID_BY_KEY_KIND_KEY_MAX ||
       header.payload_len < 8u + key_len + 1u)
      return -1;
   uint32_t kind_len = aimee_db2_get_u32(payload + 4u + key_len);
   if (kind_len == 0u || kind_len > AIMEE_DB2_FIND_ID_BY_KEY_KIND_KIND_MAX ||
       header.payload_len != 8u + key_len + kind_len || key_capacity <= key_len ||
       kind_capacity <= kind_len || memchr(payload + 4, '\\0', key_len) != NULL ||
       memchr(payload + 8u + key_len, '\\0', kind_len) != NULL)
      return -1;
   memcpy(key, payload + 4, key_len);
   key[key_len] = '\\0';
   memcpy(kind, payload + 8u + key_len, kind_len);
   kind[kind_len] = '\\0';
   return 0;
}}

static inline int aimee_db2_find_id_by_key_kind_reply_encode(
    uint32_t found, uint64_t id, uint8_t *output, size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || found > AIMEE_DB2_FIND_ID_BY_KEY_KIND_FOUND_MAX ||
       id > AIMEE_DB2_FIND_ID_BY_KEY_KIND_ID_MAX || (found == 0u && id != 0u) ||
       (found == 1u && id == 0u) || capacity < AIMEE_DB2_FIND_ID_BY_KEY_KIND_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_FIND_ID_BY_KEY_KIND,
                                     AIMEE_DB2_RESULT_OK, 12u, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, found);
   aimee_db2_put_u64(payload + 4, id);
   *output_len = AIMEE_DB2_FIND_ID_BY_KEY_KIND_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_find_id_by_key_kind_reply_decode(
    const uint8_t *input, size_t input_len, uint32_t *found, uint64_t *id)
{{
   if (found)
      *found = 0u;
   if (id)
      *id = 0u;
   if (!found || !id)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_FIND_ID_BY_KEY_KIND ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 12u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t decoded_found = aimee_db2_get_u32(payload);
   uint64_t decoded_id = aimee_db2_get_u64(payload + 4);
   if (decoded_found > AIMEE_DB2_FIND_ID_BY_KEY_KIND_FOUND_MAX ||
       decoded_id > AIMEE_DB2_FIND_ID_BY_KEY_KIND_ID_MAX ||
       (decoded_found == 0u && decoded_id != 0u) || (decoded_found == 1u && decoded_id == 0u))
      return -1;
   *found = decoded_found;
   *id = decoded_id;
   return 0;
}}

static inline int aimee_db2_key_exists_in_tier_pair_request_encode(
    const char *key, const char *tier_a, const char *tier_b, uint8_t *output, size_t capacity,
    uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!key || !tier_a || !tier_b || !output || !output_len)
      return -1;
   size_t key_len = 0u, tier_a_len = 0u, tier_b_len = 0u;
   while (key_len <= AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_KEY_MAX && key[key_len])
      ++key_len;
   while (tier_a_len <= AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_A_MAX && tier_a[tier_a_len])
      ++tier_a_len;
   while (tier_b_len <= AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_B_MAX && tier_b[tier_b_len])
      ++tier_b_len;
   size_t payload_len = 12u + key_len + tier_a_len + tier_b_len;
   if (key_len == 0u || key_len > AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_KEY_MAX ||
       tier_a_len == 0u || tier_a_len > AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_A_MAX ||
       tier_b_len == 0u || tier_b_len > AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_B_MAX ||
       capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_KEY_EXISTS_IN_TIER_PAIR, 0u,
                                       (uint32_t)payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, (uint32_t)key_len);
   memcpy(payload + 4u, key, key_len);
   aimee_db2_put_u32(payload + 4u + key_len, (uint32_t)tier_a_len);
   memcpy(payload + 8u + key_len, tier_a, tier_a_len);
   aimee_db2_put_u32(payload + 8u + key_len + tier_a_len, (uint32_t)tier_b_len);
   memcpy(payload + 12u + key_len + tier_a_len, tier_b, tier_b_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + (uint32_t)payload_len;
   return 0;
}}

static inline int aimee_db2_key_exists_in_tier_pair_request_decode(
    const uint8_t *input, size_t input_len, char *key, size_t key_capacity, char *tier_a,
    size_t tier_a_capacity, char *tier_b, size_t tier_b_capacity)
{{
   if (key && key_capacity)
      key[0] = '\\0';
   if (tier_a && tier_a_capacity)
      tier_a[0] = '\\0';
   if (tier_b && tier_b_capacity)
      tier_b[0] = '\\0';
   if (!key || key_capacity == 0u || !tier_a || tier_a_capacity == 0u || !tier_b ||
       tier_b_capacity == 0u)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_KEY_EXISTS_IN_TIER_PAIR || header.flags != 0u ||
       input_len < AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_REQUEST_MIN_LEN ||
       input_len > AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_REQUEST_MAX_LEN || header.payload_len < 15u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t key_len = aimee_db2_get_u32(payload);
   if (key_len == 0u || key_len > AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_KEY_MAX ||
       header.payload_len < 12u + key_len + 2u)
      return -1;
   uint32_t tier_a_len = aimee_db2_get_u32(payload + 4u + key_len);
   if (tier_a_len == 0u || tier_a_len > AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_A_MAX ||
       header.payload_len < 12u + key_len + tier_a_len + 1u)
      return -1;
   uint32_t tier_b_len = aimee_db2_get_u32(payload + 8u + key_len + tier_a_len);
   if (tier_b_len == 0u || tier_b_len > AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_B_MAX ||
       header.payload_len != 12u + key_len + tier_a_len + tier_b_len ||
       key_capacity <= key_len || tier_a_capacity <= tier_a_len || tier_b_capacity <= tier_b_len ||
       memchr(payload + 4u, '\\0', key_len) != NULL ||
       memchr(payload + 8u + key_len, '\\0', tier_a_len) != NULL ||
       memchr(payload + 12u + key_len + tier_a_len, '\\0', tier_b_len) != NULL)
      return -1;
   memcpy(key, payload + 4u, key_len);
   key[key_len] = '\\0';
   memcpy(tier_a, payload + 8u + key_len, tier_a_len);
   tier_a[tier_a_len] = '\\0';
   memcpy(tier_b, payload + 12u + key_len + tier_a_len, tier_b_len);
   tier_b[tier_b_len] = '\\0';
   return 0;
}}

static inline int aimee_db2_key_exists_in_tier_pair_reply_encode(
    uint32_t exists, uint8_t *output, size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || exists > AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_MAX ||
       capacity < AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_KEY_EXISTS_IN_TIER_PAIR,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, exists);
   *output_len = AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_key_exists_in_tier_pair_reply_decode(
    const uint8_t *input, size_t input_len, uint32_t *exists)
{{
   if (exists)
      *exists = 0u;
   if (!exists)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_KEY_EXISTS_IN_TIER_PAIR ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_MAX)
      return -1;
   *exists = decoded;
   return 0;
}}

static inline int aimee_db2_effectiveness_update_request_encode(
    uint64_t memory_id, uint32_t has_value, double value, uint8_t *output, size_t capacity)
{{
   uint64_t value_bits = 0u;
   if (sizeof(value) != sizeof(value_bits))
      return -1;
   memcpy(&value_bits, &value, sizeof(value_bits));
   if (!output || memory_id == 0u || memory_id > AIMEE_DB2_EFFECTIVENESS_UPDATE_MEMORY_ID_MAX ||
       has_value > AIMEE_DB2_EFFECTIVENESS_UPDATE_HAS_VALUE_MAX ||
       (has_value == 0u && value_bits != 0u) ||
       capacity < AIMEE_DB2_EFFECTIVENESS_UPDATE_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_EFFECTIVENESS_UPDATE, 0u, 20u, output,
                                       capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u64(payload, memory_id);
   aimee_db2_put_u32(payload + 8u, has_value);
   aimee_db2_put_u64(payload + 12u, value_bits);
   return 0;
}}

static inline int aimee_db2_effectiveness_update_request_decode(
    const uint8_t *input, size_t input_len, uint64_t *memory_id, uint32_t *has_value,
    double *value)
{{
   if (memory_id)
      *memory_id = 0u;
   if (has_value)
      *has_value = 0u;
   if (value)
      *value = 0.0;
   if (!memory_id || !has_value || !value || sizeof(*value) != sizeof(uint64_t))
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_EFFECTIVENESS_UPDATE_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_EFFECTIVENESS_UPDATE || header.flags != 0u ||
       header.payload_len != 20u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint64_t decoded_memory_id = aimee_db2_get_u64(payload);
   uint32_t decoded_has_value = aimee_db2_get_u32(payload + 8u);
   uint64_t value_bits = aimee_db2_get_u64(payload + 12u);
   if (decoded_memory_id == 0u ||
       decoded_memory_id > AIMEE_DB2_EFFECTIVENESS_UPDATE_MEMORY_ID_MAX ||
       decoded_has_value > AIMEE_DB2_EFFECTIVENESS_UPDATE_HAS_VALUE_MAX ||
       (decoded_has_value == 0u && value_bits != 0u))
      return -1;
   memcpy(value, &value_bits, sizeof(value_bits));
   *memory_id = decoded_memory_id;
   *has_value = decoded_has_value;
   return 0;
}}

static inline int aimee_db2_effectiveness_update_reply_encode(uint32_t result, uint8_t *output,
                                                              size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_EFFECTIVENESS_UPDATE_RESPONSE_LEN ||
       (result != AIMEE_DB2_RESULT_OK && result != AIMEE_DB2_RESULT_INVALID_STATE))
      return -1;
   return aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_EFFECTIVENESS_UPDATE, result, 0u,
                                        output, capacity);
}}

static inline int aimee_db2_effectiveness_update_reply_decode(
    const uint8_t *input, size_t input_len, uint32_t *result)
{{
   if (result)
      *result = 0u;
   if (!result)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_EFFECTIVENESS_UPDATE_RESPONSE_LEN ||
       header.operation != AIMEE_DB2_OPERATION_EFFECTIVENESS_UPDATE || header.payload_len != 0u ||
       (header.result != AIMEE_DB2_RESULT_OK && header.result != AIMEE_DB2_RESULT_INVALID_STATE))
      return -1;
   *result = header.result;
   return 0;
}}

static inline int aimee_db2_retention_enforce_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_RETENTION_ENFORCE, 0u, 0u, output,
                                           capacity);
}}

static inline int aimee_db2_retention_enforce_request_decode(const uint8_t *input,
                                                             size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_RETENTION_ENFORCE_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_RETENTION_ENFORCE &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_retention_enforce_reply_encode(uint32_t deleted_count,
                                                           uint8_t *output, size_t capacity,
                                                           uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || deleted_count > AIMEE_DB2_RETENTION_ENFORCE_MAX ||
       capacity < AIMEE_DB2_RETENTION_ENFORCE_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_RETENTION_ENFORCE,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, deleted_count);
   *output_len = AIMEE_DB2_RETENTION_ENFORCE_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_retention_enforce_reply_decode(const uint8_t *input,
                                                           size_t input_len,
                                                           uint32_t *deleted_count)
{{
   if (deleted_count)
      *deleted_count = 0u;
   if (!deleted_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_RETENTION_ENFORCE ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_RETENTION_ENFORCE_MAX)
      return -1;
   *deleted_count = decoded;
   return 0;
}}

static inline int aimee_db2_effectiveness_demote_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_EFFECTIVENESS_DEMOTE, 0u, 0u,
                                           output, capacity);
}}

static inline int aimee_db2_effectiveness_demote_request_decode(const uint8_t *input,
                                                                size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_EFFECTIVENESS_DEMOTE_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_EFFECTIVENESS_DEMOTE &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_effectiveness_demote_reply_encode(uint32_t demoted_count,
                                                              uint8_t *output, size_t capacity,
                                                              uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || demoted_count > AIMEE_DB2_EFFECTIVENESS_DEMOTE_MAX ||
       capacity < AIMEE_DB2_EFFECTIVENESS_DEMOTE_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_EFFECTIVENESS_DEMOTE,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, demoted_count);
   *output_len = AIMEE_DB2_EFFECTIVENESS_DEMOTE_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_effectiveness_demote_reply_decode(const uint8_t *input,
                                                              size_t input_len,
                                                              uint32_t *demoted_count)
{{
   if (demoted_count)
      *demoted_count = 0u;
   if (!demoted_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_EFFECTIVENESS_DEMOTE ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_EFFECTIVENESS_DEMOTE_MAX)
      return -1;
   *demoted_count = decoded;
   return 0;
}}

typedef struct
{{
   double avg_effectiveness;
   uint32_t low_effectiveness_count;
   uint32_t high_impact_count;
}} aimee_db2_effectiveness_stats_t;

static inline int aimee_db2_effectiveness_stats_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_EFFECTIVENESS_STATS, 0u, 0u, output,
                                          capacity);
}}

static inline int aimee_db2_effectiveness_stats_request_decode(const uint8_t *input,
                                                               size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_EFFECTIVENESS_STATS_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_EFFECTIVENESS_STATS &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_effectiveness_stats_reply_encode(
    const aimee_db2_effectiveness_stats_t *stats, uint8_t *output, size_t capacity,
    uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   uint64_t average_bits = 0u;
   if (!stats || !output || !output_len ||
       sizeof(stats->avg_effectiveness) != sizeof(average_bits))
      return -1;
   if (!(stats->avg_effectiveness >= AIMEE_DB2_EFFECTIVENESS_STATS_AVG_MIN) ||
       !(stats->avg_effectiveness <= AIMEE_DB2_EFFECTIVENESS_STATS_AVG_MAX) ||
       stats->low_effectiveness_count > AIMEE_DB2_EFFECTIVENESS_STATS_LOW_MAX ||
       stats->high_impact_count > AIMEE_DB2_EFFECTIVENESS_STATS_HIGH_MAX ||
       capacity < AIMEE_DB2_EFFECTIVENESS_STATS_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_EFFECTIVENESS_STATS,
                                     AIMEE_DB2_RESULT_OK, 16u, output, capacity) != 0)
      return -1;
   memcpy(&average_bits, &stats->avg_effectiveness, sizeof(average_bits));
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u64(payload, average_bits);
   aimee_db2_put_u32(payload + 8u, stats->low_effectiveness_count);
   aimee_db2_put_u32(payload + 12u, stats->high_impact_count);
   *output_len = AIMEE_DB2_EFFECTIVENESS_STATS_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_effectiveness_stats_reply_decode(
    const uint8_t *input, size_t input_len, aimee_db2_effectiveness_stats_t *stats)
{{
   if (stats)
      memset(stats, 0, sizeof(*stats));
   if (!stats || sizeof(stats->avg_effectiveness) != sizeof(uint64_t))
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_EFFECTIVENESS_STATS_RESPONSE_LEN ||
       header.operation != AIMEE_DB2_OPERATION_EFFECTIVENESS_STATS ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 16u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint64_t average_bits = aimee_db2_get_u64(payload);
   uint32_t low = aimee_db2_get_u32(payload + 8u);
   uint32_t high = aimee_db2_get_u32(payload + 12u);
   double average = 0.0;
   memcpy(&average, &average_bits, sizeof(average_bits));
   if (!(average >= AIMEE_DB2_EFFECTIVENESS_STATS_AVG_MIN) ||
       !(average <= AIMEE_DB2_EFFECTIVENESS_STATS_AVG_MAX) ||
       low > AIMEE_DB2_EFFECTIVENESS_STATS_LOW_MAX ||
       high > AIMEE_DB2_EFFECTIVENESS_STATS_HIGH_MAX)
      return -1;
   stats->avg_effectiveness = average;
   stats->low_effectiveness_count = low;
   stats->high_impact_count = high;
   return 0;
}}

static inline int aimee_db2_l2_memory_ids_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_L2_MEMORY_IDS, 0u, 0u, output,
                                          capacity);
}}

static inline int aimee_db2_l2_memory_ids_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_L2_MEMORY_IDS_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_L2_MEMORY_IDS && header.flags == 0u &&
                  header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_l2_memory_ids_reply_encode(const uint64_t *memory_ids, uint32_t count,
                                                       uint8_t *output, size_t capacity,
                                                       uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || (count > 0u && !memory_ids) ||
       count > AIMEE_DB2_L2_MEMORY_IDS_MAX)
      return -1;
   for (uint32_t index = 0u; index < count; index++)
      if (memory_ids[index] < AIMEE_DB2_L2_MEMORY_ID_MIN ||
          memory_ids[index] > AIMEE_DB2_L2_MEMORY_ID_MAX)
         return -1;
   uint32_t payload_len = 4u + count * 8u;
   if (capacity < (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_L2_MEMORY_IDS, AIMEE_DB2_RESULT_OK,
                                     payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, count);
   for (uint32_t index = 0u; index < count; index++)
      aimee_db2_put_u64(payload + 4u + index * 8u, memory_ids[index]);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_l2_memory_ids_reply_decode(const uint8_t *input, size_t input_len,
                                                       uint64_t *memory_ids, uint32_t capacity,
                                                       uint32_t *count)
{{
   if (count)
      *count = 0u;
   if (!count || (capacity > 0u && !memory_ids))
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_L2_MEMORY_IDS ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len < 4u ||
       input_len != (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t decoded = aimee_db2_get_u32(payload);
   if (decoded > AIMEE_DB2_L2_MEMORY_IDS_MAX || header.payload_len != 4u + decoded * 8u ||
       decoded > capacity)
      return -1;
   for (uint32_t index = 0u; index < decoded; index++)
   {{
      uint64_t value = aimee_db2_get_u64(payload + 4u + index * 8u);
      if (value < AIMEE_DB2_L2_MEMORY_ID_MIN || value > AIMEE_DB2_L2_MEMORY_ID_MAX)
         return -1;
      memory_ids[index] = value;
   }}
   *count = decoded;
   return 0;
}}

static inline int aimee_db2_health_record_request_encode(uint32_t promotions, uint32_t demotions,
                                                         uint32_t expirations, uint8_t *output,
                                                         size_t capacity)
{{
   if (!output || promotions > AIMEE_DB2_HEALTH_RECORD_COUNTER_MAX ||
       demotions > AIMEE_DB2_HEALTH_RECORD_COUNTER_MAX ||
       expirations > AIMEE_DB2_HEALTH_RECORD_COUNTER_MAX ||
       capacity < AIMEE_DB2_HEALTH_RECORD_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_HEALTH_RECORD, 0u, 12u, output,
                                       capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, promotions);
   aimee_db2_put_u32(payload + 4u, demotions);
   aimee_db2_put_u32(payload + 8u, expirations);
   return 0;
}}

static inline int aimee_db2_health_record_request_decode(const uint8_t *input, size_t input_len,
                                                         uint32_t *promotions,
                                                         uint32_t *demotions,
                                                         uint32_t *expirations)
{{
   if (promotions)
      *promotions = 0u;
   if (demotions)
      *demotions = 0u;
   if (expirations)
      *expirations = 0u;
   if (!promotions || !demotions || !expirations)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_HEALTH_RECORD_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_HEALTH_RECORD || header.flags != 0u ||
       header.payload_len != 12u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t decoded_promotions = aimee_db2_get_u32(payload);
   uint32_t decoded_demotions = aimee_db2_get_u32(payload + 4u);
   uint32_t decoded_expirations = aimee_db2_get_u32(payload + 8u);
   if (decoded_promotions > AIMEE_DB2_HEALTH_RECORD_COUNTER_MAX ||
       decoded_demotions > AIMEE_DB2_HEALTH_RECORD_COUNTER_MAX ||
       decoded_expirations > AIMEE_DB2_HEALTH_RECORD_COUNTER_MAX)
      return -1;
   *promotions = decoded_promotions;
   *demotions = decoded_demotions;
   *expirations = decoded_expirations;
   return 0;
}}

static inline int aimee_db2_health_record_reply_encode(uint8_t *output, size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_HEALTH_RECORD_RESPONSE_LEN)
      return -1;
   return aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_HEALTH_RECORD, AIMEE_DB2_RESULT_OK, 0u,
                                        output, capacity);
}}

static inline int aimee_db2_health_record_reply_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_reply_header_t header = {{0}};
   return aimee_db2_reply_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_HEALTH_RECORD_RESPONSE_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_HEALTH_RECORD &&
                  header.result == AIMEE_DB2_RESULT_OK && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_health_retention_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_HEALTH_RETENTION, 0u, 0u, output,
                                          capacity);
}}

static inline int aimee_db2_health_retention_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_HEALTH_RETENTION_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_HEALTH_RETENTION &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_health_retention_reply_encode(uint32_t snapshots_deleted,
                                                          uint32_t contradictions_deleted,
                                                          uint8_t *output, size_t capacity,
                                                          uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || snapshots_deleted > AIMEE_DB2_HEALTH_RETENTION_MAX ||
       contradictions_deleted > AIMEE_DB2_HEALTH_RETENTION_MAX ||
       capacity < AIMEE_DB2_HEALTH_RETENTION_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_HEALTH_RETENTION, AIMEE_DB2_RESULT_OK, 8u,
                                     output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, snapshots_deleted);
   aimee_db2_put_u32(payload + 4u, contradictions_deleted);
   *output_len = AIMEE_DB2_HEALTH_RETENTION_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_health_retention_reply_decode(const uint8_t *input, size_t input_len,
                                                          uint32_t *snapshots_deleted,
                                                          uint32_t *contradictions_deleted)
{{
   if (snapshots_deleted)
      *snapshots_deleted = 0u;
   if (contradictions_deleted)
      *contradictions_deleted = 0u;
   if (!snapshots_deleted || !contradictions_deleted)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_HEALTH_RETENTION_RESPONSE_LEN ||
       header.operation != AIMEE_DB2_OPERATION_HEALTH_RETENTION ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 8u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t snapshots = aimee_db2_get_u32(payload);
   uint32_t contradictions = aimee_db2_get_u32(payload + 4u);
   if (snapshots > AIMEE_DB2_HEALTH_RETENTION_MAX ||
       contradictions > AIMEE_DB2_HEALTH_RETENTION_MAX)
      return -1;
   *snapshots_deleted = snapshots;
   *contradictions_deleted = contradictions;
   return 0;
}}

typedef struct
{{
   uint32_t cycles;
   uint32_t total_contradictions;
   uint32_t total_promotions;
   uint32_t total_demotions;
   uint32_t total_expirations;
   uint32_t new_memories;
   uint32_t l1_eligible;
   uint32_t l2_total;
   uint32_t l2_stale_30_days;
}} aimee_db2_health_counters_t;

static inline int aimee_db2_health_counters_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_HEALTH_COUNTERS, 0u, 0u, output,
                                          capacity);
}}

static inline int aimee_db2_health_counters_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_HEALTH_COUNTERS_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_HEALTH_COUNTERS &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_health_counters_reply_encode(const aimee_db2_health_counters_t *counters,
                                                         uint8_t *output, size_t capacity,
                                                         uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!counters || !output || !output_len)
      return -1;
   const uint32_t values[AIMEE_DB2_HEALTH_COUNTERS_FIELDS] = {{
       counters->cycles,           counters->total_contradictions, counters->total_promotions,
       counters->total_demotions,  counters->total_expirations,    counters->new_memories,
       counters->l1_eligible,      counters->l2_total,             counters->l2_stale_30_days,
   }};
   for (uint32_t index = 0u; index < AIMEE_DB2_HEALTH_COUNTERS_FIELDS; index++)
      if (values[index] > AIMEE_DB2_HEALTH_COUNTERS_MAX)
         return -1;
   uint32_t payload_len = 4u * AIMEE_DB2_HEALTH_COUNTERS_FIELDS;
   if (capacity < AIMEE_DB2_HEALTH_COUNTERS_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_HEALTH_COUNTERS, AIMEE_DB2_RESULT_OK,
                                     payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   for (uint32_t index = 0u; index < AIMEE_DB2_HEALTH_COUNTERS_FIELDS; index++)
      aimee_db2_put_u32(payload + index * 4u, values[index]);
   *output_len = AIMEE_DB2_HEALTH_COUNTERS_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_health_counters_reply_decode(const uint8_t *input, size_t input_len,
                                                         aimee_db2_health_counters_t *counters)
{{
   if (counters)
      memset(counters, 0, sizeof(*counters));
   if (!counters)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_HEALTH_COUNTERS_RESPONSE_LEN ||
       header.operation != AIMEE_DB2_OPERATION_HEALTH_COUNTERS ||
       header.result != AIMEE_DB2_RESULT_OK ||
       header.payload_len != 4u * AIMEE_DB2_HEALTH_COUNTERS_FIELDS)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t values[AIMEE_DB2_HEALTH_COUNTERS_FIELDS];
   for (uint32_t index = 0u; index < AIMEE_DB2_HEALTH_COUNTERS_FIELDS; index++)
   {{
      values[index] = aimee_db2_get_u32(payload + index * 4u);
      if (values[index] > AIMEE_DB2_HEALTH_COUNTERS_MAX)
         return -1;
   }}
   counters->cycles = values[0];
   counters->total_contradictions = values[1];
   counters->total_promotions = values[2];
   counters->total_demotions = values[3];
   counters->total_expirations = values[4];
   counters->new_memories = values[5];
   counters->l1_eligible = values[6];
   counters->l2_total = values[7];
   counters->l2_stale_30_days = values[8];
   return 0;
}}

typedef struct
{{
   uint32_t tier_counts[AIMEE_DB2_STATS_COUNTS_TIERS];
   uint32_t kind_counts[AIMEE_DB2_STATS_COUNTS_KINDS];
   uint32_t total;
   uint32_t conflicts;
}} aimee_db2_memory_stats_t;

static inline int aimee_db2_stats_counts_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_STATS_COUNTS, 0u, 0u, output,
                                          capacity);
}}

static inline int aimee_db2_stats_counts_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_STATS_COUNTS_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_STATS_COUNTS && header.flags == 0u &&
                  header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_stats_counts_reply_encode(const aimee_db2_memory_stats_t *stats,
                                                      uint8_t *output, size_t capacity,
                                                      uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!stats || !output || !output_len)
      return -1;
   for (uint32_t index = 0u; index < AIMEE_DB2_STATS_COUNTS_TIERS; index++)
      if (stats->tier_counts[index] > AIMEE_DB2_STATS_COUNTS_MAX)
         return -1;
   for (uint32_t index = 0u; index < AIMEE_DB2_STATS_COUNTS_KINDS; index++)
      if (stats->kind_counts[index] > AIMEE_DB2_STATS_COUNTS_MAX)
         return -1;
   if (stats->total > AIMEE_DB2_STATS_COUNTS_MAX ||
       stats->conflicts > AIMEE_DB2_STATS_COUNTS_MAX)
      return -1;
   uint32_t payload_len = 4u * (AIMEE_DB2_STATS_COUNTS_TIERS + AIMEE_DB2_STATS_COUNTS_KINDS + 2u);
   if (capacity < AIMEE_DB2_STATS_COUNTS_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_STATS_COUNTS, AIMEE_DB2_RESULT_OK,
                                     payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t offset = 0u;
   for (uint32_t index = 0u; index < AIMEE_DB2_STATS_COUNTS_TIERS; index++, offset += 4u)
      aimee_db2_put_u32(payload + offset, stats->tier_counts[index]);
   for (uint32_t index = 0u; index < AIMEE_DB2_STATS_COUNTS_KINDS; index++, offset += 4u)
      aimee_db2_put_u32(payload + offset, stats->kind_counts[index]);
   aimee_db2_put_u32(payload + offset, stats->total);
   aimee_db2_put_u32(payload + offset + 4u, stats->conflicts);
   *output_len = AIMEE_DB2_STATS_COUNTS_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_stats_counts_reply_decode(const uint8_t *input, size_t input_len,
                                                      aimee_db2_memory_stats_t *stats)
{{
   if (stats)
      memset(stats, 0, sizeof(*stats));
   if (!stats)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   uint32_t payload_len = 4u * (AIMEE_DB2_STATS_COUNTS_TIERS + AIMEE_DB2_STATS_COUNTS_KINDS + 2u);
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_STATS_COUNTS_RESPONSE_LEN ||
       header.operation != AIMEE_DB2_OPERATION_STATS_COUNTS ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != payload_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_memory_stats_t decoded = {{0}};
   uint32_t offset = 0u;
   for (uint32_t index = 0u; index < AIMEE_DB2_STATS_COUNTS_TIERS; index++, offset += 4u)
   {{
      decoded.tier_counts[index] = aimee_db2_get_u32(payload + offset);
      if (decoded.tier_counts[index] > AIMEE_DB2_STATS_COUNTS_MAX)
         return -1;
   }}
   for (uint32_t index = 0u; index < AIMEE_DB2_STATS_COUNTS_KINDS; index++, offset += 4u)
   {{
      decoded.kind_counts[index] = aimee_db2_get_u32(payload + offset);
      if (decoded.kind_counts[index] > AIMEE_DB2_STATS_COUNTS_MAX)
         return -1;
   }}
   decoded.total = aimee_db2_get_u32(payload + offset);
   decoded.conflicts = aimee_db2_get_u32(payload + offset + 4u);
   if (decoded.total > AIMEE_DB2_STATS_COUNTS_MAX ||
       decoded.conflicts > AIMEE_DB2_STATS_COUNTS_MAX)
      return -1;
   *stats = decoded;
   return 0;
}}

static inline int aimee_db2_expire_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_EXPIRE, 0u, 0u, output, capacity);
}}

static inline int aimee_db2_expire_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_EXPIRE_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_EXPIRE && header.flags == 0u &&
                  header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_expire_reply_encode(uint32_t level0_deleted,
                                                uint32_t stale_level1_deleted, uint8_t *output,
                                                size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || level0_deleted > AIMEE_DB2_EXPIRE_MAX ||
       stale_level1_deleted > AIMEE_DB2_EXPIRE_MAX ||
       capacity < AIMEE_DB2_EXPIRE_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_EXPIRE, AIMEE_DB2_RESULT_OK, 8u, output,
                                     capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, level0_deleted);
   aimee_db2_put_u32(payload + 4u, stale_level1_deleted);
   *output_len = AIMEE_DB2_EXPIRE_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_expire_reply_decode(const uint8_t *input, size_t input_len,
                                                uint32_t *level0_deleted,
                                                uint32_t *stale_level1_deleted)
{{
   if (level0_deleted)
      *level0_deleted = 0u;
   if (stale_level1_deleted)
      *stale_level1_deleted = 0u;
   if (!level0_deleted || !stale_level1_deleted)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_EXPIRE_RESPONSE_LEN ||
       header.operation != AIMEE_DB2_OPERATION_EXPIRE ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 8u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t level0 = aimee_db2_get_u32(payload);
   uint32_t stale = aimee_db2_get_u32(payload + 4u);
   if (level0 > AIMEE_DB2_EXPIRE_MAX || stale > AIMEE_DB2_EXPIRE_MAX)
      return -1;
   *level0_deleted = level0;
   *stale_level1_deleted = stale;
   return 0;
}}

static inline int aimee_db2_demote_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_DEMOTE, 0u, 0u, output, capacity);
}}

static inline int aimee_db2_demote_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_DEMOTE_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_DEMOTE && header.flags == 0u &&
                  header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_demote_reply_encode(uint32_t demoted_count, uint32_t cascaded_count,
                                                uint8_t *output, size_t capacity,
                                                uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || demoted_count > AIMEE_DB2_DEMOTE_MAX ||
       cascaded_count > AIMEE_DB2_DEMOTE_MAX ||
       (demoted_count == 0u && cascaded_count != 0u) ||
       capacity < AIMEE_DB2_DEMOTE_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_DEMOTE, AIMEE_DB2_RESULT_OK, 8u, output,
                                     capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, demoted_count);
   aimee_db2_put_u32(payload + 4u, cascaded_count);
   *output_len = AIMEE_DB2_DEMOTE_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_demote_reply_decode(const uint8_t *input, size_t input_len,
                                                uint32_t *demoted_count,
                                                uint32_t *cascaded_count)
{{
   if (demoted_count)
      *demoted_count = 0u;
   if (cascaded_count)
      *cascaded_count = 0u;
   if (!demoted_count || !cascaded_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_DEMOTE_RESPONSE_LEN ||
       header.operation != AIMEE_DB2_OPERATION_DEMOTE ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 8u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t demoted = aimee_db2_get_u32(payload);
   uint32_t cascaded = aimee_db2_get_u32(payload + 4u);
   if (demoted > AIMEE_DB2_DEMOTE_MAX || cascaded > AIMEE_DB2_DEMOTE_MAX)
      return -1;
   /* The cascade only runs when something was demoted, so a reply claiming
    * cascaded rows with nothing demoted is malformed. */
   if (demoted == 0u && cascaded != 0u)
      return -1;
   *demoted_count = demoted;
   *cascaded_count = cascaded;
   return 0;
}}

static inline int aimee_db2_promote_stable_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_PROMOTE_STABLE, 0u, 0u, output,
                                          capacity);
}}

static inline int aimee_db2_promote_stable_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_PROMOTE_STABLE_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_PROMOTE_STABLE && header.flags == 0u &&
                  header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_promote_stable_reply_encode(uint32_t promoted_count, uint8_t *output,
                                                        size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || promoted_count > AIMEE_DB2_PROMOTE_STABLE_MAX ||
       capacity < AIMEE_DB2_PROMOTE_STABLE_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_PROMOTE_STABLE, AIMEE_DB2_RESULT_OK, 4u,
                                     output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, promoted_count);
   *output_len = AIMEE_DB2_PROMOTE_STABLE_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_promote_stable_reply_decode(const uint8_t *input, size_t input_len,
                                                        uint32_t *promoted_count)
{{
   if (promoted_count)
      *promoted_count = 0u;
   if (!promoted_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_PROMOTE_STABLE_RESPONSE_LEN ||
       header.operation != AIMEE_DB2_OPERATION_PROMOTE_STABLE ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_PROMOTE_STABLE_MAX)
      return -1;
   *promoted_count = decoded;
   return 0;
}}

static inline int aimee_db2_reclassify_directives_request_encode(uint32_t require_approval,
                                                                 uint8_t *output, size_t capacity)
{{
   if (!output || require_approval > AIMEE_DB2_RECLASSIFY_DIRECTIVES_GATE_MAX ||
       capacity < AIMEE_DB2_RECLASSIFY_DIRECTIVES_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_RECLASSIFY_DIRECTIVES, 0u, 4u, output,
                                       capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, require_approval);
   return 0;
}}

static inline int aimee_db2_reclassify_directives_request_decode(const uint8_t *input,
                                                                 size_t input_len,
                                                                 uint32_t *require_approval)
{{
   if (require_approval)
      *require_approval = 0u;
   if (!require_approval)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_RECLASSIFY_DIRECTIVES_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_RECLASSIFY_DIRECTIVES || header.flags != 0u ||
       header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_RECLASSIFY_DIRECTIVES_GATE_MAX)
      return -1;
   *require_approval = decoded;
   return 0;
}}

static inline int aimee_db2_reclassify_directives_reply_encode(uint32_t reclassified_count,
                                                               uint8_t *output, size_t capacity,
                                                               uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || reclassified_count > AIMEE_DB2_RECLASSIFY_DIRECTIVES_MAX ||
       capacity < AIMEE_DB2_RECLASSIFY_DIRECTIVES_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_RECLASSIFY_DIRECTIVES,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, reclassified_count);
   *output_len = AIMEE_DB2_RECLASSIFY_DIRECTIVES_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_reclassify_directives_reply_decode(const uint8_t *input,
                                                               size_t input_len,
                                                               uint32_t *reclassified_count)
{{
   if (reclassified_count)
      *reclassified_count = 0u;
   if (!reclassified_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_RECLASSIFY_DIRECTIVES_RESPONSE_LEN ||
       header.operation != AIMEE_DB2_OPERATION_RECLASSIFY_DIRECTIVES ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_RECLASSIFY_DIRECTIVES_MAX)
      return -1;
   *reclassified_count = decoded;
   return 0;
}}

static inline int aimee_db2_record_l4_approval_request_encode(uint64_t memory_id,
                                                              const char *approver,
                                                              const char *note, uint8_t *output,
                                                              size_t capacity,
                                                              uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!approver || !note || !output || !output_len)
      return -1;
   size_t approver_len = 0u, note_len = 0u;
   while (approver_len <= AIMEE_DB2_RECORD_L4_APPROVAL_APPROVER_MAX && approver[approver_len])
      ++approver_len;
   while (note_len <= AIMEE_DB2_RECORD_L4_APPROVAL_NOTE_MAX && note[note_len])
      ++note_len;
   size_t payload_len = 16u + approver_len + note_len;
   if (memory_id == 0u || memory_id > AIMEE_DB2_RECORD_L4_APPROVAL_MEMORY_ID_MAX ||
       approver_len == 0u || approver_len > AIMEE_DB2_RECORD_L4_APPROVAL_APPROVER_MAX ||
       note_len > AIMEE_DB2_RECORD_L4_APPROVAL_NOTE_MAX ||
       capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_RECORD_L4_APPROVAL, 0u,
                                       (uint32_t)payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u64(payload, memory_id);
   aimee_db2_put_u32(payload + 8u, (uint32_t)approver_len);
   memcpy(payload + 12u, approver, approver_len);
   aimee_db2_put_u32(payload + 12u + approver_len, (uint32_t)note_len);
   memcpy(payload + 16u + approver_len, note, note_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + (uint32_t)payload_len;
   return 0;
}}

static inline int aimee_db2_record_l4_approval_request_decode(
    const uint8_t *input, size_t input_len, uint64_t *memory_id, char *approver,
    size_t approver_capacity, char *note, size_t note_capacity)
{{
   if (memory_id)
      *memory_id = 0u;
   if (approver && approver_capacity)
      approver[0] = '\\0';
   if (note && note_capacity)
      note[0] = '\\0';
   if (!memory_id || !approver || approver_capacity == 0u || !note || note_capacity == 0u)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_RECORD_L4_APPROVAL || header.flags != 0u ||
       input_len != (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len ||
       header.payload_len < 16u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint64_t decoded_id = aimee_db2_get_u64(payload);
   uint32_t approver_len = aimee_db2_get_u32(payload + 8u);
   if (decoded_id == 0u || decoded_id > AIMEE_DB2_RECORD_L4_APPROVAL_MEMORY_ID_MAX ||
       approver_len == 0u || approver_len > AIMEE_DB2_RECORD_L4_APPROVAL_APPROVER_MAX ||
       header.payload_len < 16u + approver_len)
      return -1;
   uint32_t note_len = aimee_db2_get_u32(payload + 12u + approver_len);
   if (note_len > AIMEE_DB2_RECORD_L4_APPROVAL_NOTE_MAX ||
       header.payload_len != 16u + approver_len + note_len ||
       approver_capacity < (size_t)approver_len + 1u || note_capacity < (size_t)note_len + 1u)
      return -1;
   /* Reject embedded NULs: the backend binds these as C strings. */
   if (memchr(payload + 12u, 0, approver_len) ||
       (note_len && memchr(payload + 16u + approver_len, 0, note_len)))
      return -1;
   memcpy(approver, payload + 12u, approver_len);
   approver[approver_len] = '\\0';
   memcpy(note, payload + 16u + approver_len, note_len);
   note[note_len] = '\\0';
   *memory_id = decoded_id;
   return 0;
}}

static inline int aimee_db2_record_l4_approval_reply_encode(uint8_t *output, size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_RECORD_L4_APPROVAL_RESPONSE_LEN)
      return -1;
   return aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_RECORD_L4_APPROVAL,
                                        AIMEE_DB2_RESULT_OK, 0u, output, capacity);
}}

static inline int aimee_db2_record_l4_approval_reply_decode(const uint8_t *input,
                                                            size_t input_len)
{{
   aimee_db2_reply_header_t header = {{0}};
   return aimee_db2_reply_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_RECORD_L4_APPROVAL_RESPONSE_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_RECORD_L4_APPROVAL &&
                  header.result == AIMEE_DB2_RESULT_OK && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_prune_orphaned_l0_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_PRUNE_ORPHANED_L0, 0u, 0u,
                                           output, capacity);
}}

static inline int aimee_db2_prune_orphaned_l0_request_decode(const uint8_t *input,
                                                             size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_PRUNE_ORPHANED_L0_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_PRUNE_ORPHANED_L0 &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_prune_orphaned_l0_reply_encode(uint32_t deleted_count,
                                                           uint8_t *output, size_t capacity,
                                                           uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || deleted_count > AIMEE_DB2_PRUNE_ORPHANED_L0_COUNT_MAX ||
       capacity < AIMEE_DB2_PRUNE_ORPHANED_L0_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_PRUNE_ORPHANED_L0,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, deleted_count);
   *output_len = AIMEE_DB2_PRUNE_ORPHANED_L0_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_prune_orphaned_l0_reply_decode(const uint8_t *input,
                                                           size_t input_len,
                                                           uint32_t *deleted_count)
{{
   if (deleted_count)
      *deleted_count = 0u;
   if (!deleted_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_PRUNE_ORPHANED_L0 ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_PRUNE_ORPHANED_L0_COUNT_MAX)
      return -1;
   *deleted_count = decoded;
   return 0;
}}

static inline int aimee_db2_directive_sweep_expired_request_encode(uint8_t *output,
                                                                   size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_DIRECTIVE_SWEEP_EXPIRED, 0u, 0u,
                                          output, capacity);
}}

static inline int aimee_db2_directive_sweep_expired_request_decode(const uint8_t *input,
                                                                   size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_DIRECTIVE_SWEEP_EXPIRED &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_directive_sweep_expired_reply_encode(uint32_t directives_expired,
                                                                 uint8_t *output, size_t capacity,
                                                                 uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || directives_expired > AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_MAX ||
       capacity < AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_DIRECTIVE_SWEEP_EXPIRED,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, directives_expired);
   *output_len = AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_directive_sweep_expired_reply_decode(const uint8_t *input,
                                                                 size_t input_len,
                                                                 uint32_t *directives_expired)
{{
   if (directives_expired)
      *directives_expired = 0u;
   if (!directives_expired)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_DIRECTIVE_SWEEP_EXPIRED ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_MAX)
      return -1;
   *directives_expired = decoded;
   return 0;
}}

static inline int aimee_db2_prospective_sweep_expired_request_encode(uint8_t *output,
                                                                     size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_PROSPECTIVE_SWEEP_EXPIRED, 0u, 0u,
                                          output, capacity);
}}

static inline int aimee_db2_prospective_sweep_expired_request_decode(const uint8_t *input,
                                                                     size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_PROSPECTIVE_SWEEP_EXPIRED &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_prospective_sweep_expired_reply_encode(uint32_t expired_count,
                                                                   uint8_t *output,
                                                                   size_t capacity,
                                                                   uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || expired_count > AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_MAX ||
       capacity < AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_PROSPECTIVE_SWEEP_EXPIRED,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, expired_count);
   *output_len = AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_prospective_sweep_expired_reply_decode(const uint8_t *input,
                                                                   size_t input_len,
                                                                   uint32_t *expired_count)
{{
   if (expired_count)
      *expired_count = 0u;
   if (!expired_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_PROSPECTIVE_SWEEP_EXPIRED ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_MAX)
      return -1;
   *expired_count = decoded;
   return 0;
}}

static inline int aimee_db2_requeue_drifted_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_REQUEUE_DRIFTED, 0u, 0u, output,
                                          capacity);
}}

static inline int aimee_db2_requeue_drifted_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_REQUEUE_DRIFTED_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_REQUEUE_DRIFTED &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_requeue_drifted_reply_encode(uint32_t requeued_count, uint8_t *output,
                                                         size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || requeued_count > AIMEE_DB2_REQUEUE_DRIFTED_MAX ||
       capacity < AIMEE_DB2_REQUEUE_DRIFTED_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_REQUEUE_DRIFTED, AIMEE_DB2_RESULT_OK, 4u,
                                     output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, requeued_count);
   *output_len = AIMEE_DB2_REQUEUE_DRIFTED_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_requeue_drifted_reply_decode(const uint8_t *input, size_t input_len,
                                                         uint32_t *requeued_count)
{{
   if (requeued_count)
      *requeued_count = 0u;
   if (!requeued_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_REQUEUE_DRIFTED ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_REQUEUE_DRIFTED_MAX)
      return -1;
   *requeued_count = decoded;
   return 0;
}}

static inline int aimee_db2_purge_hidden_pollution_request_encode(uint8_t *output,
                                                                  size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_PURGE_HIDDEN_POLLUTION, 0u, 0u,
                                          output, capacity);
}}

static inline int aimee_db2_purge_hidden_pollution_request_decode(const uint8_t *input,
                                                                  size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_PURGE_HIDDEN_POLLUTION_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_PURGE_HIDDEN_POLLUTION &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_purge_hidden_pollution_reply_encode(uint32_t purged_count,
                                                                uint8_t *output, size_t capacity,
                                                                uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || purged_count > AIMEE_DB2_PURGE_HIDDEN_POLLUTION_MAX ||
       capacity < AIMEE_DB2_PURGE_HIDDEN_POLLUTION_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_PURGE_HIDDEN_POLLUTION,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, purged_count);
   *output_len = AIMEE_DB2_PURGE_HIDDEN_POLLUTION_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_purge_hidden_pollution_reply_decode(const uint8_t *input,
                                                                size_t input_len,
                                                                uint32_t *purged_count)
{{
   if (purged_count)
      *purged_count = 0u;
   if (!purged_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_PURGE_HIDDEN_POLLUTION ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_PURGE_HIDDEN_POLLUTION_MAX)
      return -1;
   *purged_count = decoded;
   return 0;
}}

static inline int aimee_db2_project_count_request_encode(uint8_t *output, size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_PROJECT_COUNT, 0u, 0u, output,
                                           capacity);
}}

static inline int aimee_db2_project_count_request_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_PROJECT_COUNT_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_PROJECT_COUNT &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_project_count_reply_encode(uint32_t project_count, uint8_t *output,
                                                       size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || project_count > AIMEE_DB2_PROJECT_COUNT_MAX ||
       capacity < AIMEE_DB2_PROJECT_COUNT_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_PROJECT_COUNT, AIMEE_DB2_RESULT_OK, 4u,
                                     output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, project_count);
   *output_len = AIMEE_DB2_PROJECT_COUNT_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_project_count_reply_decode(const uint8_t *input, size_t input_len,
                                                       uint32_t *project_count)
{{
   if (project_count)
      *project_count = 0u;
   if (!project_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_PROJECT_COUNT ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_PROJECT_COUNT_MAX)
      return -1;
   *project_count = decoded;
   return 0;
}}

static inline int aimee_db2_entity_edge_normalize_weights_request_encode(uint8_t *output,
                                                                        size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_ENTITY_EDGE_NORMALIZE_WEIGHTS, 0u,
                                           0u, output, capacity);
}}

static inline int aimee_db2_entity_edge_normalize_weights_request_decode(const uint8_t *input,
                                                                         size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_ENTITY_EDGE_NORMALIZE_WEIGHTS &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_entity_edge_normalize_weights_reply_encode(uint32_t normalized_count,
                                                                       uint8_t *output,
                                                                       size_t capacity,
                                                                       uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len ||
       normalized_count > AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_COUNT_MAX ||
       capacity < AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_ENTITY_EDGE_NORMALIZE_WEIGHTS,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, normalized_count);
   *output_len = AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_entity_edge_normalize_weights_reply_decode(const uint8_t *input,
                                                                       size_t input_len,
                                                                       uint32_t *normalized_count)
{{
   if (normalized_count)
      *normalized_count = 0u;
   if (!normalized_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_ENTITY_EDGE_NORMALIZE_WEIGHTS ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_COUNT_MAX)
      return -1;
   *normalized_count = decoded;
   return 0;
}}

static inline int aimee_db2_entity_edge_prune_orphans_request_encode(uint8_t *output,
                                                                    size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_ENTITY_EDGE_PRUNE_ORPHANS, 0u, 0u,
                                           output, capacity);
}}

static inline int aimee_db2_entity_edge_prune_orphans_request_decode(const uint8_t *input,
                                                                     size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_ENTITY_EDGE_PRUNE_ORPHANS &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_entity_edge_prune_orphans_reply_encode(uint32_t pruned_count,
                                                                   uint8_t *output,
                                                                   size_t capacity,
                                                                   uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len ||
       pruned_count > AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_COUNT_MAX ||
       capacity < AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_ENTITY_EDGE_PRUNE_ORPHANS,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, pruned_count);
   *output_len = AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_entity_edge_prune_orphans_reply_decode(const uint8_t *input,
                                                                   size_t input_len,
                                                                   uint32_t *pruned_count)
{{
   if (pruned_count)
      *pruned_count = 0u;
   if (!pruned_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_ENTITY_EDGE_PRUNE_ORPHANS ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_COUNT_MAX)
      return -1;
   *pruned_count = decoded;
   return 0;
}}

static inline int aimee_db2_count_and_max_updated_request_encode(uint8_t *output,
                                                                size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_COUNT_AND_MAX_UPDATED, 0u, 0u,
                                           output, capacity);
}}

static inline int aimee_db2_count_and_max_updated_request_decode(const uint8_t *input,
                                                                 size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_COUNT_AND_MAX_UPDATED_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_COUNT_AND_MAX_UPDATED &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_count_and_max_updated_reply_encode(uint32_t result, uint32_t count,
                                                               const char *max_updated_at,
                                                               uint8_t *output, size_t capacity,
                                                               uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len)
      return -1;
   if (result == AIMEE_DB2_RESULT_INVALID_STATE)
   {{
      /* An aggregate that could not be computed carries neither number. */
      if (count != 0u || max_updated_at != NULL ||
          capacity < AIMEE_DB2_COUNT_AND_MAX_UPDATED_ERROR_LEN ||
          aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_COUNT_AND_MAX_UPDATED, result, 0u,
                                        output, capacity) != 0)
         return -1;
      *output_len = AIMEE_DB2_COUNT_AND_MAX_UPDATED_ERROR_LEN;
      return 0;
   }}
   if (result != AIMEE_DB2_RESULT_OK || !max_updated_at ||
       count > AIMEE_DB2_COUNT_AND_MAX_UPDATED_COUNT_MAX)
      return -1;
   size_t stamp_len = 0u;
   while (stamp_len <= AIMEE_DB2_COUNT_AND_MAX_UPDATED_STAMP_MAX && max_updated_at[stamp_len])
      ++stamp_len;
   uint32_t payload_len = (uint32_t)(8u + stamp_len);
   if (stamp_len > AIMEE_DB2_COUNT_AND_MAX_UPDATED_STAMP_MAX ||
       capacity < (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_COUNT_AND_MAX_UPDATED,
                                     AIMEE_DB2_RESULT_OK, payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, count);
   aimee_db2_put_u32(payload + 4u, (uint32_t)stamp_len);
   if (stamp_len != 0u)
      memcpy(payload + 8u, max_updated_at, stamp_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_count_and_max_updated_reply_decode(const uint8_t *input,
                                                               size_t input_len,
                                                               uint32_t *result, uint32_t *count,
                                                               char *max_updated_at,
                                                               size_t stamp_capacity)
{{
   if (result)
      *result = 0u;
   if (count)
      *count = 0u;
   if (max_updated_at && stamp_capacity)
      max_updated_at[0] = '\\0';
   if (!result || !count || !max_updated_at ||
       stamp_capacity < (size_t)AIMEE_DB2_COUNT_AND_MAX_UPDATED_STAMP_MAX + 1u)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_COUNT_AND_MAX_UPDATED)
      return -1;
   if (header.result == AIMEE_DB2_RESULT_INVALID_STATE && header.payload_len == 0u &&
       input_len == AIMEE_DB2_COUNT_AND_MAX_UPDATED_ERROR_LEN)
   {{
      *result = header.result;
      return 0;
   }}
   if (header.result != AIMEE_DB2_RESULT_OK || header.payload_len < 8u ||
       (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len != input_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t decoded_count = aimee_db2_get_u32(payload);
   uint32_t stamp_len = aimee_db2_get_u32(payload + 4u);
   if (decoded_count > AIMEE_DB2_COUNT_AND_MAX_UPDATED_COUNT_MAX ||
       stamp_len > AIMEE_DB2_COUNT_AND_MAX_UPDATED_STAMP_MAX ||
       (uint32_t)8u + stamp_len != header.payload_len)
      return -1;
   for (uint32_t index = 0u; index < stamp_len; ++index)
      if (payload[8u + index] == 0u)
         return -1;
   if (stamp_len != 0u)
      memcpy(max_updated_at, payload + 8u, stamp_len);
   max_updated_at[stamp_len] = '\\0';
   *result = header.result;
   *count = decoded_count;
   return 0;
}}

static inline int aimee_db2_pick_first_temporal_ref_request_encode(uint64_t memory_id,
                                                                  uint8_t *output,
                                                                  size_t capacity)
{{
   if (!output || memory_id == 0u ||
       memory_id > AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_MEMORY_ID_MAX ||
       capacity < AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_PICK_FIRST_TEMPORAL_REF, 0u, 8u,
                                       output, capacity) != 0)
      return -1;
   aimee_db2_put_u64(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, memory_id);
   return 0;
}}

static inline int aimee_db2_pick_first_temporal_ref_request_decode(const uint8_t *input,
                                                                   size_t input_len,
                                                                   uint64_t *memory_id)
{{
   if (memory_id)
      *memory_id = 0u;
   if (!memory_id)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_PICK_FIRST_TEMPORAL_REF || header.flags != 0u ||
       header.payload_len != 8u)
      return -1;
   uint64_t decoded = aimee_db2_get_u64(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded == 0u || decoded > AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_MEMORY_ID_MAX)
      return -1;
   *memory_id = decoded;
   return 0;
}}

static inline int aimee_db2_pick_first_temporal_ref_reply_encode(uint32_t result,
                                                                 const char *ref_key,
                                                                 uint8_t *output, size_t capacity,
                                                                 uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len)
      return -1;
   if (result == AIMEE_DB2_RESULT_NOT_FOUND)
   {{
      if (ref_key != NULL || capacity < AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_ERROR_LEN ||
          aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_PICK_FIRST_TEMPORAL_REF, result, 0u,
                                        output, capacity) != 0)
         return -1;
      *output_len = AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_ERROR_LEN;
      return 0;
   }}
   if (result != AIMEE_DB2_RESULT_OK || !ref_key)
      return -1;
   size_t key_len = 0u;
   while (key_len <= AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_KEY_MAX && ref_key[key_len])
      ++key_len;
   uint32_t payload_len = (uint32_t)(4u + key_len);
   if (key_len == 0u || key_len > AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_KEY_MAX ||
       capacity < (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_PICK_FIRST_TEMPORAL_REF,
                                     AIMEE_DB2_RESULT_OK, payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, (uint32_t)key_len);
   memcpy(payload + 4u, ref_key, key_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_pick_first_temporal_ref_reply_decode(const uint8_t *input,
                                                                 size_t input_len,
                                                                 uint32_t *result, char *ref_key,
                                                                 size_t key_capacity)
{{
   if (result)
      *result = 0u;
   if (ref_key && key_capacity)
      ref_key[0] = '\\0';
   if (!result || !ref_key ||
       key_capacity < (size_t)AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_KEY_MAX + 1u)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_PICK_FIRST_TEMPORAL_REF)
      return -1;
   if (header.result == AIMEE_DB2_RESULT_NOT_FOUND && header.payload_len == 0u &&
       input_len == AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_ERROR_LEN)
   {{
      *result = header.result;
      return 0;
   }}
   if (header.result != AIMEE_DB2_RESULT_OK || header.payload_len < 5u ||
       (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len != input_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t key_len = aimee_db2_get_u32(payload);
   if (key_len == 0u || key_len > AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_KEY_MAX ||
       (uint32_t)4u + key_len != header.payload_len)
      return -1;
   for (uint32_t index = 0u; index < key_len; ++index)
      if (payload[4u + index] == 0u)
         return -1;
   memcpy(ref_key, payload + 4u, key_len);
   ref_key[key_len] = '\\0';
   *result = header.result;
   return 0;
}}

static inline int aimee_db2_get_source_session_request_encode(uint64_t memory_id,
                                                             uint8_t *output, size_t capacity)
{{
   if (!output || memory_id == 0u || memory_id > AIMEE_DB2_GET_SOURCE_SESSION_MEMORY_ID_MAX ||
       capacity < AIMEE_DB2_GET_SOURCE_SESSION_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_GET_SOURCE_SESSION, 0u, 8u, output,
                                       capacity) != 0)
      return -1;
   aimee_db2_put_u64(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, memory_id);
   return 0;
}}

static inline int aimee_db2_get_source_session_request_decode(const uint8_t *input,
                                                              size_t input_len,
                                                              uint64_t *memory_id)
{{
   if (memory_id)
      *memory_id = 0u;
   if (!memory_id)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_GET_SOURCE_SESSION_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_GET_SOURCE_SESSION || header.flags != 0u ||
       header.payload_len != 8u)
      return -1;
   uint64_t decoded = aimee_db2_get_u64(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded == 0u || decoded > AIMEE_DB2_GET_SOURCE_SESSION_MEMORY_ID_MAX)
      return -1;
   *memory_id = decoded;
   return 0;
}}

static inline int aimee_db2_get_source_session_reply_encode(uint32_t result,
                                                            const char *session_id,
                                                            uint8_t *output, size_t capacity,
                                                            uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len)
      return -1;
   if (result == AIMEE_DB2_RESULT_NOT_FOUND)
   {{
      if (session_id != NULL || capacity < AIMEE_DB2_GET_SOURCE_SESSION_ERROR_LEN ||
          aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_GET_SOURCE_SESSION, result, 0u,
                                        output, capacity) != 0)
         return -1;
      *output_len = AIMEE_DB2_GET_SOURCE_SESSION_ERROR_LEN;
      return 0;
   }}
   if (result != AIMEE_DB2_RESULT_OK || !session_id)
      return -1;
   size_t session_len = 0u;
   while (session_len <= AIMEE_DB2_GET_SOURCE_SESSION_SESSION_MAX && session_id[session_len])
      ++session_len;
   uint32_t payload_len = (uint32_t)(4u + session_len);
   /* An empty session is never an ok reply: the backend reports it as absent,
    * so encoding one here would claim a distinction it cannot make. */
   if (session_len == 0u || session_len > AIMEE_DB2_GET_SOURCE_SESSION_SESSION_MAX ||
       capacity < (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_GET_SOURCE_SESSION,
                                     AIMEE_DB2_RESULT_OK, payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, (uint32_t)session_len);
   memcpy(payload + 4u, session_id, session_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_get_source_session_reply_decode(const uint8_t *input,
                                                            size_t input_len, uint32_t *result,
                                                            char *session_id,
                                                            size_t session_capacity)
{{
   if (result)
      *result = 0u;
   if (session_id && session_capacity)
      session_id[0] = '\\0';
   if (!result || !session_id ||
       session_capacity < (size_t)AIMEE_DB2_GET_SOURCE_SESSION_SESSION_MAX + 1u)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_GET_SOURCE_SESSION)
      return -1;
   if (header.result == AIMEE_DB2_RESULT_NOT_FOUND && header.payload_len == 0u &&
       input_len == AIMEE_DB2_GET_SOURCE_SESSION_ERROR_LEN)
   {{
      *result = header.result;
      return 0;
   }}
   if (header.result != AIMEE_DB2_RESULT_OK || header.payload_len < 5u ||
       (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len != input_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t session_len = aimee_db2_get_u32(payload);
   if (session_len == 0u || session_len > AIMEE_DB2_GET_SOURCE_SESSION_SESSION_MAX ||
       (uint32_t)4u + session_len != header.payload_len)
      return -1;
   for (uint32_t index = 0u; index < session_len; ++index)
      if (payload[4u + index] == 0u)
         return -1;
   memcpy(session_id, payload + 4u, session_len);
   session_id[session_len] = '\\0';
   *result = header.result;
   return 0;
}}

static inline int aimee_db2_get_content_request_encode(uint64_t memory_id, uint8_t *output,
                                                      size_t capacity)
{{
   if (!output || memory_id == 0u || memory_id > AIMEE_DB2_GET_CONTENT_MEMORY_ID_MAX ||
       capacity < AIMEE_DB2_GET_CONTENT_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_GET_CONTENT, 0u, 8u, output,
                                       capacity) != 0)
      return -1;
   aimee_db2_put_u64(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, memory_id);
   return 0;
}}

static inline int aimee_db2_get_content_request_decode(const uint8_t *input, size_t input_len,
                                                       uint64_t *memory_id)
{{
   if (memory_id)
      *memory_id = 0u;
   if (!memory_id)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_GET_CONTENT_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_GET_CONTENT || header.flags != 0u ||
       header.payload_len != 8u)
      return -1;
   uint64_t decoded = aimee_db2_get_u64(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded == 0u || decoded > AIMEE_DB2_GET_CONTENT_MEMORY_ID_MAX)
      return -1;
   *memory_id = decoded;
   return 0;
}}

static inline int aimee_db2_get_content_reply_encode(uint32_t result, const char *content,
                                                     uint8_t *output, size_t capacity,
                                                     uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len)
      return -1;
   if (result == AIMEE_DB2_RESULT_NOT_FOUND)
   {{
      /* A missing memory carries no content at all. Encoding an empty payload
       * here would make it indistinguishable from a row holding "". */
      if (content != NULL || capacity < AIMEE_DB2_GET_CONTENT_ERROR_LEN ||
          aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_GET_CONTENT, result, 0u, output,
                                        capacity) != 0)
         return -1;
      *output_len = AIMEE_DB2_GET_CONTENT_ERROR_LEN;
      return 0;
   }}
   if (result != AIMEE_DB2_RESULT_OK || !content)
      return -1;
   size_t content_len = 0u;
   while (content_len <= AIMEE_DB2_GET_CONTENT_CONTENT_MAX && content[content_len])
      ++content_len;
   uint32_t payload_len = (uint32_t)(4u + content_len);
   if (content_len > AIMEE_DB2_GET_CONTENT_CONTENT_MAX ||
       capacity < (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_GET_CONTENT, AIMEE_DB2_RESULT_OK,
                                     payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, (uint32_t)content_len);
   if (content_len != 0u)
      memcpy(payload + 4u, content, content_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_get_content_reply_decode(const uint8_t *input, size_t input_len,
                                                     uint32_t *result, char *content,
                                                     size_t content_capacity)
{{
   if (result)
      *result = 0u;
   if (content && content_capacity)
      content[0] = '\\0';
   if (!result || !content ||
       content_capacity < (size_t)AIMEE_DB2_GET_CONTENT_CONTENT_MAX + 1u)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_GET_CONTENT)
      return -1;
   if (header.result == AIMEE_DB2_RESULT_NOT_FOUND && header.payload_len == 0u &&
       input_len == AIMEE_DB2_GET_CONTENT_ERROR_LEN)
   {{
      *result = header.result;
      return 0;
   }}
   if (header.result != AIMEE_DB2_RESULT_OK || header.payload_len < 4u ||
       (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len != input_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t content_len = aimee_db2_get_u32(payload);
   if (content_len > AIMEE_DB2_GET_CONTENT_CONTENT_MAX ||
       (uint32_t)4u + content_len != header.payload_len)
      return -1;
   for (uint32_t index = 0u; index < content_len; ++index)
      if (payload[4u + index] == 0u)
         return -1;
   if (content_len != 0u)
      memcpy(content, payload + 4u, content_len);
   content[content_len] = '\\0';
   *result = header.result;
   return 0;
}}

static inline int aimee_db2_negation_tokens_update_request_encode(uint64_t memory_id,
                                                                 const char *tokens,
                                                                 uint8_t *output, size_t capacity,
                                                                 uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!tokens || !output || !output_len)
      return -1;
   size_t tokens_len = 0u;
   while (tokens_len <= AIMEE_DB2_NEGATION_TOKENS_UPDATE_TOKENS_MAX && tokens[tokens_len])
      ++tokens_len;
   size_t payload_len = 12u + tokens_len;
   /* No lower bound: a memory with no negations legitimately extracts to
    * nothing, and storing that empty result is the point. */
   if (memory_id == 0u || memory_id > AIMEE_DB2_NEGATION_TOKENS_UPDATE_MEMORY_ID_MAX ||
       tokens_len > AIMEE_DB2_NEGATION_TOKENS_UPDATE_TOKENS_MAX ||
       capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_NEGATION_TOKENS_UPDATE, 0u,
                                       (uint32_t)payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u64(payload, memory_id);
   aimee_db2_put_u32(payload + 8u, (uint32_t)tokens_len);
   if (tokens_len != 0u)
      memcpy(payload + 12u, tokens, tokens_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + (uint32_t)payload_len;
   return 0;
}}

static inline int aimee_db2_negation_tokens_update_request_decode(const uint8_t *input,
                                                                  size_t input_len,
                                                                  uint64_t *memory_id,
                                                                  char *tokens,
                                                                  size_t tokens_capacity)
{{
   if (memory_id)
      *memory_id = 0u;
   if (tokens && tokens_capacity)
      tokens[0] = '\\0';
   if (!memory_id || !tokens ||
       tokens_capacity < (size_t)AIMEE_DB2_NEGATION_TOKENS_UPDATE_TOKENS_MAX + 1u)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 || header.flags != 0u ||
       header.operation != AIMEE_DB2_OPERATION_NEGATION_TOKENS_UPDATE ||
       header.payload_len < 12u ||
       (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len != input_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint64_t decoded_memory_id = aimee_db2_get_u64(payload);
   uint32_t tokens_len = aimee_db2_get_u32(payload + 8u);
   if (decoded_memory_id == 0u ||
       decoded_memory_id > AIMEE_DB2_NEGATION_TOKENS_UPDATE_MEMORY_ID_MAX ||
       tokens_len > AIMEE_DB2_NEGATION_TOKENS_UPDATE_TOKENS_MAX ||
       (uint32_t)12u + tokens_len != header.payload_len)
      return -1;
   for (uint32_t index = 0u; index < tokens_len; ++index)
      if (payload[12u + index] == 0u)
         return -1;
   if (tokens_len != 0u)
      memcpy(tokens, payload + 12u, tokens_len);
   tokens[tokens_len] = '\\0';
   *memory_id = decoded_memory_id;
   return 0;
}}

static inline int aimee_db2_negation_tokens_update_reply_encode(uint8_t *output, size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_NEGATION_TOKENS_UPDATE_RESPONSE_LEN)
      return -1;
   return aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_NEGATION_TOKENS_UPDATE,
                                        AIMEE_DB2_RESULT_OK, 0u, output, capacity);
}}

static inline int aimee_db2_negation_tokens_update_reply_decode(const uint8_t *input,
                                                                size_t input_len)
{{
   aimee_db2_reply_header_t header = {{0}};
   return aimee_db2_reply_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_NEGATION_TOKENS_UPDATE_RESPONSE_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_NEGATION_TOKENS_UPDATE &&
                  header.result == AIMEE_DB2_RESULT_OK && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_set_source_session_request_encode(uint64_t memory_id,
                                                             const char *session_id,
                                                             uint8_t *output, size_t capacity,
                                                             uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!session_id || !output || !output_len)
      return -1;
   size_t session_len = 0u;
   while (session_len <= AIMEE_DB2_SET_SOURCE_SESSION_SESSION_MAX && session_id[session_len])
      ++session_len;
   size_t payload_len = 12u + session_len;
   /* No lower bound on the session: an empty value clears the column, which
    * is a real operation rather than a malformed request. */
   if (memory_id == 0u || memory_id > AIMEE_DB2_SET_SOURCE_SESSION_MEMORY_ID_MAX ||
       session_len > AIMEE_DB2_SET_SOURCE_SESSION_SESSION_MAX ||
       capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_SET_SOURCE_SESSION, 0u,
                                       (uint32_t)payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u64(payload, memory_id);
   aimee_db2_put_u32(payload + 8u, (uint32_t)session_len);
   if (session_len != 0u)
      memcpy(payload + 12u, session_id, session_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + (uint32_t)payload_len;
   return 0;
}}

static inline int aimee_db2_set_source_session_request_decode(const uint8_t *input,
                                                              size_t input_len,
                                                              uint64_t *memory_id,
                                                              char *session_id,
                                                              size_t session_capacity)
{{
   if (memory_id)
      *memory_id = 0u;
   if (session_id && session_capacity)
      session_id[0] = '\\0';
   if (!memory_id || !session_id ||
       session_capacity < (size_t)AIMEE_DB2_SET_SOURCE_SESSION_SESSION_MAX + 1u)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 || header.flags != 0u ||
       header.operation != AIMEE_DB2_OPERATION_SET_SOURCE_SESSION || header.payload_len < 12u ||
       (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len != input_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint64_t decoded_memory_id = aimee_db2_get_u64(payload);
   uint32_t session_len = aimee_db2_get_u32(payload + 8u);
   if (decoded_memory_id == 0u ||
       decoded_memory_id > AIMEE_DB2_SET_SOURCE_SESSION_MEMORY_ID_MAX ||
       session_len > AIMEE_DB2_SET_SOURCE_SESSION_SESSION_MAX ||
       (uint32_t)12u + session_len != header.payload_len)
      return -1;
   for (uint32_t index = 0u; index < session_len; ++index)
      if (payload[12u + index] == 0u)
         return -1;
   if (session_len != 0u)
      memcpy(session_id, payload + 12u, session_len);
   session_id[session_len] = '\\0';
   *memory_id = decoded_memory_id;
   return 0;
}}

static inline int aimee_db2_set_source_session_reply_encode(uint8_t *output, size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_SET_SOURCE_SESSION_RESPONSE_LEN)
      return -1;
   return aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_SET_SOURCE_SESSION,
                                        AIMEE_DB2_RESULT_OK, 0u, output, capacity);
}}

static inline int aimee_db2_set_source_session_reply_decode(const uint8_t *input,
                                                            size_t input_len)
{{
   aimee_db2_reply_header_t header = {{0}};
   return aimee_db2_reply_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_SET_SOURCE_SESSION_RESPONSE_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_SET_SOURCE_SESSION &&
                  header.result == AIMEE_DB2_RESULT_OK && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_set_cognified_kind_request_encode(uint64_t memory_id,
                                                             const char *kind, uint8_t *output,
                                                             size_t capacity,
                                                             uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!kind || !output || !output_len)
      return -1;
   size_t kind_len = 0u;
   while (kind_len <= AIMEE_DB2_SET_COGNIFIED_KIND_KIND_MAX && kind[kind_len])
      ++kind_len;
   size_t payload_len = 12u + kind_len;
   if (memory_id == 0u || memory_id > AIMEE_DB2_SET_COGNIFIED_KIND_MEMORY_ID_MAX ||
       kind_len == 0u || kind_len > AIMEE_DB2_SET_COGNIFIED_KIND_KIND_MAX ||
       capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_SET_COGNIFIED_KIND, 0u,
                                       (uint32_t)payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u64(payload, memory_id);
   aimee_db2_put_u32(payload + 8u, (uint32_t)kind_len);
   memcpy(payload + 12u, kind, kind_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + (uint32_t)payload_len;
   return 0;
}}

static inline int aimee_db2_set_cognified_kind_request_decode(const uint8_t *input,
                                                              size_t input_len,
                                                              uint64_t *memory_id, char *kind,
                                                              size_t kind_capacity)
{{
   if (memory_id)
      *memory_id = 0u;
   if (kind && kind_capacity)
      kind[0] = '\\0';
   if (!memory_id || !kind ||
       kind_capacity < (size_t)AIMEE_DB2_SET_COGNIFIED_KIND_KIND_MAX + 1u)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 || header.flags != 0u ||
       header.operation != AIMEE_DB2_OPERATION_SET_COGNIFIED_KIND || header.payload_len < 13u ||
       (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len != input_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint64_t decoded_memory_id = aimee_db2_get_u64(payload);
   uint32_t kind_len = aimee_db2_get_u32(payload + 8u);
   if (decoded_memory_id == 0u ||
       decoded_memory_id > AIMEE_DB2_SET_COGNIFIED_KIND_MEMORY_ID_MAX || kind_len == 0u ||
       kind_len > AIMEE_DB2_SET_COGNIFIED_KIND_KIND_MAX ||
       (uint32_t)12u + kind_len != header.payload_len)
      return -1;
   for (uint32_t index = 0u; index < kind_len; ++index)
      if (payload[12u + index] == 0u)
         return -1;
   memcpy(kind, payload + 12u, kind_len);
   kind[kind_len] = '\\0';
   *memory_id = decoded_memory_id;
   return 0;
}}

static inline int aimee_db2_set_cognified_kind_reply_encode(uint8_t *output, size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_SET_COGNIFIED_KIND_RESPONSE_LEN)
      return -1;
   return aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_SET_COGNIFIED_KIND,
                                        AIMEE_DB2_RESULT_OK, 0u, output, capacity);
}}

static inline int aimee_db2_set_cognified_kind_reply_decode(const uint8_t *input,
                                                            size_t input_len)
{{
   aimee_db2_reply_header_t header = {{0}};
   return aimee_db2_reply_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_SET_COGNIFIED_KIND_RESPONSE_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_SET_COGNIFIED_KIND &&
                  header.result == AIMEE_DB2_RESULT_OK && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_workspace_tag_insert_request_encode(uint64_t memory_id,
                                                               const char *workspace,
                                                               uint8_t *output, size_t capacity,
                                                               uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!workspace || !output || !output_len)
      return -1;
   size_t workspace_len = 0u;
   while (workspace_len <= AIMEE_DB2_WORKSPACE_TAG_INSERT_WORKSPACE_MAX &&
          workspace[workspace_len])
      ++workspace_len;
   size_t payload_len = 12u + workspace_len;
   if (memory_id == 0u || memory_id > AIMEE_DB2_WORKSPACE_TAG_INSERT_MEMORY_ID_MAX ||
       workspace_len == 0u || workspace_len > AIMEE_DB2_WORKSPACE_TAG_INSERT_WORKSPACE_MAX ||
       capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_WORKSPACE_TAG_INSERT, 0u,
                                       (uint32_t)payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u64(payload, memory_id);
   aimee_db2_put_u32(payload + 8u, (uint32_t)workspace_len);
   memcpy(payload + 12u, workspace, workspace_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + (uint32_t)payload_len;
   return 0;
}}

static inline int aimee_db2_workspace_tag_insert_request_decode(const uint8_t *input,
                                                                size_t input_len,
                                                                uint64_t *memory_id,
                                                                char *workspace,
                                                                size_t workspace_capacity)
{{
   if (memory_id)
      *memory_id = 0u;
   if (workspace && workspace_capacity)
      workspace[0] = '\\0';
   if (!memory_id || !workspace ||
       workspace_capacity < (size_t)AIMEE_DB2_WORKSPACE_TAG_INSERT_WORKSPACE_MAX + 1u)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 || header.flags != 0u ||
       header.operation != AIMEE_DB2_OPERATION_WORKSPACE_TAG_INSERT ||
       header.payload_len < 13u ||
       (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len != input_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint64_t decoded_memory_id = aimee_db2_get_u64(payload);
   uint32_t workspace_len = aimee_db2_get_u32(payload + 8u);
   if (decoded_memory_id == 0u ||
       decoded_memory_id > AIMEE_DB2_WORKSPACE_TAG_INSERT_MEMORY_ID_MAX ||
       workspace_len == 0u ||
       workspace_len > AIMEE_DB2_WORKSPACE_TAG_INSERT_WORKSPACE_MAX ||
       (uint32_t)12u + workspace_len != header.payload_len)
      return -1;
   /* An embedded NUL would attribute the memory to a shorter workspace name
    * than the caller sent, which is a different workspace. */
   for (uint32_t index = 0u; index < workspace_len; ++index)
      if (payload[12u + index] == 0u)
         return -1;
   memcpy(workspace, payload + 12u, workspace_len);
   workspace[workspace_len] = '\\0';
   *memory_id = decoded_memory_id;
   return 0;
}}

static inline int aimee_db2_workspace_tag_insert_reply_encode(uint8_t *output, size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_WORKSPACE_TAG_INSERT_RESPONSE_LEN)
      return -1;
   return aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_WORKSPACE_TAG_INSERT,
                                        AIMEE_DB2_RESULT_OK, 0u, output, capacity);
}}

static inline int aimee_db2_workspace_tag_insert_reply_decode(const uint8_t *input,
                                                              size_t input_len)
{{
   aimee_db2_reply_header_t header = {{0}};
   return aimee_db2_reply_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_WORKSPACE_TAG_INSERT_RESPONSE_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_WORKSPACE_TAG_INSERT &&
                  header.result == AIMEE_DB2_RESULT_OK && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_decay_confidence_request_encode(uint64_t memory_id,
                                                           uint8_t *output, size_t capacity)
{{
   if (!output || memory_id == 0u || memory_id > AIMEE_DB2_DECAY_CONFIDENCE_MEMORY_ID_MAX ||
       capacity < AIMEE_DB2_DECAY_CONFIDENCE_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_DECAY_CONFIDENCE, 0u, 8u, output,
                                       capacity) != 0)
      return -1;
   aimee_db2_put_u64(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, memory_id);
   return 0;
}}

static inline int aimee_db2_decay_confidence_request_decode(const uint8_t *input,
                                                            size_t input_len, uint64_t *memory_id)
{{
   if (memory_id)
      *memory_id = 0u;
   if (!memory_id)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_DECAY_CONFIDENCE_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_DECAY_CONFIDENCE || header.flags != 0u ||
       header.payload_len != 8u)
      return -1;
   uint64_t decoded = aimee_db2_get_u64(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded == 0u || decoded > AIMEE_DB2_DECAY_CONFIDENCE_MEMORY_ID_MAX)
      return -1;
   *memory_id = decoded;
   return 0;
}}

static inline int aimee_db2_decay_confidence_reply_encode(uint8_t *output, size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_DECAY_CONFIDENCE_RESPONSE_LEN)
      return -1;
   return aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_DECAY_CONFIDENCE,
                                        AIMEE_DB2_RESULT_OK, 0u, output, capacity);
}}

static inline int aimee_db2_decay_confidence_reply_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_reply_header_t header = {{0}};
   return aimee_db2_reply_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_DECAY_CONFIDENCE_RESPONSE_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_DECAY_CONFIDENCE &&
                  header.result == AIMEE_DB2_RESULT_OK && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_update_content_request_encode(uint64_t memory_id,
                                                         const char *content, uint8_t *output,
                                                         size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!content || !output || !output_len)
      return -1;
   size_t content_len = 0u;
   while (content_len <= AIMEE_DB2_UPDATE_CONTENT_CONTENT_MAX && content[content_len])
      ++content_len;
   size_t payload_len = 12u + content_len;
   if (memory_id == 0u || memory_id > AIMEE_DB2_UPDATE_CONTENT_MEMORY_ID_MAX ||
       content_len == 0u || content_len > AIMEE_DB2_UPDATE_CONTENT_CONTENT_MAX ||
       capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_UPDATE_CONTENT, 0u,
                                       (uint32_t)payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u64(payload, memory_id);
   aimee_db2_put_u32(payload + 8u, (uint32_t)content_len);
   memcpy(payload + 12u, content, content_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + (uint32_t)payload_len;
   return 0;
}}

static inline int aimee_db2_update_content_request_decode(const uint8_t *input, size_t input_len,
                                                          uint64_t *memory_id, char *content,
                                                          size_t content_capacity)
{{
   if (memory_id)
      *memory_id = 0u;
   if (content && content_capacity)
      content[0] = '\\0';
   if (!memory_id || !content ||
       content_capacity < (size_t)AIMEE_DB2_UPDATE_CONTENT_CONTENT_MAX + 1u)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 || header.flags != 0u ||
       header.operation != AIMEE_DB2_OPERATION_UPDATE_CONTENT || header.payload_len < 13u ||
       (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len != input_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint64_t decoded_memory_id = aimee_db2_get_u64(payload);
   uint32_t content_len = aimee_db2_get_u32(payload + 8u);
   if (decoded_memory_id == 0u || decoded_memory_id > AIMEE_DB2_UPDATE_CONTENT_MEMORY_ID_MAX ||
       content_len == 0u || content_len > AIMEE_DB2_UPDATE_CONTENT_CONTENT_MAX ||
       (uint32_t)12u + content_len != header.payload_len)
      return -1;
   /* An embedded NUL would store a shorter row than the caller sent, silently
    * dropping the tail of the text it asked to persist. */
   for (uint32_t index = 0u; index < content_len; ++index)
      if (payload[12u + index] == 0u)
         return -1;
   memcpy(content, payload + 12u, content_len);
   content[content_len] = '\\0';
   *memory_id = decoded_memory_id;
   return 0;
}}

static inline int aimee_db2_update_content_reply_encode(uint32_t updated_rows, uint8_t *output,
                                                        size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || updated_rows > AIMEE_DB2_UPDATE_CONTENT_MAX ||
       capacity < AIMEE_DB2_UPDATE_CONTENT_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_UPDATE_CONTENT, AIMEE_DB2_RESULT_OK, 4u,
                                     output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, updated_rows);
   *output_len = AIMEE_DB2_UPDATE_CONTENT_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_update_content_reply_decode(const uint8_t *input, size_t input_len,
                                                        uint32_t *updated_rows)
{{
   if (updated_rows)
      *updated_rows = 0u;
   if (!updated_rows)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_UPDATE_CONTENT ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_UPDATE_CONTENT_MAX)
      return -1;
   *updated_rows = decoded;
   return 0;
}}

static inline int aimee_db2_reject_request_encode(uint64_t memory_id, uint8_t *output,
                                                 size_t capacity)
{{
   if (!output || memory_id == 0u || memory_id > AIMEE_DB2_REJECT_MEMORY_ID_MAX ||
       capacity < AIMEE_DB2_REJECT_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_REJECT, 0u, 8u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u64(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, memory_id);
   return 0;
}}

static inline int aimee_db2_reject_request_decode(const uint8_t *input, size_t input_len,
                                                  uint64_t *memory_id)
{{
   if (memory_id)
      *memory_id = 0u;
   if (!memory_id)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_REJECT_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_REJECT || header.flags != 0u ||
       header.payload_len != 8u)
      return -1;
   uint64_t decoded = aimee_db2_get_u64(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded == 0u || decoded > AIMEE_DB2_REJECT_MEMORY_ID_MAX)
      return -1;
   *memory_id = decoded;
   return 0;
}}

static inline int aimee_db2_reject_reply_encode(uint8_t *output, size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_REJECT_RESPONSE_LEN)
      return -1;
   return aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_REJECT, AIMEE_DB2_RESULT_OK, 0u,
                                        output, capacity);
}}

static inline int aimee_db2_reject_reply_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_reply_header_t header = {{0}};
   return aimee_db2_reply_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_REJECT_RESPONSE_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_REJECT &&
                  header.result == AIMEE_DB2_RESULT_OK && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_has_scope_type_request_encode(uint64_t memory_id,
                                                         const char *scope_type,
                                                         uint8_t *output, size_t capacity,
                                                         uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!scope_type || !output || !output_len)
      return -1;
   size_t scope_len = 0u;
   while (scope_len <= AIMEE_DB2_HAS_SCOPE_TYPE_SCOPE_MAX && scope_type[scope_len])
      ++scope_len;
   size_t payload_len = 12u + scope_len;
   if (memory_id == 0u || memory_id > AIMEE_DB2_HAS_SCOPE_TYPE_MEMORY_ID_MAX ||
       scope_len == 0u || scope_len > AIMEE_DB2_HAS_SCOPE_TYPE_SCOPE_MAX ||
       capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_HAS_SCOPE_TYPE, 0u,
                                       (uint32_t)payload_len, output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u64(payload, memory_id);
   aimee_db2_put_u32(payload + 8u, (uint32_t)scope_len);
   memcpy(payload + 12u, scope_type, scope_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + (uint32_t)payload_len;
   return 0;
}}

static inline int aimee_db2_has_scope_type_request_decode(const uint8_t *input, size_t input_len,
                                                          uint64_t *memory_id, char *scope_type,
                                                          size_t scope_capacity)
{{
   if (memory_id)
      *memory_id = 0u;
   if (scope_type && scope_capacity)
      scope_type[0] = '\\0';
   if (!memory_id || !scope_type ||
       scope_capacity < (size_t)AIMEE_DB2_HAS_SCOPE_TYPE_SCOPE_MAX + 1u)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 || header.flags != 0u ||
       header.operation != AIMEE_DB2_OPERATION_HAS_SCOPE_TYPE || header.payload_len < 13u ||
       (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len != input_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint64_t decoded_memory_id = aimee_db2_get_u64(payload);
   uint32_t scope_len = aimee_db2_get_u32(payload + 8u);
   if (decoded_memory_id == 0u || decoded_memory_id > AIMEE_DB2_HAS_SCOPE_TYPE_MEMORY_ID_MAX ||
       scope_len == 0u || scope_len > AIMEE_DB2_HAS_SCOPE_TYPE_SCOPE_MAX ||
       (uint32_t)12u + scope_len != header.payload_len)
      return -1;
   for (uint32_t index = 0u; index < scope_len; ++index)
      if (payload[12u + index] == 0u)
         return -1;
   memcpy(scope_type, payload + 12u, scope_len);
   scope_type[scope_len] = '\\0';
   *memory_id = decoded_memory_id;
   return 0;
}}

static inline int aimee_db2_has_scope_type_reply_encode(uint32_t present, uint8_t *output,
                                                        size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || present > AIMEE_DB2_HAS_SCOPE_TYPE_MAX ||
       capacity < AIMEE_DB2_HAS_SCOPE_TYPE_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_HAS_SCOPE_TYPE, AIMEE_DB2_RESULT_OK, 4u,
                                     output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, present);
   *output_len = AIMEE_DB2_HAS_SCOPE_TYPE_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_has_scope_type_reply_decode(const uint8_t *input, size_t input_len,
                                                        uint32_t *present)
{{
   if (present)
      *present = 0u;
   if (!present)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_HAS_SCOPE_TYPE ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_HAS_SCOPE_TYPE_MAX)
      return -1;
   *present = decoded;
   return 0;
}}

static inline int aimee_db2_valid_at_request_encode(uint64_t memory_id, const char *as_of,
                                                   uint8_t *output, size_t capacity,
                                                   uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!as_of || !output || !output_len)
      return -1;
   size_t as_of_len = 0u;
   while (as_of_len <= AIMEE_DB2_VALID_AT_AS_OF_MAX && as_of[as_of_len])
      ++as_of_len;
   size_t payload_len = 12u + as_of_len;
   if (memory_id == 0u || memory_id > AIMEE_DB2_VALID_AT_MEMORY_ID_MAX || as_of_len == 0u ||
       as_of_len > AIMEE_DB2_VALID_AT_AS_OF_MAX ||
       capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_VALID_AT, 0u, (uint32_t)payload_len,
                                       output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u64(payload, memory_id);
   aimee_db2_put_u32(payload + 8u, (uint32_t)as_of_len);
   memcpy(payload + 12u, as_of, as_of_len);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + (uint32_t)payload_len;
   return 0;
}}

static inline int aimee_db2_valid_at_request_decode(const uint8_t *input, size_t input_len,
                                                    uint64_t *memory_id, char *as_of,
                                                    size_t as_of_capacity)
{{
   if (memory_id)
      *memory_id = 0u;
   if (as_of && as_of_capacity)
      as_of[0] = '\\0';
   if (!memory_id || !as_of || as_of_capacity < (size_t)AIMEE_DB2_VALID_AT_AS_OF_MAX + 1u)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 || header.flags != 0u ||
       header.operation != AIMEE_DB2_OPERATION_VALID_AT || header.payload_len < 13u ||
       (size_t)AIMEE_DB2_ENVELOPE_HEADER_LEN + header.payload_len != input_len)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint64_t decoded_memory_id = aimee_db2_get_u64(payload);
   uint32_t as_of_len = aimee_db2_get_u32(payload + 8u);
   if (decoded_memory_id == 0u || decoded_memory_id > AIMEE_DB2_VALID_AT_MEMORY_ID_MAX ||
       as_of_len == 0u || as_of_len > AIMEE_DB2_VALID_AT_AS_OF_MAX ||
       (uint32_t)12u + as_of_len != header.payload_len)
      return -1;
   /* An embedded NUL would truncate the instant on the way into the statement,
    * so the row would be compared against a different moment than the caller
    * asked about. */
   for (uint32_t index = 0u; index < as_of_len; ++index)
      if (payload[12u + index] == 0u)
         return -1;
   memcpy(as_of, payload + 12u, as_of_len);
   as_of[as_of_len] = '\\0';
   *memory_id = decoded_memory_id;
   return 0;
}}

static inline int aimee_db2_valid_at_reply_encode(uint32_t result, uint32_t in_force,
                                                  uint8_t *output, size_t capacity,
                                                  uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len)
      return -1;
   uint32_t payload_len = 0u;
   if (result == AIMEE_DB2_RESULT_OK)
   {{
      if (in_force > AIMEE_DB2_VALID_AT_MAX || capacity < AIMEE_DB2_VALID_AT_RESPONSE_LEN)
         return -1;
      payload_len = 4u;
   }}
   else if (result != AIMEE_DB2_RESULT_INVALID_STATE || in_force != 0u ||
            capacity < AIMEE_DB2_VALID_AT_ERROR_LEN)
      return -1;
   if (aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_VALID_AT, result, payload_len, output,
                                     capacity) != 0)
      return -1;
   if (payload_len != 0u)
      aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, in_force);
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_valid_at_reply_decode(const uint8_t *input, size_t input_len,
                                                  uint32_t *result, uint32_t *in_force)
{{
   if (result)
      *result = 0u;
   if (in_force)
      *in_force = 0u;
   if (!result || !in_force)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_VALID_AT)
      return -1;
   if (header.result == AIMEE_DB2_RESULT_INVALID_STATE && header.payload_len == 0u)
   {{
      *result = header.result;
      return 0;
   }}
   if (header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_VALID_AT_MAX)
      return -1;
   *result = header.result;
   *in_force = decoded;
   return 0;
}}

static inline int aimee_db2_link_delete_request_encode(uint64_t link_id, uint8_t *output,
                                                      size_t capacity)
{{
   if (!output || link_id == 0u || link_id > AIMEE_DB2_LINK_DELETE_LINK_ID_MAX ||
       capacity < AIMEE_DB2_LINK_DELETE_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_LINK_DELETE, 0u, 8u, output,
                                       capacity) != 0)
      return -1;
   aimee_db2_put_u64(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, link_id);
   return 0;
}}

static inline int aimee_db2_link_delete_request_decode(const uint8_t *input, size_t input_len,
                                                       uint64_t *link_id)
{{
   if (link_id)
      *link_id = 0u;
   if (!link_id)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_LINK_DELETE_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_LINK_DELETE || header.flags != 0u ||
       header.payload_len != 8u)
      return -1;
   uint64_t decoded = aimee_db2_get_u64(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded == 0u || decoded > AIMEE_DB2_LINK_DELETE_LINK_ID_MAX)
      return -1;
   *link_id = decoded;
   return 0;
}}

static inline int aimee_db2_link_delete_reply_encode(uint8_t *output, size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_LINK_DELETE_RESPONSE_LEN)
      return -1;
   return aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_LINK_DELETE, AIMEE_DB2_RESULT_OK, 0u,
                                        output, capacity);
}}

static inline int aimee_db2_link_delete_reply_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_reply_header_t header = {{0}};
   return aimee_db2_reply_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_LINK_DELETE_RESPONSE_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_LINK_DELETE &&
                  header.result == AIMEE_DB2_RESULT_OK && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_touch_request_encode(uint64_t memory_id, uint8_t *output,
                                                size_t capacity)
{{
   if (!output || memory_id == 0u || memory_id > AIMEE_DB2_TOUCH_MEMORY_ID_MAX ||
       capacity < AIMEE_DB2_TOUCH_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_TOUCH, 0u, 8u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u64(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, memory_id);
   return 0;
}}

static inline int aimee_db2_touch_request_decode(const uint8_t *input, size_t input_len,
                                                 uint64_t *memory_id)
{{
   if (memory_id)
      *memory_id = 0u;
   if (!memory_id)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_TOUCH_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_TOUCH || header.flags != 0u ||
       header.payload_len != 8u)
      return -1;
   uint64_t decoded = aimee_db2_get_u64(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded == 0u || decoded > AIMEE_DB2_TOUCH_MEMORY_ID_MAX)
      return -1;
   *memory_id = decoded;
   return 0;
}}

static inline int aimee_db2_touch_reply_encode(uint8_t *output, size_t capacity)
{{
   if (!output || capacity < AIMEE_DB2_TOUCH_RESPONSE_LEN)
      return -1;
   return aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_TOUCH, AIMEE_DB2_RESULT_OK, 0u,
                                        output, capacity);
}}

static inline int aimee_db2_touch_reply_decode(const uint8_t *input, size_t input_len)
{{
   aimee_db2_reply_header_t header = {{0}};
   return aimee_db2_reply_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_TOUCH_RESPONSE_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_TOUCH &&
                  header.result == AIMEE_DB2_RESULT_OK && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_delete_row_request_encode(uint64_t memory_id, uint8_t *output,
                                                     size_t capacity)
{{
   if (!output || memory_id == 0u || memory_id > AIMEE_DB2_DELETE_ROW_MEMORY_ID_MAX ||
       capacity < AIMEE_DB2_DELETE_ROW_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_DELETE_ROW, 0u, 8u, output,
                                       capacity) != 0)
      return -1;
   aimee_db2_put_u64(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, memory_id);
   return 0;
}}

static inline int aimee_db2_delete_row_request_decode(const uint8_t *input, size_t input_len,
                                                      uint64_t *memory_id)
{{
   if (memory_id)
      *memory_id = 0u;
   if (!memory_id)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_DELETE_ROW_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_DELETE_ROW || header.flags != 0u ||
       header.payload_len != 8u)
      return -1;
   uint64_t decoded = aimee_db2_get_u64(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded == 0u || decoded > AIMEE_DB2_DELETE_ROW_MEMORY_ID_MAX)
      return -1;
   *memory_id = decoded;
   return 0;
}}

static inline int aimee_db2_delete_row_reply_encode(uint32_t deleted_rows, uint8_t *output,
                                                    size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || deleted_rows > AIMEE_DB2_DELETE_ROW_MAX ||
       capacity < AIMEE_DB2_DELETE_ROW_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_DELETE_ROW, AIMEE_DB2_RESULT_OK, 4u,
                                     output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, deleted_rows);
   *output_len = AIMEE_DB2_DELETE_ROW_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_delete_row_reply_decode(const uint8_t *input, size_t input_len,
                                                    uint32_t *deleted_rows)
{{
   if (deleted_rows)
      *deleted_rows = 0u;
   if (!deleted_rows)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_DELETE_ROW ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_DELETE_ROW_MAX)
      return -1;
   *deleted_rows = decoded;
   return 0;
}}

static inline int aimee_db2_has_workspace_tag_request_encode(uint64_t memory_id,
                                                            uint8_t *output, size_t capacity)
{{
   if (!output || memory_id == 0u || memory_id > AIMEE_DB2_HAS_WORKSPACE_TAG_MEMORY_ID_MAX ||
       capacity < AIMEE_DB2_HAS_WORKSPACE_TAG_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_HAS_WORKSPACE_TAG, 0u, 8u, output,
                                       capacity) != 0)
      return -1;
   aimee_db2_put_u64(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, memory_id);
   return 0;
}}

static inline int aimee_db2_has_workspace_tag_request_decode(const uint8_t *input,
                                                             size_t input_len,
                                                             uint64_t *memory_id)
{{
   if (memory_id)
      *memory_id = 0u;
   if (!memory_id)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_HAS_WORKSPACE_TAG_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_HAS_WORKSPACE_TAG || header.flags != 0u ||
       header.payload_len != 8u)
      return -1;
   uint64_t decoded = aimee_db2_get_u64(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded == 0u || decoded > AIMEE_DB2_HAS_WORKSPACE_TAG_MEMORY_ID_MAX)
      return -1;
   *memory_id = decoded;
   return 0;
}}

static inline int aimee_db2_has_workspace_tag_reply_encode(uint32_t tagged, uint8_t *output,
                                                           size_t capacity,
                                                           uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || tagged > AIMEE_DB2_HAS_WORKSPACE_TAG_MAX ||
       capacity < AIMEE_DB2_HAS_WORKSPACE_TAG_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_HAS_WORKSPACE_TAG,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, tagged);
   *output_len = AIMEE_DB2_HAS_WORKSPACE_TAG_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_has_workspace_tag_reply_decode(const uint8_t *input,
                                                           size_t input_len, uint32_t *tagged)
{{
   if (tagged)
      *tagged = 0u;
   if (!tagged)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_HAS_WORKSPACE_TAG ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_HAS_WORKSPACE_TAG_MAX)
      return -1;
   *tagged = decoded;
   return 0;
}}

static inline int aimee_db2_demote_id_request_encode(uint64_t memory_id, uint8_t *output,
                                                    size_t capacity)
{{
   if (!output || memory_id == 0u || memory_id > AIMEE_DB2_DEMOTE_ID_MEMORY_ID_MAX ||
       capacity < AIMEE_DB2_DEMOTE_ID_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_DEMOTE_ID, 0u, 8u, output,
                                       capacity) != 0)
      return -1;
   aimee_db2_put_u64(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, memory_id);
   return 0;
}}

static inline int aimee_db2_demote_id_request_decode(const uint8_t *input, size_t input_len,
                                                     uint64_t *memory_id)
{{
   if (memory_id)
      *memory_id = 0u;
   if (!memory_id)
      return -1;
   aimee_db2_request_header_t header = {{0}};
   if (aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_DEMOTE_ID_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_DEMOTE_ID || header.flags != 0u ||
       header.payload_len != 8u)
      return -1;
   uint64_t decoded = aimee_db2_get_u64(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded == 0u || decoded > AIMEE_DB2_DEMOTE_ID_MEMORY_ID_MAX)
      return -1;
   *memory_id = decoded;
   return 0;
}}

static inline int aimee_db2_demote_id_reply_encode(uint32_t demoted_count, uint8_t *output,
                                                   size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len || demoted_count > AIMEE_DB2_DEMOTE_ID_COUNT_MAX ||
       capacity < AIMEE_DB2_DEMOTE_ID_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_DEMOTE_ID, AIMEE_DB2_RESULT_OK, 4u,
                                     output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, demoted_count);
   *output_len = AIMEE_DB2_DEMOTE_ID_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_demote_id_reply_decode(const uint8_t *input, size_t input_len,
                                                   uint32_t *demoted_count)
{{
   if (demoted_count)
      *demoted_count = 0u;
   if (!demoted_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_DEMOTE_ID ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_DEMOTE_ID_COUNT_MAX)
      return -1;
   *demoted_count = decoded;
   return 0;
}}

static inline int aimee_db2_lifecycle_sweep_expired_request_encode(uint8_t *output,
                                                                  size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_LIFECYCLE_SWEEP_EXPIRED, 0u, 0u,
                                           output, capacity);
}}

static inline int aimee_db2_lifecycle_sweep_expired_request_decode(const uint8_t *input,
                                                                   size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_LIFECYCLE_SWEEP_EXPIRED &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_lifecycle_sweep_expired_reply_encode(uint32_t archived_count,
                                                                 uint8_t *output, size_t capacity,
                                                                 uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len ||
       archived_count > AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_COUNT_MAX ||
       capacity < AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_RESPONSE_LEN ||
       aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_LIFECYCLE_SWEEP_EXPIRED,
                                     AIMEE_DB2_RESULT_OK, 4u, output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, archived_count);
   *output_len = AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_RESPONSE_LEN;
   return 0;
}}

static inline int aimee_db2_lifecycle_sweep_expired_reply_decode(const uint8_t *input,
                                                                 size_t input_len,
                                                                 uint32_t *archived_count)
{{
   if (archived_count)
      *archived_count = 0u;
   if (!archived_count)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_LIFECYCLE_SWEEP_EXPIRED ||
       header.result != AIMEE_DB2_RESULT_OK || header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_COUNT_MAX)
      return -1;
   *archived_count = decoded;
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

static inline int aimee_db2_reembed_clear_maintenance_valid(
    const aimee_db2_reembed_clear_maintenance_t *status)
{{
   return status && status->was_in_progress <= 1u &&
          status->recorded_dimension <= AIMEE_DB2_REEMBED_DIMENSION_MAX &&
          status->running_dimension >= AIMEE_DB2_REEMBED_DIMENSION_MIN &&
          status->running_dimension <= AIMEE_DB2_REEMBED_DIMENSION_MAX;
}}

static inline int aimee_db2_reembed_clear_maintenance_request_encode(
    uint32_t force, uint8_t *output, size_t capacity)
{{
   if (force > 1u || !output || capacity < AIMEE_DB2_REEMBED_MAINT_CLEAR_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_REEMBED_MAINT_CLEAR, 0u, 4u,
                                       output, capacity) != 0)
      return -1;
   aimee_db2_put_u32(output + AIMEE_DB2_ENVELOPE_HEADER_LEN, force);
   return 0;
}}

static inline int aimee_db2_reembed_clear_maintenance_request_decode(
    const uint8_t *input, size_t input_len, uint32_t *force)
{{
   if (force)
      *force = 0u;
   aimee_db2_request_header_t header = {{0}};
   if (!force || aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_REEMBED_MAINT_CLEAR_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_REEMBED_MAINT_CLEAR || header.flags != 0u ||
       header.payload_len != 4u)
      return -1;
   uint32_t decoded = aimee_db2_get_u32(input + AIMEE_DB2_ENVELOPE_HEADER_LEN);
   if (decoded > 1u)
      return -1;
   *force = decoded;
   return 0;
}}

static inline int aimee_db2_reembed_clear_maintenance_reply_encode(
    uint32_t result, const aimee_db2_reembed_clear_maintenance_t *status, uint8_t *output,
    size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len)
      return -1;
   uint32_t payload_len = 0u;
   if (result == AIMEE_DB2_RESULT_OK || result == AIMEE_DB2_RESULT_CONFLICT)
   {{
      if (!aimee_db2_reembed_clear_maintenance_valid(status) ||
          (result == AIMEE_DB2_RESULT_CONFLICT &&
           (status->recorded_dimension == 0u ||
            status->recorded_dimension == status->running_dimension)) ||
          capacity < AIMEE_DB2_REEMBED_MAINT_CLEAR_RESPONSE_LEN)
         return -1;
      payload_len = 12u;
   }}
   else if (result != AIMEE_DB2_RESULT_INVALID_STATE || status ||
            capacity < AIMEE_DB2_REEMBED_MAINT_CLEAR_ERROR_LEN)
      return -1;
   if (aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_REEMBED_MAINT_CLEAR, result,
                                     payload_len, output, capacity) != 0)
      return -1;
   if (status)
   {{
      uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
      aimee_db2_put_u32(payload, status->was_in_progress);
      aimee_db2_put_u32(payload + 4, status->recorded_dimension);
      aimee_db2_put_u32(payload + 8, status->running_dimension);
   }}
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_reembed_clear_maintenance_reply_decode(
    const uint8_t *input, size_t input_len, uint32_t *result,
    aimee_db2_reembed_clear_maintenance_t *status)
{{
   if (result)
      *result = 0u;
   if (status)
      *status = (aimee_db2_reembed_clear_maintenance_t){{0}};
   if (!result || !status)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_REEMBED_MAINT_CLEAR)
      return -1;
   if (header.result == AIMEE_DB2_RESULT_INVALID_STATE && header.payload_len == 0u)
   {{
      *result = header.result;
      return 0;
   }}
   if ((header.result != AIMEE_DB2_RESULT_OK && header.result != AIMEE_DB2_RESULT_CONFLICT) ||
       header.payload_len != 12u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_reembed_clear_maintenance_t decoded = {{
       .was_in_progress = aimee_db2_get_u32(payload),
       .recorded_dimension = aimee_db2_get_u32(payload + 4),
       .running_dimension = aimee_db2_get_u32(payload + 8),
   }};
   if (!aimee_db2_reembed_clear_maintenance_valid(&decoded) ||
       (header.result == AIMEE_DB2_RESULT_CONFLICT &&
        (decoded.recorded_dimension == 0u ||
         decoded.recorded_dimension == decoded.running_dimension)))
      return -1;
   *result = header.result;
   *status = decoded;
   return 0;
}}

static inline int aimee_db2_embedder_serving_id_request_encode(uint8_t *output,
                                                               size_t capacity)
{{
   return aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_EMBEDDER_SERVING_ID, 0u, 0u,
                                           output, capacity);
}}

static inline int aimee_db2_embedder_serving_id_request_decode(const uint8_t *input,
                                                               size_t input_len)
{{
   aimee_db2_request_header_t header = {{0}};
   return aimee_db2_request_header_decode(input, input_len, &header) == 0 &&
                  input_len == AIMEE_DB2_EMBEDDER_SERVING_ID_REQUEST_LEN &&
                  header.operation == AIMEE_DB2_OPERATION_EMBEDDER_SERVING_ID &&
                  header.flags == 0u && header.payload_len == 0u
              ? 0
              : -1;
}}

static inline int aimee_db2_embedder_serving_id_reply_encode(
    uint32_t result, const char *serving_id, uint8_t *output, size_t capacity,
    uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len)
      return -1;
   uint32_t payload_len = 0u;
   size_t serving_id_len = 0u;
   if (result == AIMEE_DB2_RESULT_OK)
   {{
      if (!serving_id)
         return -1;
      while (serving_id_len <= AIMEE_DB2_EMBEDDER_SERVING_ID_MAX && serving_id[serving_id_len])
         ++serving_id_len;
      if (serving_id_len > AIMEE_DB2_EMBEDDER_SERVING_ID_MAX ||
          capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u + serving_id_len)
         return -1;
      payload_len = 4u + (uint32_t)serving_id_len;
   }}
   else if (result != AIMEE_DB2_RESULT_INVALID_STATE || serving_id ||
            capacity < AIMEE_DB2_EMBEDDER_SERVING_ID_ERROR_LEN)
      return -1;
   if (aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_EMBEDDER_SERVING_ID, result,
                                     payload_len, output, capacity) != 0)
      return -1;
   if (result == AIMEE_DB2_RESULT_OK)
   {{
      uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
      aimee_db2_put_u32(payload, (uint32_t)serving_id_len);
      memcpy(payload + 4, serving_id, serving_id_len);
   }}
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_embedder_serving_id_reply_decode(
    const uint8_t *input, size_t input_len, uint32_t *result, char *serving_id,
    size_t serving_id_capacity)
{{
   if (result)
      *result = 0u;
   if (serving_id && serving_id_capacity)
      serving_id[0] = '\\0';
   if (!result || !serving_id || serving_id_capacity == 0u)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_EMBEDDER_SERVING_ID)
      return -1;
   if (header.result == AIMEE_DB2_RESULT_INVALID_STATE && header.payload_len == 0u)
   {{
      *result = header.result;
      return 0;
   }}
   if (header.result != AIMEE_DB2_RESULT_OK || header.payload_len < 4u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t decoded_len = aimee_db2_get_u32(payload);
   if (decoded_len > AIMEE_DB2_EMBEDDER_SERVING_ID_MAX ||
       header.payload_len != 4u + decoded_len || serving_id_capacity <= decoded_len ||
       memchr(payload + 4, '\\0', decoded_len) != NULL)
      return -1;
   memcpy(serving_id, payload + 4, decoded_len);
   serving_id[decoded_len] = '\\0';
   *result = header.result;
   return 0;
}}

static inline int aimee_db2_dimension_reset_valid(const aimee_db2_dimension_reset_t *status)
{{
   return status && status->recorded_dimension <= AIMEE_DB2_REEMBED_DIMENSION_MAX &&
          status->target_dimension >= AIMEE_DB2_REEMBED_DIMENSION_MIN &&
          status->target_dimension <= AIMEE_DB2_REEMBED_DIMENSION_MAX &&
          status->tables_discovered <= AIMEE_DB2_DIMENSION_RESET_TABLES_MAX &&
          status->tables_dropped <= status->tables_discovered &&
          status->curator_requeued >= -1 && status->evidence_requeued >= -1;
}}

static inline int aimee_db2_dimension_reset_request_encode(
    uint32_t target_dimension, uint32_t force, uint32_t dry_run, uint8_t *output,
    size_t capacity)
{{
   if (target_dimension < AIMEE_DB2_REEMBED_DIMENSION_MIN ||
       target_dimension > AIMEE_DB2_REEMBED_DIMENSION_MAX || force > 1u || dry_run > 1u ||
       !output || capacity < AIMEE_DB2_DIMENSION_RESET_REQUEST_LEN ||
       aimee_db2_request_header_encode(AIMEE_DB2_OPERATION_DIMENSION_RESET, 0u, 12u,
                                       output, capacity) != 0)
      return -1;
   uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_put_u32(payload, target_dimension);
   aimee_db2_put_u32(payload + 4, force);
   aimee_db2_put_u32(payload + 8, dry_run);
   return 0;
}}

static inline int aimee_db2_dimension_reset_request_decode(
    const uint8_t *input, size_t input_len, uint32_t *target_dimension, uint32_t *force,
    uint32_t *dry_run)
{{
   if (target_dimension)
      *target_dimension = 0u;
   if (force)
      *force = 0u;
   if (dry_run)
      *dry_run = 0u;
   aimee_db2_request_header_t header = {{0}};
   if (!target_dimension || !force || !dry_run ||
       aimee_db2_request_header_decode(input, input_len, &header) != 0 ||
       input_len != AIMEE_DB2_DIMENSION_RESET_REQUEST_LEN ||
       header.operation != AIMEE_DB2_OPERATION_DIMENSION_RESET || header.flags != 0u ||
       header.payload_len != 12u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   uint32_t decoded_target = aimee_db2_get_u32(payload);
   uint32_t decoded_force = aimee_db2_get_u32(payload + 4);
   uint32_t decoded_dry_run = aimee_db2_get_u32(payload + 8);
   if (decoded_target < AIMEE_DB2_REEMBED_DIMENSION_MIN ||
       decoded_target > AIMEE_DB2_REEMBED_DIMENSION_MAX || decoded_force > 1u ||
       decoded_dry_run > 1u)
      return -1;
   *target_dimension = decoded_target;
   *force = decoded_force;
   *dry_run = decoded_dry_run;
   return 0;
}}

static inline int aimee_db2_dimension_reset_reply_encode(
    uint32_t result, const aimee_db2_dimension_reset_t *status, uint8_t *output,
    size_t capacity, uint32_t *output_len)
{{
   if (output_len)
      *output_len = 0u;
   if (!output || !output_len)
      return -1;
   uint32_t payload_len = 0u;
   if (result == AIMEE_DB2_RESULT_OK || result == AIMEE_DB2_RESULT_CONFLICT ||
       result == AIMEE_DB2_RESULT_DENIED)
   {{
      if (!aimee_db2_dimension_reset_valid(status) ||
          capacity < AIMEE_DB2_DIMENSION_RESET_RESPONSE_LEN)
         return -1;
      payload_len = 32u;
   }}
   else if (result != AIMEE_DB2_RESULT_INVALID_STATE || status ||
            capacity < AIMEE_DB2_DIMENSION_RESET_ERROR_LEN)
      return -1;
   if (aimee_db2_reply_header_encode(AIMEE_DB2_OPERATION_DIMENSION_RESET, result, payload_len,
                                     output, capacity) != 0)
      return -1;
   if (status)
   {{
      uint8_t *payload = output + AIMEE_DB2_ENVELOPE_HEADER_LEN;
      aimee_db2_put_u32(payload, status->recorded_dimension);
      aimee_db2_put_u32(payload + 4, status->target_dimension);
      aimee_db2_put_u32(payload + 8, status->tables_discovered);
      aimee_db2_put_u32(payload + 12, status->tables_dropped);
      aimee_db2_put_u64(payload + 16, status->rows_cleared);
      aimee_db2_put_u32(payload + 24, (uint32_t)status->curator_requeued);
      aimee_db2_put_u32(payload + 28, (uint32_t)status->evidence_requeued);
   }}
   *output_len = AIMEE_DB2_ENVELOPE_HEADER_LEN + payload_len;
   return 0;
}}

static inline int aimee_db2_dimension_reset_reply_decode(
    const uint8_t *input, size_t input_len, uint32_t *result,
    aimee_db2_dimension_reset_t *status)
{{
   if (result)
      *result = 0u;
   if (status)
      *status = (aimee_db2_dimension_reset_t){{0}};
   if (!result || !status)
      return -1;
   aimee_db2_reply_header_t header = {{0}};
   if (aimee_db2_reply_header_decode(input, input_len, &header) != 0 ||
       header.operation != AIMEE_DB2_OPERATION_DIMENSION_RESET)
      return -1;
   if (header.result == AIMEE_DB2_RESULT_INVALID_STATE && header.payload_len == 0u)
   {{
      *result = header.result;
      return 0;
   }}
   if ((header.result != AIMEE_DB2_RESULT_OK && header.result != AIMEE_DB2_RESULT_CONFLICT &&
        header.result != AIMEE_DB2_RESULT_DENIED) ||
       header.payload_len != 32u)
      return -1;
   const uint8_t *payload = input + AIMEE_DB2_ENVELOPE_HEADER_LEN;
   aimee_db2_dimension_reset_t decoded = {{
       .recorded_dimension = aimee_db2_get_u32(payload),
       .target_dimension = aimee_db2_get_u32(payload + 4),
       .tables_discovered = aimee_db2_get_u32(payload + 8),
       .tables_dropped = aimee_db2_get_u32(payload + 12),
       .rows_cleared = aimee_db2_get_u64(payload + 16),
       .curator_requeued = (int32_t)aimee_db2_get_u32(payload + 24),
       .evidence_requeued = (int32_t)aimee_db2_get_u32(payload + 28),
   }};
   if (!aimee_db2_dimension_reset_valid(&decoded))
      return -1;
   *result = header.result;
   *status = decoded;
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

   aimee_module_call_result_t aimee_db2_level3_count_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_level2_count_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_orphaned_l0_count_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_total_count_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t *count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_session_l2_count_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       const char *source_session, uint32_t *count,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_key_exists_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       const char *key, uint32_t *exists, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

   aimee_module_call_result_t aimee_db2_find_id_by_key_kind_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       const char *key, const char *kind, uint32_t *found, uint64_t *id,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_key_exists_in_tier_pair_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       const char *key, const char *tier_a, const char *tier_b, uint32_t *exists,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_effectiveness_update_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, uint32_t has_value, double value, uint32_t *domain_result,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_retention_enforce_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *deleted_count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_effectiveness_demote_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *demoted_count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_effectiveness_stats_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       aimee_db2_effectiveness_stats_t *stats, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

   aimee_module_call_result_t aimee_db2_l2_memory_ids_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t *memory_ids, uint32_t capacity, uint32_t *count,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_health_record_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t promotions, uint32_t demotions, uint32_t expirations,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_health_retention_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *snapshots_deleted, uint32_t *contradictions_deleted,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_health_counters_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       aimee_db2_health_counters_t *counters, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

   aimee_module_call_result_t aimee_db2_stats_counts_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       aimee_db2_memory_stats_t *stats, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_expire_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *level0_deleted, uint32_t *stale_level1_deleted,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_demote_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *demoted_count, uint32_t *cascaded_count, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

   aimee_module_call_result_t aimee_db2_promote_stable_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *promoted_count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_reclassify_directives_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t require_approval, uint32_t *reclassified_count,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_record_l4_approval_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, const char *approver, const char *note,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_prune_orphaned_l0_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *deleted_count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_lifecycle_sweep_expired_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *archived_count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_demote_id_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, uint32_t *demoted_count, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

   aimee_module_call_result_t aimee_db2_has_workspace_tag_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, uint32_t *tagged, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

   aimee_module_call_result_t aimee_db2_delete_row_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, uint32_t *deleted_rows, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

   aimee_module_call_result_t aimee_db2_touch_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_link_delete_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t link_id, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_valid_at_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, const char *as_of, uint32_t *domain_result, uint32_t *in_force,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_has_scope_type_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, const char *scope_type, uint32_t *present,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_reject_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_update_content_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, const char *content, uint32_t *updated_rows,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_decay_confidence_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_workspace_tag_insert_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, const char *workspace, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

   aimee_module_call_result_t aimee_db2_set_cognified_kind_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, const char *kind, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

   aimee_module_call_result_t aimee_db2_set_source_session_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, const char *session_id, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

   aimee_module_call_result_t aimee_db2_negation_tokens_update_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, const char *tokens, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

   aimee_module_call_result_t aimee_db2_get_content_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, uint32_t *domain_result, char *content, size_t content_capacity,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_get_source_session_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, uint32_t *domain_result, char *session_id, size_t session_capacity,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_pick_first_temporal_ref_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint64_t memory_id, uint32_t *domain_result, char *ref_key, size_t key_capacity,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_count_and_max_updated_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *domain_result, uint32_t *count, char *max_updated_at, size_t stamp_capacity,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_entity_edge_prune_orphans_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *pruned_count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_entity_edge_normalize_weights_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *normalized_count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_project_count_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *project_count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_purge_hidden_pollution_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *purged_count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_requeue_drifted_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *requeued_count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_prospective_sweep_expired_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *expired_count, aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_directive_sweep_expired_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *directives_expired, aimee_module_cancelled_fn cancelled, void *cancel_context);

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

   aimee_module_call_result_t aimee_db2_reembed_clear_maintenance_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t force, uint32_t *domain_result, aimee_db2_reembed_clear_maintenance_t *status,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_embedder_serving_id_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t *domain_result, char *serving_id, size_t serving_id_capacity,
       aimee_module_cancelled_fn cancelled, void *cancel_context);

   aimee_module_call_result_t aimee_db2_dimension_reset_call(
       aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
       uint32_t target_dimension, uint32_t force, uint32_t dry_run, uint32_t *domain_result,
       aimee_db2_dimension_reset_t *status, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

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

aimee_module_call_result_t aimee_db2_level3_count_call(aimee_db2_call_fn call, void *call_context,
                                                       uint64_t trace_id, uint64_t deadline_ns,
                                                       uint32_t *count,
                                                       aimee_module_cancelled_fn cancelled,
                                                       void *cancel_context)
{
   if (count)
      *count = 0u;
   if (!call || !count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_LEVEL3_COUNT_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_LEVEL3_COUNT_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_level3_count_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_LEVEL3_COUNT, AIMEE_DB2_STAGE_LEVEL3_COUNT, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_level3_count_reply_decode(response, response_len, count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_level2_count_call(aimee_db2_call_fn call, void *call_context,
                                                       uint64_t trace_id, uint64_t deadline_ns,
                                                       uint32_t *count,
                                                       aimee_module_cancelled_fn cancelled,
                                                       void *cancel_context)
{
   if (count)
      *count = 0u;
   if (!call || !count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_LEVEL2_COUNT_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_LEVEL2_COUNT_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_level2_count_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_LEVEL2_COUNT, AIMEE_DB2_STAGE_LEVEL2_COUNT, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_level2_count_reply_decode(response, response_len, count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_orphaned_l0_count_call(aimee_db2_call_fn call,
                                                            void *call_context, uint64_t trace_id,
                                                            uint64_t deadline_ns, uint32_t *count,
                                                            aimee_module_cancelled_fn cancelled,
                                                            void *cancel_context)
{
   if (count)
      *count = 0u;
   if (!call || !count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_ORPHANED_L0_COUNT_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_ORPHANED_L0_COUNT_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_orphaned_l0_count_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_ORPHANED_L0_COUNT, AIMEE_DB2_STAGE_ORPHANED_L0_COUNT,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_orphaned_l0_count_reply_decode(response, response_len, count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_total_count_call(aimee_db2_call_fn call, void *call_context,
                                                      uint64_t trace_id, uint64_t deadline_ns,
                                                      uint64_t *count,
                                                      aimee_module_cancelled_fn cancelled,
                                                      void *cancel_context)
{
   if (count)
      *count = 0u;
   if (!call || !count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_TOTAL_COUNT_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_TOTAL_COUNT_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_total_count_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_TOTAL_COUNT, AIMEE_DB2_STAGE_TOTAL_COUNT, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_total_count_reply_decode(response, response_len, count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_session_l2_count_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                uint64_t deadline_ns, const char *source_session, uint32_t *count,
                                aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (count)
      *count = 0u;
   if (!call || !source_session || !count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_SESSION_L2_COUNT_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_SESSION_L2_COUNT_RESPONSE_LEN];
   uint32_t request_len = 0u, response_len = 0u;
   if (aimee_db2_session_l2_count_request_encode(source_session, request, sizeof(request),
                                                 &request_len) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_SESSION_L2_COUNT, AIMEE_DB2_STAGE_SESSION_L2_COUNT,
            trace_id, deadline_ns, request, request_len, response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_session_l2_count_reply_decode(response, response_len, count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_key_exists_call(aimee_db2_call_fn call, void *call_context,
                                                     uint64_t trace_id, uint64_t deadline_ns,
                                                     const char *key, uint32_t *exists,
                                                     aimee_module_cancelled_fn cancelled,
                                                     void *cancel_context)
{
   if (exists)
      *exists = 0u;
   if (!call || !key || !exists)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_KEY_EXISTS_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_KEY_EXISTS_RESPONSE_LEN];
   uint32_t request_len = 0u, response_len = 0u;
   if (aimee_db2_key_exists_request_encode(key, request, sizeof(request), &request_len) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport = call(
       call_context, AIMEE_DB2_EVENT_KEY_EXISTS, AIMEE_DB2_STAGE_KEY_EXISTS, trace_id, deadline_ns,
       request, request_len, response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_key_exists_reply_decode(response, response_len, exists) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_find_id_by_key_kind_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                   uint64_t deadline_ns, const char *key, const char *kind,
                                   uint32_t *found, uint64_t *id,
                                   aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (found)
      *found = 0u;
   if (id)
      *id = 0u;
   if (!call || !key || !kind || !found || !id)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_FIND_ID_BY_KEY_KIND_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_FIND_ID_BY_KEY_KIND_RESPONSE_LEN];
   uint32_t request_len = 0u, response_len = 0u;
   if (aimee_db2_find_id_by_key_kind_request_encode(key, kind, request, sizeof(request),
                                                    &request_len) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_FIND_ID_BY_KEY_KIND, AIMEE_DB2_STAGE_FIND_ID_BY_KEY_KIND,
            trace_id, deadline_ns, request, request_len, response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_find_id_by_key_kind_reply_decode(response, response_len, found, id) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_key_exists_in_tier_pair_call(aimee_db2_call_fn call, void *call_context,
                                       uint64_t trace_id, uint64_t deadline_ns, const char *key,
                                       const char *tier_a, const char *tier_b, uint32_t *exists,
                                       aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (exists)
      *exists = 0u;
   if (!call || !key || !tier_a || !tier_b || !exists)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_RESPONSE_LEN];
   uint32_t request_len = 0u, response_len = 0u;
   if (aimee_db2_key_exists_in_tier_pair_request_encode(key, tier_a, tier_b, request,
                                                        sizeof(request), &request_len) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_KEY_EXISTS_IN_TIER_PAIR,
            AIMEE_DB2_STAGE_KEY_EXISTS_IN_TIER_PAIR, trace_id, deadline_ns, request, request_len,
            response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_key_exists_in_tier_pair_reply_decode(response, response_len, exists) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_effectiveness_update_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                    uint64_t deadline_ns, uint64_t memory_id, uint32_t has_value,
                                    double value, uint32_t *domain_result,
                                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (domain_result)
      *domain_result = 0u;
   if (!call || !domain_result)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_EFFECTIVENESS_UPDATE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EFFECTIVENESS_UPDATE_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_effectiveness_update_request_encode(memory_id, has_value, value, request,
                                                     sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_EFFECTIVENESS_UPDATE,
            AIMEE_DB2_STAGE_EFFECTIVENESS_UPDATE, trace_id, deadline_ns, request, sizeof(request),
            response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_effectiveness_update_reply_decode(response, response_len, domain_result) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_retention_enforce_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                 uint64_t deadline_ns, uint32_t *deleted_count,
                                 aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (deleted_count)
      *deleted_count = 0u;
   if (!call || !deleted_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_RETENTION_ENFORCE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_RETENTION_ENFORCE_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_retention_enforce_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_RETENTION_ENFORCE, AIMEE_DB2_STAGE_RETENTION_ENFORCE,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_retention_enforce_reply_decode(response, response_len, deleted_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_effectiveness_demote_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                    uint64_t deadline_ns, uint32_t *demoted_count,
                                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (demoted_count)
      *demoted_count = 0u;
   if (!call || !demoted_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_EFFECTIVENESS_DEMOTE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EFFECTIVENESS_DEMOTE_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_effectiveness_demote_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_EFFECTIVENESS_DEMOTE,
            AIMEE_DB2_STAGE_EFFECTIVENESS_DEMOTE, trace_id, deadline_ns, request, sizeof(request),
            response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_effectiveness_demote_reply_decode(response, response_len, demoted_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_effectiveness_stats_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                   uint64_t deadline_ns, aimee_db2_effectiveness_stats_t *stats,
                                   aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (stats)
      memset(stats, 0, sizeof(*stats));
   if (!call || !stats)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_EFFECTIVENESS_STATS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EFFECTIVENESS_STATS_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_effectiveness_stats_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_EFFECTIVENESS_STATS, AIMEE_DB2_STAGE_EFFECTIVENESS_STATS,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_effectiveness_stats_reply_decode(response, response_len, stats) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_l2_memory_ids_call(aimee_db2_call_fn call, void *call_context,
                                                        uint64_t trace_id, uint64_t deadline_ns,
                                                        uint64_t *memory_ids, uint32_t capacity,
                                                        uint32_t *count,
                                                        aimee_module_cancelled_fn cancelled,
                                                        void *cancel_context)
{
   if (count)
      *count = 0u;
   if (!call || !count || (capacity > 0u && !memory_ids))
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_L2_MEMORY_IDS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_L2_MEMORY_IDS_RESPONSE_MAX_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_l2_memory_ids_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_L2_MEMORY_IDS, AIMEE_DB2_STAGE_L2_MEMORY_IDS, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_l2_memory_ids_reply_decode(response, response_len, memory_ids, capacity, count) !=
       0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_health_record_call(aimee_db2_call_fn call, void *call_context,
                                                        uint64_t trace_id, uint64_t deadline_ns,
                                                        uint32_t promotions, uint32_t demotions,
                                                        uint32_t expirations,
                                                        aimee_module_cancelled_fn cancelled,
                                                        void *cancel_context)
{
   if (!call)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_HEALTH_RECORD_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_HEALTH_RECORD_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_health_record_request_encode(promotions, demotions, expirations, request,
                                              sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_HEALTH_RECORD, AIMEE_DB2_STAGE_HEALTH_RECORD, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_health_record_reply_decode(response, response_len) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_health_retention_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                uint64_t deadline_ns, uint32_t *snapshots_deleted,
                                uint32_t *contradictions_deleted,
                                aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (snapshots_deleted)
      *snapshots_deleted = 0u;
   if (contradictions_deleted)
      *contradictions_deleted = 0u;
   if (!call || !snapshots_deleted || !contradictions_deleted)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_HEALTH_RETENTION_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_HEALTH_RETENTION_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_health_retention_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_HEALTH_RETENTION, AIMEE_DB2_STAGE_HEALTH_RETENTION,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_health_retention_reply_decode(response, response_len, snapshots_deleted,
                                               contradictions_deleted) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_health_counters_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                               uint64_t deadline_ns, aimee_db2_health_counters_t *counters,
                               aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (counters)
      memset(counters, 0, sizeof(*counters));
   if (!call || !counters)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_HEALTH_COUNTERS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_HEALTH_COUNTERS_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_health_counters_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_HEALTH_COUNTERS, AIMEE_DB2_STAGE_HEALTH_COUNTERS,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_health_counters_reply_decode(response, response_len, counters) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_stats_counts_call(aimee_db2_call_fn call, void *call_context,
                                                       uint64_t trace_id, uint64_t deadline_ns,
                                                       aimee_db2_memory_stats_t *stats,
                                                       aimee_module_cancelled_fn cancelled,
                                                       void *cancel_context)
{
   if (stats)
      memset(stats, 0, sizeof(*stats));
   if (!call || !stats)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_STATS_COUNTS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_STATS_COUNTS_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_stats_counts_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_STATS_COUNTS, AIMEE_DB2_STAGE_STATS_COUNTS, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_stats_counts_reply_decode(response, response_len, stats) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_expire_call(aimee_db2_call_fn call, void *call_context,
                                                 uint64_t trace_id, uint64_t deadline_ns,
                                                 uint32_t *level0_deleted,
                                                 uint32_t *stale_level1_deleted,
                                                 aimee_module_cancelled_fn cancelled,
                                                 void *cancel_context)
{
   if (level0_deleted)
      *level0_deleted = 0u;
   if (stale_level1_deleted)
      *stale_level1_deleted = 0u;
   if (!call || !level0_deleted || !stale_level1_deleted)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_EXPIRE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EXPIRE_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_expire_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport = call(
       call_context, AIMEE_DB2_EVENT_EXPIRE, AIMEE_DB2_STAGE_EXPIRE, trace_id, deadline_ns, request,
       sizeof(request), response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_expire_reply_decode(response, response_len, level0_deleted,
                                     stale_level1_deleted) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_demote_call(aimee_db2_call_fn call, void *call_context,
                                                 uint64_t trace_id, uint64_t deadline_ns,
                                                 uint32_t *demoted_count, uint32_t *cascaded_count,
                                                 aimee_module_cancelled_fn cancelled,
                                                 void *cancel_context)
{
   if (demoted_count)
      *demoted_count = 0u;
   if (cascaded_count)
      *cascaded_count = 0u;
   if (!call || !demoted_count || !cascaded_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_DEMOTE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DEMOTE_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_demote_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport = call(
       call_context, AIMEE_DB2_EVENT_DEMOTE, AIMEE_DB2_STAGE_DEMOTE, trace_id, deadline_ns, request,
       sizeof(request), response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_demote_reply_decode(response, response_len, demoted_count, cascaded_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_promote_stable_call(aimee_db2_call_fn call, void *call_context,
                                                         uint64_t trace_id, uint64_t deadline_ns,
                                                         uint32_t *promoted_count,
                                                         aimee_module_cancelled_fn cancelled,
                                                         void *cancel_context)
{
   if (promoted_count)
      *promoted_count = 0u;
   if (!call || !promoted_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_PROMOTE_STABLE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PROMOTE_STABLE_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_promote_stable_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_PROMOTE_STABLE, AIMEE_DB2_STAGE_PROMOTE_STABLE, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_promote_stable_reply_decode(response, response_len, promoted_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_reclassify_directives_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                     uint64_t deadline_ns, uint32_t require_approval,
                                     uint32_t *reclassified_count,
                                     aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (reclassified_count)
      *reclassified_count = 0u;
   if (!call || !reclassified_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_RECLASSIFY_DIRECTIVES_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_RECLASSIFY_DIRECTIVES_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_reclassify_directives_request_encode(require_approval, request, sizeof(request)) !=
       0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_RECLASSIFY_DIRECTIVES,
            AIMEE_DB2_STAGE_RECLASSIFY_DIRECTIVES, trace_id, deadline_ns, request, sizeof(request),
            response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_reclassify_directives_reply_decode(response, response_len, reclassified_count) !=
       0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_record_l4_approval_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                  uint64_t deadline_ns, uint64_t memory_id, const char *approver,
                                  const char *note, aimee_module_cancelled_fn cancelled,
                                  void *cancel_context)
{
   if (!call)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_RECORD_L4_APPROVAL_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_RECORD_L4_APPROVAL_RESPONSE_LEN];
   uint32_t request_len = 0u, response_len = 0u;
   if (aimee_db2_record_l4_approval_request_encode(memory_id, approver, note, request,
                                                   sizeof(request), &request_len) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_RECORD_L4_APPROVAL, AIMEE_DB2_STAGE_RECORD_L4_APPROVAL,
            trace_id, deadline_ns, request, request_len, response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_record_l4_approval_reply_decode(response, response_len) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_prune_orphaned_l0_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                 uint64_t deadline_ns, uint32_t *deleted_count,
                                 aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call || !deleted_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *deleted_count = 0u;
   uint8_t request[AIMEE_DB2_PRUNE_ORPHANED_L0_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PRUNE_ORPHANED_L0_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_prune_orphaned_l0_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_PRUNE_ORPHANED_L0, AIMEE_DB2_STAGE_PRUNE_ORPHANED_L0,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_prune_orphaned_l0_reply_decode(response, response_len, deleted_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_lifecycle_sweep_expired_call(
    aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
    uint32_t *archived_count, aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call || !archived_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *archived_count = 0u;
   uint8_t request[AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_lifecycle_sweep_expired_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_LIFECYCLE_SWEEP_EXPIRED,
            AIMEE_DB2_STAGE_LIFECYCLE_SWEEP_EXPIRED, trace_id, deadline_ns, request,
            sizeof(request), response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_lifecycle_sweep_expired_reply_decode(response, response_len, archived_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_demote_id_call(aimee_db2_call_fn call, void *call_context,
                                                    uint64_t trace_id, uint64_t deadline_ns,
                                                    uint64_t memory_id, uint32_t *demoted_count,
                                                    aimee_module_cancelled_fn cancelled,
                                                    void *cancel_context)
{
   if (!call || !demoted_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *demoted_count = 0u;
   uint8_t request[AIMEE_DB2_DEMOTE_ID_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DEMOTE_ID_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_demote_id_request_encode(memory_id, request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_DEMOTE_ID, AIMEE_DB2_STAGE_DEMOTE_ID, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_demote_id_reply_decode(response, response_len, demoted_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_has_workspace_tag_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                 uint64_t deadline_ns, uint64_t memory_id, uint32_t *tagged,
                                 aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call || !tagged)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *tagged = 0u;
   uint8_t request[AIMEE_DB2_HAS_WORKSPACE_TAG_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_HAS_WORKSPACE_TAG_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_has_workspace_tag_request_encode(memory_id, request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_HAS_WORKSPACE_TAG, AIMEE_DB2_STAGE_HAS_WORKSPACE_TAG,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_has_workspace_tag_reply_decode(response, response_len, tagged) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_delete_row_call(aimee_db2_call_fn call, void *call_context,
                                                     uint64_t trace_id, uint64_t deadline_ns,
                                                     uint64_t memory_id, uint32_t *deleted_rows,
                                                     aimee_module_cancelled_fn cancelled,
                                                     void *cancel_context)
{
   if (!call || !deleted_rows)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *deleted_rows = 0u;
   uint8_t request[AIMEE_DB2_DELETE_ROW_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DELETE_ROW_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_delete_row_request_encode(memory_id, request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_DELETE_ROW, AIMEE_DB2_STAGE_DELETE_ROW, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_delete_row_reply_decode(response, response_len, deleted_rows) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_touch_call(aimee_db2_call_fn call, void *call_context,
                                                uint64_t trace_id, uint64_t deadline_ns,
                                                uint64_t memory_id,
                                                aimee_module_cancelled_fn cancelled,
                                                void *cancel_context)
{
   if (!call)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_TOUCH_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_TOUCH_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_touch_request_encode(memory_id, request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport = call(
       call_context, AIMEE_DB2_EVENT_TOUCH, AIMEE_DB2_STAGE_TOUCH, trace_id, deadline_ns, request,
       sizeof(request), response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_touch_reply_decode(response, response_len) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_link_delete_call(aimee_db2_call_fn call, void *call_context,
                                                      uint64_t trace_id, uint64_t deadline_ns,
                                                      uint64_t link_id,
                                                      aimee_module_cancelled_fn cancelled,
                                                      void *cancel_context)
{
   if (!call)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_LINK_DELETE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_LINK_DELETE_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_link_delete_request_encode(link_id, request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_LINK_DELETE, AIMEE_DB2_STAGE_LINK_DELETE, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_link_delete_reply_decode(response, response_len) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_valid_at_call(aimee_db2_call_fn call, void *call_context,
                                                   uint64_t trace_id, uint64_t deadline_ns,
                                                   uint64_t memory_id, const char *as_of,
                                                   uint32_t *domain_result, uint32_t *in_force,
                                                   aimee_module_cancelled_fn cancelled,
                                                   void *cancel_context)
{
   if (!call || !domain_result || !in_force)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *domain_result = 0u;
   *in_force = 0u;
   uint8_t request[AIMEE_DB2_VALID_AT_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_VALID_AT_RESPONSE_LEN];
   uint32_t request_len = 0u, response_len = 0u;
   if (aimee_db2_valid_at_request_encode(memory_id, as_of, request, sizeof(request),
                                         &request_len) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport = call(
       call_context, AIMEE_DB2_EVENT_VALID_AT, AIMEE_DB2_STAGE_VALID_AT, trace_id, deadline_ns,
       request, request_len, response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_valid_at_reply_decode(response, response_len, domain_result, in_force) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_has_scope_type_call(aimee_db2_call_fn call, void *call_context,
                                                         uint64_t trace_id, uint64_t deadline_ns,
                                                         uint64_t memory_id, const char *scope_type,
                                                         uint32_t *present,
                                                         aimee_module_cancelled_fn cancelled,
                                                         void *cancel_context)
{
   if (!call || !present)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *present = 0u;
   uint8_t request[AIMEE_DB2_HAS_SCOPE_TYPE_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_HAS_SCOPE_TYPE_RESPONSE_LEN];
   uint32_t request_len = 0u, response_len = 0u;
   if (aimee_db2_has_scope_type_request_encode(memory_id, scope_type, request, sizeof(request),
                                               &request_len) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_HAS_SCOPE_TYPE, AIMEE_DB2_STAGE_HAS_SCOPE_TYPE, trace_id,
            deadline_ns, request, request_len, response, sizeof(response), &response_len, cancelled,
            cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_has_scope_type_reply_decode(response, response_len, present) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_reject_call(aimee_db2_call_fn call, void *call_context,
                                                 uint64_t trace_id, uint64_t deadline_ns,
                                                 uint64_t memory_id,
                                                 aimee_module_cancelled_fn cancelled,
                                                 void *cancel_context)
{
   if (!call)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_REJECT_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REJECT_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_reject_request_encode(memory_id, request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport = call(
       call_context, AIMEE_DB2_EVENT_REJECT, AIMEE_DB2_STAGE_REJECT, trace_id, deadline_ns, request,
       sizeof(request), response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_reject_reply_decode(response, response_len) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_update_content_call(aimee_db2_call_fn call, void *call_context,
                                                         uint64_t trace_id, uint64_t deadline_ns,
                                                         uint64_t memory_id, const char *content,
                                                         uint32_t *updated_rows,
                                                         aimee_module_cancelled_fn cancelled,
                                                         void *cancel_context)
{
   if (!call || !updated_rows)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *updated_rows = 0u;
   static _Thread_local uint8_t request[AIMEE_DB2_UPDATE_CONTENT_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_UPDATE_CONTENT_RESPONSE_LEN];
   uint32_t request_len = 0u, response_len = 0u;
   if (aimee_db2_update_content_request_encode(memory_id, content, request, sizeof(request),
                                               &request_len) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_UPDATE_CONTENT, AIMEE_DB2_STAGE_UPDATE_CONTENT, trace_id,
            deadline_ns, request, request_len, response, sizeof(response), &response_len, cancelled,
            cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_update_content_reply_decode(response, response_len, updated_rows) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_decay_confidence_call(aimee_db2_call_fn call,
                                                           void *call_context, uint64_t trace_id,
                                                           uint64_t deadline_ns, uint64_t memory_id,
                                                           aimee_module_cancelled_fn cancelled,
                                                           void *cancel_context)
{
   if (!call)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_DECAY_CONFIDENCE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DECAY_CONFIDENCE_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_decay_confidence_request_encode(memory_id, request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_DECAY_CONFIDENCE, AIMEE_DB2_STAGE_DECAY_CONFIDENCE,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_decay_confidence_reply_decode(response, response_len) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_workspace_tag_insert_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                    uint64_t deadline_ns, uint64_t memory_id, const char *workspace,
                                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_WORKSPACE_TAG_INSERT_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_WORKSPACE_TAG_INSERT_RESPONSE_LEN];
   uint32_t request_len = 0u, response_len = 0u;
   if (aimee_db2_workspace_tag_insert_request_encode(memory_id, workspace, request, sizeof(request),
                                                     &request_len) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_WORKSPACE_TAG_INSERT,
            AIMEE_DB2_STAGE_WORKSPACE_TAG_INSERT, trace_id, deadline_ns, request, request_len,
            response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_workspace_tag_insert_reply_decode(response, response_len) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_set_cognified_kind_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                  uint64_t deadline_ns, uint64_t memory_id, const char *kind,
                                  aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_SET_COGNIFIED_KIND_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_SET_COGNIFIED_KIND_RESPONSE_LEN];
   uint32_t request_len = 0u, response_len = 0u;
   if (aimee_db2_set_cognified_kind_request_encode(memory_id, kind, request, sizeof(request),
                                                   &request_len) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_SET_COGNIFIED_KIND, AIMEE_DB2_STAGE_SET_COGNIFIED_KIND,
            trace_id, deadline_ns, request, request_len, response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_set_cognified_kind_reply_decode(response, response_len) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_set_source_session_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                  uint64_t deadline_ns, uint64_t memory_id, const char *session_id,
                                  aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   uint8_t request[AIMEE_DB2_SET_SOURCE_SESSION_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_SET_SOURCE_SESSION_RESPONSE_LEN];
   uint32_t request_len = 0u, response_len = 0u;
   if (aimee_db2_set_source_session_request_encode(memory_id, session_id, request, sizeof(request),
                                                   &request_len) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_SET_SOURCE_SESSION, AIMEE_DB2_STAGE_SET_SOURCE_SESSION,
            trace_id, deadline_ns, request, request_len, response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_set_source_session_reply_decode(response, response_len) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_negation_tokens_update_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                      uint64_t deadline_ns, uint64_t memory_id, const char *tokens,
                                      aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   static _Thread_local uint8_t request[AIMEE_DB2_NEGATION_TOKENS_UPDATE_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_NEGATION_TOKENS_UPDATE_RESPONSE_LEN];
   uint32_t request_len = 0u, response_len = 0u;
   if (aimee_db2_negation_tokens_update_request_encode(memory_id, tokens, request, sizeof(request),
                                                       &request_len) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_NEGATION_TOKENS_UPDATE,
            AIMEE_DB2_STAGE_NEGATION_TOKENS_UPDATE, trace_id, deadline_ns, request, request_len,
            response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_negation_tokens_update_reply_decode(response, response_len) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_get_content_call(aimee_db2_call_fn call, void *call_context,
                                                      uint64_t trace_id, uint64_t deadline_ns,
                                                      uint64_t memory_id, uint32_t *domain_result,
                                                      char *content, size_t content_capacity,
                                                      aimee_module_cancelled_fn cancelled,
                                                      void *cancel_context)
{
   if (!call || !domain_result || !content ||
       content_capacity < (size_t)AIMEE_DB2_GET_CONTENT_CONTENT_MAX + 1u)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *domain_result = 0u;
   content[0] = '\\0';
   uint8_t request[AIMEE_DB2_GET_CONTENT_REQUEST_LEN];
   static _Thread_local uint8_t response[AIMEE_DB2_GET_CONTENT_RESPONSE_MAX_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_get_content_request_encode(memory_id, request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_GET_CONTENT, AIMEE_DB2_STAGE_GET_CONTENT, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_get_content_reply_decode(response, response_len, domain_result, content,
                                          content_capacity) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_get_source_session_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                  uint64_t deadline_ns, uint64_t memory_id, uint32_t *domain_result,
                                  char *session_id, size_t session_capacity,
                                  aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call || !domain_result || !session_id ||
       session_capacity < (size_t)AIMEE_DB2_GET_SOURCE_SESSION_SESSION_MAX + 1u)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *domain_result = 0u;
   session_id[0] = '\\0';
   uint8_t request[AIMEE_DB2_GET_SOURCE_SESSION_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_GET_SOURCE_SESSION_RESPONSE_MAX_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_get_source_session_request_encode(memory_id, request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_GET_SOURCE_SESSION, AIMEE_DB2_STAGE_GET_SOURCE_SESSION,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_get_source_session_reply_decode(response, response_len, domain_result, session_id,
                                                 session_capacity) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_pick_first_temporal_ref_call(aimee_db2_call_fn call, void *call_context,
                                       uint64_t trace_id, uint64_t deadline_ns, uint64_t memory_id,
                                       uint32_t *domain_result, char *ref_key, size_t key_capacity,
                                       aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call || !domain_result || !ref_key ||
       key_capacity < (size_t)AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_KEY_MAX + 1u)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *domain_result = 0u;
   ref_key[0] = '\\0';
   uint8_t request[AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_RESPONSE_MAX_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_pick_first_temporal_ref_request_encode(memory_id, request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_PICK_FIRST_TEMPORAL_REF,
            AIMEE_DB2_STAGE_PICK_FIRST_TEMPORAL_REF, trace_id, deadline_ns, request,
            sizeof(request), response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_pick_first_temporal_ref_reply_decode(response, response_len, domain_result,
                                                      ref_key, key_capacity) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_count_and_max_updated_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                     uint64_t deadline_ns, uint32_t *domain_result, uint32_t *count,
                                     char *max_updated_at, size_t stamp_capacity,
                                     aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call || !domain_result || !count || !max_updated_at ||
       stamp_capacity < (size_t)AIMEE_DB2_COUNT_AND_MAX_UPDATED_STAMP_MAX + 1u)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *domain_result = 0u;
   *count = 0u;
   max_updated_at[0] = '\\0';
   uint8_t request[AIMEE_DB2_COUNT_AND_MAX_UPDATED_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_COUNT_AND_MAX_UPDATED_RESPONSE_MAX_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_count_and_max_updated_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_COUNT_AND_MAX_UPDATED,
            AIMEE_DB2_STAGE_COUNT_AND_MAX_UPDATED, trace_id, deadline_ns, request, sizeof(request),
            response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_count_and_max_updated_reply_decode(response, response_len, domain_result, count,
                                                    max_updated_at, stamp_capacity) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_entity_edge_prune_orphans_call(
    aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
    uint32_t *pruned_count, aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call || !pruned_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *pruned_count = 0u;
   uint8_t request[AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_entity_edge_prune_orphans_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_ENTITY_EDGE_PRUNE_ORPHANS,
            AIMEE_DB2_STAGE_ENTITY_EDGE_PRUNE_ORPHANS, trace_id, deadline_ns, request,
            sizeof(request), response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_entity_edge_prune_orphans_reply_decode(response, response_len, pruned_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_entity_edge_normalize_weights_call(
    aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
    uint32_t *normalized_count, aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call || !normalized_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *normalized_count = 0u;
   uint8_t request[AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_entity_edge_normalize_weights_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_ENTITY_EDGE_NORMALIZE_WEIGHTS,
            AIMEE_DB2_STAGE_ENTITY_EDGE_NORMALIZE_WEIGHTS, trace_id, deadline_ns, request,
            sizeof(request), response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_entity_edge_normalize_weights_reply_decode(response, response_len,
                                                            normalized_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_project_count_call(aimee_db2_call_fn call, void *call_context,
                                                        uint64_t trace_id, uint64_t deadline_ns,
                                                        uint32_t *project_count,
                                                        aimee_module_cancelled_fn cancelled,
                                                        void *cancel_context)
{
   if (!call || !project_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *project_count = 0u;
   uint8_t request[AIMEE_DB2_PROJECT_COUNT_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PROJECT_COUNT_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_project_count_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_PROJECT_COUNT, AIMEE_DB2_STAGE_PROJECT_COUNT, trace_id,
            deadline_ns, request, sizeof(request), response, sizeof(response), &response_len,
            cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_project_count_reply_decode(response, response_len, project_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_purge_hidden_pollution_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                      uint64_t deadline_ns, uint32_t *purged_count,
                                      aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call || !purged_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *purged_count = 0u;
   uint8_t request[AIMEE_DB2_PURGE_HIDDEN_POLLUTION_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PURGE_HIDDEN_POLLUTION_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_purge_hidden_pollution_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_PURGE_HIDDEN_POLLUTION,
            AIMEE_DB2_STAGE_PURGE_HIDDEN_POLLUTION, trace_id, deadline_ns, request, sizeof(request),
            response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_purge_hidden_pollution_reply_decode(response, response_len, purged_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_requeue_drifted_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                               uint64_t deadline_ns, uint32_t *requeued_count,
                               aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call || !requeued_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *requeued_count = 0u;
   uint8_t request[AIMEE_DB2_REQUEUE_DRIFTED_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REQUEUE_DRIFTED_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_requeue_drifted_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_REQUEUE_DRIFTED, AIMEE_DB2_STAGE_REQUEUE_DRIFTED,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_requeue_drifted_reply_decode(response, response_len, requeued_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_prospective_sweep_expired_call(
    aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
    uint32_t *expired_count, aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call || !expired_count)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *expired_count = 0u;
   uint8_t request[AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_prospective_sweep_expired_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_PROSPECTIVE_SWEEP_EXPIRED,
            AIMEE_DB2_STAGE_PROSPECTIVE_SWEEP_EXPIRED, trace_id, deadline_ns, request,
            sizeof(request), response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_prospective_sweep_expired_reply_decode(response, response_len, expired_count) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_directive_sweep_expired_call(
    aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
    uint32_t *directives_expired, aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (!call || !directives_expired)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;

   *directives_expired = 0u;
   uint8_t request[AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_directive_sweep_expired_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_DIRECTIVE_SWEEP_EXPIRED,
            AIMEE_DB2_STAGE_DIRECTIVE_SWEEP_EXPIRED, trace_id, deadline_ns, request,
            sizeof(request), response, sizeof(response), &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_directive_sweep_expired_reply_decode(response, response_len, directives_expired) !=
       0)
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

aimee_module_call_result_t aimee_db2_reembed_clear_maintenance_call(
    aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
    uint32_t force, uint32_t *domain_result, aimee_db2_reembed_clear_maintenance_t *status,
    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (domain_result)
      *domain_result = 0u;
   if (status)
      *status = (aimee_db2_reembed_clear_maintenance_t){0};
   if (!call || !domain_result || !status || force > 1u)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   uint8_t request[AIMEE_DB2_REEMBED_MAINT_CLEAR_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REEMBED_MAINT_CLEAR_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (aimee_db2_reembed_clear_maintenance_request_encode(force, request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_REEMBED_MAINT_CLEAR, AIMEE_DB2_STAGE_REEMBED_MAINT_CLEAR,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_reembed_clear_maintenance_reply_decode(response, response_len, domain_result,
                                                        status) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t
aimee_db2_embedder_serving_id_call(aimee_db2_call_fn call, void *call_context, uint64_t trace_id,
                                   uint64_t deadline_ns, uint32_t *domain_result, char *serving_id,
                                   size_t serving_id_capacity, aimee_module_cancelled_fn cancelled,
                                   void *cancel_context)
{
   if (domain_result)
      *domain_result = 0u;
   if (serving_id && serving_id_capacity)
      serving_id[0] = '\\0';
   if (!call || !domain_result || !serving_id ||
       serving_id_capacity < AIMEE_DB2_EMBEDDER_SERVING_ID_MAX + 1u)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   uint8_t request[AIMEE_DB2_EMBEDDER_SERVING_ID_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EMBEDDER_SERVING_ID_RESPONSE_MAX_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_embedder_serving_id_request_encode(request, sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_EMBEDDER_SERVING_ID, AIMEE_DB2_STAGE_EMBEDDER_SERVING_ID,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_embedder_serving_id_reply_decode(response, response_len, domain_result, serving_id,
                                                  serving_id_capacity) != 0)
      return AIMEE_MODULE_CALL_PROTOCOL;
   return AIMEE_MODULE_CALL_OK;
}

aimee_module_call_result_t aimee_db2_dimension_reset_call(
    aimee_db2_call_fn call, void *call_context, uint64_t trace_id, uint64_t deadline_ns,
    uint32_t target_dimension, uint32_t force, uint32_t dry_run, uint32_t *domain_result,
    aimee_db2_dimension_reset_t *status, aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (domain_result)
      *domain_result = 0u;
   if (status)
      *status = (aimee_db2_dimension_reset_t){0};
   if (!call || !domain_result || !status || target_dimension < AIMEE_DB2_REEMBED_DIMENSION_MIN ||
       target_dimension > AIMEE_DB2_REEMBED_DIMENSION_MAX || force > 1u || dry_run > 1u)
      return AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   uint8_t request[AIMEE_DB2_DIMENSION_RESET_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DIMENSION_RESET_RESPONSE_LEN];
   uint32_t response_len = 0u;
   if (aimee_db2_dimension_reset_request_encode(target_dimension, force, dry_run, request,
                                                sizeof(request)) != 0)
      return AIMEE_MODULE_CALL_INTERNAL;
   aimee_module_call_result_t transport =
       call(call_context, AIMEE_DB2_EVENT_DIMENSION_RESET, AIMEE_DB2_STAGE_DIMENSION_RESET,
            trace_id, deadline_ns, request, sizeof(request), response, sizeof(response),
            &response_len, cancelled, cancel_context);
   if (transport != AIMEE_MODULE_CALL_OK)
      return transport;
   if (aimee_db2_dimension_reset_reply_decode(response, response_len, domain_result, status) != 0)
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
    reembed_clear_maintenance = catalog["operations"][7]
    embedder_serving_id = catalog["operations"][8]
    dimension_reset = catalog["operations"][9]
    level3_count = catalog["operations"][10]
    level2_count = catalog["operations"][11]
    orphaned_l0_count = catalog["operations"][12]
    total_count = catalog["operations"][13]
    session_l2_count = catalog["operations"][14]
    key_exists = catalog["operations"][15]
    find_id_by_key_kind = catalog["operations"][16]
    key_exists_in_tier_pair = catalog["operations"][17]
    effectiveness_update = catalog["operations"][18]
    retention_enforce = catalog["operations"][19]
    effectiveness_demote = catalog["operations"][20]
    effectiveness_stats = catalog["operations"][21]
    l2_memory_ids = catalog["operations"][22]
    health_record = catalog["operations"][23]
    health_retention = catalog["operations"][24]
    health_counters = catalog["operations"][25]
    stats_counts = catalog["operations"][26]
    expire = catalog["operations"][27]
    demote = catalog["operations"][28]
    promote_stable = catalog["operations"][29]
    reclassify_directives = catalog["operations"][30]
    record_l4_approval = catalog["operations"][31]
    prune_orphaned_l0 = catalog["operations"][32]
    lifecycle_sweep_expired = catalog["operations"][33]
    demote_id = catalog["operations"][34]
    has_workspace_tag = catalog["operations"][35]
    delete_row = catalog["operations"][36]
    touch = catalog["operations"][37]
    link_delete = catalog["operations"][38]
    valid_at = catalog["operations"][39]
    has_scope_type = catalog["operations"][40]
    reject = catalog["operations"][41]
    update_content = catalog["operations"][42]
    decay_confidence = catalog["operations"][43]
    workspace_tag_insert = catalog["operations"][44]
    set_cognified_kind = catalog["operations"][45]
    set_source_session = catalog["operations"][46]
    negation_tokens_update = catalog["operations"][47]
    get_content = catalog["operations"][48]
    get_source_session = catalog["operations"][49]
    pick_first_temporal_ref = catalog["operations"][50]
    count_and_max_updated = catalog["operations"][51]
    entity_edge_prune_orphans = catalog["operations"][52]
    entity_edge_normalize_weights = catalog["operations"][53]
    project_count = catalog["operations"][54]
    purge_hidden_pollution = catalog["operations"][55]
    requeue_drifted = catalog["operations"][56]
    prospective_sweep_expired = catalog["operations"][57]
    directive_sweep_expired = catalog["operations"][58]
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
\t"math"
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
const EventReembedClearMaintenance = EventLifecycle
const StageReembedClearMaintenance = FamilyLifecycle
const OperationReembedClearMaintenance uint32 = {reembed_clear_maintenance['id']}
const EventEmbedderServingID = EventLifecycle
const StageEmbedderServingID = FamilyLifecycle
const OperationEmbedderServingID uint32 = {embedder_serving_id['id']}
const EmbedderServingIDMax = 159
const EventDimensionReset = EventLifecycle
const StageDimensionReset = FamilyLifecycle
const OperationDimensionReset uint32 = {dimension_reset['id']}
const DimensionResetTablesMax uint32 = 16
const EventLevel3Count = EventMemory
const StageLevel3Count = FamilyMemory
const OperationLevel3Count uint32 = {level3_count['id']}
const Level3CountMax uint32 = {level3_count['reply']['field']['maximum']}
const EventLevel2Count = EventMemory
const StageLevel2Count = FamilyMemory
const OperationLevel2Count uint32 = {level2_count['id']}
const Level2CountMax uint32 = {level2_count['reply']['field']['maximum']}
const EventOrphanedL0Count = EventMemory
const StageOrphanedL0Count = FamilyMemory
const OperationOrphanedL0Count uint32 = {orphaned_l0_count['id']}
const OrphanedL0CountMax uint32 = {orphaned_l0_count['reply']['field']['maximum']}
const EventTotalCount = EventMemory
const StageTotalCount = FamilyMemory
const OperationTotalCount uint32 = {total_count['id']}
const TotalCountMax uint64 = {total_count['reply']['field']['maximum']}
const EventSessionL2Count = EventMemory
const StageSessionL2Count = FamilyMemory
const OperationSessionL2Count uint32 = {session_l2_count['id']}
const SessionL2CountSessionMax = {session_l2_count['request']['field']['maximum_bytes']}
const SessionL2CountMax uint32 = {session_l2_count['reply']['field']['maximum']}
const EventKeyExists = EventMemory
const StageKeyExists = FamilyMemory
const OperationKeyExists uint32 = {key_exists['id']}
const KeyExistsKeyMax = {key_exists['request']['field']['maximum_bytes']}
const KeyExistsMax uint32 = {key_exists['reply']['field']['maximum']}
const EventFindIDByKeyKind = EventMemory
const StageFindIDByKeyKind = FamilyMemory
const OperationFindIDByKeyKind uint32 = {find_id_by_key_kind['id']}
const FindIDByKeyKindKeyMax = {find_id_by_key_kind['request']['fields'][0]['maximum_bytes']}
const FindIDByKeyKindKindMax = {find_id_by_key_kind['request']['fields'][1]['maximum_bytes']}
const FindIDByKeyKindFoundMax uint32 = {find_id_by_key_kind['reply']['fields'][0]['maximum']}
const FindIDByKeyKindIDMax uint64 = {find_id_by_key_kind['reply']['fields'][1]['maximum']}
const EventKeyExistsInTierPair = EventMemory
const StageKeyExistsInTierPair = FamilyMemory
const OperationKeyExistsInTierPair uint32 = {key_exists_in_tier_pair['id']}
const KeyExistsInTierPairKeyMax = {key_exists_in_tier_pair['request']['fields'][0]['maximum_bytes']}
const KeyExistsInTierPairTierAMax = {key_exists_in_tier_pair['request']['fields'][1]['maximum_bytes']}
const KeyExistsInTierPairTierBMax = {key_exists_in_tier_pair['request']['fields'][2]['maximum_bytes']}
const KeyExistsInTierPairMax uint32 = {key_exists_in_tier_pair['reply']['field']['maximum']}
const EventEffectivenessUpdate = EventMemory
const StageEffectivenessUpdate = FamilyMemory
const OperationEffectivenessUpdate uint32 = {effectiveness_update['id']}
const EffectivenessUpdateMemoryIDMax uint64 = {effectiveness_update['request']['fields'][0]['maximum']}
const EffectivenessUpdateHasValueMax uint32 = {effectiveness_update['request']['fields'][1]['maximum']}
const EventRetentionEnforce = EventMemory
const StageRetentionEnforce = FamilyMemory
const OperationRetentionEnforce uint32 = {retention_enforce['id']}
const RetentionRestricted = "{retention_enforce['request']['policy'][0]['sensitivity']}"
const RetentionRestrictedDays uint32 = {retention_enforce['request']['policy'][0]['retention_days']}
const RetentionSensitive = "{retention_enforce['request']['policy'][1]['sensitivity']}"
const RetentionSensitiveDays uint32 = {retention_enforce['request']['policy'][1]['retention_days']}
const RetentionEnforceMax uint32 = {retention_enforce['reply']['field']['maximum']}
const EventEffectivenessDemote = EventMemory
const StageEffectivenessDemote = FamilyMemory
const OperationEffectivenessDemote uint32 = {effectiveness_demote['id']}
const EffectivenessDemoteThresholdBits uint64 = {effectiveness_demote['request']['policy']['threshold_binary64_bits']}
const EffectivenessDemoteMax uint32 = {effectiveness_demote['reply']['field']['maximum']}
const EventEffectivenessStats = EventMemory
const StageEffectivenessStats = FamilyMemory
const OperationEffectivenessStats uint32 = {effectiveness_stats['id']}
const EffectivenessStatsLowThresholdBits uint64 = {effectiveness_stats['request']['policy']['low_threshold_binary64_bits']}
const EffectivenessStatsAvgMin = {_binary64_literal(effectiveness_stats['reply']['fields'][0]['minimum_binary64_bits'])}
const EffectivenessStatsAvgMax = {_binary64_literal(effectiveness_stats['reply']['fields'][0]['maximum_binary64_bits'])}
const EffectivenessStatsLowMax uint32 = {effectiveness_stats['reply']['fields'][1]['maximum']}
const EffectivenessStatsHighMax uint32 = {effectiveness_stats['reply']['fields'][2]['maximum']}
const EventL2MemoryIDs = EventMemory
const StageL2MemoryIDs = FamilyMemory
const OperationL2MemoryIDs uint32 = {l2_memory_ids['id']}
const L2MemoryIDsMax uint32 = {l2_memory_ids['reply']['field']['maximum_items']}
const L2MemoryIDMin uint64 = {l2_memory_ids['reply']['field']['item_minimum']}
const L2MemoryIDMax uint64 = {l2_memory_ids['reply']['field']['item_maximum']}
const EventHealthRecord = EventMemory
const StageHealthRecord = FamilyMemory
const OperationHealthRecord uint32 = {health_record['id']}
const HealthRecordConflictWindowDays uint32 = {health_record['request']['policy']['conflict_window_days']}
const HealthRecordCounterMax uint32 = {health_record['request']['fields'][0]['maximum']}
const EventHealthRetention = EventMemory
const StageHealthRetention = FamilyMemory
const OperationHealthRetention uint32 = {health_retention['id']}
const HealthRetentionSnapshotDays uint32 = {health_retention['request']['policy']['snapshot_retention_days']}
const HealthRetentionContradictionDays uint32 = {health_retention['request']['policy']['contradiction_retention_days']}
const HealthRetentionMax uint32 = {health_retention['reply']['fields'][0]['maximum']}
const EventHealthCounters = EventMemory
const StageHealthCounters = FamilyMemory
const OperationHealthCounters uint32 = {health_counters['id']}
const HealthCountersPromoteUseCount uint32 = {health_counters['request']['policy']['promote_use_count']}
const HealthCountersPromoteConfidenceBits uint64 = {health_counters['request']['policy']['promote_confidence_binary64_bits']}
const HealthCountersFields = {len(HEALTH_COUNTERS)}
const HealthCountersMax uint32 = {health_counters['reply']['fields'][0]['maximum']}
const EventStatsCounts = EventMemory
const StageStatsCounts = FamilyMemory
const OperationStatsCounts uint32 = {stats_counts['id']}
const StatsCountsTiers = {len(MEMORY_TIERS)}
const StatsCountsKinds = {len(MEMORY_KINDS)}
const StatsCountsMax uint32 = {stats_counts['reply']['fields'][2]['maximum']}
const EventExpire = EventMemory
const StageExpire = FamilyMemory
const OperationExpire uint32 = {expire['id']}
const ExpireStaleTier = "{expire['request']['policy']['stale_l1_tier']}"
const ExpireKindsMax uint32 = {expire['request']['policy']['maximum_kinds']}
const ExpireMax uint32 = {expire['reply']['fields'][0]['maximum']}
const EventDemote = EventMemory
const StageDemote = FamilyMemory
const OperationDemote uint32 = {demote['id']}
const DemoteTier = "{demote['request']['policy']['demote_tier']}"
const DemoteKindsMax uint32 = {demote['request']['policy']['maximum_kinds']}
const DemoteMax uint32 = {demote['reply']['fields'][0]['maximum']}
const EventPromoteStable = EventMemory
const StagePromoteStable = FamilyMemory
const OperationPromoteStable uint32 = {promote_stable['id']}
const PromoteStableSourceTier = "{promote_stable['request']['policy']['source_tier']}"
const PromoteStableTargetTier = "{promote_stable['request']['policy']['target_tier']}"
const PromoteStableConfidenceBits uint64 = {promote_stable['request']['policy']['minimum_confidence_binary64_bits']}
const PromoteStableUseCount uint32 = {promote_stable['request']['policy']['minimum_use_count']}
const PromoteStableDays uint32 = {promote_stable['request']['policy']['stable_days']}
const PromoteStableMax uint32 = {promote_stable['reply']['field']['maximum']}
const EventReclassifyDirectives = EventMemory
const StageReclassifyDirectives = FamilyMemory
const OperationReclassifyDirectives uint32 = {reclassify_directives['id']}
const ReclassifyDirectivesSourceTier = "{reclassify_directives['request']['policy']['source_tier']}"
const ReclassifyDirectivesTargetTier = "{reclassify_directives['request']['policy']['target_tier']}"
const ReclassifyDirectivesGatedKind = "{reclassify_directives['request']['policy']['gated_kind']}"
const ReclassifyDirectivesGateMax uint32 = {reclassify_directives['request']['field']['maximum']}
const ReclassifyDirectivesMax uint32 = {reclassify_directives['reply']['field']['maximum']}
const EventRecordL4Approval = EventMemory
const StageRecordL4Approval = FamilyMemory
const OperationRecordL4Approval uint32 = {record_l4_approval['id']}
const RecordL4ApprovalTier = "{record_l4_approval['request']['policy']['target_tier']}"
const RecordL4ApprovalMemoryIDMax uint64 = {record_l4_approval['request']['fields'][0]['maximum']}
const RecordL4ApprovalApproverMax = {record_l4_approval['request']['fields'][1]['maximum_bytes']}
const RecordL4ApprovalNoteMax = {record_l4_approval['request']['fields'][2]['maximum_bytes']}
const EventPruneOrphanedL0 = EventMemory
const StagePruneOrphanedL0 = FamilyMemory
const OperationPruneOrphanedL0 uint32 = {prune_orphaned_l0['id']}
const PruneOrphanedL0Tier = "{prune_orphaned_l0['request']['policy']['tier']}"
const PruneOrphanedL0MaxAge = "{prune_orphaned_l0['request']['policy']['maximum_age']}"
const PruneOrphanedL0CountMax uint32 = {prune_orphaned_l0['reply']['field']['maximum']}
const EventLifecycleSweepExpired = EventMemory
const StageLifecycleSweepExpired = FamilyMemory
const OperationLifecycleSweepExpired uint32 = {lifecycle_sweep_expired['id']}
const LifecycleSweepExpiredSourceState = "{lifecycle_sweep_expired['request']['policy']['source_state']}"
const LifecycleSweepExpiredTargetState = "{lifecycle_sweep_expired['request']['policy']['target_state']}"
const LifecycleSweepExpiredReason = "{lifecycle_sweep_expired['request']['policy']['archive_reason']}"
const LifecycleSweepExpiredCountMax uint32 = {lifecycle_sweep_expired['reply']['field']['maximum']}
const EventDemoteID = EventMemory
const StageDemoteID = FamilyMemory
const OperationDemoteID uint32 = {demote_id['id']}
const DemoteIDMemoryIDMax uint64 = {demote_id['request']['field']['maximum']}
const DemoteIDCountMax uint32 = {demote_id['reply']['field']['maximum']}
const DemoteIDMultiplierBits uint64 = {demote_id['request']['policy']['confidence_multiplier_binary64_bits']}
const DemoteIDMinimumConfidenceBits uint64 = {demote_id['request']['policy']['minimum_confidence_binary64_bits']}
const EventHasWorkspaceTag = EventMemory
const StageHasWorkspaceTag = FamilyMemory
const OperationHasWorkspaceTag uint32 = {has_workspace_tag['id']}
const HasWorkspaceTagMemoryIDMax uint64 = {has_workspace_tag['request']['field']['maximum']}
const HasWorkspaceTagMax uint32 = {has_workspace_tag['reply']['field']['maximum']}
const EventDeleteRow = EventMemory
const StageDeleteRow = FamilyMemory
const OperationDeleteRow uint32 = {delete_row['id']}
const DeleteRowMemoryIDMax uint64 = {delete_row['request']['field']['maximum']}
const DeleteRowMax uint32 = {delete_row['reply']['field']['maximum']}
const EventTouch = EventMemory
const StageTouch = FamilyMemory
const OperationTouch uint32 = {touch['id']}
const TouchMemoryIDMax uint64 = {touch['request']['field']['maximum']}
const EventLinkDelete = EventMemory
const StageLinkDelete = FamilyMemory
const OperationLinkDelete uint32 = {link_delete['id']}
const LinkDeleteLinkIDMax uint64 = {link_delete['request']['field']['maximum']}
const EventValidAt = EventMemory
const StageValidAt = FamilyMemory
const OperationValidAt uint32 = {valid_at['id']}
const ValidAtMemoryIDMax uint64 = {valid_at['request']['fields'][0]['maximum']}
const ValidAtAsOfMax = {valid_at['request']['fields'][1]['maximum_bytes']}
const ValidAtMax uint32 = {valid_at['reply']['field']['maximum']}
const EventHasScopeType = EventMemory
const StageHasScopeType = FamilyMemory
const OperationHasScopeType uint32 = {has_scope_type['id']}
const HasScopeTypeMemoryIDMax uint64 = {has_scope_type['request']['fields'][0]['maximum']}
const HasScopeTypeScopeMax = {has_scope_type['request']['fields'][1]['maximum_bytes']}
const HasScopeTypeMax uint32 = {has_scope_type['reply']['field']['maximum']}
const EventReject = EventMemory
const StageReject = FamilyMemory
const OperationReject uint32 = {reject['id']}
const RejectMemoryIDMax uint64 = {reject['request']['field']['maximum']}
const RejectPenaltyBits uint64 = {reject['request']['policy']['confidence_penalty_binary64_bits']}
const RejectFloorBits uint64 = {reject['request']['policy']['confidence_floor_binary64_bits']}
const EventUpdateContent = EventMemory
const StageUpdateContent = FamilyMemory
const OperationUpdateContent uint32 = {update_content['id']}
const UpdateContentMemoryIDMax uint64 = {update_content['request']['fields'][0]['maximum']}
const UpdateContentContentMax = {update_content['request']['fields'][1]['maximum_bytes']}
const UpdateContentMax uint32 = {update_content['reply']['field']['maximum']}
const EventDecayConfidence = EventMemory
const StageDecayConfidence = FamilyMemory
const OperationDecayConfidence uint32 = {decay_confidence['id']}
const DecayConfidenceMemoryIDMax uint64 = {decay_confidence['request']['field']['maximum']}
const DecayConfidenceMultiplierBits uint64 = {decay_confidence['request']['policy']['confidence_multiplier_binary64_bits']}
const EventWorkspaceTagInsert = EventMemory
const StageWorkspaceTagInsert = FamilyMemory
const OperationWorkspaceTagInsert uint32 = {workspace_tag_insert['id']}
const WorkspaceTagInsertMemoryIDMax uint64 = {workspace_tag_insert['request']['fields'][0]['maximum']}
const WorkspaceTagInsertWorkspaceMax = {workspace_tag_insert['request']['fields'][1]['maximum_bytes']}
const EventSetCognifiedKind = EventMemory
const StageSetCognifiedKind = FamilyMemory
const OperationSetCognifiedKind uint32 = {set_cognified_kind['id']}
const SetCognifiedKindMemoryIDMax uint64 = {set_cognified_kind['request']['fields'][0]['maximum']}
const SetCognifiedKindKindMax = {set_cognified_kind['request']['fields'][1]['maximum_bytes']}
const EventSetSourceSession = EventMemory
const StageSetSourceSession = FamilyMemory
const OperationSetSourceSession uint32 = {set_source_session['id']}
const SetSourceSessionMemoryIDMax uint64 = {set_source_session['request']['fields'][0]['maximum']}
const SetSourceSessionSessionMax = {set_source_session['request']['fields'][1]['maximum_bytes']}
const EventNegationTokensUpdate = EventMemory
const StageNegationTokensUpdate = FamilyMemory
const OperationNegationTokensUpdate uint32 = {negation_tokens_update['id']}
const NegationTokensUpdateMemoryIDMax uint64 = {negation_tokens_update['request']['fields'][0]['maximum']}
const NegationTokensUpdateTokensMax = {negation_tokens_update['request']['fields'][1]['maximum_bytes']}
const EventGetContent = EventMemory
const StageGetContent = FamilyMemory
const OperationGetContent uint32 = {get_content['id']}
const GetContentMemoryIDMax uint64 = {get_content['request']['field']['maximum']}
const GetContentContentMax = {get_content['reply']['field']['maximum_bytes']}
const EventGetSourceSession = EventMemory
const StageGetSourceSession = FamilyMemory
const OperationGetSourceSession uint32 = {get_source_session['id']}
const GetSourceSessionMemoryIDMax uint64 = {get_source_session['request']['field']['maximum']}
const GetSourceSessionSessionMax = {get_source_session['reply']['field']['maximum_bytes']}
const EventPickFirstTemporalRef = EventMemory
const StagePickFirstTemporalRef = FamilyMemory
const OperationPickFirstTemporalRef uint32 = {pick_first_temporal_ref['id']}
const PickFirstTemporalRefMemoryIDMax uint64 = {pick_first_temporal_ref['request']['field']['maximum']}
const PickFirstTemporalRefKeyMax = {pick_first_temporal_ref['reply']['field']['maximum_bytes']}
const EventCountAndMaxUpdated = EventMemory
const StageCountAndMaxUpdated = FamilyMemory
const OperationCountAndMaxUpdated uint32 = {count_and_max_updated['id']}
const CountAndMaxUpdatedCountMax uint32 = {count_and_max_updated['reply']['fields'][0]['maximum']}
const CountAndMaxUpdatedStampMax = {count_and_max_updated['reply']['fields'][1]['maximum_bytes']}
const EventEntityEdgePruneOrphans = EventIndex
const StageEntityEdgePruneOrphans = FamilyIndex
const OperationEntityEdgePruneOrphans uint32 = {entity_edge_prune_orphans['id']}
const EntityEdgePruneOrphansCountMax uint32 = {entity_edge_prune_orphans['reply']['field']['maximum']}
const EventEntityEdgeNormalizeWeights = EventIndex
const StageEntityEdgeNormalizeWeights = FamilyIndex
const OperationEntityEdgeNormalizeWeights uint32 = {entity_edge_normalize_weights['id']}
const EntityEdgeNormalizeWeightsScale uint32 = {entity_edge_normalize_weights['request']['policy']['scale']}
const EntityEdgeNormalizeWeightsCountMax uint32 = {entity_edge_normalize_weights['reply']['field']['maximum']}
const EventProjectCount = EventIndex
const StageProjectCount = FamilyIndex
const OperationProjectCount uint32 = {project_count['id']}
const ProjectCountMax uint32 = {project_count['reply']['field']['maximum']}
const EventPurgeHiddenPollution = EventIndex
const StagePurgeHiddenPollution = FamilyIndex
const OperationPurgeHiddenPollution uint32 = {purge_hidden_pollution['id']}
const PurgeHiddenPollutionMax uint32 = {purge_hidden_pollution['reply']['field']['maximum']}
const EventRequeueDrifted = EventIndex
const StageRequeueDrifted = FamilyIndex
const OperationRequeueDrifted uint32 = {requeue_drifted['id']}
const RequeueDriftedMax uint32 = {requeue_drifted['reply']['field']['maximum']}
const EventProspectiveSweepExpired = EventMaintenance
const StageProspectiveSweepExpired = FamilyMaintenance
const OperationProspectiveSweepExpired uint32 = {prospective_sweep_expired['id']}
const ProspectiveSweepExpiredMax uint32 = {prospective_sweep_expired['reply']['field']['maximum']}
const EventDirectiveSweepExpired = EventMaintenance
const StageDirectiveSweepExpired = FamilyMaintenance
const OperationDirectiveSweepExpired uint32 = {directive_sweep_expired['id']}
const DirectiveSweepExpiredMax uint32 = {directive_sweep_expired['reply']['field']['maximum']}

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

// EncodeLevel3CountRequest emits the empty request envelope for the global L3 count.
func EncodeLevel3CountRequest() []byte {{
	header, err := EncodeRequestHeader(OperationLevel3Count, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeLevel3CountRequest validates the exact memory-family operation envelope.
func DecodeLevel3CountRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationLevel3Count ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeLevel3CountReply emits one bounded u32 success payload.
func EncodeLevel3CountReply(count uint32) ([]byte, error) {{
	if count > Level3CountMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationLevel3Count, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], count)
	return reply, nil
}}

// DecodeLevel3CountReply validates the operation and bounded count.
func DecodeLevel3CountReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationLevel3Count || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	count := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if count > Level3CountMax {{
		return 0, ErrMalformedEnvelope
	}}
	return count, nil
}}

// EncodeLevel2CountRequest emits the empty request envelope for the global L2 count.
func EncodeLevel2CountRequest() []byte {{
	header, err := EncodeRequestHeader(OperationLevel2Count, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeLevel2CountRequest validates the exact memory-family operation envelope.
func DecodeLevel2CountRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationLevel2Count ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeLevel2CountReply emits one bounded u32 success payload.
func EncodeLevel2CountReply(count uint32) ([]byte, error) {{
	if count > Level2CountMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationLevel2Count, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], count)
	return reply, nil
}}

// DecodeLevel2CountReply validates the operation and bounded count.
func DecodeLevel2CountReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationLevel2Count || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	count := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if count > Level2CountMax {{
		return 0, ErrMalformedEnvelope
	}}
	return count, nil
}}

// EncodeOrphanedL0CountRequest emits the empty request envelope for the orphaned L0 count.
func EncodeOrphanedL0CountRequest() []byte {{
	header, err := EncodeRequestHeader(OperationOrphanedL0Count, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeOrphanedL0CountRequest validates the exact memory-family operation envelope.
func DecodeOrphanedL0CountRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationOrphanedL0Count ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeOrphanedL0CountReply emits one bounded u32 success payload.
func EncodeOrphanedL0CountReply(count uint32) ([]byte, error) {{
	if count > OrphanedL0CountMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationOrphanedL0Count, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], count)
	return reply, nil
}}

// DecodeOrphanedL0CountReply validates the operation and bounded count.
func DecodeOrphanedL0CountReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationOrphanedL0Count || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	count := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if count > OrphanedL0CountMax {{
		return 0, ErrMalformedEnvelope
	}}
	return count, nil
}}

// EncodePruneOrphanedL0Request emits the empty request envelope; the tier and
// age window are fixed policy and never travel on the wire.
func EncodePruneOrphanedL0Request() []byte {{
	header, err := EncodeRequestHeader(OperationPruneOrphanedL0, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodePruneOrphanedL0Request validates the exact memory-family operation envelope.
func DecodePruneOrphanedL0Request(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationPruneOrphanedL0 ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodePruneOrphanedL0Reply emits one bounded u32 deletion count.
func EncodePruneOrphanedL0Reply(deletedCount uint32) ([]byte, error) {{
	if deletedCount > PruneOrphanedL0CountMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationPruneOrphanedL0, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], deletedCount)
	return reply, nil
}}

// DecodePruneOrphanedL0Reply validates the operation and bounded deletion count.
func DecodePruneOrphanedL0Reply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationPruneOrphanedL0 || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	deletedCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if deletedCount > PruneOrphanedL0CountMax {{
		return 0, ErrMalformedEnvelope
	}}
	return deletedCount, nil
}}

// EncodeLifecycleSweepExpiredRequest emits the empty request envelope; the
// lifecycle states and archive reason are fixed policy and never travel.
func EncodeLifecycleSweepExpiredRequest() []byte {{
	header, err := EncodeRequestHeader(OperationLifecycleSweepExpired, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeLifecycleSweepExpiredRequest validates the exact memory-family envelope.
func DecodeLifecycleSweepExpiredRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationLifecycleSweepExpired ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeLifecycleSweepExpiredReply emits one bounded u32 archived count.
func EncodeLifecycleSweepExpiredReply(archivedCount uint32) ([]byte, error) {{
	if archivedCount > LifecycleSweepExpiredCountMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationLifecycleSweepExpired, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], archivedCount)
	return reply, nil
}}

// DecodeLifecycleSweepExpiredReply validates the operation and bounded count.
func DecodeLifecycleSweepExpiredReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationLifecycleSweepExpired ||
		header.Result != ResultOK || header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	archivedCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if archivedCount > LifecycleSweepExpiredCountMax {{
		return 0, ErrMalformedEnvelope
	}}
	return archivedCount, nil
}}

// EncodeDemoteIDRequest emits the single memory the caller wants decayed. The
// multiplier and floor are policy and never travel.
func EncodeDemoteIDRequest(memoryID uint64) ([]byte, error) {{
	if memoryID == 0 || memoryID > DemoteIDMemoryIDMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationDemoteID, 0, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 8)...)
	binary.LittleEndian.PutUint64(request[EnvelopeHeaderLen:], memoryID)
	return request, nil
}}

// DecodeDemoteIDRequest validates the envelope and the bounded memory.
func DecodeDemoteIDRequest(request []byte) (uint64, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationDemoteID || header.Flags != 0 ||
		header.PayloadLen != 8 || len(request) != int(EnvelopeHeaderLen)+8 {{
		return 0, ErrMalformedEnvelope
	}}
	memoryID := binary.LittleEndian.Uint64(request[EnvelopeHeaderLen:])
	if memoryID == 0 || memoryID > DemoteIDMemoryIDMax {{
		return 0, ErrMalformedEnvelope
	}}
	return memoryID, nil
}}

// EncodeDemoteIDReply emits the single-row demotion count.
func EncodeDemoteIDReply(demotedCount uint32) ([]byte, error) {{
	if demotedCount > DemoteIDCountMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationDemoteID, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], demotedCount)
	return reply, nil
}}

// DecodeDemoteIDReply validates the operation and the single-row bound.
func DecodeDemoteIDReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationDemoteID || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	demotedCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if demotedCount > DemoteIDCountMax {{
		return 0, ErrMalformedEnvelope
	}}
	return demotedCount, nil
}}

// EncodeHasWorkspaceTagRequest emits the memory whose attribution is probed.
func EncodeHasWorkspaceTagRequest(memoryID uint64) ([]byte, error) {{
	if memoryID == 0 || memoryID > HasWorkspaceTagMemoryIDMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationHasWorkspaceTag, 0, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 8)...)
	binary.LittleEndian.PutUint64(request[EnvelopeHeaderLen:], memoryID)
	return request, nil
}}

// DecodeHasWorkspaceTagRequest validates the envelope and the bounded memory.
func DecodeHasWorkspaceTagRequest(request []byte) (uint64, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationHasWorkspaceTag || header.Flags != 0 ||
		header.PayloadLen != 8 || len(request) != int(EnvelopeHeaderLen)+8 {{
		return 0, ErrMalformedEnvelope
	}}
	memoryID := binary.LittleEndian.Uint64(request[EnvelopeHeaderLen:])
	if memoryID == 0 || memoryID > HasWorkspaceTagMemoryIDMax {{
		return 0, ErrMalformedEnvelope
	}}
	return memoryID, nil
}}

// EncodeHasWorkspaceTagReply emits the Boolean attribution flag.
func EncodeHasWorkspaceTagReply(tagged uint32) ([]byte, error) {{
	if tagged > HasWorkspaceTagMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationHasWorkspaceTag, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], tagged)
	return reply, nil
}}

// DecodeHasWorkspaceTagReply validates the operation and the Boolean bound.
func DecodeHasWorkspaceTagReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationHasWorkspaceTag || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	tagged := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if tagged > HasWorkspaceTagMax {{
		return 0, ErrMalformedEnvelope
	}}
	return tagged, nil
}}

// EncodeDeleteRowRequest emits the memory the caller wants removed.
func EncodeDeleteRowRequest(memoryID uint64) ([]byte, error) {{
	if memoryID == 0 || memoryID > DeleteRowMemoryIDMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationDeleteRow, 0, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 8)...)
	binary.LittleEndian.PutUint64(request[EnvelopeHeaderLen:], memoryID)
	return request, nil
}}

// DecodeDeleteRowRequest validates the envelope and the bounded memory.
func DecodeDeleteRowRequest(request []byte) (uint64, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationDeleteRow || header.Flags != 0 ||
		header.PayloadLen != 8 || len(request) != int(EnvelopeHeaderLen)+8 {{
		return 0, ErrMalformedEnvelope
	}}
	memoryID := binary.LittleEndian.Uint64(request[EnvelopeHeaderLen:])
	if memoryID == 0 || memoryID > DeleteRowMemoryIDMax {{
		return 0, ErrMalformedEnvelope
	}}
	return memoryID, nil
}}

// EncodeDeleteRowReply emits whether the named row existed.
func EncodeDeleteRowReply(deletedRows uint32) ([]byte, error) {{
	if deletedRows > DeleteRowMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationDeleteRow, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], deletedRows)
	return reply, nil
}}

// DecodeDeleteRowReply validates the operation and the single-row bound.
func DecodeDeleteRowReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationDeleteRow || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	deletedRows := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if deletedRows > DeleteRowMax {{
		return 0, ErrMalformedEnvelope
	}}
	return deletedRows, nil
}}

// EncodeTouchRequest emits the memory whose retrieval is being recorded.
func EncodeTouchRequest(memoryID uint64) ([]byte, error) {{
	if memoryID == 0 || memoryID > TouchMemoryIDMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationTouch, 0, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 8)...)
	binary.LittleEndian.PutUint64(request[EnvelopeHeaderLen:], memoryID)
	return request, nil
}}

// DecodeTouchRequest validates the envelope and the bounded memory.
func DecodeTouchRequest(request []byte) (uint64, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationTouch || header.Flags != 0 ||
		header.PayloadLen != 8 || len(request) != int(EnvelopeHeaderLen)+8 {{
		return 0, ErrMalformedEnvelope
	}}
	memoryID := binary.LittleEndian.Uint64(request[EnvelopeHeaderLen:])
	if memoryID == 0 || memoryID > TouchMemoryIDMax {{
		return 0, ErrMalformedEnvelope
	}}
	return memoryID, nil
}}

// EncodeTouchReply acknowledges the bump without a payload.
func EncodeTouchReply() ([]byte, error) {{
	header, err := EncodeReplyHeader(OperationTouch, ResultOK, 0)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	return header, nil
}}

// DecodeTouchReply validates the acknowledgement and refuses any payload.
func DecodeTouchReply(reply []byte) error {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationTouch || header.Result != ResultOK ||
		header.PayloadLen != 0 || len(reply) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeLinkDeleteRequest emits the relation the caller wants removed.
func EncodeLinkDeleteRequest(linkID uint64) ([]byte, error) {{
	if linkID == 0 || linkID > LinkDeleteLinkIDMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationLinkDelete, 0, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 8)...)
	binary.LittleEndian.PutUint64(request[EnvelopeHeaderLen:], linkID)
	return request, nil
}}

// DecodeLinkDeleteRequest validates the envelope and the bounded link.
func DecodeLinkDeleteRequest(request []byte) (uint64, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationLinkDelete || header.Flags != 0 ||
		header.PayloadLen != 8 || len(request) != int(EnvelopeHeaderLen)+8 {{
		return 0, ErrMalformedEnvelope
	}}
	linkID := binary.LittleEndian.Uint64(request[EnvelopeHeaderLen:])
	if linkID == 0 || linkID > LinkDeleteLinkIDMax {{
		return 0, ErrMalformedEnvelope
	}}
	return linkID, nil
}}

// EncodeLinkDeleteReply acknowledges the delete without a payload.
func EncodeLinkDeleteReply() ([]byte, error) {{
	header, err := EncodeReplyHeader(OperationLinkDelete, ResultOK, 0)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	return header, nil
}}

// DecodeLinkDeleteReply validates the acknowledgement and refuses any payload.
func DecodeLinkDeleteReply(reply []byte) error {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationLinkDelete || header.Result != ResultOK ||
		header.PayloadLen != 0 || len(reply) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeValidAtRequest emits the memory and the instant to test it against.
func EncodeValidAtRequest(memoryID uint64, asOf string) ([]byte, error) {{
	if memoryID == 0 || memoryID > ValidAtMemoryIDMax || len(asOf) == 0 ||
		len(asOf) > ValidAtAsOfMax || hasNUL(asOf) {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 12 + len(asOf)
	header, err := EncodeRequestHeader(OperationValidAt, 0, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, payloadLen)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint64(payload, memoryID)
	binary.LittleEndian.PutUint32(payload[8:], uint32(len(asOf)))
	copy(payload[12:], asOf)
	return request, nil
}}

// DecodeValidAtRequest validates the envelope, the memory, and the instant.
func DecodeValidAtRequest(request []byte) (uint64, string, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationValidAt || header.Flags != 0 ||
		header.PayloadLen < 13 ||
		len(request) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return 0, "", ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	memoryID := binary.LittleEndian.Uint64(payload)
	asOfLen := binary.LittleEndian.Uint32(payload[8:])
	if memoryID == 0 || memoryID > ValidAtMemoryIDMax || asOfLen == 0 ||
		asOfLen > uint32(ValidAtAsOfMax) || 12+asOfLen != header.PayloadLen {{
		return 0, "", ErrMalformedEnvelope
	}}
	asOf := string(payload[12 : 12+asOfLen])
	if hasNUL(asOf) {{
		return 0, "", ErrMalformedEnvelope
	}}
	return memoryID, asOf, nil
}}

// EncodeValidAtReply emits the verdict, or the refusal to reach one.
func EncodeValidAtReply(result uint32, inForce uint32) ([]byte, error) {{
	if result == ResultInvalidState {{
		if inForce != 0 {{
			return nil, ErrMalformedEnvelope
		}}
		header, err := EncodeReplyHeader(OperationValidAt, result, 0)
		if err != nil {{
			return nil, ErrMalformedEnvelope
		}}
		return header, nil
	}}
	if result != ResultOK || inForce > ValidAtMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationValidAt, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], inForce)
	return reply, nil
}}

// DecodeValidAtReply keeps "could not evaluate" distinct from "not in force".
func DecodeValidAtReply(reply []byte) (uint32, uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationValidAt {{
		return 0, 0, ErrMalformedEnvelope
	}}
	if header.Result == ResultInvalidState && header.PayloadLen == 0 {{
		return header.Result, 0, nil
	}}
	if header.Result != ResultOK || header.PayloadLen != 4 {{
		return 0, 0, ErrMalformedEnvelope
	}}
	inForce := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if inForce > ValidAtMax {{
		return 0, 0, ErrMalformedEnvelope
	}}
	return header.Result, inForce, nil
}}

// EncodeHasScopeTypeRequest emits the memory and the scope kind to probe for.
func EncodeHasScopeTypeRequest(memoryID uint64, scopeType string) ([]byte, error) {{
	if memoryID == 0 || memoryID > HasScopeTypeMemoryIDMax || len(scopeType) == 0 ||
		len(scopeType) > HasScopeTypeScopeMax || hasNUL(scopeType) {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 12 + len(scopeType)
	header, err := EncodeRequestHeader(OperationHasScopeType, 0, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, payloadLen)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint64(payload, memoryID)
	binary.LittleEndian.PutUint32(payload[8:], uint32(len(scopeType)))
	copy(payload[12:], scopeType)
	return request, nil
}}

// DecodeHasScopeTypeRequest validates the envelope, memory, and scope kind.
func DecodeHasScopeTypeRequest(request []byte) (uint64, string, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationHasScopeType || header.Flags != 0 ||
		header.PayloadLen < 13 ||
		len(request) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return 0, "", ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	memoryID := binary.LittleEndian.Uint64(payload)
	scopeLen := binary.LittleEndian.Uint32(payload[8:])
	if memoryID == 0 || memoryID > HasScopeTypeMemoryIDMax || scopeLen == 0 ||
		scopeLen > uint32(HasScopeTypeScopeMax) || 12+scopeLen != header.PayloadLen {{
		return 0, "", ErrMalformedEnvelope
	}}
	scopeType := string(payload[12 : 12+scopeLen])
	if hasNUL(scopeType) {{
		return 0, "", ErrMalformedEnvelope
	}}
	return memoryID, scopeType, nil
}}

// EncodeHasScopeTypeReply emits the Boolean attribution flag.
func EncodeHasScopeTypeReply(present uint32) ([]byte, error) {{
	if present > HasScopeTypeMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationHasScopeType, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], present)
	return reply, nil
}}

// DecodeHasScopeTypeReply validates the operation and the Boolean bound.
func DecodeHasScopeTypeReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationHasScopeType || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	present := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if present > HasScopeTypeMax {{
		return 0, ErrMalformedEnvelope
	}}
	return present, nil
}}

// EncodeRejectRequest emits the memory to penalise. No reason travels: the
// backend discards the one it is handed and nothing persists it.
func EncodeRejectRequest(memoryID uint64) ([]byte, error) {{
	if memoryID == 0 || memoryID > RejectMemoryIDMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationReject, 0, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 8)...)
	binary.LittleEndian.PutUint64(request[EnvelopeHeaderLen:], memoryID)
	return request, nil
}}

// DecodeRejectRequest validates the envelope and the bounded memory.
func DecodeRejectRequest(request []byte) (uint64, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationReject || header.Flags != 0 ||
		header.PayloadLen != 8 || len(request) != int(EnvelopeHeaderLen)+8 {{
		return 0, ErrMalformedEnvelope
	}}
	memoryID := binary.LittleEndian.Uint64(request[EnvelopeHeaderLen:])
	if memoryID == 0 || memoryID > RejectMemoryIDMax {{
		return 0, ErrMalformedEnvelope
	}}
	return memoryID, nil
}}

// EncodeRejectReply acknowledges the penalty without a payload.
func EncodeRejectReply() ([]byte, error) {{
	header, err := EncodeReplyHeader(OperationReject, ResultOK, 0)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	return header, nil
}}

// DecodeRejectReply validates the acknowledgement and refuses any payload.
func DecodeRejectReply(reply []byte) error {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationReject || header.Result != ResultOK ||
		header.PayloadLen != 0 || len(reply) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeUpdateContentRequest emits the memory and its replacement text.
func EncodeUpdateContentRequest(memoryID uint64, content string) ([]byte, error) {{
	if memoryID == 0 || memoryID > UpdateContentMemoryIDMax || len(content) == 0 ||
		len(content) > UpdateContentContentMax || hasNUL(content) {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 12 + len(content)
	header, err := EncodeRequestHeader(OperationUpdateContent, 0, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, payloadLen)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint64(payload, memoryID)
	binary.LittleEndian.PutUint32(payload[8:], uint32(len(content)))
	copy(payload[12:], content)
	return request, nil
}}

// DecodeUpdateContentRequest validates the envelope, memory, and text.
func DecodeUpdateContentRequest(request []byte) (uint64, string, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationUpdateContent || header.Flags != 0 ||
		header.PayloadLen < 13 ||
		len(request) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return 0, "", ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	memoryID := binary.LittleEndian.Uint64(payload)
	contentLen := binary.LittleEndian.Uint32(payload[8:])
	if memoryID == 0 || memoryID > UpdateContentMemoryIDMax || contentLen == 0 ||
		contentLen > uint32(UpdateContentContentMax) || 12+contentLen != header.PayloadLen {{
		return 0, "", ErrMalformedEnvelope
	}}
	content := string(payload[12 : 12+contentLen])
	if hasNUL(content) {{
		return 0, "", ErrMalformedEnvelope
	}}
	return memoryID, content, nil
}}

// EncodeUpdateContentReply emits whether the named row existed.
func EncodeUpdateContentReply(updatedRows uint32) ([]byte, error) {{
	if updatedRows > UpdateContentMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationUpdateContent, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], updatedRows)
	return reply, nil
}}

// DecodeUpdateContentReply validates the operation and the single-row bound.
func DecodeUpdateContentReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationUpdateContent || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	updatedRows := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if updatedRows > UpdateContentMax {{
		return 0, ErrMalformedEnvelope
	}}
	return updatedRows, nil
}}

// EncodeDecayConfidenceRequest emits the memory whose confidence decays.
func EncodeDecayConfidenceRequest(memoryID uint64) ([]byte, error) {{
	if memoryID == 0 || memoryID > DecayConfidenceMemoryIDMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationDecayConfidence, 0, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 8)...)
	binary.LittleEndian.PutUint64(request[EnvelopeHeaderLen:], memoryID)
	return request, nil
}}

// DecodeDecayConfidenceRequest validates the envelope and the bounded memory.
func DecodeDecayConfidenceRequest(request []byte) (uint64, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationDecayConfidence || header.Flags != 0 ||
		header.PayloadLen != 8 || len(request) != int(EnvelopeHeaderLen)+8 {{
		return 0, ErrMalformedEnvelope
	}}
	memoryID := binary.LittleEndian.Uint64(request[EnvelopeHeaderLen:])
	if memoryID == 0 || memoryID > DecayConfidenceMemoryIDMax {{
		return 0, ErrMalformedEnvelope
	}}
	return memoryID, nil
}}

// EncodeDecayConfidenceReply acknowledges the decay without a payload.
func EncodeDecayConfidenceReply() ([]byte, error) {{
	header, err := EncodeReplyHeader(OperationDecayConfidence, ResultOK, 0)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	return header, nil
}}

// DecodeDecayConfidenceReply validates the acknowledgement and refuses payload.
func DecodeDecayConfidenceReply(reply []byte) error {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationDecayConfidence || header.Result != ResultOK ||
		header.PayloadLen != 0 || len(reply) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeWorkspaceTagInsertRequest emits the memory and the workspace it is
// attributed to.
func EncodeWorkspaceTagInsertRequest(memoryID uint64, workspace string) ([]byte, error) {{
	if memoryID == 0 || memoryID > WorkspaceTagInsertMemoryIDMax || len(workspace) == 0 ||
		len(workspace) > WorkspaceTagInsertWorkspaceMax || hasNUL(workspace) {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 12 + len(workspace)
	header, err := EncodeRequestHeader(OperationWorkspaceTagInsert, 0, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, payloadLen)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint64(payload, memoryID)
	binary.LittleEndian.PutUint32(payload[8:], uint32(len(workspace)))
	copy(payload[12:], workspace)
	return request, nil
}}

// DecodeWorkspaceTagInsertRequest validates the envelope, memory, and workspace.
func DecodeWorkspaceTagInsertRequest(request []byte) (uint64, string, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationWorkspaceTagInsert || header.Flags != 0 ||
		header.PayloadLen < 13 ||
		len(request) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return 0, "", ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	memoryID := binary.LittleEndian.Uint64(payload)
	workspaceLen := binary.LittleEndian.Uint32(payload[8:])
	if memoryID == 0 || memoryID > WorkspaceTagInsertMemoryIDMax || workspaceLen == 0 ||
		workspaceLen > uint32(WorkspaceTagInsertWorkspaceMax) ||
		12+workspaceLen != header.PayloadLen {{
		return 0, "", ErrMalformedEnvelope
	}}
	workspace := string(payload[12 : 12+workspaceLen])
	if hasNUL(workspace) {{
		return 0, "", ErrMalformedEnvelope
	}}
	return memoryID, workspace, nil
}}

// EncodeWorkspaceTagInsertReply acknowledges the attribution without a payload.
func EncodeWorkspaceTagInsertReply() ([]byte, error) {{
	header, err := EncodeReplyHeader(OperationWorkspaceTagInsert, ResultOK, 0)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	return header, nil
}}

// DecodeWorkspaceTagInsertReply validates it and refuses any payload.
func DecodeWorkspaceTagInsertReply(reply []byte) error {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationWorkspaceTagInsert ||
		header.Result != ResultOK || header.PayloadLen != 0 ||
		len(reply) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeSetCognifiedKindRequest emits the memory and its cognified kind.
func EncodeSetCognifiedKindRequest(memoryID uint64, kind string) ([]byte, error) {{
	if memoryID == 0 || memoryID > SetCognifiedKindMemoryIDMax || len(kind) == 0 ||
		len(kind) > SetCognifiedKindKindMax || hasNUL(kind) {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 12 + len(kind)
	header, err := EncodeRequestHeader(OperationSetCognifiedKind, 0, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, payloadLen)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint64(payload, memoryID)
	binary.LittleEndian.PutUint32(payload[8:], uint32(len(kind)))
	copy(payload[12:], kind)
	return request, nil
}}

// DecodeSetCognifiedKindRequest validates the envelope, memory, and kind.
func DecodeSetCognifiedKindRequest(request []byte) (uint64, string, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationSetCognifiedKind || header.Flags != 0 ||
		header.PayloadLen < 13 ||
		len(request) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return 0, "", ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	memoryID := binary.LittleEndian.Uint64(payload)
	kindLen := binary.LittleEndian.Uint32(payload[8:])
	if memoryID == 0 || memoryID > SetCognifiedKindMemoryIDMax || kindLen == 0 ||
		kindLen > uint32(SetCognifiedKindKindMax) || 12+kindLen != header.PayloadLen {{
		return 0, "", ErrMalformedEnvelope
	}}
	kind := string(payload[12 : 12+kindLen])
	if hasNUL(kind) {{
		return 0, "", ErrMalformedEnvelope
	}}
	return memoryID, kind, nil
}}

// EncodeSetCognifiedKindReply acknowledges the write without a payload.
func EncodeSetCognifiedKindReply() ([]byte, error) {{
	header, err := EncodeReplyHeader(OperationSetCognifiedKind, ResultOK, 0)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	return header, nil
}}

// DecodeSetCognifiedKindReply validates it and refuses any payload.
func DecodeSetCognifiedKindReply(reply []byte) error {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationSetCognifiedKind ||
		header.Result != ResultOK || header.PayloadLen != 0 ||
		len(reply) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeSetSourceSessionRequest emits the memory and its session. An empty
// session is accepted and clears the column.
func EncodeSetSourceSessionRequest(memoryID uint64, sessionID string) ([]byte, error) {{
	if memoryID == 0 || memoryID > SetSourceSessionMemoryIDMax ||
		len(sessionID) > SetSourceSessionSessionMax || hasNUL(sessionID) {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 12 + len(sessionID)
	header, err := EncodeRequestHeader(OperationSetSourceSession, 0, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, payloadLen)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint64(payload, memoryID)
	binary.LittleEndian.PutUint32(payload[8:], uint32(len(sessionID)))
	copy(payload[12:], sessionID)
	return request, nil
}}

// DecodeSetSourceSessionRequest validates the envelope, memory, and session.
func DecodeSetSourceSessionRequest(request []byte) (uint64, string, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationSetSourceSession || header.Flags != 0 ||
		header.PayloadLen < 12 ||
		len(request) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return 0, "", ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	memoryID := binary.LittleEndian.Uint64(payload)
	sessionLen := binary.LittleEndian.Uint32(payload[8:])
	if memoryID == 0 || memoryID > SetSourceSessionMemoryIDMax ||
		sessionLen > uint32(SetSourceSessionSessionMax) ||
		12+sessionLen != header.PayloadLen {{
		return 0, "", ErrMalformedEnvelope
	}}
	sessionID := string(payload[12 : 12+sessionLen])
	if hasNUL(sessionID) {{
		return 0, "", ErrMalformedEnvelope
	}}
	return memoryID, sessionID, nil
}}

// EncodeSetSourceSessionReply acknowledges the write without a payload.
func EncodeSetSourceSessionReply() ([]byte, error) {{
	header, err := EncodeReplyHeader(OperationSetSourceSession, ResultOK, 0)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	return header, nil
}}

// DecodeSetSourceSessionReply validates it and refuses any payload.
func DecodeSetSourceSessionReply(reply []byte) error {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationSetSourceSession ||
		header.Result != ResultOK || header.PayloadLen != 0 ||
		len(reply) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeNegationTokensUpdateRequest emits the memory and its extracted tokens.
// An empty token set is accepted and clears the column.
func EncodeNegationTokensUpdateRequest(memoryID uint64, tokens string) ([]byte, error) {{
	if memoryID == 0 || memoryID > NegationTokensUpdateMemoryIDMax ||
		len(tokens) > NegationTokensUpdateTokensMax || hasNUL(tokens) {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 12 + len(tokens)
	header, err := EncodeRequestHeader(OperationNegationTokensUpdate, 0, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, payloadLen)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint64(payload, memoryID)
	binary.LittleEndian.PutUint32(payload[8:], uint32(len(tokens)))
	copy(payload[12:], tokens)
	return request, nil
}}

// DecodeNegationTokensUpdateRequest validates the envelope, memory, and tokens.
func DecodeNegationTokensUpdateRequest(request []byte) (uint64, string, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationNegationTokensUpdate || header.Flags != 0 ||
		header.PayloadLen < 12 ||
		len(request) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return 0, "", ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	memoryID := binary.LittleEndian.Uint64(payload)
	tokensLen := binary.LittleEndian.Uint32(payload[8:])
	if memoryID == 0 || memoryID > NegationTokensUpdateMemoryIDMax ||
		tokensLen > uint32(NegationTokensUpdateTokensMax) ||
		12+tokensLen != header.PayloadLen {{
		return 0, "", ErrMalformedEnvelope
	}}
	tokens := string(payload[12 : 12+tokensLen])
	if hasNUL(tokens) {{
		return 0, "", ErrMalformedEnvelope
	}}
	return memoryID, tokens, nil
}}

// EncodeNegationTokensUpdateReply acknowledges the write without a payload.
func EncodeNegationTokensUpdateReply() ([]byte, error) {{
	header, err := EncodeReplyHeader(OperationNegationTokensUpdate, ResultOK, 0)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	return header, nil
}}

// DecodeNegationTokensUpdateReply validates it and refuses any payload.
func DecodeNegationTokensUpdateReply(reply []byte) error {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationNegationTokensUpdate ||
		header.Result != ResultOK || header.PayloadLen != 0 ||
		len(reply) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeGetContentRequest emits the memory whose text is read.
func EncodeGetContentRequest(memoryID uint64) ([]byte, error) {{
	if memoryID == 0 || memoryID > GetContentMemoryIDMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationGetContent, 0, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 8)...)
	binary.LittleEndian.PutUint64(request[EnvelopeHeaderLen:], memoryID)
	return request, nil
}}

// DecodeGetContentRequest validates the envelope and the bounded memory.
func DecodeGetContentRequest(request []byte) (uint64, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationGetContent || header.Flags != 0 ||
		header.PayloadLen != 8 || len(request) != int(EnvelopeHeaderLen)+8 {{
		return 0, ErrMalformedEnvelope
	}}
	memoryID := binary.LittleEndian.Uint64(request[EnvelopeHeaderLen:])
	if memoryID == 0 || memoryID > GetContentMemoryIDMax {{
		return 0, ErrMalformedEnvelope
	}}
	return memoryID, nil
}}

// EncodeGetContentReply carries the content, or reports that the memory is
// absent. A missing row and a row holding "" are different answers, so
// not_found carries no payload at all rather than an empty one.
func EncodeGetContentReply(result uint32, content string) ([]byte, error) {{
	if result == ResultNotFound {{
		if content != "" {{
			return nil, ErrMalformedEnvelope
		}}
		header, err := EncodeReplyHeader(OperationGetContent, result, 0)
		if err != nil {{
			return nil, ErrMalformedEnvelope
		}}
		return header, nil
	}}
	if result != ResultOK || len(content) > GetContentContentMax || hasNUL(content) {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 4 + len(content)
	header, err := EncodeReplyHeader(OperationGetContent, ResultOK, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, payloadLen)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload, uint32(len(content)))
	copy(payload[4:], content)
	return reply, nil
}}

// DecodeGetContentReply keeps "no such memory" distinct from empty content.
func DecodeGetContentReply(reply []byte) (uint32, string, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationGetContent {{
		return 0, "", ErrMalformedEnvelope
	}}
	if header.Result == ResultNotFound && header.PayloadLen == 0 &&
		len(reply) == int(EnvelopeHeaderLen) {{
		return header.Result, "", nil
	}}
	if header.Result != ResultOK || header.PayloadLen < 4 ||
		len(reply) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return 0, "", ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	contentLen := binary.LittleEndian.Uint32(payload)
	if contentLen > uint32(GetContentContentMax) || 4+contentLen != header.PayloadLen {{
		return 0, "", ErrMalformedEnvelope
	}}
	content := string(payload[4 : 4+contentLen])
	if hasNUL(content) {{
		return 0, "", ErrMalformedEnvelope
	}}
	return header.Result, content, nil
}}

// EncodeGetSourceSessionRequest emits the memory whose session is read.
func EncodeGetSourceSessionRequest(memoryID uint64) ([]byte, error) {{
	if memoryID == 0 || memoryID > GetSourceSessionMemoryIDMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationGetSourceSession, 0, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 8)...)
	binary.LittleEndian.PutUint64(request[EnvelopeHeaderLen:], memoryID)
	return request, nil
}}

// DecodeGetSourceSessionRequest validates the envelope and the bounded memory.
func DecodeGetSourceSessionRequest(request []byte) (uint64, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationGetSourceSession || header.Flags != 0 ||
		header.PayloadLen != 8 || len(request) != int(EnvelopeHeaderLen)+8 {{
		return 0, ErrMalformedEnvelope
	}}
	memoryID := binary.LittleEndian.Uint64(request[EnvelopeHeaderLen:])
	if memoryID == 0 || memoryID > GetSourceSessionMemoryIDMax {{
		return 0, ErrMalformedEnvelope
	}}
	return memoryID, nil
}}

// EncodeGetSourceSessionReply carries a non-empty session, or reports that
// none is set. Unlike get_content there is no empty-ok: this backend cannot
// tell a blank column from an absent memory, so neither can the wire.
func EncodeGetSourceSessionReply(result uint32, sessionID string) ([]byte, error) {{
	if result == ResultNotFound {{
		if sessionID != "" {{
			return nil, ErrMalformedEnvelope
		}}
		header, err := EncodeReplyHeader(OperationGetSourceSession, result, 0)
		if err != nil {{
			return nil, ErrMalformedEnvelope
		}}
		return header, nil
	}}
	if result != ResultOK || len(sessionID) == 0 ||
		len(sessionID) > GetSourceSessionSessionMax || hasNUL(sessionID) {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 4 + len(sessionID)
	header, err := EncodeReplyHeader(OperationGetSourceSession, ResultOK, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, payloadLen)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload, uint32(len(sessionID)))
	copy(payload[4:], sessionID)
	return reply, nil
}}

// DecodeGetSourceSessionReply refuses an empty ok for the same reason.
func DecodeGetSourceSessionReply(reply []byte) (uint32, string, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationGetSourceSession {{
		return 0, "", ErrMalformedEnvelope
	}}
	if header.Result == ResultNotFound && header.PayloadLen == 0 &&
		len(reply) == int(EnvelopeHeaderLen) {{
		return header.Result, "", nil
	}}
	if header.Result != ResultOK || header.PayloadLen < 5 ||
		len(reply) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return 0, "", ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	sessionLen := binary.LittleEndian.Uint32(payload)
	if sessionLen == 0 || sessionLen > uint32(GetSourceSessionSessionMax) ||
		4+sessionLen != header.PayloadLen {{
		return 0, "", ErrMalformedEnvelope
	}}
	sessionID := string(payload[4 : 4+sessionLen])
	if hasNUL(sessionID) {{
		return 0, "", ErrMalformedEnvelope
	}}
	return header.Result, sessionID, nil
}}

// EncodePickFirstTemporalRefRequest emits the memory whose references are
// ranked. The ordering is fixed inside the statement, so nothing steers it.
func EncodePickFirstTemporalRefRequest(memoryID uint64) ([]byte, error) {{
	if memoryID == 0 || memoryID > PickFirstTemporalRefMemoryIDMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationPickFirstTemporalRef, 0, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 8)...)
	binary.LittleEndian.PutUint64(request[EnvelopeHeaderLen:], memoryID)
	return request, nil
}}

// DecodePickFirstTemporalRefRequest validates the envelope and the memory.
func DecodePickFirstTemporalRefRequest(request []byte) (uint64, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationPickFirstTemporalRef || header.Flags != 0 ||
		header.PayloadLen != 8 || len(request) != int(EnvelopeHeaderLen)+8 {{
		return 0, ErrMalformedEnvelope
	}}
	memoryID := binary.LittleEndian.Uint64(request[EnvelopeHeaderLen:])
	if memoryID == 0 || memoryID > PickFirstTemporalRefMemoryIDMax {{
		return 0, ErrMalformedEnvelope
	}}
	return memoryID, nil
}}

// EncodePickFirstTemporalRefReply carries a non-empty reference key, or
// reports that the memory has none.
func EncodePickFirstTemporalRefReply(result uint32, refKey string) ([]byte, error) {{
	if result == ResultNotFound {{
		if refKey != "" {{
			return nil, ErrMalformedEnvelope
		}}
		header, err := EncodeReplyHeader(OperationPickFirstTemporalRef, result, 0)
		if err != nil {{
			return nil, ErrMalformedEnvelope
		}}
		return header, nil
	}}
	if result != ResultOK || len(refKey) == 0 ||
		len(refKey) > PickFirstTemporalRefKeyMax || hasNUL(refKey) {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 4 + len(refKey)
	header, err := EncodeReplyHeader(OperationPickFirstTemporalRef, ResultOK, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, payloadLen)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload, uint32(len(refKey)))
	copy(payload[4:], refKey)
	return reply, nil
}}

// DecodePickFirstTemporalRefReply refuses an empty ok for the same reason.
func DecodePickFirstTemporalRefReply(reply []byte) (uint32, string, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationPickFirstTemporalRef {{
		return 0, "", ErrMalformedEnvelope
	}}
	if header.Result == ResultNotFound && header.PayloadLen == 0 &&
		len(reply) == int(EnvelopeHeaderLen) {{
		return header.Result, "", nil
	}}
	if header.Result != ResultOK || header.PayloadLen < 5 ||
		len(reply) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return 0, "", ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	keyLen := binary.LittleEndian.Uint32(payload)
	if keyLen == 0 || keyLen > uint32(PickFirstTemporalRefKeyMax) ||
		4+keyLen != header.PayloadLen {{
		return 0, "", ErrMalformedEnvelope
	}}
	refKey := string(payload[4 : 4+keyLen])
	if hasNUL(refKey) {{
		return 0, "", ErrMalformedEnvelope
	}}
	return header.Result, refKey, nil
}}

// EncodeCountAndMaxUpdatedRequest emits the empty request envelope.
func EncodeCountAndMaxUpdatedRequest() []byte {{
	header, err := EncodeRequestHeader(OperationCountAndMaxUpdated, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeCountAndMaxUpdatedRequest validates the exact envelope.
func DecodeCountAndMaxUpdatedRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationCountAndMaxUpdated ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeCountAndMaxUpdatedReply carries the count and the stamp together, or
// reports that the aggregate could not be computed. An empty stamp is a real
// answer -- an empty corpus has no latest update -- so it rides on ok.
func EncodeCountAndMaxUpdatedReply(result uint32, count uint32, maxUpdatedAt string) ([]byte, error) {{
	if result == ResultInvalidState {{
		if count != 0 || maxUpdatedAt != "" {{
			return nil, ErrMalformedEnvelope
		}}
		header, err := EncodeReplyHeader(OperationCountAndMaxUpdated, result, 0)
		if err != nil {{
			return nil, ErrMalformedEnvelope
		}}
		return header, nil
	}}
	if result != ResultOK || count > CountAndMaxUpdatedCountMax ||
		len(maxUpdatedAt) > CountAndMaxUpdatedStampMax || hasNUL(maxUpdatedAt) {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 8 + len(maxUpdatedAt)
	header, err := EncodeReplyHeader(OperationCountAndMaxUpdated, ResultOK, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, payloadLen)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload, count)
	binary.LittleEndian.PutUint32(payload[4:], uint32(len(maxUpdatedAt)))
	copy(payload[8:], maxUpdatedAt)
	return reply, nil
}}

// DecodeCountAndMaxUpdatedReply keeps an empty corpus distinct from a failure.
func DecodeCountAndMaxUpdatedReply(reply []byte) (uint32, uint32, string, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationCountAndMaxUpdated {{
		return 0, 0, "", ErrMalformedEnvelope
	}}
	if header.Result == ResultInvalidState && header.PayloadLen == 0 &&
		len(reply) == int(EnvelopeHeaderLen) {{
		return header.Result, 0, "", nil
	}}
	if header.Result != ResultOK || header.PayloadLen < 8 ||
		len(reply) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return 0, 0, "", ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	count := binary.LittleEndian.Uint32(payload)
	stampLen := binary.LittleEndian.Uint32(payload[4:])
	if count > CountAndMaxUpdatedCountMax || stampLen > uint32(CountAndMaxUpdatedStampMax) ||
		8+stampLen != header.PayloadLen {{
		return 0, 0, "", ErrMalformedEnvelope
	}}
	maxUpdatedAt := string(payload[8 : 8+stampLen])
	if hasNUL(maxUpdatedAt) {{
		return 0, 0, "", ErrMalformedEnvelope
	}}
	return header.Result, count, maxUpdatedAt, nil
}}

// EncodeEntityEdgePruneOrphansRequest emits the empty request envelope. The
// tiers that count as a surviving reference are policy and never travel.
func EncodeEntityEdgePruneOrphansRequest() []byte {{
	header, err := EncodeRequestHeader(OperationEntityEdgePruneOrphans, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeEntityEdgePruneOrphansRequest validates the exact index-family envelope.
func DecodeEntityEdgePruneOrphansRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationEntityEdgePruneOrphans ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeEntityEdgePruneOrphansReply emits one bounded u32 prune count.
func EncodeEntityEdgePruneOrphansReply(prunedCount uint32) ([]byte, error) {{
	if prunedCount > EntityEdgePruneOrphansCountMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationEntityEdgePruneOrphans, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], prunedCount)
	return reply, nil
}}

// DecodeEntityEdgePruneOrphansReply validates the operation and bounded count.
func DecodeEntityEdgePruneOrphansReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationEntityEdgePruneOrphans ||
		header.Result != ResultOK || header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	prunedCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if prunedCount > EntityEdgePruneOrphansCountMax {{
		return 0, ErrMalformedEnvelope
	}}
	return prunedCount, nil
}}

// EncodeEntityEdgeNormalizeWeightsRequest emits the empty request envelope.
func EncodeEntityEdgeNormalizeWeightsRequest() []byte {{
	header, err := EncodeRequestHeader(OperationEntityEdgeNormalizeWeights, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeEntityEdgeNormalizeWeightsRequest validates the exact envelope.
func DecodeEntityEdgeNormalizeWeightsRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationEntityEdgeNormalizeWeights ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeEntityEdgeNormalizeWeightsReply emits one bounded u32 rescale count.
// A converged graph reports zero: the statement skips rows already holding
// their normalised value rather than rewriting them.
func EncodeEntityEdgeNormalizeWeightsReply(normalizedCount uint32) ([]byte, error) {{
	if normalizedCount > EntityEdgeNormalizeWeightsCountMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationEntityEdgeNormalizeWeights, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], normalizedCount)
	return reply, nil
}}

// DecodeEntityEdgeNormalizeWeightsReply validates operation and bounded count.
func DecodeEntityEdgeNormalizeWeightsReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationEntityEdgeNormalizeWeights ||
		header.Result != ResultOK || header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	normalizedCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if normalizedCount > EntityEdgeNormalizeWeightsCountMax {{
		return 0, ErrMalformedEnvelope
	}}
	return normalizedCount, nil
}}

// EncodeProjectCountRequest emits the empty request envelope. Which lifecycle
// state counts is policy and never travels.
func EncodeProjectCountRequest() []byte {{
	header, err := EncodeRequestHeader(OperationProjectCount, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeProjectCountRequest validates the exact index-family envelope.
func DecodeProjectCountRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationProjectCount ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeProjectCountReply emits one bounded u32 project count.
func EncodeProjectCountReply(projectCount uint32) ([]byte, error) {{
	if projectCount > ProjectCountMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationProjectCount, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], projectCount)
	return reply, nil
}}

// DecodeProjectCountReply validates the operation and bounded count.
func DecodeProjectCountReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationProjectCount || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	projectCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if projectCount > ProjectCountMax {{
		return 0, ErrMalformedEnvelope
	}}
	return projectCount, nil
}}

// EncodePurgeHiddenPollutionRequest emits the empty request envelope. The
// sweep's reach is policy and never travels.
func EncodePurgeHiddenPollutionRequest() []byte {{
	header, err := EncodeRequestHeader(OperationPurgeHiddenPollution, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodePurgeHiddenPollutionRequest validates the exact index-family envelope.
func DecodePurgeHiddenPollutionRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationPurgeHiddenPollution ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodePurgeHiddenPollutionReply emits one bounded u32 purge count.
func EncodePurgeHiddenPollutionReply(purgedCount uint32) ([]byte, error) {{
	if purgedCount > PurgeHiddenPollutionMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationPurgeHiddenPollution, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], purgedCount)
	return reply, nil
}}

// DecodePurgeHiddenPollutionReply validates the operation and bounded count.
func DecodePurgeHiddenPollutionReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationPurgeHiddenPollution ||
		header.Result != ResultOK || header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	purgedCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if purgedCount > PurgeHiddenPollutionMax {{
		return 0, ErrMalformedEnvelope
	}}
	return purgedCount, nil
}}

// EncodeRequeueDriftedRequest emits the empty request envelope. The forced
// re-ingest and the dedup rule are policy and never travel.
func EncodeRequeueDriftedRequest() []byte {{
	header, err := EncodeRequestHeader(OperationRequeueDrifted, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeRequeueDriftedRequest validates the exact index-family envelope.
func DecodeRequeueDriftedRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationRequeueDrifted ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeRequeueDriftedReply emits one bounded u32 requeue count.
func EncodeRequeueDriftedReply(requeuedCount uint32) ([]byte, error) {{
	if requeuedCount > RequeueDriftedMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationRequeueDrifted, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], requeuedCount)
	return reply, nil
}}

// DecodeRequeueDriftedReply validates the operation and bounded count.
func DecodeRequeueDriftedReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationRequeueDrifted || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	requeuedCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if requeuedCount > RequeueDriftedMax {{
		return 0, ErrMalformedEnvelope
	}}
	return requeuedCount, nil
}}

// EncodeProspectiveSweepExpiredRequest emits the empty request envelope. The
// states and the clock are policy and never travel.
func EncodeProspectiveSweepExpiredRequest() []byte {{
	header, err := EncodeRequestHeader(OperationProspectiveSweepExpired, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeProspectiveSweepExpiredRequest validates the exact maintenance-family
// envelope. It is the first operation of that family on the wire.
func DecodeProspectiveSweepExpiredRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationProspectiveSweepExpired ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeProspectiveSweepExpiredReply emits one bounded u32 expiry count.
func EncodeProspectiveSweepExpiredReply(expiredCount uint32) ([]byte, error) {{
	if expiredCount > ProspectiveSweepExpiredMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationProspectiveSweepExpired, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], expiredCount)
	return reply, nil
}}

// DecodeProspectiveSweepExpiredReply validates the operation and bounded count.
func DecodeProspectiveSweepExpiredReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationProspectiveSweepExpired ||
		header.Result != ResultOK || header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	expiredCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if expiredCount > ProspectiveSweepExpiredMax {{
		return 0, ErrMalformedEnvelope
	}}
	return expiredCount, nil
}}

// EncodeDirectiveSweepExpiredRequest emits the empty request envelope. The
// states and the clock are policy and never travel.
func EncodeDirectiveSweepExpiredRequest() []byte {{
	header, err := EncodeRequestHeader(OperationDirectiveSweepExpired, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeDirectiveSweepExpiredRequest validates the exact maintenance-family
// envelope.
func DecodeDirectiveSweepExpiredRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationDirectiveSweepExpired ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeDirectiveSweepExpiredReply emits one bounded u32 directive expiry count.
func EncodeDirectiveSweepExpiredReply(directivesExpired uint32) ([]byte, error) {{
	if directivesExpired > DirectiveSweepExpiredMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationDirectiveSweepExpired, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], directivesExpired)
	return reply, nil
}}

// DecodeDirectiveSweepExpiredReply validates the operation and bounded count.
func DecodeDirectiveSweepExpiredReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationDirectiveSweepExpired ||
		header.Result != ResultOK || header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	directivesExpired := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if directivesExpired > DirectiveSweepExpiredMax {{
		return 0, ErrMalformedEnvelope
	}}
	return directivesExpired, nil
}}

// EncodeTotalCountRequest emits the empty request envelope for the global memory count.
func EncodeTotalCountRequest() []byte {{
	header, err := EncodeRequestHeader(OperationTotalCount, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeTotalCountRequest validates the exact memory-family operation envelope.
func DecodeTotalCountRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationTotalCount ||
		header.Flags != 0 || header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeTotalCountReply emits one bounded u64 success payload.
func EncodeTotalCountReply(count uint64) ([]byte, error) {{
	if count > TotalCountMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationTotalCount, ResultOK, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 8)...)
	binary.LittleEndian.PutUint64(reply[EnvelopeHeaderLen:], count)
	return reply, nil
}}

// DecodeTotalCountReply validates the operation and bounded count.
func DecodeTotalCountReply(reply []byte) (uint64, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationTotalCount || header.Result != ResultOK ||
		header.PayloadLen != 8 {{
		return 0, ErrMalformedEnvelope
	}}
	count := binary.LittleEndian.Uint64(reply[EnvelopeHeaderLen:])
	if count > TotalCountMax {{
		return 0, ErrMalformedEnvelope
	}}
	return count, nil
}}

// EncodeSessionL2CountRequest emits one non-empty bounded session identifier.
func EncodeSessionL2CountRequest(sourceSession string) ([]byte, error) {{
	if len(sourceSession) == 0 || len(sourceSession) > SessionL2CountSessionMax {{
		return nil, ErrMalformedEnvelope
	}}
	for index := 0; index < len(sourceSession); index++ {{
		if sourceSession[index] == 0 {{
			return nil, ErrMalformedEnvelope
		}}
	}}
	header, err := EncodeRequestHeader(OperationSessionL2Count, 0, uint32(4+len(sourceSession)))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 4+len(sourceSession))...)
	binary.LittleEndian.PutUint32(request[EnvelopeHeaderLen:], uint32(len(sourceSession)))
	copy(request[EnvelopeHeaderLen+4:], sourceSession)
	return request, nil
}}

// DecodeSessionL2CountRequest validates and returns the bounded session identifier.
func DecodeSessionL2CountRequest(request []byte) (string, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationSessionL2Count || header.Flags != 0 ||
		header.PayloadLen < 5 {{
		return "", ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	decodedLen := binary.LittleEndian.Uint32(payload[:4])
	if decodedLen == 0 || decodedLen > SessionL2CountSessionMax ||
		header.PayloadLen != 4+decodedLen {{
		return "", ErrMalformedEnvelope
	}}
	sourceSession := string(payload[4:])
	for index := 0; index < len(sourceSession); index++ {{
		if sourceSession[index] == 0 {{
			return "", ErrMalformedEnvelope
		}}
	}}
	return sourceSession, nil
}}

// EncodeSessionL2CountReply emits one bounded u32 success payload.
func EncodeSessionL2CountReply(count uint32) ([]byte, error) {{
	if count > SessionL2CountMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationSessionL2Count, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], count)
	return reply, nil
}}

// DecodeSessionL2CountReply validates the operation and bounded count.
func DecodeSessionL2CountReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationSessionL2Count || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	count := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if count > SessionL2CountMax {{
		return 0, ErrMalformedEnvelope
	}}
	return count, nil
}}

// EncodeKeyExistsRequest emits one non-empty bounded canonical memory key.
func EncodeKeyExistsRequest(key string) ([]byte, error) {{
	if len(key) == 0 || len(key) > KeyExistsKeyMax {{
		return nil, ErrMalformedEnvelope
	}}
	for index := 0; index < len(key); index++ {{
		if key[index] == 0 {{
			return nil, ErrMalformedEnvelope
		}}
	}}
	header, err := EncodeRequestHeader(OperationKeyExists, 0, uint32(4+len(key)))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 4+len(key))...)
	binary.LittleEndian.PutUint32(request[EnvelopeHeaderLen:], uint32(len(key)))
	copy(request[EnvelopeHeaderLen+4:], key)
	return request, nil
}}

// DecodeKeyExistsRequest validates and returns the bounded canonical memory key.
func DecodeKeyExistsRequest(request []byte) (string, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationKeyExists || header.Flags != 0 ||
		header.PayloadLen < 5 {{
		return "", ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	decodedLen := binary.LittleEndian.Uint32(payload[:4])
	if decodedLen == 0 || decodedLen > KeyExistsKeyMax || header.PayloadLen != 4+decodedLen {{
		return "", ErrMalformedEnvelope
	}}
	key := string(payload[4:])
	for index := 0; index < len(key); index++ {{
		if key[index] == 0 {{
			return "", ErrMalformedEnvelope
		}}
	}}
	return key, nil
}}

// EncodeKeyExistsReply emits one boolean u32 success payload.
func EncodeKeyExistsReply(exists uint32) ([]byte, error) {{
	if exists > KeyExistsMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationKeyExists, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], exists)
	return reply, nil
}}

// DecodeKeyExistsReply validates the operation and boolean value.
func DecodeKeyExistsReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationKeyExists || header.Result != ResultOK ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	exists := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if exists > KeyExistsMax {{
		return 0, ErrMalformedEnvelope
	}}
	return exists, nil
}}

// EncodeFindIDByKeyKindRequest emits bounded canonical key and kind fields.
func EncodeFindIDByKeyKindRequest(key, kind string) ([]byte, error) {{
	if len(key) == 0 || len(key) > FindIDByKeyKindKeyMax ||
		len(kind) == 0 || len(kind) > FindIDByKeyKindKindMax {{
		return nil, ErrMalformedEnvelope
	}}
	for index := 0; index < len(key); index++ {{
		if key[index] == 0 {{
			return nil, ErrMalformedEnvelope
		}}
	}}
	for index := 0; index < len(kind); index++ {{
		if kind[index] == 0 {{
			return nil, ErrMalformedEnvelope
		}}
	}}
	payloadLen := 8 + len(key) + len(kind)
	header, err := EncodeRequestHeader(OperationFindIDByKeyKind, 0, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, payloadLen)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload, uint32(len(key)))
	copy(payload[4:], key)
	binary.LittleEndian.PutUint32(payload[4+len(key):], uint32(len(kind)))
	copy(payload[8+len(key):], kind)
	return request, nil
}}

// DecodeFindIDByKeyKindRequest validates and returns the canonical key and kind.
func DecodeFindIDByKeyKindRequest(request []byte) (string, string, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationFindIDByKeyKind || header.Flags != 0 ||
		header.PayloadLen < 10 {{
		return "", "", ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	keyLen := binary.LittleEndian.Uint32(payload[:4])
	if keyLen == 0 || keyLen > FindIDByKeyKindKeyMax ||
		uint64(8)+uint64(keyLen)+1 > uint64(header.PayloadLen) {{
		return "", "", ErrMalformedEnvelope
	}}
	kindOffset := 4 + int(keyLen)
	kindLen := binary.LittleEndian.Uint32(payload[kindOffset : kindOffset+4])
	if kindLen == 0 || kindLen > FindIDByKeyKindKindMax ||
		uint64(header.PayloadLen) != 8+uint64(keyLen)+uint64(kindLen) {{
		return "", "", ErrMalformedEnvelope
	}}
	key := string(payload[4:kindOffset])
	kind := string(payload[kindOffset+4:])
	for index := 0; index < len(key); index++ {{
		if key[index] == 0 {{
			return "", "", ErrMalformedEnvelope
		}}
	}}
	for index := 0; index < len(kind); index++ {{
		if kind[index] == 0 {{
			return "", "", ErrMalformedEnvelope
		}}
	}}
	return key, kind, nil
}}

// EncodeFindIDByKeyKindReply emits consistent found state and identifier fields.
func EncodeFindIDByKeyKindReply(found uint32, id uint64) ([]byte, error) {{
	if found > FindIDByKeyKindFoundMax || id > FindIDByKeyKindIDMax ||
		found == 0 && id != 0 || found == 1 && id == 0 {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationFindIDByKeyKind, ResultOK, 12)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 12)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], found)
	binary.LittleEndian.PutUint64(reply[EnvelopeHeaderLen+4:], id)
	return reply, nil
}}

// DecodeFindIDByKeyKindReply validates the operation and consistent lookup result.
func DecodeFindIDByKeyKindReply(reply []byte) (uint32, uint64, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationFindIDByKeyKind ||
		header.Result != ResultOK || header.PayloadLen != 12 {{
		return 0, 0, ErrMalformedEnvelope
	}}
	found := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	id := binary.LittleEndian.Uint64(reply[EnvelopeHeaderLen+4:])
	if found > FindIDByKeyKindFoundMax || id > FindIDByKeyKindIDMax ||
		found == 0 && id != 0 || found == 1 && id == 0 {{
		return 0, 0, ErrMalformedEnvelope
	}}
	return found, id, nil
}}

// EncodeKeyExistsInTierPairRequest emits a bounded canonical key and tier pair.
func EncodeKeyExistsInTierPairRequest(key, tierA, tierB string) ([]byte, error) {{
	if len(key) == 0 || len(key) > KeyExistsInTierPairKeyMax ||
		len(tierA) == 0 || len(tierA) > KeyExistsInTierPairTierAMax ||
		len(tierB) == 0 || len(tierB) > KeyExistsInTierPairTierBMax {{
		return nil, ErrMalformedEnvelope
	}}
	for _, value := range []string{{key, tierA, tierB}} {{
		for index := 0; index < len(value); index++ {{
			if value[index] == 0 {{
				return nil, ErrMalformedEnvelope
			}}
		}}
	}}
	payloadLen := 12 + len(key) + len(tierA) + len(tierB)
	header, err := EncodeRequestHeader(OperationKeyExistsInTierPair, 0, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, payloadLen)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload, uint32(len(key)))
	copy(payload[4:], key)
	tierAOffset := 4 + len(key)
	binary.LittleEndian.PutUint32(payload[tierAOffset:], uint32(len(tierA)))
	copy(payload[tierAOffset+4:], tierA)
	tierBOffset := tierAOffset + 4 + len(tierA)
	binary.LittleEndian.PutUint32(payload[tierBOffset:], uint32(len(tierB)))
	copy(payload[tierBOffset+4:], tierB)
	return request, nil
}}

// DecodeKeyExistsInTierPairRequest validates and returns the canonical key and tier pair.
func DecodeKeyExistsInTierPairRequest(request []byte) (string, string, string, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationKeyExistsInTierPair || header.Flags != 0 ||
		header.PayloadLen < 15 {{
		return "", "", "", ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	keyLen := binary.LittleEndian.Uint32(payload[:4])
	if keyLen == 0 || keyLen > KeyExistsInTierPairKeyMax ||
		uint64(12)+uint64(keyLen)+2 > uint64(header.PayloadLen) {{
		return "", "", "", ErrMalformedEnvelope
	}}
	tierAOffset := 4 + int(keyLen)
	tierALen := binary.LittleEndian.Uint32(payload[tierAOffset : tierAOffset+4])
	if tierALen == 0 || tierALen > KeyExistsInTierPairTierAMax ||
		uint64(12)+uint64(keyLen)+uint64(tierALen)+1 > uint64(header.PayloadLen) {{
		return "", "", "", ErrMalformedEnvelope
	}}
	tierBOffset := tierAOffset + 4 + int(tierALen)
	tierBLen := binary.LittleEndian.Uint32(payload[tierBOffset : tierBOffset+4])
	if tierBLen == 0 || tierBLen > KeyExistsInTierPairTierBMax ||
		uint64(header.PayloadLen) != 12+uint64(keyLen)+uint64(tierALen)+uint64(tierBLen) {{
		return "", "", "", ErrMalformedEnvelope
	}}
	key := string(payload[4:tierAOffset])
	tierA := string(payload[tierAOffset+4 : tierBOffset])
	tierB := string(payload[tierBOffset+4:])
	for _, value := range []string{{key, tierA, tierB}} {{
		for index := 0; index < len(value); index++ {{
			if value[index] == 0 {{
				return "", "", "", ErrMalformedEnvelope
			}}
		}}
	}}
	return key, tierA, tierB, nil
}}

// EncodeKeyExistsInTierPairReply emits one boolean u32 success payload.
func EncodeKeyExistsInTierPairReply(exists uint32) ([]byte, error) {{
	if exists > KeyExistsInTierPairMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationKeyExistsInTierPair, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], exists)
	return reply, nil
}}

// DecodeKeyExistsInTierPairReply validates the operation and boolean value.
func DecodeKeyExistsInTierPairReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationKeyExistsInTierPair ||
		header.Result != ResultOK || header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	exists := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if exists > KeyExistsInTierPairMax {{
		return 0, ErrMalformedEnvelope
	}}
	return exists, nil
}}

// EncodeEffectivenessUpdateRequest emits a canonical nullable binary64 update.
func EncodeEffectivenessUpdateRequest(memoryID uint64, hasValue uint32, value float64) ([]byte, error) {{
	valueBits := math.Float64bits(value)
	if memoryID == 0 || memoryID > EffectivenessUpdateMemoryIDMax ||
		hasValue > EffectivenessUpdateHasValueMax || hasValue == 0 && valueBits != 0 {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationEffectivenessUpdate, 0, 20)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 20)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint64(payload, memoryID)
	binary.LittleEndian.PutUint32(payload[8:], hasValue)
	binary.LittleEndian.PutUint64(payload[12:], valueBits)
	return request, nil
}}

// DecodeEffectivenessUpdateRequest validates and returns the nullable binary64 update.
func DecodeEffectivenessUpdateRequest(request []byte) (uint64, uint32, float64, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationEffectivenessUpdate || header.Flags != 0 ||
		header.PayloadLen != 20 {{
		return 0, 0, 0, ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	memoryID := binary.LittleEndian.Uint64(payload)
	hasValue := binary.LittleEndian.Uint32(payload[8:])
	valueBits := binary.LittleEndian.Uint64(payload[12:])
	if memoryID == 0 || memoryID > EffectivenessUpdateMemoryIDMax ||
		hasValue > EffectivenessUpdateHasValueMax || hasValue == 0 && valueBits != 0 {{
		return 0, 0, 0, ErrMalformedEnvelope
	}}
	return memoryID, hasValue, math.Float64frombits(valueBits), nil
}}

// EncodeEffectivenessUpdateReply emits a closed success or invalid-state result.
func EncodeEffectivenessUpdateReply(result uint32) ([]byte, error) {{
	if result != ResultOK && result != ResultInvalidState {{
		return nil, ErrMalformedEnvelope
	}}
	return EncodeReplyHeader(OperationEffectivenessUpdate, result, 0)
}}

// DecodeEffectivenessUpdateReply validates the closed result.
func DecodeEffectivenessUpdateReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationEffectivenessUpdate || header.PayloadLen != 0 ||
		header.Result != ResultOK && header.Result != ResultInvalidState {{
		return 0, ErrMalformedEnvelope
	}}
	return header.Result, nil
}}

// EncodeRetentionEnforceRequest emits the empty request for the fixed retention policy.
func EncodeRetentionEnforceRequest() []byte {{
	header, err := EncodeRequestHeader(OperationRetentionEnforce, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeRetentionEnforceRequest validates the exact empty operation envelope.
func DecodeRetentionEnforceRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationRetentionEnforce || header.Flags != 0 ||
		header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeRetentionEnforceReply emits the bounded total number of deleted rows.
func EncodeRetentionEnforceReply(deletedCount uint32) ([]byte, error) {{
	if deletedCount > RetentionEnforceMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationRetentionEnforce, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], deletedCount)
	return reply, nil
}}

// DecodeRetentionEnforceReply validates the operation and bounded deletion count.
func DecodeRetentionEnforceReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationRetentionEnforce ||
		header.Result != ResultOK || header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	deletedCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if deletedCount > RetentionEnforceMax {{
		return 0, ErrMalformedEnvelope
	}}
	return deletedCount, nil
}}

// EncodeEffectivenessDemoteRequest emits the empty request for the fixed threshold policy.
func EncodeEffectivenessDemoteRequest() []byte {{
	header, err := EncodeRequestHeader(OperationEffectivenessDemote, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeEffectivenessDemoteRequest validates the exact empty operation envelope.
func DecodeEffectivenessDemoteRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationEffectivenessDemote || header.Flags != 0 ||
		header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeEffectivenessDemoteReply emits the bounded number of demoted rows.
func EncodeEffectivenessDemoteReply(demotedCount uint32) ([]byte, error) {{
	if demotedCount > EffectivenessDemoteMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationEffectivenessDemote, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], demotedCount)
	return reply, nil
}}

// DecodeEffectivenessDemoteReply validates the operation and bounded demotion count.
func DecodeEffectivenessDemoteReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationEffectivenessDemote ||
		header.Result != ResultOK || header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	demotedCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if demotedCount > EffectivenessDemoteMax {{
		return 0, ErrMalformedEnvelope
	}}
	return demotedCount, nil
}}

// EffectivenessStats is the bounded aggregate effectiveness summary over the memory corpus.
type EffectivenessStats struct {{
	AvgEffectiveness      float64
	LowEffectivenessCount uint32
	HighImpactCount       uint32
}}

// hasNUL reports whether a wire string carries an embedded NUL. The C backend
// binds these as C strings, so a NUL would silently truncate the stored value.
func hasNUL(value string) bool {{
	for index := 0; index < len(value); index++ {{
		if value[index] == 0 {{
			return true
		}}
	}}
	return false
}}

// EncodeRecordL4ApprovalRequest emits the memory, the approver, and the note.
func EncodeRecordL4ApprovalRequest(memoryID uint64, approver, note string) ([]byte, error) {{
	if memoryID == 0 || memoryID > RecordL4ApprovalMemoryIDMax ||
		len(approver) == 0 || len(approver) > RecordL4ApprovalApproverMax ||
		len(note) > RecordL4ApprovalNoteMax ||
		hasNUL(approver) || hasNUL(note) {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 16 + len(approver) + len(note)
	header, err := EncodeRequestHeader(OperationRecordL4Approval, 0, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, payloadLen)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint64(payload, memoryID)
	binary.LittleEndian.PutUint32(payload[8:], uint32(len(approver)))
	copy(payload[12:], approver)
	binary.LittleEndian.PutUint32(payload[12+len(approver):], uint32(len(note)))
	copy(payload[16+len(approver):], note)
	return request, nil
}}

// DecodeRecordL4ApprovalRequest validates the operation and every bounded field.
func DecodeRecordL4ApprovalRequest(request []byte) (uint64, string, string, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationRecordL4Approval || header.Flags != 0 ||
		header.PayloadLen < 16 ||
		len(request) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return 0, "", "", ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	memoryID := binary.LittleEndian.Uint64(payload)
	approverLen := binary.LittleEndian.Uint32(payload[8:])
	if memoryID == 0 || memoryID > RecordL4ApprovalMemoryIDMax || approverLen == 0 ||
		approverLen > uint32(RecordL4ApprovalApproverMax) ||
		header.PayloadLen < 16+approverLen {{
		return 0, "", "", ErrMalformedEnvelope
	}}
	noteLen := binary.LittleEndian.Uint32(payload[12+approverLen:])
	if noteLen > uint32(RecordL4ApprovalNoteMax) ||
		header.PayloadLen != 16+approverLen+noteLen {{
		return 0, "", "", ErrMalformedEnvelope
	}}
	approver := string(payload[12 : 12+approverLen])
	note := string(payload[16+approverLen : 16+approverLen+noteLen])
	if hasNUL(approver) || hasNUL(note) {{
		return 0, "", "", ErrMalformedEnvelope
	}}
	return memoryID, approver, note, nil
}}

// EncodeRecordL4ApprovalReply emits the payload-free acknowledgement.
func EncodeRecordL4ApprovalReply() ([]byte, error) {{
	header, err := EncodeReplyHeader(OperationRecordL4Approval, ResultOK, 0)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	return header, nil
}}

// DecodeRecordL4ApprovalReply validates the payload-free acknowledgement.
func DecodeRecordL4ApprovalReply(reply []byte) error {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationRecordL4Approval || header.Result != ResultOK ||
		header.PayloadLen != 0 || len(reply) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeReclassifyDirectivesRequest emits the approval gate, the operation's only input.
func EncodeReclassifyDirectivesRequest(requireApproval uint32) ([]byte, error) {{
	if requireApproval > ReclassifyDirectivesGateMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationReclassifyDirectives, 0, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(request[EnvelopeHeaderLen:], requireApproval)
	return request, nil
}}

// DecodeReclassifyDirectivesRequest validates the operation and the bounded gate.
func DecodeReclassifyDirectivesRequest(request []byte) (uint32, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationReclassifyDirectives || header.Flags != 0 ||
		header.PayloadLen != 4 || len(request) != int(EnvelopeHeaderLen)+4 {{
		return 0, ErrMalformedEnvelope
	}}
	requireApproval := binary.LittleEndian.Uint32(request[EnvelopeHeaderLen:])
	if requireApproval > ReclassifyDirectivesGateMax {{
		return 0, ErrMalformedEnvelope
	}}
	return requireApproval, nil
}}

// EncodeReclassifyDirectivesReply emits the bounded number of reclassified rows.
func EncodeReclassifyDirectivesReply(reclassifiedCount uint32) ([]byte, error) {{
	if reclassifiedCount > ReclassifyDirectivesMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationReclassifyDirectives, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], reclassifiedCount)
	return reply, nil
}}

// DecodeReclassifyDirectivesReply validates the operation and bounded count.
func DecodeReclassifyDirectivesReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationReclassifyDirectives ||
		header.Result != ResultOK || header.PayloadLen != 4 ||
		len(reply) != int(EnvelopeHeaderLen)+4 {{
		return 0, ErrMalformedEnvelope
	}}
	reclassifiedCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if reclassifiedCount > ReclassifyDirectivesMax {{
		return 0, ErrMalformedEnvelope
	}}
	return reclassifiedCount, nil
}}

// EncodePromoteStableRequest emits the empty request for the fixed stability policy.
func EncodePromoteStableRequest() []byte {{
	header, err := EncodeRequestHeader(OperationPromoteStable, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodePromoteStableRequest validates the exact empty operation envelope.
func DecodePromoteStableRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationPromoteStable || header.Flags != 0 ||
		header.PayloadLen != 0 || len(request) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodePromoteStableReply emits the bounded number of promoted rows.
func EncodePromoteStableReply(promotedCount uint32) ([]byte, error) {{
	if promotedCount > PromoteStableMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationPromoteStable, ResultOK, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], promotedCount)
	return reply, nil
}}

// DecodePromoteStableReply validates the operation and bounded promotion count.
func DecodePromoteStableReply(reply []byte) (uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationPromoteStable || header.Result != ResultOK ||
		header.PayloadLen != 4 || len(reply) != int(EnvelopeHeaderLen)+4 {{
		return 0, ErrMalformedEnvelope
	}}
	promotedCount := binary.LittleEndian.Uint32(reply[EnvelopeHeaderLen:])
	if promotedCount > PromoteStableMax {{
		return 0, ErrMalformedEnvelope
	}}
	return promotedCount, nil
}}

// EncodeDemoteRequest emits the empty request for the fixed demotion policy.
func EncodeDemoteRequest() []byte {{
	header, err := EncodeRequestHeader(OperationDemote, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeDemoteRequest validates the exact empty operation envelope.
func DecodeDemoteRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationDemote || header.Flags != 0 ||
		header.PayloadLen != 0 || len(request) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeDemoteReply emits the demotion count and its cascade count.
func EncodeDemoteReply(demotedCount, cascadedCount uint32) ([]byte, error) {{
	// The cascade only runs when something was demoted.
	if demotedCount > DemoteMax || cascadedCount > DemoteMax ||
		(demotedCount == 0 && cascadedCount != 0) {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationDemote, ResultOK, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 8)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload, demotedCount)
	binary.LittleEndian.PutUint32(payload[4:], cascadedCount)
	return reply, nil
}}

// DecodeDemoteReply validates the operation, both bounded counts, and the cascade invariant.
func DecodeDemoteReply(reply []byte) (uint32, uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationDemote || header.Result != ResultOK ||
		header.PayloadLen != 8 || len(reply) != int(EnvelopeHeaderLen)+8 {{
		return 0, 0, ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	demoted := binary.LittleEndian.Uint32(payload)
	cascaded := binary.LittleEndian.Uint32(payload[4:])
	if demoted > DemoteMax || cascaded > DemoteMax || (demoted == 0 && cascaded != 0) {{
		return 0, 0, ErrMalformedEnvelope
	}}
	return demoted, cascaded, nil
}}

// EncodeExpireRequest emits the empty request for the fixed expiry policy.
func EncodeExpireRequest() []byte {{
	header, err := EncodeRequestHeader(OperationExpire, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeExpireRequest validates the exact empty operation envelope.
func DecodeExpireRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationExpire || header.Flags != 0 ||
		header.PayloadLen != 0 || len(request) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeExpireReply emits both bounded deletion counts.
func EncodeExpireReply(level0Deleted, staleLevel1Deleted uint32) ([]byte, error) {{
	if level0Deleted > ExpireMax || staleLevel1Deleted > ExpireMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationExpire, ResultOK, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 8)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload, level0Deleted)
	binary.LittleEndian.PutUint32(payload[4:], staleLevel1Deleted)
	return reply, nil
}}

// DecodeExpireReply validates the operation and both bounded counts.
func DecodeExpireReply(reply []byte) (uint32, uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationExpire || header.Result != ResultOK ||
		header.PayloadLen != 8 || len(reply) != int(EnvelopeHeaderLen)+8 {{
		return 0, 0, ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	level0 := binary.LittleEndian.Uint32(payload)
	stale := binary.LittleEndian.Uint32(payload[4:])
	if level0 > ExpireMax || stale > ExpireMax {{
		return 0, 0, ErrMalformedEnvelope
	}}
	return level0, stale, nil
}}

// MemoryStats is the corpus breakdown by tier and kind, plus the totals.
type MemoryStats struct {{
	TierCounts [StatsCountsTiers]uint32
	KindCounts [StatsCountsKinds]uint32
	Total      uint32
	Conflicts  uint32
}}

// EncodeStatsCountsRequest emits the empty request envelope.
func EncodeStatsCountsRequest() []byte {{
	header, err := EncodeRequestHeader(OperationStatsCounts, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeStatsCountsRequest validates the exact empty operation envelope.
func DecodeStatsCountsRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationStatsCounts || header.Flags != 0 ||
		header.PayloadLen != 0 || len(request) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeStatsCountsReply emits every bounded bucket in the contract's order.
func EncodeStatsCountsReply(stats MemoryStats) ([]byte, error) {{
	for _, value := range stats.TierCounts {{
		if value > StatsCountsMax {{
			return nil, ErrMalformedEnvelope
		}}
	}}
	for _, value := range stats.KindCounts {{
		if value > StatsCountsMax {{
			return nil, ErrMalformedEnvelope
		}}
	}}
	if stats.Total > StatsCountsMax || stats.Conflicts > StatsCountsMax {{
		return nil, ErrMalformedEnvelope
	}}
	payloadLen := 4 * (StatsCountsTiers + StatsCountsKinds + 2)
	header, err := EncodeReplyHeader(OperationStatsCounts, ResultOK, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, payloadLen)...)
	payload := reply[EnvelopeHeaderLen:]
	offset := 0
	for _, value := range stats.TierCounts {{
		binary.LittleEndian.PutUint32(payload[offset:], value)
		offset += 4
	}}
	for _, value := range stats.KindCounts {{
		binary.LittleEndian.PutUint32(payload[offset:], value)
		offset += 4
	}}
	binary.LittleEndian.PutUint32(payload[offset:], stats.Total)
	binary.LittleEndian.PutUint32(payload[offset+4:], stats.Conflicts)
	return reply, nil
}}

// DecodeStatsCountsReply validates the operation and every bounded bucket.
func DecodeStatsCountsReply(reply []byte) (MemoryStats, error) {{
	payloadLen := 4 * (StatsCountsTiers + StatsCountsKinds + 2)
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationStatsCounts || header.Result != ResultOK ||
		header.PayloadLen != uint32(payloadLen) ||
		len(reply) != int(EnvelopeHeaderLen)+payloadLen {{
		return MemoryStats{{}}, ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	var stats MemoryStats
	offset := 0
	for index := range stats.TierCounts {{
		stats.TierCounts[index] = binary.LittleEndian.Uint32(payload[offset:])
		if stats.TierCounts[index] > StatsCountsMax {{
			return MemoryStats{{}}, ErrMalformedEnvelope
		}}
		offset += 4
	}}
	for index := range stats.KindCounts {{
		stats.KindCounts[index] = binary.LittleEndian.Uint32(payload[offset:])
		if stats.KindCounts[index] > StatsCountsMax {{
			return MemoryStats{{}}, ErrMalformedEnvelope
		}}
		offset += 4
	}}
	stats.Total = binary.LittleEndian.Uint32(payload[offset:])
	stats.Conflicts = binary.LittleEndian.Uint32(payload[offset+4:])
	if stats.Total > StatsCountsMax || stats.Conflicts > StatsCountsMax {{
		return MemoryStats{{}}, ErrMalformedEnvelope
	}}
	return stats, nil
}}

// HealthCounters is the rolling health-window aggregate the host derives its rates from.
type HealthCounters struct {{
	Cycles              uint32
	TotalContradictions uint32
	TotalPromotions     uint32
	TotalDemotions      uint32
	TotalExpirations    uint32
	NewMemories         uint32
	L1Eligible          uint32
	L2Total             uint32
	L2Stale30Days       uint32
}}

func (c HealthCounters) values() [HealthCountersFields]uint32 {{
	return [HealthCountersFields]uint32{{
		c.Cycles, c.TotalContradictions, c.TotalPromotions, c.TotalDemotions,
		c.TotalExpirations, c.NewMemories, c.L1Eligible, c.L2Total, c.L2Stale30Days,
	}}
}}

// EncodeHealthCountersRequest emits the empty request for the fixed promotion policy.
func EncodeHealthCountersRequest() []byte {{
	header, err := EncodeRequestHeader(OperationHealthCounters, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeHealthCountersRequest validates the exact empty operation envelope.
func DecodeHealthCountersRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationHealthCounters || header.Flags != 0 ||
		header.PayloadLen != 0 || len(request) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeHealthCountersReply emits every bounded counter in the contract's order.
func EncodeHealthCountersReply(counters HealthCounters) ([]byte, error) {{
	values := counters.values()
	for _, value := range values {{
		if value > HealthCountersMax {{
			return nil, ErrMalformedEnvelope
		}}
	}}
	payloadLen := 4 * HealthCountersFields
	header, err := EncodeReplyHeader(OperationHealthCounters, ResultOK, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, payloadLen)...)
	payload := reply[EnvelopeHeaderLen:]
	for index, value := range values {{
		binary.LittleEndian.PutUint32(payload[index*4:], value)
	}}
	return reply, nil
}}

// DecodeHealthCountersReply validates the operation and every bounded counter.
func DecodeHealthCountersReply(reply []byte) (HealthCounters, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationHealthCounters || header.Result != ResultOK ||
		header.PayloadLen != uint32(4*HealthCountersFields) ||
		len(reply) != int(EnvelopeHeaderLen)+4*HealthCountersFields {{
		return HealthCounters{{}}, ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	var values [HealthCountersFields]uint32
	for index := range values {{
		values[index] = binary.LittleEndian.Uint32(payload[index*4:])
		if values[index] > HealthCountersMax {{
			return HealthCounters{{}}, ErrMalformedEnvelope
		}}
	}}
	return HealthCounters{{
		Cycles: values[0], TotalContradictions: values[1], TotalPromotions: values[2],
		TotalDemotions: values[3], TotalExpirations: values[4], NewMemories: values[5],
		L1Eligible: values[6], L2Total: values[7], L2Stale30Days: values[8],
	}}, nil
}}

// EncodeHealthRetentionRequest emits the empty request for the complete fixed policy.
func EncodeHealthRetentionRequest() []byte {{
	header, err := EncodeRequestHeader(OperationHealthRetention, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeHealthRetentionRequest validates the exact empty operation envelope.
func DecodeHealthRetentionRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationHealthRetention || header.Flags != 0 ||
		header.PayloadLen != 0 || len(request) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeHealthRetentionReply emits both bounded deletion counts.
func EncodeHealthRetentionReply(snapshotsDeleted, contradictionsDeleted uint32) ([]byte, error) {{
	if snapshotsDeleted > HealthRetentionMax || contradictionsDeleted > HealthRetentionMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationHealthRetention, ResultOK, 8)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 8)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload, snapshotsDeleted)
	binary.LittleEndian.PutUint32(payload[4:], contradictionsDeleted)
	return reply, nil
}}

// DecodeHealthRetentionReply validates the operation and both bounded counts.
func DecodeHealthRetentionReply(reply []byte) (uint32, uint32, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationHealthRetention || header.Result != ResultOK ||
		header.PayloadLen != 8 || len(reply) != int(EnvelopeHeaderLen)+8 {{
		return 0, 0, ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	snapshots := binary.LittleEndian.Uint32(payload)
	contradictions := binary.LittleEndian.Uint32(payload[4:])
	if snapshots > HealthRetentionMax || contradictions > HealthRetentionMax {{
		return 0, 0, ErrMalformedEnvelope
	}}
	return snapshots, contradictions, nil
}}

// EncodeHealthRecordRequest emits the three bounded health-cycle counters.
func EncodeHealthRecordRequest(promotions, demotions, expirations uint32) ([]byte, error) {{
	if promotions > HealthRecordCounterMax || demotions > HealthRecordCounterMax ||
		expirations > HealthRecordCounterMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationHealthRecord, 0, 12)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 12)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload, promotions)
	binary.LittleEndian.PutUint32(payload[4:], demotions)
	binary.LittleEndian.PutUint32(payload[8:], expirations)
	return request, nil
}}

// DecodeHealthRecordRequest validates the operation and the three bounded counters.
func DecodeHealthRecordRequest(request []byte) (uint32, uint32, uint32, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationHealthRecord || header.Flags != 0 ||
		header.PayloadLen != 12 || len(request) != int(EnvelopeHeaderLen)+12 {{
		return 0, 0, 0, ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	promotions := binary.LittleEndian.Uint32(payload)
	demotions := binary.LittleEndian.Uint32(payload[4:])
	expirations := binary.LittleEndian.Uint32(payload[8:])
	if promotions > HealthRecordCounterMax || demotions > HealthRecordCounterMax ||
		expirations > HealthRecordCounterMax {{
		return 0, 0, 0, ErrMalformedEnvelope
	}}
	return promotions, demotions, expirations, nil
}}

// EncodeHealthRecordReply emits the payload-free acknowledgement.
func EncodeHealthRecordReply() ([]byte, error) {{
	header, err := EncodeReplyHeader(OperationHealthRecord, ResultOK, 0)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	return header, nil
}}

// DecodeHealthRecordReply validates the payload-free acknowledgement.
func DecodeHealthRecordReply(reply []byte) error {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationHealthRecord || header.Result != ResultOK ||
		header.PayloadLen != 0 || len(reply) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeL2MemoryIDsRequest emits the empty request for the fixed identifier bound.
func EncodeL2MemoryIDsRequest() []byte {{
	header, err := EncodeRequestHeader(OperationL2MemoryIDs, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeL2MemoryIDsRequest validates the exact empty operation envelope.
func DecodeL2MemoryIDsRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationL2MemoryIDs || header.Flags != 0 ||
		header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	if len(request) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeL2MemoryIDsReply emits the counted, bounded L2 identifier list.
func EncodeL2MemoryIDsReply(memoryIDs []uint64) ([]byte, error) {{
	if uint32(len(memoryIDs)) > L2MemoryIDsMax {{
		return nil, ErrMalformedEnvelope
	}}
	for _, id := range memoryIDs {{
		if id < L2MemoryIDMin || id > L2MemoryIDMax {{
			return nil, ErrMalformedEnvelope
		}}
	}}
	payloadLen := 4 + len(memoryIDs)*8
	header, err := EncodeReplyHeader(OperationL2MemoryIDs, ResultOK, uint32(payloadLen))
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, payloadLen)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload, uint32(len(memoryIDs)))
	for index, id := range memoryIDs {{
		binary.LittleEndian.PutUint64(payload[4+index*8:], id)
	}}
	return reply, nil
}}

// DecodeL2MemoryIDsReply validates the operation and every bounded identifier.
func DecodeL2MemoryIDsReply(reply []byte) ([]uint64, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationL2MemoryIDs || header.Result != ResultOK ||
		header.PayloadLen < 4 ||
		len(reply) != int(EnvelopeHeaderLen)+int(header.PayloadLen) {{
		return nil, ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	count := binary.LittleEndian.Uint32(payload)
	if count > L2MemoryIDsMax || header.PayloadLen != 4+count*8 {{
		return nil, ErrMalformedEnvelope
	}}
	memoryIDs := make([]uint64, count)
	for index := range memoryIDs {{
		id := binary.LittleEndian.Uint64(payload[4+index*8:])
		if id < L2MemoryIDMin || id > L2MemoryIDMax {{
			return nil, ErrMalformedEnvelope
		}}
		memoryIDs[index] = id
	}}
	return memoryIDs, nil
}}

// EncodeEffectivenessStatsRequest emits the empty request for the fixed low-threshold policy.
func EncodeEffectivenessStatsRequest() []byte {{
	header, err := EncodeRequestHeader(OperationEffectivenessStats, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

// DecodeEffectivenessStatsRequest validates the exact empty operation envelope.
func DecodeEffectivenessStatsRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationEffectivenessStats || header.Flags != 0 ||
		header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	if len(request) != int(EnvelopeHeaderLen) {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

// EncodeEffectivenessStatsReply emits the bounded average and the two bounded counts.
func EncodeEffectivenessStatsReply(stats EffectivenessStats) ([]byte, error) {{
	if !(stats.AvgEffectiveness >= EffectivenessStatsAvgMin) ||
		!(stats.AvgEffectiveness <= EffectivenessStatsAvgMax) ||
		stats.LowEffectivenessCount > EffectivenessStatsLowMax ||
		stats.HighImpactCount > EffectivenessStatsHighMax {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationEffectivenessStats, ResultOK, 16)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	reply := append(header, make([]byte, 16)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint64(payload, math.Float64bits(stats.AvgEffectiveness))
	binary.LittleEndian.PutUint32(payload[8:], stats.LowEffectivenessCount)
	binary.LittleEndian.PutUint32(payload[12:], stats.HighImpactCount)
	return reply, nil
}}

// DecodeEffectivenessStatsReply validates the operation and the bounded summary fields.
func DecodeEffectivenessStatsReply(reply []byte) (EffectivenessStats, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationEffectivenessStats ||
		header.Result != ResultOK || header.PayloadLen != 16 {{
		return EffectivenessStats{{}}, ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	stats := EffectivenessStats{{
		AvgEffectiveness:      math.Float64frombits(binary.LittleEndian.Uint64(payload)),
		LowEffectivenessCount: binary.LittleEndian.Uint32(payload[8:]),
		HighImpactCount:       binary.LittleEndian.Uint32(payload[12:]),
	}}
	if !(stats.AvgEffectiveness >= EffectivenessStatsAvgMin) ||
		!(stats.AvgEffectiveness <= EffectivenessStatsAvgMax) ||
		stats.LowEffectivenessCount > EffectivenessStatsLowMax ||
		stats.HighImpactCount > EffectivenessStatsHighMax {{
		return EffectivenessStats{{}}, ErrMalformedEnvelope
	}}
	return stats, nil
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

type ReembedClearMaintenance struct {{
	WasInProgress     uint32
	RecordedDimension uint32
	RunningDimension  uint32
}}

func validReembedClearMaintenance(status ReembedClearMaintenance) bool {{
	return status.WasInProgress <= 1 && status.RecordedDimension <= ReembedDimensionMax &&
		status.RunningDimension >= ReembedDimensionMin &&
		status.RunningDimension <= ReembedDimensionMax
}}

func EncodeReembedClearMaintenanceRequest(force uint32) ([]byte, error) {{
	if force > 1 {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationReembedClearMaintenance, 0, 4)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 4)...)
	binary.LittleEndian.PutUint32(request[EnvelopeHeaderLen:], force)
	return request, nil
}}

func DecodeReembedClearMaintenanceRequest(request []byte) (uint32, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationReembedClearMaintenance || header.Flags != 0 ||
		header.PayloadLen != 4 {{
		return 0, ErrMalformedEnvelope
	}}
	force := binary.LittleEndian.Uint32(request[EnvelopeHeaderLen:])
	if force > 1 {{
		return 0, ErrMalformedEnvelope
	}}
	return force, nil
}}

func EncodeReembedClearMaintenanceReply(result uint32,
	status ReembedClearMaintenance) ([]byte, error) {{
	var payloadLen uint32
	if result == ResultOK || result == ResultConflict {{
		if !validReembedClearMaintenance(status) || result == ResultConflict &&
			(status.RecordedDimension == 0 || status.RecordedDimension == status.RunningDimension) {{
			return nil, ErrMalformedEnvelope
		}}
		payloadLen = 12
	}} else if result != ResultInvalidState || status != (ReembedClearMaintenance{{}}) {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationReembedClearMaintenance, result, payloadLen)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	if payloadLen == 0 {{
		return header, nil
	}}
	reply := append(header, make([]byte, payloadLen)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload[0:4], status.WasInProgress)
	binary.LittleEndian.PutUint32(payload[4:8], status.RecordedDimension)
	binary.LittleEndian.PutUint32(payload[8:12], status.RunningDimension)
	return reply, nil
}}

func DecodeReembedClearMaintenanceReply(reply []byte) (uint32, ReembedClearMaintenance, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationReembedClearMaintenance {{
		return 0, ReembedClearMaintenance{{}}, ErrMalformedEnvelope
	}}
	if header.Result == ResultInvalidState && header.PayloadLen == 0 {{
		return header.Result, ReembedClearMaintenance{{}}, nil
	}}
	if (header.Result != ResultOK && header.Result != ResultConflict) || header.PayloadLen != 12 {{
		return 0, ReembedClearMaintenance{{}}, ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	status := ReembedClearMaintenance{{
		WasInProgress:     binary.LittleEndian.Uint32(payload[0:4]),
		RecordedDimension: binary.LittleEndian.Uint32(payload[4:8]),
		RunningDimension:  binary.LittleEndian.Uint32(payload[8:12]),
	}}
	if !validReembedClearMaintenance(status) || header.Result == ResultConflict &&
		(status.RecordedDimension == 0 || status.RecordedDimension == status.RunningDimension) {{
		return 0, ReembedClearMaintenance{{}}, ErrMalformedEnvelope
	}}
	return header.Result, status, nil
}}

func validEmbedderServingID(servingID string) bool {{
	if len(servingID) > EmbedderServingIDMax {{
		return false
	}}
	for index := 0; index < len(servingID); index++ {{
		if servingID[index] == 0 {{
			return false
		}}
	}}
	return true
}}

func EncodeEmbedderServingIDRequest() []byte {{
	header, err := EncodeRequestHeader(OperationEmbedderServingID, 0, 0)
	if err != nil {{
		panic(err)
	}}
	return header
}}

func DecodeEmbedderServingIDRequest(request []byte) error {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationEmbedderServingID || header.Flags != 0 ||
		header.PayloadLen != 0 {{
		return ErrMalformedEnvelope
	}}
	return nil
}}

func EncodeEmbedderServingIDReply(result uint32, servingID string) ([]byte, error) {{
	var payloadLen uint32
	if result == ResultOK {{
		if !validEmbedderServingID(servingID) {{
			return nil, ErrMalformedEnvelope
		}}
		payloadLen = uint32(4 + len(servingID))
	}} else if result != ResultInvalidState || servingID != "" {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationEmbedderServingID, result, payloadLen)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	if payloadLen == 0 {{
		return header, nil
	}}
	reply := append(header, make([]byte, payloadLen)...)
	binary.LittleEndian.PutUint32(reply[EnvelopeHeaderLen:], uint32(len(servingID)))
	copy(reply[EnvelopeHeaderLen+4:], servingID)
	return reply, nil
}}

func DecodeEmbedderServingIDReply(reply []byte) (uint32, string, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationEmbedderServingID {{
		return 0, "", ErrMalformedEnvelope
	}}
	if header.Result == ResultInvalidState && header.PayloadLen == 0 {{
		return header.Result, "", nil
	}}
	if header.Result != ResultOK || header.PayloadLen < 4 {{
		return 0, "", ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	decodedLen := binary.LittleEndian.Uint32(payload[:4])
	if decodedLen > EmbedderServingIDMax || header.PayloadLen != 4+decodedLen {{
		return 0, "", ErrMalformedEnvelope
	}}
	servingID := string(payload[4:])
	if !validEmbedderServingID(servingID) {{
		return 0, "", ErrMalformedEnvelope
	}}
	return header.Result, servingID, nil
}}

type DimensionReset struct {{
	RecordedDimension uint32
	TargetDimension   uint32
	TablesDiscovered  uint32
	TablesDropped     uint32
	RowsCleared       uint64
	CuratorRequeued   int32
	EvidenceRequeued  int32
}}

func validDimensionReset(status DimensionReset) bool {{
	return status.RecordedDimension <= ReembedDimensionMax &&
		status.TargetDimension >= ReembedDimensionMin &&
		status.TargetDimension <= ReembedDimensionMax &&
		status.TablesDiscovered <= DimensionResetTablesMax &&
		status.TablesDropped <= status.TablesDiscovered &&
		status.CuratorRequeued >= -1 && status.EvidenceRequeued >= -1
}}

func EncodeDimensionResetRequest(targetDimension, force, dryRun uint32) ([]byte, error) {{
	if targetDimension < ReembedDimensionMin || targetDimension > ReembedDimensionMax ||
		force > 1 || dryRun > 1 {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeRequestHeader(OperationDimensionReset, 0, 12)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	request := append(header, make([]byte, 12)...)
	payload := request[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload[0:4], targetDimension)
	binary.LittleEndian.PutUint32(payload[4:8], force)
	binary.LittleEndian.PutUint32(payload[8:12], dryRun)
	return request, nil
}}

func DecodeDimensionResetRequest(request []byte) (uint32, uint32, uint32, error) {{
	header, err := DecodeRequestHeader(request)
	if err != nil || header.Operation != OperationDimensionReset || header.Flags != 0 ||
		header.PayloadLen != 12 {{
		return 0, 0, 0, ErrMalformedEnvelope
	}}
	payload := request[EnvelopeHeaderLen:]
	targetDimension := binary.LittleEndian.Uint32(payload[0:4])
	force := binary.LittleEndian.Uint32(payload[4:8])
	dryRun := binary.LittleEndian.Uint32(payload[8:12])
	if targetDimension < ReembedDimensionMin || targetDimension > ReembedDimensionMax ||
		force > 1 || dryRun > 1 {{
		return 0, 0, 0, ErrMalformedEnvelope
	}}
	return targetDimension, force, dryRun, nil
}}

func EncodeDimensionResetReply(result uint32, status DimensionReset) ([]byte, error) {{
	var payloadLen uint32
	if result == ResultOK || result == ResultConflict || result == ResultDenied {{
		if !validDimensionReset(status) {{
			return nil, ErrMalformedEnvelope
		}}
		payloadLen = 32
	}} else if result != ResultInvalidState || status != (DimensionReset{{}}) {{
		return nil, ErrMalformedEnvelope
	}}
	header, err := EncodeReplyHeader(OperationDimensionReset, result, payloadLen)
	if err != nil {{
		return nil, ErrMalformedEnvelope
	}}
	if payloadLen == 0 {{
		return header, nil
	}}
	reply := append(header, make([]byte, payloadLen)...)
	payload := reply[EnvelopeHeaderLen:]
	binary.LittleEndian.PutUint32(payload[0:4], status.RecordedDimension)
	binary.LittleEndian.PutUint32(payload[4:8], status.TargetDimension)
	binary.LittleEndian.PutUint32(payload[8:12], status.TablesDiscovered)
	binary.LittleEndian.PutUint32(payload[12:16], status.TablesDropped)
	binary.LittleEndian.PutUint64(payload[16:24], status.RowsCleared)
	binary.LittleEndian.PutUint32(payload[24:28], uint32(status.CuratorRequeued))
	binary.LittleEndian.PutUint32(payload[28:32], uint32(status.EvidenceRequeued))
	return reply, nil
}}

func DecodeDimensionResetReply(reply []byte) (uint32, DimensionReset, error) {{
	header, err := DecodeReplyHeader(reply)
	if err != nil || header.Operation != OperationDimensionReset {{
		return 0, DimensionReset{{}}, ErrMalformedEnvelope
	}}
	if header.Result == ResultInvalidState && header.PayloadLen == 0 {{
		return header.Result, DimensionReset{{}}, nil
	}}
	if (header.Result != ResultOK && header.Result != ResultConflict &&
		header.Result != ResultDenied) || header.PayloadLen != 32 {{
		return 0, DimensionReset{{}}, ErrMalformedEnvelope
	}}
	payload := reply[EnvelopeHeaderLen:]
	status := DimensionReset{{
		RecordedDimension: binary.LittleEndian.Uint32(payload[0:4]),
		TargetDimension:   binary.LittleEndian.Uint32(payload[4:8]),
		TablesDiscovered:  binary.LittleEndian.Uint32(payload[8:12]),
		TablesDropped:     binary.LittleEndian.Uint32(payload[12:16]),
		RowsCleared:       binary.LittleEndian.Uint64(payload[16:24]),
		CuratorRequeued:   int32(binary.LittleEndian.Uint32(payload[24:28])),
		EvidenceRequeued:  int32(binary.LittleEndian.Uint32(payload[28:32])),
	}}
	if !validDimensionReset(status) {{
		return 0, DimensionReset{{}}, ErrMalformedEnvelope
	}}
	return header.Result, status, nil
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
