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
import copy
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
LEGACY_PROBE_FLAGS = (
    "-fno-lto -fno-common -fno-stack-protector "
    "-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"
)
PROBE_FLAGS = LEGACY_PROBE_FLAGS + " -DAIMEE_DB1_DISABLED -DAIMEE_DISABLE_DB2_SQLITE_SHIM"
SUPPORT_COMPILE_FLAGS = (
    "-std=c11 -Os -Wall -Wextra -Werror " + PROBE_FLAGS
)
LEGACY_SUPPORT_COMPILE_FLAGS = (
    "-std=c11 -Os -Wall -Wextra -Werror " + LEGACY_PROBE_FLAGS
)
SUPPORT_INCLUDE_ROOTS = ["src/modules/db2/support"]
GENERATED_REL_SEED = Path("src/modules/db2/support/rel_seed_primitives.c")
HOST_ADAPTER_REHOMES = {
    "src/modules/db2/c/kb_service_backend_agent.c":
        "src/kb/db2_adapters/kb_service_backend_agent.c",
    "src/modules/db2/c/kb_service_backend_export.c":
        "src/kb/db2_adapters/kb_service_backend_export.c",
    "src/modules/db2/c/kb_service_backend_memory.c":
        "src/kb/db2_adapters/kb_service_backend_memory.c",
}
CJSON_DEFINES = [
    "cJSON_AddArrayToObject",
    "cJSON_AddBoolToObject",
    "cJSON_AddFalseToObject",
    "cJSON_AddItemReferenceToArray",
    "cJSON_AddItemReferenceToObject",
    "cJSON_AddItemToArray",
    "cJSON_AddItemToObject",
    "cJSON_AddItemToObjectCS",
    "cJSON_AddNullToObject",
    "cJSON_AddNumberToObject",
    "cJSON_AddObjectToObject",
    "cJSON_AddRawToObject",
    "cJSON_AddStringToObject",
    "cJSON_AddTrueToObject",
    "cJSON_Compare",
    "cJSON_CreateArray",
    "cJSON_CreateArrayReference",
    "cJSON_CreateBool",
    "cJSON_CreateDoubleArray",
    "cJSON_CreateFalse",
    "cJSON_CreateFloatArray",
    "cJSON_CreateIntArray",
    "cJSON_CreateNull",
    "cJSON_CreateNumber",
    "cJSON_CreateObject",
    "cJSON_CreateObjectReference",
    "cJSON_CreateRaw",
    "cJSON_CreateString",
    "cJSON_CreateStringArray",
    "cJSON_CreateStringReference",
    "cJSON_CreateTrue",
    "cJSON_Delete",
    "cJSON_DeleteItemFromArray",
    "cJSON_DeleteItemFromObject",
    "cJSON_DeleteItemFromObjectCaseSensitive",
    "cJSON_DetachItemFromArray",
    "cJSON_DetachItemFromObject",
    "cJSON_DetachItemFromObjectCaseSensitive",
    "cJSON_DetachItemViaPointer",
    "cJSON_Duplicate",
    "cJSON_Duplicate_rec",
    "cJSON_GetArrayItem",
    "cJSON_GetArraySize",
    "cJSON_GetErrorPtr",
    "cJSON_GetNumberValue",
    "cJSON_GetObjectItem",
    "cJSON_GetObjectItemCaseSensitive",
    "cJSON_GetStringValue",
    "cJSON_HasObjectItem",
    "cJSON_InitHooks",
    "cJSON_InsertItemInArray",
    "cJSON_IsArray",
    "cJSON_IsBool",
    "cJSON_IsFalse",
    "cJSON_IsInvalid",
    "cJSON_IsNull",
    "cJSON_IsNumber",
    "cJSON_IsObject",
    "cJSON_IsRaw",
    "cJSON_IsString",
    "cJSON_IsTrue",
    "cJSON_Minify",
    "cJSON_Parse",
    "cJSON_ParseWithLength",
    "cJSON_ParseWithLengthOpts",
    "cJSON_ParseWithOpts",
    "cJSON_Print",
    "cJSON_PrintBuffered",
    "cJSON_PrintPreallocated",
    "cJSON_PrintUnformatted",
    "cJSON_ReplaceItemInArray",
    "cJSON_ReplaceItemInObject",
    "cJSON_ReplaceItemInObjectCaseSensitive",
    "cJSON_ReplaceItemViaPointer",
    "cJSON_SetNumberHelper",
    "cJSON_SetValuestring",
    "cJSON_Version",
    "cJSON_free",
    "cJSON_malloc",
]
CJSON_BASE_REFERENCES = {
    "cJSON_AddArrayToObject": [
        "src/modules/db2/c/code_audit.c",
        "src/modules/db2/c/collab_rules.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/kb_service_backend.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
    ],
    "cJSON_AddBoolToObject": [
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
        "src/modules/db2/c/vector_status.c",
    ],
    "cJSON_AddItemToArray": [
        "src/modules/db2/c/code_audit.c",
        "src/modules/db2/c/collab_rules.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/kb_service_backend.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_export.c",
        "src/modules/db2/c/kb_service_backend_ingest.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
        "src/modules/db2/c/memory_payload.c",
    ],
    "cJSON_AddItemToObject": [
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/kb_service_backend.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_export.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
        "src/modules/db2/c/memory_payload.c",
    ],
    "cJSON_AddNumberToObject": [
        "src/modules/db2/c/code_audit.c",
        "src/modules/db2/c/collab_rules.c",
        "src/modules/db2/c/corpus_structural.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/fidelity.c",
        "src/modules/db2/c/kb_payload.c",
        "src/modules/db2/c/kb_service_backend.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_export.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
        "src/modules/db2/c/memory_export.c",
        "src/modules/db2/c/memory_payload.c",
        "src/modules/db2/c/rules.c",
        "src/modules/db2/c/vector_status.c",
    ],
    "cJSON_AddObjectToObject": ["src/modules/db2/c/kb_service_backend_memory.c"],
    "cJSON_AddStringToObject": [
        "src/modules/db2/c/artifacts.c",
        "src/modules/db2/c/code_audit.c",
        "src/modules/db2/c/collab_rules.c",
        "src/modules/db2/c/corpus_structural.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/fidelity.c",
        "src/modules/db2/c/kb_payload.c",
        "src/modules/db2/c/kb_service_backend.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_export.c",
        "src/modules/db2/c/kb_service_backend_ingest.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
        "src/modules/db2/c/memory_export.c",
        "src/modules/db2/c/memory_payload.c",
        "src/modules/db2/c/rules.c",
        "src/modules/db2/c/vector_status.c",
    ],
    "cJSON_CreateArray": [
        "src/modules/db2/c/code_audit.c",
        "src/modules/db2/c/collab_rules.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_export.c",
        "src/modules/db2/c/kb_service_backend_ingest.c",
        "src/modules/db2/c/memory_payload.c",
    ],
    "cJSON_CreateNumber": [
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
    ],
    "cJSON_CreateObject": [
        "src/modules/db2/c/artifacts.c",
        "src/modules/db2/c/code_audit.c",
        "src/modules/db2/c/collab_rules.c",
        "src/modules/db2/c/corpus_structural.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/fidelity.c",
        "src/modules/db2/c/kb_payload.c",
        "src/modules/db2/c/kb_service_backend.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_export.c",
        "src/modules/db2/c/kb_service_backend_ingest.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
        "src/modules/db2/c/memory_export.c",
        "src/modules/db2/c/memory_payload.c",
        "src/modules/db2/c/rules.c",
        "src/modules/db2/c/vector_status.c",
    ],
    "cJSON_CreateString": [
        "src/modules/db2/c/code_audit.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
        "src/modules/db2/c/memory_payload.c",
    ],
    "cJSON_Delete": [
        "src/modules/db2/c/artifacts.c",
        "src/modules/db2/c/calibration.c",
        "src/modules/db2/c/code_audit.c",
        "src/modules/db2/c/collab_rules.c",
        "src/modules/db2/c/corpus_structural.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/fidelity.c",
        "src/modules/db2/c/kb_payload.c",
        "src/modules/db2/c/kb_service_backend.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_export.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
        "src/modules/db2/c/memory_export.c",
        "src/modules/db2/c/memory_payload.c",
        "src/modules/db2/c/org_model_catalog.c",
        "src/modules/db2/c/pgvec_transport.c",
        "src/modules/db2/c/rules.c",
    ],
    "cJSON_DeleteItemFromObjectCaseSensitive": ["src/modules/db2/c/demotion.c"],
    "cJSON_GetArrayItem": [
        "src/modules/db2/c/calibration.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/org_model_catalog.c",
    ],
    "cJSON_GetArraySize": [
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/memory_payload.c",
        "src/modules/db2/c/org_model_catalog.c",
    ],
    "cJSON_GetObjectItemCaseSensitive": [
        "src/modules/db2/c/artifacts.c",
        "src/modules/db2/c/calibration.c",
        "src/modules/db2/c/code_audit.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/fidelity.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_export.c",
        "src/modules/db2/c/pgvec_transport.c",
    ],
    "cJSON_IsArray": [
        "src/modules/db2/c/artifacts.c",
        "src/modules/db2/c/calibration.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_export.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
        "src/modules/db2/c/org_model_catalog.c",
    ],
    "cJSON_IsBool": ["src/modules/db2/c/kb_service_backend_agent.c"],
    "cJSON_IsNumber": [
        "src/modules/db2/c/artifacts.c",
        "src/modules/db2/c/calibration.c",
        "src/modules/db2/c/code_audit.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/fidelity.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_export.c",
    ],
    "cJSON_IsObject": ["src/modules/db2/c/kb_service_backend_agent.c"],
    "cJSON_IsString": [
        "src/modules/db2/c/artifacts.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/fidelity.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_export.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
        "src/modules/db2/c/org_model_catalog.c",
        "src/modules/db2/c/pgvec_transport.c",
    ],
    "cJSON_IsTrue": ["src/modules/db2/c/kb_service_backend_agent.c"],
    "cJSON_Parse": [
        "src/modules/db2/c/artifacts.c",
        "src/modules/db2/c/calibration.c",
        "src/modules/db2/c/code_audit.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/fidelity.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/pgvec_transport.c",
    ],
    "cJSON_ParseWithOpts": ["src/modules/db2/c/org_model_catalog.c"],
    "cJSON_PrintUnformatted": [
        "src/modules/db2/c/artifacts.c",
        "src/modules/db2/c/collab_rules.c",
        "src/modules/db2/c/corpus_structural.c",
        "src/modules/db2/c/demotion.c",
        "src/modules/db2/c/fidelity.c",
        "src/modules/db2/c/kb_payload.c",
        "src/modules/db2/c/kb_service_backend_agent.c",
        "src/modules/db2/c/kb_service_backend_memory.c",
        "src/modules/db2/c/memory_export.c",
        "src/modules/db2/c/memory_payload.c",
        "src/modules/db2/c/rules.c",
    ],
}
for _references in CJSON_BASE_REFERENCES.values():
    _references[:] = [
        path for path in _references if path not in HOST_ADAPTER_REHOMES
    ]
SUPPORT_UNITS: list[dict[str, object]] = [{
    "path": "src/modules/db2/support/cert_serial_primitives.c",
    "source_sha256": "9104b934e60e7f5d72ced9f4dee60feb8fa2d26ea48d51dafea05ecdbb8ff057",
    "header": "src/modules/db2/support/db2_cert_serial.h",
    "header_sha256": "8075795205e0e4dd9fd05d81ac7a06ad27159d149fc55dc7403c9a6be98add91",
    "defines": ["kb_cert_serial_normalize"],
    "resolves": ["kb_cert_serial_normalize"],
    "resolution_disposition": "injected-module-contract",
    "allowed_includes": ["db2_cert_serial.h", "ctype.h", "string.h"],
    "allowed_header_includes": ["stddef.h"],
    "allowed_undefined": ["__ctype_tolower_loc", "memcpy", "strlen"],
    "base_references": {
        "kb_cert_serial_normalize": ["src/modules/db2/c/enrollments.c"],
    },
    "provenance": "The certificate-serial canonicalizer is promoted from "
                  "src/kb/kb_identity.c; the sole DB2 call is pinned to enrollments.c.",
    "evidence": "The descriptor owns prefix and separator removal, process-locale lowercase, "
                "leading-zero collapse, bounded output, and fail-with-empty-output behavior. "
                "Normal and sanitizer parity cover NULL, every non-NUL byte, all short output "
                "capacities, and input lengths across the internal 512-byte boundary. Only "
                "ctype, memcpy, and strlen are imported; there is no identity object, DB, bus, "
                "provider, pgvector, DB3, allocation, configuration, I/O, or logging edge.",
}, {
    "path": "src/modules/db2/support/cjson.c",
    "source_sha256": "c17f53aaa58dddb899f452b02dc313b98af9111c81d87797c72607c1f6d6b4d4",
    "header": "src/modules/db2/support/cJSON.h",
    "header_sha256": "b3e16fec5613b4150c1e6636f4576cb54e2d8641bce391c9470dd19aa346e15a",
    "origin_source": "src/vendor/cJSON.c",
    "origin_header": "src/vendor/headers/cJSON.h",
    "defines": CJSON_DEFINES,
    "resolves": sorted(CJSON_BASE_REFERENCES),
    "allowed_includes": [
        "string.h", "stdio.h", "math.h", "stdlib.h", "limits.h",
        "ctype.h", "float.h", "locale.h", "cJSON.h",
    ],
    "allowed_header_includes": ["stddef.h"],
    "allowed_undefined": [
        "_GLOBAL_OFFSET_TABLE_", "__ctype_tolower_loc", "__isoc99_sscanf", "free",
        "malloc", "realloc", "sprintf", "strcmp", "strcpy", "strlen", "strncmp", "strtod",
    ],
    "base_references": CJSON_BASE_REFERENCES,
    "provenance": "Exact descriptor-owned copies of src/vendor/cJSON.c and its public header; "
                  "every DB2 reference is frozen in this policy and byte drift from the canonical "
                  "vendored input fails closed.",
    "evidence": "The pinned cJSON implementation resolves all 25 reviewed generated-input "
                "dependencies while exporting the complete canonical API and importing only its "
                "reviewed C runtime surface; it contains no DB2, bus, provider, or database code.",
}, {
    "path": "src/modules/db2/support/cochange_primitives.c",
    "source_sha256": "26bf099472cd22f26ced227013f0fb7ec056ff8d20099a13e68e550c27da4cb1",
    "header": "src/modules/db2/support/db2_cochange.h",
    "header_sha256": "27c4c5592af28510e924a26adcd77de8a4847ba4cef17df89e3deab60e1deb03",
    "defines": ["cochange_is_hex_sha", "cochange_pairs_for_commit"],
    "resolves": ["cochange_is_hex_sha", "cochange_pairs_for_commit"],
    "resolution_disposition": "injected-module-contract",
    "allowed_includes": ["db2_cochange.h", "stdio.h", "stdlib.h", "string.h"],
    "allowed_header_includes": [],
    "allowed_undefined": ["memcpy", "qsort", "snprintf", "strcmp"],
    "base_references": {
        "cochange_is_hex_sha": ["src/modules/db2/c/canonical_index.c"],
        "cochange_pairs_for_commit": ["src/modules/db2/c/canonical_index.c"],
    },
    "provenance": "The two pure co-change policy definitions are promoted from src/cochange.c; "
                  "both DB2 calls are pinned to canonical_index.c.",
    "evidence": "The descriptor owns the fixed pair ABI, lowercase object-id validator, "
                "deduplication, bulk-commit gate, lexical ordering, and output cap. Monolith "
                "parity and sanitizer tests cover boundary inputs; the implementation imports "
                "only C runtime functions and contains no git, DB, bus, provider, pgvector, "
                "DB3, allocation, configuration, or logging dependency.",
}, {
    "path": "src/modules/db2/support/code_audit_graph_primitives.c",
    "source_sha256": "902153774ae34755e87bb8d1935e209ab00c14e57d11d7ddba393575ad2ec9a4",
    "header": "src/modules/db2/support/db2_code_audit_graph.h",
    "header_sha256": "e098b60c7fada7f866c075bd787ac04a1305a87bbcacd995d0f8d47d7b285887",
    "defines": ["code_audit_dead_exports", "code_audit_find_cycles"],
    "resolves": ["code_audit_dead_exports", "code_audit_find_cycles"],
    "resolution_disposition": "injected-module-contract",
    "allowed_includes": ["db2_code_audit_graph.h", "stdio.h", "stdlib.h", "string.h"],
    "allowed_header_includes": [],
    "allowed_undefined": [
        "calloc", "free", "malloc", "memcpy", "realloc", "snprintf", "strcmp", "strlen",
        "strncmp",
    ],
    "base_references": {
        "code_audit_dead_exports": ["src/modules/db2/c/code_audit.c"],
        "code_audit_find_cycles": ["src/modules/db2/c/code_audit.c"],
    },
    "provenance": "Both pure graph algorithms and their required static helpers are promoted "
                  "from src/code_audit_graph.c; every DB2 call is pinned to "
                  "src/modules/db2/c/code_audit.c.",
    "evidence": "The descriptor owns export/import/reference tail matching, borrowed-pointer "
                "output, the 4096-node cap, deterministic DFS traversal, duplicate suppression, "
                "cycle rendering, result limits, and cleanup. Normal and sanitizer parity cover "
                "NULL and negative bounds, prefix variants, duplicate exports and edges, DAGs, "
                "self/two/three-node and overlapping cycles, null endpoints, disconnected graphs, "
                "limits, and a generated 64-node graph. Only bounded allocation, string, and "
                "formatting APIs are imported; there is no DB, JSON, bus, provider, pgvector, "
                "DB3, filesystem, network, configuration, or logging edge.",
}, {
    "path": "src/modules/db2/support/code_import_primitives.c",
    "source_sha256": "747342db5ec2b2f8cb2f66aa20c87c9f8aee27a2707a5cdb41e0a3051ab8ace4",
    "header": "src/modules/db2/support/db2_code_import.h",
    "header_sha256": "67f300db20b72e013894d6c6271e64015d66729da1064dea5c000646b2b4375b",
    "defines": [
        "code_import_identity", "code_import_resolves_path", "code_path_import_identity",
    ],
    "resolves": ["code_import_identity", "code_import_resolves_path"],
    "resolution_disposition": "injected-module-contract",
    "allowed_includes": ["db2_code_import.h", "stdio.h", "string.h"],
    "allowed_header_includes": ["stddef.h"],
    "allowed_undefined": ["snprintf", "strcmp", "strlen", "strncmp", "strrchr"],
    "base_references": {
        "code_import_identity": ["src/modules/db2/c/code_index.c"],
        "code_import_resolves_path": ["src/modules/db2/c/code_index.c"],
    },
    "provenance": "The path/import identity definitions and required slash-normalization helper "
                  "are promoted from src/extractors.c; every DB2 call is pinned to "
                  "src/modules/db2/c/code_index.c.",
    "evidence": "The descriptor owns the 4096-byte normalization boundary, Python relative-"
                "import resolution, __init__ identity rule, separator normalization, output "
                "truncation, and empty-input behavior. Normal and sanitizer parity cover NULL, "
                "every non-NUL byte, POSIX and Windows separators, relative-dot levels, all "
                "short output capacities, the internal path boundary, and a cross-product of "
                "importer/import/target identities. Only bounded C string and formatting APIs "
                "are imported; there is no extractor, tree-sitter, filesystem, DB, bus, "
                "provider, pgvector, DB3, allocation, configuration, I/O, or logging edge.",
}, {
    "path": "src/modules/db2/support/code_match_primitives.c",
    "source_sha256": "a6190eb5fc93657cf7a6e9142d99977c322c8e9765811a628c8eb70970231876",
    "header": "src/modules/db2/support/db2_code_match.h",
    "header_sha256": "e1a2da6d369a3519405f980f703ec4325a07ced6426408d167d084b741e23d3a",
    "defines": ["code_match_line"],
    "resolves": ["code_match_line"],
    "resolution_disposition": "injected-module-contract",
    "allowed_includes": ["db2_code_match.h", "string.h"],
    "allowed_header_includes": [],
    "allowed_undefined": ["strncmp", "strstr"],
    "base_references": {
        "code_match_line": ["src/modules/db2/c/code_index.c"],
    },
    "provenance": "The string-only line-enrichment definition is promoted from "
                  "src/code_match.c; the sole DB2 call is pinned to code_index.c.",
    "evidence": "The descriptor owns marker extraction, empty-token rejection, first-verbatim-"
                "match selection, and one-based line counting. Normal and sanitizer parity cover "
                "NULL, malformed and repeated markers, every non-NUL byte, 256 lines, and token "
                "lengths through 2048. Only strncmp and strstr are imported; there is no FTS, "
                "DB, bus, provider, pgvector, DB3, allocation, configuration, I/O, or logging "
                "edge.",
}, {
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
    "path": "src/modules/db2/support/extractor_primitives.c",
    "source_sha256": "a45293cfe677b6dbe091784cd0664a7730b673eb26a2789d33995732431b9fad",
    "header": "src/modules/db2/support/db2_extractors.h",
    "header_sha256": "025041d001c95f10cc3c69796f2104eda877a7a5c7aee35763fb97574b09c1a6",
    "defines": [
        "add_call", "add_def", "add_str", "c_call_line", "c_def_line", "c_export_line",
        "c_import_line", "c_macro_def_line", "code_def_end_line", "cs_def_line",
        "cs_export_line", "cs_import_line", "cs_route_line", "css_export_line",
        "css_import_line", "dart_def_line", "dart_export_line", "dart_import_line",
        "extract_calls", "extract_definitions", "extract_exports", "extract_ident",
        "extract_imports", "extract_imports_sys", "extract_quoted", "extract_routes",
        "for_each_line", "generic_call_line", "go_call_line", "index_has_extractor",
        "java_def_line", "java_export_line", "java_import_line", "java_route_line",
        "js_call_line", "kotlin_def_line", "kotlin_export_line", "kotlin_import_line",
        "kotlin_route_line", "lua_def_line", "lua_export_line", "lua_import_line",
        "php_def_line", "php_export_line", "php_import_line", "py_call_line",
        "ruby_def_line", "ruby_export_line", "ruby_import_line", "ruby_route_line",
        "rust_def_line", "rust_export_line", "rust_import_line", "sh_def_line",
        "sh_import_line", "skip_ws", "swift_def_line", "swift_export_line",
        "swift_import_line",
    ],
    "resolves": [
        "extract_calls", "extract_definitions", "extract_exports", "extract_imports_sys",
        "extract_routes",
    ],
    "allowed_includes": [
        "db2_extractors.h", "ctype.h", "stdio.h", "stdlib.h", "string.h",
    ],
    "allowed_header_includes": ["stddef.h"],
    "allowed_undefined": [
        "__ctype_b_loc", "malloc", "memcpy", "memset", "snprintf", "strchr", "strcmp",
        "strlen", "strncmp", "strrchr", "strstr",
    ],
    "base_references": {
        "extract_calls": ["src/modules/db2/c/canonical_index.c"],
        "extract_definitions": ["src/modules/db2/c/canonical_index.c"],
        "extract_exports": ["src/modules/db2/c/canonical_index.c"],
        "extract_imports_sys": ["src/modules/db2/c/canonical_index.c"],
        "extract_routes": ["src/modules/db2/c/canonical_index.c"],
    },
    "provenance": "Generated from the fallback parser bodies in src/extractors.c, "
                  "src/extractors_extra.c, and src/extractors_new_langs.c; import-identity "
                  "helpers remain outside this unit and the C system-header table is embedded.",
    "evidence": "One reproducible, database-free parser cluster resolves all five canonical-index "
                "entry points. Private ABI mirrors isolate index.h; the default tree-sitter seam "
                "remains unavailable exactly as in the normal build. No DB, bus, provider, "
                "pgvector, process, filesystem, network, logging, or configuration import.",
}, {
    "path": "src/modules/db2/support/log_primitives.c",
    "source_sha256": "67a6e3bc54ee59c6fe47c5741ee36bb2c5c7776c293d34ae319f8033ad633131",
    "header": "src/modules/db2/support/db2_log.h",
    "header_sha256": "124c268149b35dd6236b90ff7503e37e0588b27995a2715bc5f4b99cba2ef186",
    "defines": ["aimee_log", "db2_log_install"],
    "resolves": ["aimee_log"],
    "allowed_includes": ["db2_log.h", "stdio.h"],
    "allowed_header_includes": ["stdarg.h"],
    "allowed_undefined": ["vsnprintf"],
    "base_references": {
        "aimee_log": [
            "src/modules/db2/c/canonical_index.c",
            "src/modules/db2/c/code_index.c",
            "src/modules/db2/c/cross_repo_build.c",
            "src/modules/db2/c/cross_repo_deps.c",
            "src/modules/db2/c/cross_repo_identity.c",
            "src/modules/db2/c/cross_repo_review.c",
            "src/modules/db2/c/cross_repo_route.c",
            "src/modules/db2/c/cross_repo_stats.c",
            "src/modules/db2/c/db2_init.c",
            "src/modules/db2/c/db2_reembed.c",
            "src/modules/db2/c/db2_tenant.c",
            "src/modules/db2/c/enrollments.c",
            "src/modules/db2/c/fact_ingest.c",
            "src/modules/db2/c/kb_payload.c",
            "src/modules/db2/c/learning.c",
            "src/modules/db2/c/pgvec_transport.c",
            "src/modules/db2/c/vault_pg.c",
        ],
    },
    "provenance": "The monolithic logger is replaced by a process-startup-installed sink; all "
                  "seventeen DB2 logging translation units are pinned to this bounded surface.",
    "evidence": "One formatting export and one startup installer preserve DB2 log level, module, "
                "and message semantics with a bounded message buffer and only vsnprintf imported. "
                "The sink carries no KB logger state, database, bus, provider, or allocation edge.",
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
    "path": "src/modules/db2/support/model_validation_primitives.c",
    "source_sha256": "a7d77ac228ab363f2b3c8b5381c5e9e09897f80574f8b6f07c223027439f47ba",
    "header": "src/modules/db2/support/db2_model_validation.h",
    "header_sha256": "e7bbdb2c7238692047cd7144b5592779add14c636c0ee9e7dae1adcc5df6eb99",
    "defines": [
        "kb_models_endpoint_valid", "kb_models_name_clean", "kb_models_wire_valid",
    ],
    "resolves": [
        "kb_models_endpoint_valid", "kb_models_name_clean", "kb_models_wire_valid",
    ],
    "resolution_disposition": "injected-module-contract",
    "allowed_includes": ["db2_model_validation.h", "string.h"],
    "allowed_header_includes": [],
    "allowed_undefined": ["strcmp", "strlen", "strncmp"],
    "base_references": {
        "kb_models_endpoint_valid": ["src/modules/db2/c/org_model_catalog.c"],
        "kb_models_name_clean": ["src/modules/db2/c/org_model_catalog.c"],
        "kb_models_wire_valid": ["src/modules/db2/c/org_model_catalog.c"],
    },
    "provenance": "The three pure model-catalog admission definitions are promoted from "
                  "src/kb/http/kb_models_validate.c; all DB2 calls are pinned to "
                  "org_model_catalog.c.",
    "evidence": "The descriptor owns the exact wire whitelist, printable-name bounds, and "
                "HTTP(S) endpoint grammar used by DB2's pre-database storage choke point. "
                "Normal and sanitizer parity cover NULL, every non-NUL byte, length boundaries, "
                "schemes, and legacy empty-endpoint behavior. Only strcmp, strlen, and strncmp "
                "are imported; there is no HTTP, JSON, DB, bus, provider, pgvector, DB3, "
                "allocation, configuration, I/O, or logging dependency.",
}, {
    "path": "src/modules/db2/support/node_kind_text_primitives.c",
    "source_sha256": "0b733803311e92c8baad98e3a14f8d43eac0819f82f83ab3a08a7b52eaa08116",
    "header": "src/modules/db2/support/db2_node_kind_text.h",
    "header_sha256": "890b4eb247c73135cb0df53fc67f520d16d2d032429f397e2665522ce6898668",
    "defines": ["memory_ontology_node_kind_to_text"],
    "resolves": ["memory_ontology_node_kind_to_text"],
    "resolution_disposition": "injected-module-contract",
    "allowed_includes": ["db2_node_kind_text.h"],
    "allowed_header_includes": [],
    "allowed_undefined": [],
    "base_references": {
        "memory_ontology_node_kind_to_text": ["src/modules/db2/c/rel_types_store.c"],
    },
    "provenance": "Definition promoted from the DB-free node-kind table in "
                  "src/modules/memory/memory_episodes.c; the sole DB2 call is pinned to "
                  "src/modules/db2/c/rel_types_store.c.",
    "evidence": "A deterministic integer-to-text switch with descriptor-owned numeric ABI and "
                "no imports, shared ontology header, allocation, I/O, DB, event-bus, provider, "
                "platform, pgvector, DB3, configuration, or logging dependency. Normal and "
                "sanitizer parity cover the complete signed 16-bit partition plus int boundaries.",
}, {
    "path": "src/modules/db2/support/pii_inject_gate_primitives.c",
    "source_sha256": "41096c30f976075f8f4b97a7a1825bbb53e340f5d112eb704a4a31ff4be1fef6",
    "header": "src/modules/db2/support/db2_pii_inject_gate.h",
    "header_sha256": "e1f544b3bd70ed8d4d012f34845f99922f219896bce0329efb9f2b687d1bd9af",
    "defines": ["memory_pii_should_inject"],
    "resolves": ["memory_pii_should_inject"],
    "resolution_disposition": "injected-module-contract",
    "allowed_includes": ["db2_pii_inject_gate.h"],
    "allowed_header_includes": [],
    "allowed_undefined": [],
    "base_references": {
        "memory_pii_should_inject": ["src/modules/db2/c/fact_recall.c"],
    },
    "provenance": "The allocation-free recall decision is promoted from "
                  "src/modules/memory/memory_pii_gate.c; the sole DB2 call is pinned to "
                  "src/modules/db2/c/fact_recall.c.",
    "evidence": "The descriptor owns the three-value sensitivity ABI, confidence floor, "
                "truth-value handling, and fail-closed treatment of NaN, low confidence, "
                "credentials, and unknown sensitivity values. The support object has no "
                "imports or memory classifier state and no allocation, I/O, DB, event-bus, "
                "provider, platform, pgvector, DB3, configuration, or logging edge. Normal "
                "and sanitizer parity cover the complete signed 16-bit sensitivity partition, "
                "int boundaries, finite confidence boundaries, infinities, NaN, and full-width "
                "turn-request truth values.",
}, {
    "path": "src/modules/db2/support/random_primitives.c",
    "source_sha256": "2e0182a05983d863952d080b754cb90eb4ee6dcf491f4bda7140ef205c0db69f",
    "header": "src/modules/db2/support/db2_random.h",
    "header_sha256": "bbbab168e217e7286ffae27ad3f1adaf8ed1797ae70a46c6c1e84bbcd32a98bb",
    "defines": ["platform_random_bytes", "platform_random_hex"],
    "resolves": ["platform_random_bytes", "platform_random_hex"],
    "allowed_includes": ["db2_random.h", "bcrypt.h", "stdio.h", "string.h"],
    "allowed_header_includes": ["stddef.h"],
    "allowed_undefined": ["fclose", "fopen", "fread", "memset", "snprintf"],
    "base_references": {
        "platform_random_bytes": [
            "src/modules/db2/c/artifacts.c",
            "src/modules/db2/c/management_action_journal.c",
            "src/modules/db2/c/management_identity_journal.c",
        ],
        "platform_random_hex": ["src/modules/db2/c/management_read_journal.c"],
    },
    "provenance": "Portable CSPRNG and lowercase-hex definitions promoted from "
                  "src/posix/platform_random.c, src/windows/platform_random.c, and "
                  "src/platform_random.c; all four DB2 referencing units are pinned.",
    "evidence": "The descriptor-owned implementation preserves the POSIX /dev/urandom and "
                "Windows BCryptGenRandom branches, exports only the two reviewed APIs, and "
                "imports only its bounded system I/O and formatting surface. It has no DB, "
                "event-bus, provider, pgvector, DB3, config, logging, or heap dependency.",
}, {
    "path": "src/modules/db2/support/rel_enum_text_primitives.c",
    "source_sha256": "231f5255d1350c752317529e2d0b2bab3528dbc6e0186e4fec780c3319bb8884",
    "header": "src/modules/db2/support/db2_rel_enum_text.h",
    "header_sha256": "169b08838b6425976afd817ad315b12db51a86df655b1d8a3dfb1c3ee7f23773",
    "defines": ["correction_behavior_to_text", "rel_sensitivity_to_text"],
    "resolves": ["correction_behavior_to_text", "rel_sensitivity_to_text"],
    "allowed_includes": ["db2_rel_enum_text.h"],
    "allowed_header_includes": [],
    "allowed_undefined": [],
    "base_references": {
        "correction_behavior_to_text": ["src/modules/db2/c/rel_types_store.c"],
        "rel_sensitivity_to_text": ["src/modules/db2/c/rel_types_store.c"],
    },
    "provenance": "Definitions promoted from the DB-free enum text core in src/rel_types.c; both "
                  "DB2 calls audited in src/modules/db2/c/rel_types_store.c.",
    "evidence": "Two deterministic three-value switches with descriptor-owned numeric ABI and no "
                "imports, shared ontology header, allocation, I/O, DB, event-bus, provider, "
                "platform, pgvector, DB3, or logging dependency; enum ABI and parity tested.",
}, {
    "path": "src/modules/db2/support/rel_seed_primitives.c",
    "source_sha256": "2794ca2836bcd26f6849165750abb8c6689f65b6073e7df602dafff7d892241c",
    "header": "src/modules/db2/support/db2_rel_seed.h",
    "header_sha256": "a9fdcb84c1dca6d8fa295fe0586be5e2ac43eb1ba17e53bb2197d47175307423",
    "defines": ["rel_types_seed_at", "rel_types_seed_count", "rel_types_seed_lookup"],
    "resolves": ["rel_types_seed_at", "rel_types_seed_count", "rel_types_seed_lookup"],
    "allowed_includes": ["db2_rel_seed.h", "db2_rel_type_helpers.h", "string.h"],
    "allowed_header_includes": [],
    "allowed_undefined": ["rel_type_normalize", "strcmp"],
    "base_references": {
        "rel_types_seed_at": ["src/modules/db2/c/rel_types_store.c"],
        "rel_types_seed_count": ["src/modules/db2/c/rel_types_store.c"],
        "rel_types_seed_lookup": [
            "src/modules/db2/c/entity_edges.c", "src/modules/db2/c/fact_lifecycle.c",
        ],
    },
    "provenance": "Full relationship seed rows generated by walking the compiled canonical "
                  "SEED_ONTOLOGY in src/rel_types.c; all four DB2 references are pinned.",
    "evidence": "The descriptor owns the generated database-free table and a private ABI mirror; "
                "size, offsets, enum widths, every field, iteration bounds, pointer identity, "
                "normalization, misses, and sanitizer behavior are compared with the monolith. "
                "Only the adjacent normalization support API and strcmp are imported.",
}, {
    "path": "src/modules/db2/support/rel_type_primitives.c",
    "source_sha256": "a3a9e88f2a90c0de5d09f952c60ff90843f4f332a9c2a411e2dc1e081f31cce1",
    "header": "src/modules/db2/support/db2_rel_type_helpers.h",
    "header_sha256": "cd1b904cb2fe0ff443ab94d1044ce6eefd71aa4e004ddca4641de27be9a391a2",
    "defines": ["rel_type_is_functional", "rel_type_normalize"],
    "resolves": ["rel_type_is_functional", "rel_type_normalize"],
    "allowed_includes": ["ctype.h", "db2_rel_type_helpers.h", "string.h"],
    "allowed_header_includes": ["stddef.h"],
    "allowed_undefined": ["__ctype_b_loc", "__ctype_tolower_loc", "strcmp"],
    "base_references": {
        "rel_type_is_functional": ["src/modules/db2/c/entity_edges.c"],
        "rel_type_normalize": [
            "src/modules/db2/c/fact_lifecycle.c",
            "src/modules/db2/c/ontology_evolution.c",
            "src/modules/db2/c/rel_types_store.c",
        ],
    },
    "provenance": "Definitions promoted from the DB-free core in src/rel_types.c; all DB2 calls "
                  "audited in entity_edges.c, fact_lifecycle.c, ontology_evolution.c, and "
                  "rel_types_store.c.",
    "evidence": "Relation normalization preserves the legacy process-locale ctype behavior and "
                "functional classification; only ctype and strcmp are imported. No ontology "
                "enum/header, DB, event-bus, provider, platform, pgvector, DB3, allocation, I/O, "
                "or logging dependency.",
}, {
    "path": "src/modules/db2/support/runtime_config_primitives.c",
    "source_sha256": "392a2271241a13046b61bc49a99f3e4efe22edbe8becf2290f13dcb3052400ed",
    "header": "src/modules/db2/support/db2_runtime_config.h",
    "header_sha256": "e8dd532f6729eb7ae42929168c1faa7abf423ea49da9632ae1a7977f0c48d2cc",
    "defines": [
        "config_audit_worm_enabled", "config_cache_disabled",
        "config_code_cochange_git_enabled", "config_css_style_graph_enabled",
        "config_embedder_command_current",
        "config_kb_curator_cross_repo_caller_collision_c",
        "config_kb_curator_cross_repo_distinctiveness_v",
        "config_kb_curator_cross_repo_graph_enabled", "config_kb_curator_cross_repo_k",
        "config_kb_curator_cross_repo_len_min", "config_kb_curator_cross_repo_m",
        "config_kb_curator_cross_repo_max_candidates",
        "config_kb_curator_cross_repo_p_pct",
        "config_kb_curator_cross_repo_review_queue_max", "config_kb_pdf_vector_enabled",
        "config_kb_purge_fence_ttl_s", "config_present", "config_typed_facts_enabled",
        "db2_runtime_config_install",
    ],
    "resolves": [
        "config_audit_worm_enabled", "config_cache_disabled",
        "config_code_cochange_git_enabled", "config_css_style_graph_enabled",
        "config_embedder_command_current",
        "config_kb_curator_cross_repo_caller_collision_c",
        "config_kb_curator_cross_repo_distinctiveness_v",
        "config_kb_curator_cross_repo_graph_enabled", "config_kb_curator_cross_repo_k",
        "config_kb_curator_cross_repo_len_min", "config_kb_curator_cross_repo_m",
        "config_kb_curator_cross_repo_max_candidates",
        "config_kb_curator_cross_repo_p_pct",
        "config_kb_curator_cross_repo_review_queue_max", "config_kb_pdf_vector_enabled",
        "config_kb_purge_fence_ttl_s", "config_present", "config_typed_facts_enabled",
    ],
    "resolution_disposition": "injected-module-contract",
    "allowed_includes": ["db2_runtime_config.h", "string.h"],
    "allowed_header_includes": [],
    "allowed_undefined": ["memchr"],
    "base_references": {
        "config_audit_worm_enabled": ["src/modules/db2/c/kb_audit_worm.c"],
        "config_cache_disabled": ["src/modules/db2/c/rules.c"],
        "config_code_cochange_git_enabled": ["src/modules/db2/c/canonical_index.c"],
        "config_css_style_graph_enabled": [
            "src/modules/db2/c/canonical_index.c", "src/modules/db2/c/css_migration.c",
            "src/modules/db2/c/css_render.c",
        ],
        "config_embedder_command_current": [
            "src/modules/db2/c/kb_payload.c", "src/modules/db2/c/kb_service_backend.c",
        ],
        "config_kb_curator_cross_repo_caller_collision_c": [
            "src/modules/db2/c/cross_repo_deps.c",
        ],
        "config_kb_curator_cross_repo_distinctiveness_v": [
            "src/modules/db2/c/cross_repo_deps.c",
        ],
        "config_kb_curator_cross_repo_graph_enabled": [
            "src/modules/db2/c/cross_repo_deps.c",
        ],
        "config_kb_curator_cross_repo_k": ["src/modules/db2/c/cross_repo_deps.c"],
        "config_kb_curator_cross_repo_len_min": ["src/modules/db2/c/cross_repo_deps.c"],
        "config_kb_curator_cross_repo_m": ["src/modules/db2/c/cross_repo_deps.c"],
        "config_kb_curator_cross_repo_max_candidates": [
            "src/modules/db2/c/cross_repo_deps.c",
        ],
        "config_kb_curator_cross_repo_p_pct": ["src/modules/db2/c/cross_repo_deps.c"],
        "config_kb_curator_cross_repo_review_queue_max": [
            "src/modules/db2/c/cross_repo_deps.c",
        ],
        "config_kb_pdf_vector_enabled": ["src/modules/db2/c/kb_payload.c"],
        "config_kb_purge_fence_ttl_s": ["src/modules/db2/c/kb_runtime_state.c"],
        "config_present": ["src/modules/db2/c/canonical_index.c"],
        "config_typed_facts_enabled": [
            "src/modules/db2/c/css_migration.c", "src/modules/db2/c/fact_ingest.c",
        ],
    },
    "provenance": "The complete DB2 config read set is captured once as a versioned immutable "
                  "startup snapshot; every legacy call site is pinned to the matching field.",
    "evidence": "Nineteen exact exports replace eighteen live host getters with one bounded "
                "install operation. Invalid ABI, NULL, and unterminated snapshots fail atomically; "
                "the implementation imports only memchr and contains no file, DB, bus, vector, "
                "provider, allocation, or reload dependency.",
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
}, {
    "path": "src/modules/db2/support/time_primitives.c",
    "source_sha256": "3a75fa922a9d7ddfbd51a4297461a266d9e14a2c995422a1ae6165116545f04c",
    "header": "src/modules/db2/support/db2_time.h",
    "header_sha256": "f6b01dfcda9c2d6e10d2ffb8fae9376dac4ac2fc0cc84bcc10f16f23235117e8",
    "defines": ["now_utc", "parse_utc_ts"],
    "resolves": ["now_utc", "parse_utc_ts"],
    "allowed_includes": ["db2_time.h", "stdio.h", "string.h"],
    "allowed_header_includes": ["stddef.h", "time.h"],
    "allowed_undefined": [
        "__isoc23_sscanf", "__isoc99_sscanf", "gmtime_r", "strftime", "time", "timegm",
    ],
    "base_references": {
        "now_utc": [
            "src/modules/db2/c/artifacts.c",
            "src/modules/db2/c/bandit.c",
            "src/modules/db2/c/calibration.c",
            "src/modules/db2/c/canonical_index.c",
            "src/modules/db2/c/code_index.c",
            "src/modules/db2/c/code_index_ops.c",
            "src/modules/db2/c/code_project_lifecycle.c",
            "src/modules/db2/c/corpus_jobs.c",
            "src/modules/db2/c/corpus_structural.c",
            "src/modules/db2/c/demotion.c",
            "src/modules/db2/c/feature_rows.c",
            "src/modules/db2/c/feedback.c",
            "src/modules/db2/c/kb_docs.c",
            "src/modules/db2/c/kb_releases.c",
            "src/modules/db2/c/report_enrichments.c",
            "src/modules/db2/c/rules.c",
            "src/modules/db2/c/tasks.c",
            "src/modules/db2/c/vector_index_ops.c",
        ],
        "parse_utc_ts": [
            "src/modules/db2/c/code_index.c",
            "src/modules/db2/c/demotion.c",
        ],
    },
    "provenance": "Adjacent UTC formatter and parser definitions promoted from src/util.c; all "
                  "eighteen formatter and two parser DB2 units audited.",
    "evidence": "Shared UTC timestamp contract with only system time/parsing imports; Linux uses "
                "timegm and the preserved Windows branch uses _mkgmtime. No DB, event-bus, "
                "provider, pgvector, DB3, allocation, I/O, or logging dependency.",
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
    "snprintf", "socket", "sprintf", "sqlite3_close", "sqlite3_errmsg", "sqlite3_exec", "sqlite3_free",
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
    if len(result) != len(set(result)):
        fail("support-include", f"includes must be unique in {raw}: {result}")
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
        support_definitions: set[str] = set()
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
            support_definitions.update(definitions)
            undefined = _nm_undefined(
                _run(["nm", "-u", "--format=posix", str(obj)], root)
            )
            allowed_undefined = set(unit["allowed_undefined"])
            if not undefined <= allowed_undefined:
                fail("support-undefined", f"{raw}: forbidden undefined symbols "
                     f"{sorted(undefined - allowed_undefined)}")
            support_objects.append(obj)

        # An allowlist is not provenance by itself. Every non-system import
        # admitted by one support object must be supplied by another object in
        # this same descriptor-owned closure; otherwise an arbitrary project
        # dependency could be hidden behind allowed_undefined.
        _validate_support_import_provenance(support_units, support_definitions)

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


def _validate_support_import_provenance(
    support_units: list[dict[str, object]], support_definitions: set[str]
) -> None:
    for unit in support_units:
        raw = str(unit["path"])
        unowned = sorted(
            symbol for symbol in unit["allowed_undefined"]
            if symbol not in support_definitions and classify(symbol)[0] != "system-link"
        )
        if unowned:
            fail("support-undefined-provenance", f"{raw}: imports have no descriptor-owned "
                 f"provider or system-link classification: {unowned}")


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
        if Path(raw) == GENERATED_REL_SEED:
            _verify_generated_rel_seed(root, support_source)
        header_raw = str(unit["header"])
        support_header = _safe_file(root, header_raw, SUPPORT_BOUNDARY)
        actual_header_sha256 = hashlib.sha256(support_header.read_bytes()).hexdigest()
        if actual_header_sha256 != unit["header_sha256"]:
            fail("support-header-hash", f"{header_raw}: reviewed header content changed")
        origin_source_raw = unit.get("origin_source")
        origin_header_raw = unit.get("origin_header")
        if (origin_source_raw is None) != (origin_header_raw is None):
            fail("support-origin", f"{raw}: source and header origins must be declared together")
        if origin_source_raw is not None:
            if not isinstance(origin_source_raw, str) or not isinstance(origin_header_raw, str):
                fail("support-origin", f"{raw}: origins must be paths")
            origin_source = _safe_file(root, origin_source_raw, Path("src/vendor"))
            origin_header = _safe_file(root, origin_header_raw, Path("src/vendor"))
            if support_source.read_bytes() != origin_source.read_bytes():
                fail("support-origin-drift", f"{raw}: differs from {origin_source_raw}")
            if support_header.read_bytes() != origin_header.read_bytes():
                fail("support-origin-drift", f"{header_raw}: differs from {origin_header_raw}")
        includes = _support_includes(root, raw)
        if includes != unit["allowed_includes"]:
            fail("support-include", f"{raw}: expected={unit['allowed_includes']}, "
                 f"actual={includes}")
        header_includes = _support_includes(root, header_raw)
        if header_includes != unit["allowed_header_includes"]:
            fail("support-header-include", f"{header_raw}: "
                 f"expected={unit['allowed_header_includes']}, actual={header_includes}")
    return policy


def _verify_generated_rel_seed(root: Path, checked_in: Path) -> None:
    """Regenerate the DB2 ontology copy inside the closure gate itself."""
    generator_source = _safe_file(
        root, "scripts/gen-memory-ontology-seed.c", Path("scripts")
    )
    canonical_source = _safe_file(root, "src/rel_types.c", Path("src"))
    with tempfile.TemporaryDirectory(prefix="db2-rel-seed-generate-") as raw_tmp:
        tmp = Path(raw_tmp)
        generator = tmp / "gen-memory-ontology-seed"
        _run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root / 'src'}", f"-I{root / 'src/headers'}",
            "-o", str(generator), str(generator_source), str(canonical_source),
        ], root)
        generated = tmp / "rel_seed_primitives.c"
        _run([
            str(generator), str(tmp / "ontology_seed.go"),
            str(tmp / "ontology_seed.tsv"), str(generated),
        ], root)
        if generated.read_bytes() != checked_in.read_bytes():
            fail("support-generated-drift", f"{GENERATED_REL_SEED}: compiled canonical seed "
                 "does not reproduce the checked-in source")


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


def _ordered_string_list(value: object, label: str) -> list[str]:
    if not isinstance(value, list) or not all(isinstance(item, str) and item for item in value):
        fail("contract-shape", f"{label} must be a string array")
    result = list(value)
    if len(result) != len(set(result)):
        fail("contract-order", f"{label} must be unique")
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
    origin_fields = {"origin_source", "origin_header"}
    disposition_fields = {"resolution_disposition"}
    for index, unit in enumerate(value):
        keys = frozenset(unit) if isinstance(unit, dict) else frozenset()
        if (not isinstance(unit, dict) or
                keys not in {
                    frozenset(required),
                    frozenset(required | origin_fields),
                    frozenset(required | disposition_fields),
                    frozenset(required | origin_fields | disposition_fields),
                }):
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
        if origin_fields <= set(unit):
            for field in sorted(origin_fields):
                origin = unit[field]
                if not isinstance(origin, str):
                    fail("support-origin", f"{path}: {field} must be a path")
                if check_files:
                    _safe_file(root, origin, Path("src/vendor"))
        resolution_disposition = unit.get(
            "resolution_disposition",
            "descriptor-owned-copy/generated-input" if origin_fields <= set(unit)
            else "portable-core-promotion",
        )
        if resolution_disposition not in {
            "portable-core-promotion", "descriptor-owned-copy/generated-input",
            "injected-module-contract",
        }:
            fail("support-disposition", f"{path}: invalid resolution_disposition")
        defines = _string_list(unit["defines"], f"support[{index}].defines", symbols=True)
        resolves = _string_list(unit["resolves"], f"support[{index}].resolves", symbols=True)
        includes = _ordered_string_list(unit["allowed_includes"],
                                        f"support[{index}].allowed_includes")
        header_includes = _ordered_string_list(
            unit["allowed_header_includes"],
            f"support[{index}].allowed_header_includes",
        )
        allowed_undefined = _string_list(
            unit["allowed_undefined"], f"support[{index}].allowed_undefined", symbols=True
        )
        if not set(resolves) <= set(defines):
            fail("support-resolves", f"{path}: resolves must be a subset of global definitions")
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
    legacy_probe = dict(expected_probe)
    legacy_probe["extra_c_flags"] = LEGACY_PROBE_FLAGS
    if version == SCHEMA_VERSION:
        legacy_probe["support_compile_flags"] = LEGACY_SUPPORT_COMPILE_FLAGS
    if probe_value != expected_probe and (check_files or probe_value != legacy_probe):
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
    unexpected_removals = sorted(set(removed_units) - set(HOST_ADAPTER_REHOMES))
    if unexpected_removals:
        fail("previous-source-removal", "legacy DB2 translation units disappeared without a "
             f"reviewed host-adapter rehome: {unexpected_removals}")
    for old_path in removed_units:
        new_path = HOST_ADAPTER_REHOMES[old_path]
        _safe_file(root, new_path, Path("src/kb/db2_adapters"))
    previous_support_by_path = {str(unit["path"]): unit for unit in previous_support}
    current_support_by_path = {str(unit["path"]): unit for unit in current_support}
    removed_support = sorted(set(previous_support_by_path) - set(current_support_by_path))
    if removed_support:
        fail("previous-support-removal", f"descriptor support units disappeared: {removed_support}")
    # A host-adapter rehome can only shrink a support unit's frozen base-call
    # provenance. HOST_ADAPTER_REHOMES is the explicit reviewed admission list;
    # a removed unit not named there already fails above. Preserve mapping and
    # list order while filtering those exact paths so reordering remains drift.
    def without_rehomed_references(unit: dict[str, object]) -> dict[str, object]:
        normalized = copy.deepcopy(unit)
        references = normalized.get("base_references")
        if isinstance(references, dict):
            for symbol, paths in references.items():
                if isinstance(paths, list):
                    references[symbol] = [
                        path for path in paths if path not in HOST_ADAPTER_REHOMES
                    ]
        return normalized

    changed_support = sorted(
        path for path in set(previous_support_by_path) & set(current_support_by_path)
        if without_rehomed_references(previous_support_by_path[path]) !=
        without_rehomed_references(current_support_by_path[path])
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
        expected_disposition = unit.get(
            "resolution_disposition",
            "descriptor-owned-copy/generated-input"
            if "origin_source" in unit else "portable-core-promotion",
        )
        for symbol in unit["resolves"]:
            before = previous_rows.get(str(symbol))
            before_references = [
                path for path in before["references"] if path not in HOST_ADAPTER_REHOMES
            ] if before is not None else None
            if (before is None or before["disposition"] != expected_disposition or
                    before_references != base_references[symbol]):
                fail("previous-support-admission",
                     f"{unit['path']}: {symbol} lacks exact {expected_disposition} base evidence")
            if symbol in current_rows:
                fail("previous-support-resolution",
                     f"{unit['path']}: declared resolution {symbol} remains unresolved")
    added_symbols = sorted(set(current_rows) - set(previous_rows))
    rejected_added: list[str] = []
    probe_mode_migrated = (
        isinstance(previous, dict) and isinstance(current, dict) and
        isinstance(previous.get("probe"), dict) and isinstance(current.get("probe"), dict) and
        previous["probe"].get("extra_c_flags") == LEGACY_PROBE_FLAGS and
        current["probe"].get("extra_c_flags") == PROBE_FLAGS
    )
    for symbol in added_symbols:
        row = current_rows[symbol]
        references = set(row["references"])
        if (probe_mode_migrated and symbol == "getpid" and
                references == {"src/modules/db2/c/db2_init.c"} and
                row["disposition"] == "system-link"):
            continue
        permitted_paths = {
            str(unit["path"]) for unit in added_support
            if symbol in unit["allowed_undefined"]
        }
        if (row["disposition"] != "system-link" or
                not references or not references <= permitted_paths):
            rejected_added.append(symbol)
    if rejected_added:
        fail("previous-symbol-growth", f"new unresolved symbols are forbidden: {rejected_added}")
    expanded: list[str] = []
    for symbol in sorted(set(current_rows) & set(previous_rows)):
        before = set(previous_rows[symbol]["references"])
        after = set(current_rows[symbol]["references"])
        growth = after - before
        if (probe_mode_migrated and symbol == "getenv" and
                growth == {"src/modules/db2/c/db2_init.c"} and
                current_rows[symbol]["disposition"] == "system-link"):
            continue
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
