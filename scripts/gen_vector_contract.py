#!/usr/bin/env python3
"""Validate the vector module's provider protocol and generate its C/Go wire contract."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import NoReturn


ROOT = Path(__file__).resolve().parent.parent
REGISTRY = Path("src/modules/protocol-contracts.json")
CATALOG = Path("src/modules/db2/eventcontract/vector.json")
DESCRIPTOR = Path("src/modules/db2/module.yaml")
PROCESS_CONTRACTS = Path("src/modules/process-contracts.json")
DB1_CATALOG = Path("src/modules/db1/eventcontract/operations.json")
DB2_CATALOG = Path("src/modules/db2/eventcontract/operations.json")
HEADER = Path("src/modules/db2/include/aimee/db2/vector_contract.h")
GO_CONTRACT = Path("server-go/vector/contract_generated.go")
BASELINE = Path("tests/baselines/modules/vector-wire-v1.json")
MAX_BYTES = 1_048_576
MAX_DEPTH = 32
MAX_ARRAY = 4096
NAME = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
STALE_VECTOR_EVENT = re.compile(r"(?<![0-9A-Za-z_])(?:1177[7-9]|1178[01]|0[xX]2[eE]0[1-5])(?:[uUlL]*)(?![0-9A-Za-z_])")


class ContractError(ValueError):
    """A fail-closed registry, catalog, or generated-artifact error."""


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
        value = json.loads(
            raw.decode("utf-8", "strict"),
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


def validate_registry(value: object) -> tuple[dict[str, object], dict[str, object]]:
    registry = _keys(value, {"schema_version", "namespace", "protocols"}, "registry")
    if registry["schema_version"] != 1:
        fail("registry-version", "schema_version must equal 1")
    namespace = _keys(
        registry["namespace"],
        {"kind_flag", "protocol_shift", "max_protocol_id", "max_event_id"},
        "registry.namespace",
    )
    expected = {
        "kind_flag": 0x80000000,
        "protocol_shift": 16,
        "max_protocol_id": 0x7fff,
        "max_event_id": 0xffff,
    }
    if namespace != expected:
        fail("registry-namespace", f"namespace must equal {expected}")
    protocols = registry["protocols"]
    if not isinstance(protocols, list) or not protocols:
        fail("registry-protocols", "protocols must be a nonempty array")
    seen_ids: set[int] = set()
    seen_names: set[str] = set()
    vector = None
    previous = 0
    for index, raw in enumerate(protocols):
        item = _keys(raw, {"id", "name", "owner", "catalog"}, f"protocols[{index}]")
        identifier = _integer(item["id"], f"protocols[{index}].id", 1, 0x7fff)
        name = item["name"]
        if not isinstance(name, str) or not NAME.fullmatch(name):
            fail("registry-name", f"invalid protocol name {name!r}")
        if identifier in seen_ids or name in seen_names:
            fail("registry-duplicate", f"duplicate protocol {identifier}/{name}")
        if identifier <= previous:
            fail("registry-order", "protocols must be sorted by id")
        if not isinstance(item["owner"], str) or not isinstance(item["catalog"], str):
            fail("registry-binding", f"protocol {name} has an invalid owner/catalog")
        previous = identifier
        seen_ids.add(identifier)
        seen_names.add(name)
        if name == "vector":
            vector = item
    if vector != {"id": 3, "name": "vector", "owner": "db2",
                  "catalog": CATALOG.as_posix()}:
        fail("registry-vector",
             "the vector protocol must own canonical ID 3 under DB2")
    return namespace, vector


def validate_catalog(value: object) -> dict[str, object]:
    catalog = _keys(
        value,
        {"schema_version", "protocol", "protocol_id", "owner", "wire_version", "events",
         "limits", "wire", "filter_ops"},
        "catalog",
    )
    if catalog["schema_version"] != 1 or catalog["wire_version"] != 1:
        fail("catalog-version", "schema_version and wire_version must equal 1")
    if catalog["protocol"] != "vector" or catalog["protocol_id"] != 3 or catalog["owner"] != "db2":
        fail("catalog-identity", "catalog must describe DB2-owned protocol DB3/id 3")
    expected_events = (
        (1, "capabilities", "notification", "provider", "db2"),
        (2, "apply", "notification", "db2", "all-providers"),
        (3, "applied", "notification", "provider", "db2"),
        (4, "search", "request-reply", "db2", "selected-provider"),
        (5, "route", "request-reply", "control", "vector-router"),
    )
    events = catalog["events"]
    if not isinstance(events, list) or len(events) != len(expected_events):
        fail("events", "events must contain the five closed DB3 v1 entries")
    for index, (raw, expected) in enumerate(zip(events, expected_events, strict=True)):
        event = _keys(raw, {"id", "name", "pattern", "source", "delivery"}, f"events[{index}]")
        actual = tuple(event[key] for key in ("id", "name", "pattern", "source", "delivery"))
        if actual != expected:
            fail("event-semantics", f"events[{index}] must equal {expected}")
    limits = _keys(
        catalog["limits"],
        {"scope_bytes", "record_type_bytes", "collection_bytes", "label_count",
         "label_key_bytes", "label_value_bytes", "labels_bytes", "dimension", "top_k",
         "filter_count", "filter_values", "filters_bytes"},
        "limits",
    )
    if limits != {
        "scope_bytes": 64,
        "record_type_bytes": 32,
        "collection_bytes": 32,
        "label_count": 16,
        "label_key_bytes": 32,
        "label_value_bytes": 256,
        "labels_bytes": 4096,
        "dimension": 4096,
        "top_k": 256,
        # Search v2. Sixteen filters matches the label ceiling, because a filter
        # asks about a label. 256 values is what a whole-corpus currency search
        # needs -- one project-generation pair per project -- and 16 KiB is the
        # budget those fit in; past either, the request is refused rather than
        # narrowed for the caller.
        "filter_count": 16,
        "filter_values": 256,
        "filters_bytes": 16384,
    }:
        fail("limits", "vector-module limits changed")
    operators = _keys(catalog["filter_ops"], {"eq", "ne", "in"}, "filter_ops")
    if operators != {"eq": 1, "ne": 2, "in": 3}:
        fail("filter-ops", "the filter operators are pinned; zero is not an operator")
    wire = _keys(
        catalog["wire"],
        {
            "capabilities", "search_request", "search_request_v2", "search_reply", "apply",
            "apply_v2", "apply_chunk",
            "applied", "search_failure", "route_request", "route_reply",
        },
        "wire",
    )
    capabilities = _keys(wire["capabilities"], {"magic", "header_bytes"}, "wire.capabilities")
    request = _keys(wire["search_request"], {"magic", "header_bytes"}, "wire.search_request")
    request_v2 = _keys(
        wire["search_request_v2"],
        {"magic", "wire_version", "header_bytes", "filter_header_bytes"},
        "wire.search_request_v2",
    )
    reply = _keys(
        wire["search_reply"], {"magic", "header_bytes", "candidate_bytes"},
        "wire.search_reply",
    )
    apply = _keys(wire["apply"], {"magic", "header_bytes"}, "wire.apply")
    apply_v2 = _keys(
        wire["apply_v2"],
        {"magic", "wire_version", "header_bytes", "label_header_bytes"},
        "wire.apply_v2",
    )
    apply_chunk = _keys(
        wire["apply_chunk"], {"magic", "header_bytes"}, "wire.apply_chunk",
    )
    applied = _keys(wire["applied"], {"magic", "header_bytes"}, "wire.applied")
    search_failure = _keys(
        wire["search_failure"], {"magic", "header_bytes"}, "wire.search_failure",
    )
    route_request = _keys(
        wire["route_request"], {"magic", "header_bytes"}, "wire.route_request",
    )
    route_reply = _keys(
        wire["route_reply"], {"magic", "header_bytes"}, "wire.route_reply",
    )
    if capabilities != {"magic": 0x43334244, "header_bytes": 48}:
        fail("capabilities-wire", "capabilities wire differs from DB3 v1")
    if request != {"magic": 0x53334244, "header_bytes": 36}:
        fail("search-request-wire", "search request wire differs from version 1")
    # Same magic and the v1 fields at their v1 offsets, so a v1 reader refuses
    # this on the version rather than misreading it. The header grows by eight:
    # collection length, filter count, filter bytes, and a reserved pair.
    if request_v2 != {
        "magic": 0x53334244, "wire_version": 2, "header_bytes": 44,
        "filter_header_bytes": 4,
    }:
        fail("search-request-v2-wire",
             "search request v2 wire differs from the canonical filter extension")
    if reply != {"magic": 0x52334244, "header_bytes": 28, "candidate_bytes": 16}:
        fail("search-reply-wire", "search reply wire differs from DB3 v1")
    if apply != {"magic": 0x41334244, "header_bytes": 36}:
        fail("apply-wire", "apply wire differs from DB3 v1")
    if apply_v2 != {
        "magic": 0x41334244, "wire_version": 2, "header_bytes": 40,
        "label_header_bytes": 4,
    }:
        fail("apply-v2-wire", "apply v2 wire differs from the canonical label extension")
    if apply_chunk != {"magic": 0x4b334244, "header_bytes": 32}:
        fail("apply-chunk-wire", "apply chunk wire differs from DB3 v1")
    if applied != {"magic": 0x44334244, "header_bytes": 40}:
        fail("applied-wire", "applied wire differs from DB3 v1")
    if search_failure != {"magic": 0x45334244, "header_bytes": 24}:
        fail("search-failure-wire", "search failure wire differs from DB3 v1")
    if route_request != {"magic": 0x54334244, "header_bytes": 40}:
        fail("route-request-wire", "route request wire differs from DB3 v1")
    if route_reply != {"magic": 0x55334244, "header_bytes": 40}:
        fail("route-reply-wire", "route reply wire differs from DB3 v1")
    return catalog


def event_kind(namespace: dict[str, object], protocol_id: int, event_id: int) -> int:
    if protocol_id == 0 or event_id == 0:
        fail("event-id-zero", "protocol and event IDs must be nonzero")
    if protocol_id > int(namespace["max_protocol_id"]) or event_id > int(namespace["max_event_id"]):
        fail("event-capacity", "protocol or event ID exceeds its namespace field")
    return int(namespace["kind_flag"]) | (protocol_id << int(namespace["protocol_shift"])) | event_id


def _collect_event_kinds(value: object) -> set[int]:
    result: set[int] = set()
    if isinstance(value, dict):
        for key, item in value.items():
            if key == "event_kind" and type(item) is int:
                result.add(item)
            else:
                result.update(_collect_event_kinds(item))
    elif isinstance(value, list):
        for item in value:
            result.update(_collect_event_kinds(item))
    return result


def validate_repository(root: Path, catalog: dict[str, object], namespace: dict[str, object],
                        registry_vector: dict[str, object]) -> list[dict[str, object]]:
    descriptor = load_json(root / DESCRIPTOR)
    if not isinstance(descriptor, dict) or not isinstance(descriptor.get("contracts"), list) or \
            descriptor["contracts"].count(CATALOG.as_posix()) != 1:
        fail("descriptor-ownership", f"{DESCRIPTOR} must own {CATALOG} exactly once")
    if registry_vector["owner"] != descriptor.get("id"):
        fail("registry-owner", "registry owner must match the module descriptor")
    events = catalog["events"]
    assert isinstance(events, list)
    generated = [
        {**event, "event_kind": event_kind(namespace, int(catalog["protocol_id"]), int(event["id"]))}
        for event in events
    ]
    kinds = [int(event["event_kind"]) for event in generated]
    if len(kinds) != len(set(kinds)):
        fail("event-kind-duplicate", "DB3 generated duplicate event kinds")
    occupied: set[int] = set()
    for relative in (PROCESS_CONTRACTS, DB1_CATALOG, DB2_CATALOG):
        occupied.update(_collect_event_kinds(load_json(root / relative)))
    collision = occupied.intersection(kinds)
    if collision:
        fail("event-kind-collision", f"DB3 collides with canonical events {sorted(collision)}")
    for path in (root / "src/modules/db2").rglob("*"):
        if path.suffix not in {".c", ".h"} or path == root / HEADER:
            continue
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            fail("stale-event-scan", f"cannot inspect {path}: {exc}")
        match = STALE_VECTOR_EVENT.search(source)
        if match:
            fail("stale-event-kind", f"{path} embeds retired DB3 event {match.group(0)}")
    return generated


def fingerprint(catalog: dict[str, object], registry: dict[str, object]) -> str:
    canonical = json.dumps(
        {"catalog": catalog, "registry": registry}, sort_keys=True, separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def header_bytes(catalog: dict[str, object], registry: dict[str, object],
                 events: list[dict[str, object]]) -> bytes:
    limits = catalog["limits"]
    wire = catalog["wire"]
    ops = catalog["filter_ops"]
    assert isinstance(limits, dict) and isinstance(wire, dict) and isinstance(ops, dict)
    event_width = max(len(str(event["name"])) for event in events)
    event_lines = "\n".join(
        f"#define AIMEE_VECTOR_EVENT_{str(event['name']).upper():<{event_width}} 0x{int(event['event_kind']):08x}u"
        for event in events
    )
    text = f'''/* Generated by scripts/gen_vector_contract.py; do not edit. */
#ifndef AIMEE_DB2_VECTOR_CONTRACT_H
#define AIMEE_DB2_VECTOR_CONTRACT_H 1

#define AIMEE_VECTOR_CONTRACT_SHA256 "{fingerprint(catalog, registry)}"
#define AIMEE_VECTOR_PROTOCOL_KIND_FLAG 0x{int(registry['namespace']['kind_flag']):08x}u
#define AIMEE_VECTOR_PROTOCOL_ID        {catalog['protocol_id']}u
#define AIMEE_VECTOR_WIRE_VERSION       {catalog['wire_version']}u

{event_lines}

#define AIMEE_VECTOR_MAX_SCOPE       {limits['scope_bytes']}u
#define AIMEE_VECTOR_MAX_RECORD_TYPE {limits['record_type_bytes']}u
#define AIMEE_VECTOR_MAX_COLLECTION  {limits['collection_bytes']}u
#define AIMEE_VECTOR_MAX_LABELS       {limits['label_count']}u
#define AIMEE_VECTOR_MAX_LABEL_KEY    {limits['label_key_bytes']}u
#define AIMEE_VECTOR_MAX_LABEL_VALUE  {limits['label_value_bytes']}u
#define AIMEE_VECTOR_MAX_LABEL_BYTES  {limits['labels_bytes']}u
#define AIMEE_VECTOR_MAX_DIM         {limits['dimension']}u
#define AIMEE_VECTOR_MAX_TOP_K       {limits['top_k']}u
#define AIMEE_VECTOR_MAX_FILTERS       {limits['filter_count']}u
#define AIMEE_VECTOR_MAX_FILTER_VALUES {limits['filter_values']}u
#define AIMEE_VECTOR_MAX_FILTER_BYTES  {limits['filters_bytes']}u

/* Filter operators. A conjunction of these, and nothing else: no OR, no
 * nesting, no precedence. Scope visibility is a disjunction in SQL and becomes
 * one AIMEE_VECTOR_FILTER_IN over a multi-valued label, which is why OR is not
 * needed rather than merely not offered. */
#define AIMEE_VECTOR_FILTER_EQ {ops['eq']}u
#define AIMEE_VECTOR_FILTER_NE {ops['ne']}u
#define AIMEE_VECTOR_FILTER_IN {ops['in']}u

#define AIMEE_VECTOR_SEARCH_REQUEST_MAGIC  0x{wire['search_request']['magic']:08x}u
#define AIMEE_VECTOR_SEARCH_REPLY_MAGIC    0x{wire['search_reply']['magic']:08x}u
#define AIMEE_VECTOR_APPLY_MAGIC           0x{wire['apply']['magic']:08x}u
#define AIMEE_VECTOR_APPLY_V2_VERSION      {wire['apply_v2']['wire_version']}u
#define AIMEE_VECTOR_CAPABILITIES_MAGIC    0x{wire['capabilities']['magic']:08x}u
#define AIMEE_VECTOR_APPLY_CHUNK_MAGIC     0x{wire['apply_chunk']['magic']:08x}u
#define AIMEE_VECTOR_APPLIED_MAGIC         0x{wire['applied']['magic']:08x}u
#define AIMEE_VECTOR_SEARCH_FAILURE_MAGIC  0x{wire['search_failure']['magic']:08x}u
#define AIMEE_VECTOR_ROUTE_REQUEST_MAGIC   0x{wire['route_request']['magic']:08x}u
#define AIMEE_VECTOR_ROUTE_REPLY_MAGIC     0x{wire['route_reply']['magic']:08x}u
#define AIMEE_VECTOR_SEARCH_REQUEST_HEADER {wire['search_request']['header_bytes']}u
#define AIMEE_VECTOR_SEARCH_REQUEST_V2_VERSION {wire['search_request_v2']['wire_version']}u
#define AIMEE_VECTOR_SEARCH_REQUEST_V2_HEADER  {wire['search_request_v2']['header_bytes']}u
#define AIMEE_VECTOR_FILTER_HEADER             {wire['search_request_v2']['filter_header_bytes']}u
#define AIMEE_VECTOR_SEARCH_REPLY_HEADER   {wire['search_reply']['header_bytes']}u
#define AIMEE_VECTOR_CANDIDATE_BYTES       {wire['search_reply']['candidate_bytes']}u
#define AIMEE_VECTOR_APPLY_HEADER          {wire['apply']['header_bytes']}u
#define AIMEE_VECTOR_APPLY_V2_HEADER       {wire['apply_v2']['header_bytes']}u
#define AIMEE_VECTOR_LABEL_HEADER          {wire['apply_v2']['label_header_bytes']}u
#define AIMEE_VECTOR_CAPABILITIES_HEADER   {wire['capabilities']['header_bytes']}u
#define AIMEE_VECTOR_APPLY_CHUNK_HEADER    {wire['apply_chunk']['header_bytes']}u
#define AIMEE_VECTOR_APPLIED_HEADER        {wire['applied']['header_bytes']}u
#define AIMEE_VECTOR_SEARCH_FAILURE_HEADER {wire['search_failure']['header_bytes']}u
#define AIMEE_VECTOR_ROUTE_REQUEST_HEADER  {wire['route_request']['header_bytes']}u
#define AIMEE_VECTOR_ROUTE_REPLY_HEADER    {wire['route_reply']['header_bytes']}u

#endif /* AIMEE_DB2_VECTOR_CONTRACT_H */
'''
    return text.encode("utf-8")


def _go_name(value: str) -> str:
    return "".join(part.title() for part in value.split("-"))


def go_bytes(catalog: dict[str, object], registry: dict[str, object],
             events: list[dict[str, object]]) -> bytes:
    limits = catalog["limits"]
    wire = catalog["wire"]
    ops = catalog["filter_ops"]
    assert isinstance(limits, dict) and isinstance(wire, dict) and isinstance(ops, dict)
    event_lines = "\n".join(
        f"const Event{_go_name(str(event['name']))} uint32 = 0x{int(event['event_kind']):08x}"
        for event in events
    )
    text = f'''// Code generated by scripts/gen_vector_contract.py; DO NOT EDIT.

// Package vector is the public provider-neutral vector database protocol.
package vector

import (
\t"encoding/binary"
\t"errors"
\t"math"
)

const ContractSHA256 = "{fingerprint(catalog, registry)}"
const ProtocolID uint32 = {catalog['protocol_id']}
const WireVersion uint16 = {catalog['wire_version']}

{event_lines}

const MaxScopeBytes = {limits['scope_bytes']}
const MaxRecordTypeBytes = {limits['record_type_bytes']}
const MaxCollectionBytes = {limits['collection_bytes']}
const MaxLabelCount = {limits['label_count']}
const MaxLabelKeyBytes = {limits['label_key_bytes']}
const MaxLabelValueBytes = {limits['label_value_bytes']}
const MaxLabelsBytes = {limits['labels_bytes']}
const MaxDimension = {limits['dimension']}
const MaxTopK = {limits['top_k']}
const MaxFilterCount = {limits['filter_count']}
const MaxFilterValues = {limits['filter_values']}
const MaxFiltersBytes = {limits['filters_bytes']}

// Filter operators. A conjunction of these and nothing else: no OR, no nesting,
// no precedence. Scope visibility is a disjunction in SQL and becomes one
// FilterIn over a multi-valued label, which is why OR is not needed rather than
// merely not offered.
const (
	FilterEq uint8 = {ops['eq']}
	FilterNe uint8 = {ops['ne']}
	FilterIn uint8 = {ops['in']}
)

const searchRequestMagic uint32 = 0x{wire['search_request']['magic']:08x}
const searchReplyMagic uint32 = 0x{wire['search_reply']['magic']:08x}
const applyMagic uint32 = 0x{wire['apply']['magic']:08x}
const capabilitiesMagic uint32 = 0x{wire['capabilities']['magic']:08x}
const applyChunkMagic uint32 = 0x{wire['apply_chunk']['magic']:08x}
const appliedMagic uint32 = 0x{wire['applied']['magic']:08x}
const searchFailureMagic uint32 = 0x{wire['search_failure']['magic']:08x}
const routeRequestMagic uint32 = 0x{wire['route_request']['magic']:08x}
const routeReplyMagic uint32 = 0x{wire['route_reply']['magic']:08x}
const searchRequestHeader = {wire['search_request']['header_bytes']}
const searchRequestV2Version uint16 = {wire['search_request_v2']['wire_version']}
const searchRequestV2Header = {wire['search_request_v2']['header_bytes']}
const filterHeader = {wire['search_request_v2']['filter_header_bytes']}
const searchReplyHeader = {wire['search_reply']['header_bytes']}
const candidateBytes = {wire['search_reply']['candidate_bytes']}
const applyHeader = {wire['apply']['header_bytes']}
const applyV2Version uint16 = {wire['apply_v2']['wire_version']}
const applyV2Header = {wire['apply_v2']['header_bytes']}
const labelHeader = {wire['apply_v2']['label_header_bytes']}
const capabilitiesHeader = {wire['capabilities']['header_bytes']}
const applyChunkHeader = {wire['apply_chunk']['header_bytes']}
const appliedHeader = {wire['applied']['header_bytes']}
const searchFailureHeader = {wire['search_failure']['header_bytes']}
const routeRequestHeader = {wire['route_request']['header_bytes']}
const routeReplyHeader = {wire['route_reply']['header_bytes']}

var ErrMalformed = errors.New("vector: malformed version-1 frame")

type ApplyKind uint8

const (
\tApplyUpsert ApplyKind = 1
\tApplyDelete ApplyKind = 2
\tApplyTombstone ApplyKind = 3
)

type Candidate struct {{
\tPointID int64
\tScore float64
}}

type SearchRequest struct {{
\tRequestID uint64
\tRequiredGeneration uint64
\tWorkspace string
\tProject string
\tRecordType string
\tTopK uint32
\tVector []float32
}}

type SearchReply struct {{
\tRequestID uint64
\tGeneration uint64
\tCandidates []Candidate
}}

type ExactLabel struct {{
\tKey string
\tValue string
}}

type Apply struct {{
\tOperationID uint64
\tGeneration uint64
\tPointID int64
\tKind ApplyKind
\tCollection string
\tVector []float32
\tLabels []ExactLabel
}}

func validText(value string, capacity int, empty bool) bool {{
\tif len(value) >= capacity || (!empty && len(value) == 0) {{ return false }}
\tfor i := 0; i < len(value); i++ {{
\t\tif value[i] < 0x21 || value[i] > 0x7e {{ return false }}
\t}}
\treturn true
}}

func finite32(values []float32) bool {{
\tfor _, value := range values {{
\t\tif math.IsNaN(float64(value)) || math.IsInf(float64(value), 0) {{ return false }}
\t}}
\treturn true
}}

func validLabelKey(value string) bool {{
\tif !validText(value, MaxLabelKeyBytes, false) || value[0] < 'a' || value[0] > 'z' {{
\t\treturn false
\t}}
\tfor i := 1; i < len(value); i++ {{
\t\tchar := value[i]
\t\tif (char < 'a' || char > 'z') && (char < '0' || char > '9') &&
\t\t\tchar != '_' && char != '.' && char != '-' {{
\t\t\treturn false
\t\t}}
\t}}
\treturn true
}}

func validLabelValue(value string) bool {{
\tif len(value) >= MaxLabelValueBytes {{ return false }}
\tfor i := 0; i < len(value); i++ {{
\t\tif value[i] < 0x20 || value[i] > 0x7e {{ return false }}
\t}}
\treturn true
}}

func labelsSize(labels []ExactLabel) (int, bool) {{
\tif len(labels) > MaxLabelCount {{ return 0, false }}
\ttotal := 0
\tprevious := ""
\tfor _, label := range labels {{
\t\tif !validLabelKey(label.Key) || !validLabelValue(label.Value) ||
\t\t\t(previous != "" && label.Key <= previous) {{ return 0, false }}
\t\ttotal += labelHeader + len(label.Key) + len(label.Value)
\t\tif total > MaxLabelsBytes {{ return 0, false }}
\t\tprevious = label.Key
\t}}
\treturn total, true
}}

func (request SearchRequest) Validate() error {{
\tif request.RequestID == 0 || request.RequiredGeneration == 0 ||
\t\tlen(request.Vector) == 0 || len(request.Vector) > MaxDimension ||
\t\trequest.TopK == 0 || request.TopK > MaxTopK ||
\t\t!validText(request.Workspace, MaxScopeBytes, true) ||
\t\t!validText(request.Project, MaxScopeBytes, true) ||
\t\t(request.Workspace == "" && request.Project == "") ||
\t\t!validText(request.RecordType, MaxRecordTypeBytes, false) || !finite32(request.Vector) {{
\t\treturn ErrMalformed
\t}}
\treturn nil
}}

func (reply SearchReply) Validate() error {{
\tif reply.RequestID == 0 || reply.Generation == 0 || len(reply.Candidates) > MaxTopK {{
\t\treturn ErrMalformed
\t}}
\tseen := make(map[int64]struct{{}}, len(reply.Candidates))
\tfor _, candidate := range reply.Candidates {{
\t\tif candidate.PointID <= 0 || math.IsNaN(candidate.Score) || math.IsInf(candidate.Score, 0) {{
\t\t\treturn ErrMalformed
\t\t}}
\t\tif _, exists := seen[candidate.PointID]; exists {{ return ErrMalformed }}
\t\tseen[candidate.PointID] = struct{{}}{{}}
\t}}
\treturn nil
}}

func ValidateSearchReply(request SearchRequest, reply SearchReply) error {{
\tif request.Validate() != nil || reply.Validate() != nil ||
\t\treply.RequestID != request.RequestID || reply.Generation != request.RequiredGeneration ||
\t\tuint32(len(reply.Candidates)) > request.TopK {{ return ErrMalformed }}
\treturn nil
}}

func (apply Apply) Validate() error {{
\tif apply.OperationID == 0 || apply.Generation == 0 || apply.PointID <= 0 ||
\t\t!validText(apply.Collection, MaxCollectionBytes, false) {{ return ErrMalformed }}
\tif _, ok := labelsSize(apply.Labels); !ok {{ return ErrMalformed }}
\tswitch apply.Kind {{
\tcase ApplyUpsert:
\t\tif len(apply.Vector) == 0 || len(apply.Vector) > MaxDimension || !finite32(apply.Vector) {{
\t\t\treturn ErrMalformed
\t\t}}
\tcase ApplyDelete, ApplyTombstone:
\t\tif len(apply.Vector) != 0 || len(apply.Labels) != 0 {{ return ErrMalformed }}
\tdefault:
\t\treturn ErrMalformed
\t}}
\treturn nil
}}

func EncodeSearchRequest(request SearchRequest) ([]byte, error) {{
\tif request.Validate() != nil {{ return nil, ErrMalformed }}
\ttotal := searchRequestHeader + len(request.Workspace) + len(request.Project) + len(request.RecordType) + 4*len(request.Vector)
\tout := make([]byte, total)
\tbinary.LittleEndian.PutUint32(out[0:4], searchRequestMagic)
\tbinary.LittleEndian.PutUint16(out[4:6], WireVersion)
\tbinary.LittleEndian.PutUint16(out[6:8], searchRequestHeader)
\tbinary.LittleEndian.PutUint64(out[8:16], request.RequestID)
\tbinary.LittleEndian.PutUint64(out[16:24], request.RequiredGeneration)
\tbinary.LittleEndian.PutUint16(out[24:26], uint16(len(request.Workspace)))
\tbinary.LittleEndian.PutUint16(out[26:28], uint16(len(request.Project)))
\tbinary.LittleEndian.PutUint16(out[28:30], uint16(len(request.RecordType)))
\tbinary.LittleEndian.PutUint16(out[30:32], uint16(len(request.Vector)))
\tbinary.LittleEndian.PutUint16(out[32:34], uint16(request.TopK))
\toffset := searchRequestHeader
\toffset += copy(out[offset:], request.Workspace)
\toffset += copy(out[offset:], request.Project)
\toffset += copy(out[offset:], request.RecordType)
\tfor _, value := range request.Vector {{
\t\tbinary.LittleEndian.PutUint32(out[offset:offset+4], math.Float32bits(value)); offset += 4
\t}}
\treturn out, nil
}}

func DecodeSearchRequest(input []byte) (SearchRequest, error) {{
\tif len(input) < searchRequestHeader || binary.LittleEndian.Uint32(input[0:4]) != searchRequestMagic ||
\t\tbinary.LittleEndian.Uint16(input[4:6]) != WireVersion ||
\t\tbinary.LittleEndian.Uint16(input[6:8]) != searchRequestHeader ||
\t\tbinary.LittleEndian.Uint16(input[34:36]) != 0 {{ return SearchRequest{{}}, ErrMalformed }}
\tw, p, r := int(binary.LittleEndian.Uint16(input[24:26])), int(binary.LittleEndian.Uint16(input[26:28])), int(binary.LittleEndian.Uint16(input[28:30]))
\tdim, topK := int(binary.LittleEndian.Uint16(input[30:32])), uint32(binary.LittleEndian.Uint16(input[32:34]))
\ttotal := searchRequestHeader + w + p + r + 4*dim
\tif w >= MaxScopeBytes || p >= MaxScopeBytes || r == 0 || r >= MaxRecordTypeBytes ||
\t\tdim == 0 || dim > MaxDimension || topK == 0 || topK > MaxTopK || total != len(input) {{
\t\treturn SearchRequest{{}}, ErrMalformed
\t}}
\toffset := searchRequestHeader
\trequest := SearchRequest{{RequestID: binary.LittleEndian.Uint64(input[8:16]), RequiredGeneration: binary.LittleEndian.Uint64(input[16:24]), TopK: topK}}
\trequest.Workspace = string(input[offset:offset+w]); offset += w
\trequest.Project = string(input[offset:offset+p]); offset += p
\trequest.RecordType = string(input[offset:offset+r]); offset += r
\trequest.Vector = make([]float32, dim)
\tfor i := range request.Vector {{ request.Vector[i] = math.Float32frombits(binary.LittleEndian.Uint32(input[offset:offset+4])); offset += 4 }}
\tif request.Validate() != nil {{ return SearchRequest{{}}, ErrMalformed }}
\treturn request, nil
}}

func EncodeSearchReply(reply SearchReply) ([]byte, error) {{
\tif reply.Validate() != nil {{ return nil, ErrMalformed }}
\tout := make([]byte, searchReplyHeader+candidateBytes*len(reply.Candidates))
\tbinary.LittleEndian.PutUint32(out[0:4], searchReplyMagic)
\tbinary.LittleEndian.PutUint16(out[4:6], WireVersion)
\tbinary.LittleEndian.PutUint16(out[6:8], searchReplyHeader)
\tbinary.LittleEndian.PutUint64(out[8:16], reply.RequestID)
\tbinary.LittleEndian.PutUint64(out[16:24], reply.Generation)
\tbinary.LittleEndian.PutUint32(out[24:28], uint32(len(reply.Candidates)))
\toffset := searchReplyHeader
\tfor _, candidate := range reply.Candidates {{
\t\tbinary.LittleEndian.PutUint64(out[offset:offset+8], uint64(candidate.PointID))
\t\tbinary.LittleEndian.PutUint64(out[offset+8:offset+16], math.Float64bits(candidate.Score)); offset += candidateBytes
\t}}
\treturn out, nil
}}

func DecodeSearchReply(input []byte) (SearchReply, error) {{
\tif len(input) < searchReplyHeader || binary.LittleEndian.Uint32(input[0:4]) != searchReplyMagic ||
\t\tbinary.LittleEndian.Uint16(input[4:6]) != WireVersion ||
\t\tbinary.LittleEndian.Uint16(input[6:8]) != searchReplyHeader {{ return SearchReply{{}}, ErrMalformed }}
\trawCount := binary.LittleEndian.Uint32(input[24:28])
\tif rawCount > MaxTopK {{ return SearchReply{{}}, ErrMalformed }}
\tcount := int(rawCount)
\tif len(input) != searchReplyHeader+candidateBytes*count {{ return SearchReply{{}}, ErrMalformed }}
\treply := SearchReply{{RequestID: binary.LittleEndian.Uint64(input[8:16]), Generation: binary.LittleEndian.Uint64(input[16:24]), Candidates: make([]Candidate, count)}}
\toffset := searchReplyHeader
\tfor i := range reply.Candidates {{
\t\tpoint := binary.LittleEndian.Uint64(input[offset:offset+8])
\t\tif point == 0 || point > math.MaxInt64 {{ return SearchReply{{}}, ErrMalformed }}
\t\treply.Candidates[i] = Candidate{{PointID: int64(point), Score: math.Float64frombits(binary.LittleEndian.Uint64(input[offset+8:offset+16]))}}; offset += candidateBytes
\t}}
\tif reply.Validate() != nil {{ return SearchReply{{}}, ErrMalformed }}
\treturn reply, nil
}}

func EncodeApply(apply Apply) ([]byte, error) {{
\tif apply.Validate() != nil {{ return nil, ErrMalformed }}
\tlabelsBytes, _ := labelsSize(apply.Labels)
\theader := applyHeader
\tversion := WireVersion
\tif len(apply.Labels) != 0 {{ header = applyV2Header; version = applyV2Version }}
\tout := make([]byte, header+len(apply.Collection)+4*len(apply.Vector)+labelsBytes)
\tbinary.LittleEndian.PutUint32(out[0:4], applyMagic)
\tbinary.LittleEndian.PutUint16(out[4:6], version)
\tout[6] = byte(apply.Kind)
\tbinary.LittleEndian.PutUint64(out[8:16], apply.OperationID)
\tbinary.LittleEndian.PutUint64(out[16:24], apply.Generation)
\tbinary.LittleEndian.PutUint64(out[24:32], uint64(apply.PointID))
\tbinary.LittleEndian.PutUint16(out[32:34], uint16(len(apply.Collection)))
\tbinary.LittleEndian.PutUint16(out[34:36], uint16(len(apply.Vector)))
\tif version == applyV2Version {{
\t\tbinary.LittleEndian.PutUint16(out[36:38], uint16(len(apply.Labels)))
\t\tbinary.LittleEndian.PutUint16(out[38:40], uint16(labelsBytes))
\t}}
\toffset := header
\toffset += copy(out[offset:], apply.Collection)
\tfor _, value := range apply.Vector {{ binary.LittleEndian.PutUint32(out[offset:offset+4], math.Float32bits(value)); offset += 4 }}
\tfor _, label := range apply.Labels {{
\t\tbinary.LittleEndian.PutUint16(out[offset:offset+2], uint16(len(label.Key)))
\t\tbinary.LittleEndian.PutUint16(out[offset+2:offset+4], uint16(len(label.Value)))
\t\toffset += labelHeader
\t\toffset += copy(out[offset:], label.Key)
\t\toffset += copy(out[offset:], label.Value)
\t}}
\treturn out, nil
}}

func DecodeApply(input []byte) (Apply, error) {{
\tif len(input) < applyHeader || binary.LittleEndian.Uint32(input[0:4]) != applyMagic ||
\t\tinput[7] != 0 {{ return Apply{{}}, ErrMalformed }}
\tversion := binary.LittleEndian.Uint16(input[4:6])
\theader, labelCount, labelsBytes := applyHeader, 0, 0
\tif version == applyV2Version {{
\t\tif len(input) < applyV2Header {{ return Apply{{}}, ErrMalformed }}
\t\theader = applyV2Header
\t\tlabelCount = int(binary.LittleEndian.Uint16(input[36:38]))
\t\tlabelsBytes = int(binary.LittleEndian.Uint16(input[38:40]))
\t\tif labelCount == 0 || labelCount > MaxLabelCount || labelsBytes > MaxLabelsBytes {{ return Apply{{}}, ErrMalformed }}
\t}} else if version != WireVersion {{ return Apply{{}}, ErrMalformed }}
\tcollection, dim := int(binary.LittleEndian.Uint16(input[32:34])), int(binary.LittleEndian.Uint16(input[34:36]))
\tif collection == 0 || collection >= MaxCollectionBytes || dim > MaxDimension ||
\t\tlen(input) != header+collection+4*dim+labelsBytes {{ return Apply{{}}, ErrMalformed }}
\tpoint := binary.LittleEndian.Uint64(input[24:32])
\tif point == 0 || point > math.MaxInt64 {{ return Apply{{}}, ErrMalformed }}
\tapply := Apply{{OperationID: binary.LittleEndian.Uint64(input[8:16]), Generation: binary.LittleEndian.Uint64(input[16:24]), PointID: int64(point), Kind: ApplyKind(input[6])}}
\toffset := header
\tapply.Collection = string(input[offset:offset+collection]); offset += collection
\tif dim > 0 {{
\t\tapply.Vector = make([]float32, dim)
\t\tfor i := range apply.Vector {{ apply.Vector[i] = math.Float32frombits(binary.LittleEndian.Uint32(input[offset:offset+4])); offset += 4 }}
\t}}
\tif labelCount != 0 {{
\t\tend := offset + labelsBytes
\t\tapply.Labels = make([]ExactLabel, labelCount)
\t\tfor i := range apply.Labels {{
\t\t\tif offset+labelHeader > end {{ return Apply{{}}, ErrMalformed }}
\t\t\tkeyBytes := int(binary.LittleEndian.Uint16(input[offset:offset+2]))
\t\t\tvalueBytes := int(binary.LittleEndian.Uint16(input[offset+2:offset+4]))
\t\t\toffset += labelHeader
\t\t\tif keyBytes == 0 || offset+keyBytes+valueBytes > end {{ return Apply{{}}, ErrMalformed }}
\t\t\tapply.Labels[i] = ExactLabel{{Key: string(input[offset:offset+keyBytes])}}
\t\t\toffset += keyBytes
\t\t\tapply.Labels[i].Value = string(input[offset:offset+valueBytes])
\t\t\toffset += valueBytes
\t\t}}
\t\tif offset != end {{ return Apply{{}}, ErrMalformed }}
\t}}
\tif apply.Validate() != nil {{ return Apply{{}}, ErrMalformed }}
\treturn apply, nil
}}
'''
    try:
        formatted = subprocess.run(
            ["gofmt"], input=text.encode("utf-8"), stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False,
        )
    except OSError as exc:
        fail("gofmt", f"cannot execute gofmt: {exc}")
    if formatted.returncode != 0:
        fail("gofmt", formatted.stderr.decode("utf-8", "replace").strip())
    return formatted.stdout


def _u16(value: int) -> bytes:
    return value.to_bytes(2, "little")


def _u32(value: int) -> bytes:
    return value.to_bytes(4, "little")


def _u64(value: int) -> bytes:
    return value.to_bytes(8, "little")


def baseline_bytes(catalog: dict[str, object], registry: dict[str, object],
                   events: list[dict[str, object]]) -> bytes:
    import struct
    capabilities = (_u32(0x43334244) + _u16(1) + _u16(48) + _u64(7) +
                    _u32(3) + _u32(1) + _u32(0) + _u32(1) +
                    _u32(4096) + _u32(64) + _u32(256) + _u32(0))
    request = (_u32(0x53334244) + _u16(1) + _u16(36) + _u64(77) + _u64(7) +
               _u16(11) + _u16(9) + _u16(6) + _u16(3) + _u16(2) + _u16(0) +
               b"workspace-a" + b"project-a" + b"memory" + struct.pack("<fff", .3, .2, .1))
    reply = (_u32(0x52334244) + _u16(1) + _u16(28) + _u64(77) + _u64(7) + _u32(2) +
             _u64(41) + struct.pack("<d", .95) + _u64(42) + struct.pack("<d", .75))
    apply = (_u32(0x41334244) + _u16(1) + bytes((1, 0)) + _u64(1001) + _u64(7) + _u64(41) +
             _u16(6) + _u16(3) + b"memory" + struct.pack("<fff", .1, .2, .3))
    labels = (
        _u16(7) + _u16(9) + b"project" + b"project-a" +
        _u16(11) + _u16(6) + b"record_type" + b"memory" +
        _u16(9) + _u16(11) + b"workspace" + b"workspace-a"
    )
    apply_v2 = (
        _u32(0x41334244) + _u16(2) + bytes((1, 0)) + _u64(1002) + _u64(7) + _u64(42) +
        _u16(6) + _u16(3) + _u16(3) + _u16(len(labels)) +
        b"memory" + struct.pack("<fff", .3, .2, .1) + labels
    )
    apply_chunk = (_u32(0x4b334244) + _u16(1) + _u16(32) + _u64(1001) +
                   _u32(len(apply)) + _u32(0) + _u32(len(apply)) + _u32(0) + apply)
    applied = (_u32(0x44334244) + _u16(1) + _u16(40) + _u64(1001) + _u64(7) +
               _u64(1001) + _u32(0) + _u32(0))
    search_failure = (_u32(0x45334244) + _u16(1) + _u16(24) + _u64(77) +
                      _u32(2) + _u32(0))
    route_request = (_u32(0x54334244) + _u16(1) + _u16(40) + _u64(91) +
                     bytes((2, 1)) + _u16(0) + _u32(1001) + _u64(7) + _u64(0))
    route_reply = (_u32(0x55334244) + _u16(1) + _u16(40) + _u64(91) + _u32(0) +
                   _u32(1001) + _u32(1) + _u32(0) + _u64(7))
    value = {
        "schema_version": 1,
        "contract_sha256": fingerprint(catalog, registry),
        "protocol_id": 3,
        "events": events,
        "capabilities_hex": capabilities.hex(),
        "search_request_hex": request.hex(),
        "search_reply_hex": reply.hex(),
        "apply_hex": apply.hex(),
        "apply_v2_hex": apply_v2.hex(),
        "apply_chunk_hex": apply_chunk.hex(),
        "applied_hex": applied.hex(),
        "search_failure_hex": search_failure.hex(),
        "route_request_hex": route_request.hex(),
        "route_reply_hex": route_reply.hex(),
    }
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def generated(root: Path) -> tuple[bytes, bytes, bytes]:
    raw_registry = load_json(root / REGISTRY)
    namespace, vector = validate_registry(raw_registry)
    catalog = validate_catalog(load_json(root / CATALOG))
    events = validate_repository(root, catalog, namespace, vector)
    assert isinstance(raw_registry, dict)
    return (
        header_bytes(catalog, raw_registry, events),
        go_bytes(catalog, raw_registry, events),
        baseline_bytes(catalog, raw_registry, events),
    )


def _write(path: Path, content: bytes) -> None:
    if path.is_symlink():
        fail("output-symlink", f"refusing to overwrite symlink {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def run(root: Path, write: bool) -> None:
    contents = generated(root)
    for relative, expected in zip((HEADER, GO_CONTRACT, BASELINE), contents, strict=True):
        if write:
            _write(root / relative, expected)
        else:
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
        print(f"gen_vector_contract: error: {exc}", file=sys.stderr)
        return 1
    action = "wrote" if args.write else "ok"
    print(f"gen_vector_contract: {action} ({HEADER}, {GO_CONTRACT}, {BASELINE})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
