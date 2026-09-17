#!/usr/bin/env python3
"""Freeze remaining native memory debt and prevent retired C clients returning."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


ALLOWED_C = {
    "src/modules/memory/gw_stage_memory.c",
    "src/modules/memory/memory_content_gate_bus.c",
    "src/modules/memory/memory_data_bus.c",
    "src/modules/memory/memory_domain_bus.c",
    "src/modules/memory/memory_domain_runtime_bus.c",
    "src/modules/memory/memory_scope_connection.c",
    "src/modules/memory/memory_embed_bus.c",
}

FORBIDDEN_INCLUDES = (
    "db1_client/",
    "modules/db2/c/",
    "db_postgres.h",
    "sqlite3.h",
    "libpq-fe.h",
)

RETIRED_POLICY_C = (
    "src/posix/memory_embed.c",
    "src/kb/kb_memory_embed.c",
    "src/modules/memory/memory_activation.h",
    "src/modules/memory/memory_pii_gate.c",
    "src/modules/memory/memory_pii_gate.h",
    "src/modules/memory/include/aimee/memory/pii_provider.h",
    "src/modules/memory/memory_assemble_util.h",
    "src/modules/memory/memory_extract_patterns.c",
    "src/modules/memory/memory_extract_patterns.h",
    "src/modules/memory/memory_fact_gate.c",
    "src/posix/memory.c",
    "src/windows/memory.c",
    "src/modules/db2/c/prospective_memories.c",
    "src/kb/memory_rewrite_llm.c",
    "src/modules/memory/memory_rewrite_llm.h",
    "src/modules/memory/memory_legacy_bus.c",
    "src/kb/fact_grounding.c",
    "src/kb/fact_grounding.h",
)

# These callbacks have no production consumers. The Go client and handler now
# cover their wire/domain fixtures. Reject relocation as well as restoration;
# the remaining C inventory is unfinished G0 work, not permission to add a shim.
RETIRED_NATIVE_SYMBOLS = re.compile(
    r"\b(?:memory_fact_gate_check|memory_fact_gate_register_checker|"
    r"memory_fact_gate_checker_fn|memory_extract_patterns|memory_pattern_scan_turn|"
    r"memory_extract_register_extractor|memory_extract_register_turn_scanner|"
    r"memory_pattern_extractor_fn|memory_pattern_turn_scanner_fn|"
    r"pattern_triple_t|memory_pattern_turn_t|assemble_texts_near_duplicate|"
    r"memory_pii_(?:register_\w+|turn_requests_sensitive|rel_sensitivity(?:_batch)?|should_inject)|"
    r"gate_check_ephemeral|gate_has_evidence_markers|memory_scan_content|"
    r"platform_memory_background_embed(?:_set_suppressed)?|"
    r"assemble_token_bits|context_recency_from_age_days|context_apply_recency|"
    r"context_xml_tag_for_header|memory_recall_activated|db2_kb_service_memory_recall_json|"
    r"db2_kb_service_memory_prospective_\w+|"
    r"db2_kb_service_memory_(?:delete|update|touch|reject|restore|upsert_workflow)_json|"
    r"db2_kb_service_memory_(?:briefing|alerts|assemble_context|compact_windows|query_edges|check_drift)_json|"
    r"db2_kb_service_memory_(?:get|list|fact_history|top_l2_facts|load_eval_corpus|list_session_scope_priority|list_session_scope_priority_like|search_facts_patterns_by_keyword|scope_visibility_rank|tag_workspace|tag_scope)_json|"
    r"db2_kb_service_memory_(?:key_exists|find_id_by_key_kind|list_low_effectiveness|list_unused_l2|list_superseded_keys|review_list|set_artifact|effectiveness_stats)_json|"
    r"db2_kb_service_memory_(?:lint|maintenance_run|entity_profile|entity_edges|search_graph|search_graph_as_of|get_episode|get_provenance|link_query|link_create|link_delete|list_conflicts|query_health|stats)_json|"
    r"db2_kb_service_directive_(?:create|resolve|suppress|sweep_expired|list_json)|"
    r"memory_activation_(?:t|row_t|load|last_loaded|last_turn|in_cooldown|is_sticky|is_delayed|record))\b"
)

FORBIDDEN_STORE_CALLS = (
    "db2_conn(",
    "aimee_pg_",
)

EXTERNAL_CONNECTION_C = {
    "src/modules/db2/c/fact_recall.c",
}

KB_CONNECTION_C = {
    "src/kb/kb_memory_facts.c",
}

FORBIDDEN_KB_MEMORY_POLICY = (
    "SELECT ",
    "INSERT INTO",
    "UPDATE ",
    "DELETE FROM",
    "SHA256",
    "fact_grounded",
    "fact_norm_text",
    "rel_type_canonicalize",
    "mf_claim_job",
    "mf_mark_done",
    "mf_mark_retry_or_fail",
    "mf_build_system_prompt",
    "mf_commit_facts",
    "mf_subject_kind",
)

# This adapter binds the already-authorized request scope onto a prepared
# PostgreSQL statement. It does not create statements or execute storage work;
# binding is connection plumbing and is the explicit exception to the direct
# store-call ban.
STATEMENT_BINDING_C = {
    "src/modules/memory/memory_scope_connection.c",
}


class BoundaryError(ValueError):
    pass


def validate(root: Path) -> None:
    descriptor_path = root / "src/modules/memory/module.yaml"
    descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
    declared = {source for source in descriptor.get("sources", []) if source.endswith(".c")}
    physical = {
        path.relative_to(root).as_posix()
        for path in (root / "src/modules/memory").glob("*.c")
    }
    if declared != ALLOWED_C or physical != ALLOWED_C:
        raise BoundaryError(
            f"rule=memory-c-allowlist declared={sorted(declared)} physical={sorted(physical)} "
            f"expected={sorted(ALLOWED_C)}"
        )

    retired = sorted((root / "src/modules/db2/c").glob("memory_*.c"))
    if retired:
        raise BoundaryError(
            "rule=retired-db2-memory-c files="
            + repr([path.relative_to(root).as_posix() for path in retired])
        )

    returned_policy = [relative for relative in RETIRED_POLICY_C if (root / relative).exists()]
    if returned_policy:
        raise BoundaryError(
            f"rule=retired-platform-memory-policy files={returned_policy}"
        )

    for path in sorted((root / "src").rglob("*")):
        if path.suffix not in {".c", ".h"} or "build" in path.relative_to(root).parts:
            continue
        # Ignore historical comments; declarations, calls and macro aliases
        # must not reintroduce a retired memory client in another native owner.
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(encoding="utf-8"), flags=re.S)
        match = RETIRED_NATIVE_SYMBOLS.search(source)
        if match:
            raise BoundaryError(
                f"rule=retired-memory-native-client file={path.relative_to(root)} "
                f"symbol={match.group(0)}"
            )

    missing_connections = [relative for relative in KB_CONNECTION_C if not (root / relative).exists()]
    if missing_connections:
        raise BoundaryError(f"rule=memory-c-connection-inventory missing={missing_connections}")

    for relative in sorted(KB_CONNECTION_C):
        text = (root / relative).read_text(encoding="utf-8")
        returned = [token for token in FORBIDDEN_KB_MEMORY_POLICY if token in text]
        if returned:
            raise BoundaryError(
                f"rule=kb-memory-c-policy file={relative} tokens={returned}"
            )

    violations: list[str] = []
    for relative in sorted(ALLOWED_C | EXTERNAL_CONNECTION_C):
        text = (root / relative).read_text(encoding="utf-8")
        for include in FORBIDDEN_INCLUDES:
            if include in text:
                violations.append(f"{relative}: {include}")
        for call in FORBIDDEN_STORE_CALLS:
            if call in text and not (relative in STATEMENT_BINDING_C and call == "aimee_pg_"):
                violations.append(f"{relative}: {call}")
    if violations:
        raise BoundaryError(
            f"rule=memory-c-direct-store-access violations={violations}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        validate(args.root.resolve())
    except (BoundaryError, FileNotFoundError, json.JSONDecodeError) as exc:
        print(f"check_memory_c_boundary: {exc}")
        return 1
    print("check_memory_c_boundary: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
