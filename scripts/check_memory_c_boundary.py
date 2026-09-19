#!/usr/bin/env python3
"""Freeze remaining native memory debt and prevent retired C clients returning."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


ALLOWED_C = {
    "src/modules/memory/memory_data_bus.c",
    "src/modules/memory/memory_scope_connection.c",
}

FORBIDDEN_INCLUDES = (
    "db1_client/",
    "modules/db2/c/",
    "db_postgres.h",
    "sqlite3.h",
    "libpq-fe.h",
)

RETIRED_POLICY_C = (
    "src/modules/db2/c/entity_registry.c",
    "src/modules/db2/c/entity_registry.h",
    "src/modules/db2/c/ontology_evolution.c",
    "src/modules/db2/c/ontology_evolution.h",
    "src/modules/db2/c/rel_types_store.c",
    "src/modules/db2/c/rel_types_store.h",
    "src/modules/db2/c/fact_lifecycle.c",
    "src/modules/db2/c/fact_lifecycle.h",
    "src/modules/memory/memory_fact_gate.h",
    "src/tests/support/memory_policy_stub.h",
    "src/modules/db2/c/typed_facts.c",
    "src/modules/db2/c/typed_facts.h",
    "src/modules/memory/memory_domain_bus.c",
    "src/modules/db2/c/trace_mining.c",
    "src/modules/db2/c/trace_mining.h",
    "src/kb/kb_conventions.c",
    "scripts/gen-memory-ontology-seed.c",
    "src/modules/db2/c/memory_conflicts.h",
    "src/modules/db2/c/memory_lint.h",
    "src/kb/kb_demote.c",
    "src/kb_demote.h",
    "src/modules/memory/gw_stage_memory.c",
    "src/modules/memory/gw_stage_memory.h",
    'src/modules/db2/c/fact_ingest.c',
    'src/modules/db2/c/fact_ingest.h',
    'src/modules/db2/c/fact_recall.c',
    'src/modules/db2/c/fact_recall.h',

    "src/modules/memory/memory_graph_fusion.h",
    "src/modules/kb_client/kb_client_prospective.c",
    "src/modules/db2/c/epistemic_directives.c",
    "src/modules/db2/c/epistemic_directives.h",
    "src/modules/memory/memory_domain_runtime_bus.c",
    "src/modules/memory/memory_embed_bus.c",
    "src/modules/memory/memory_profile_pack.h",
    "src/modules/memory/memory_content_gate_bus.c",
    "src/modules/memory/memory_platform.h",
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
    "src/modules/kb_client/kb_client_data.c",
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
    r"\bmem_eval_load_corpus\b(?=\s*\()|"
    r"\bmcp_memory_maintain_(?:required_cap|model_modes)\b(?=\s*\()|"
    r"\b(?:entity_name_normalize|db2_entity_(?:register(?:_named)?|alias_bind|resolve|kind|mark_merged|aliases_for|conflict_\w+))\b(?=\s*\()|"
    r"\b(?:db2_entity_(?:merge|unmerge)(?:_as)?|kb_client_entities_(?:merge|unmerge)|db2_kb_service_entities_(?:merge|unmerge)_json)\b(?=\s*\()|"
    r"\b(?:db2_ontology_\w+|db2_rel_types_resolve|db2_entity_(?:merge_)?summaries)\b(?=\s*\()|"
    r"\b(?:aimee_db2_register_fact_gate_provider|db2_rel_types_stage_provisional|db2_fact_commit(?:_with_actor|_with_evidence)?|db2_fact_mutation_promote_supported|db2_fact_mutation_expire_candidates|db2_fact_expire_speculative|db2_fact_promote_durable)\b(?=\s*\()|"
    r"\b(?:kbs_typed_flag|kbs_typed_budget|kbs_estimate_tokens|kbs_pack_trace|kbs_typed_channel|kbs_channel_try_add|kbs_typed_watermarks|kbs_observation_visible|kbs_action_visible)\b(?=\s*\()|"
    r"\b(?:db2_semantic_assertion_search|db2_semantic_assertion_get_filtered|db2_semantic_assertion_index_list|db2_memory_scope_bind_current)\b(?=\s*\()|"
    r"\b(?:memory_find_facts|pgvec_memory_search)\b(?=\s*\()|"
    r"\b(?:db2_trace_mining_last_id|db2_trace_mining_record)\b(?=\s*\()|"
    r"\b(?:memory_alerts|kb_client_memory_alerts_json|db2_memory_lifecycle_(?:list_newly_superseded|update_state))\b(?=\s*\()|"
    r"\b(?:memory_briefing|kb_client_memory_briefing|db2_memory_briefing_list_key_facts|db2_memory_briefing_list_recent_activity|db2_memory_briefing_list_active_entities)\b(?=\s*\()|"
    r"\bkb_client_memory_find_facts_visible\b(?=\s*\()|"
    r"\bkb_client_memory_(?:link_create|link_query|link_delete|restore|review_list_json|stats|stats_json|query_health|effectiveness_stats|fact_history|list_low_effectiveness|list_unused_l2|list_superseded_keys)\b(?=\s*\()|"
    r"\b(?:reflect_call_synthesis_agent|reflect_synthesis_result_t|REFLECT_MAX_RESULTS|REFLECT_RULE_THRESHOLD)\b|"
    r"\bmemory_touch(?:_many)?\b(?=\s*\()|"
    r"\bdb2_memory_(?:top_l2_facts|list_session_scope_priority(?:_like)?)\b(?=\s*\()|"
    r"\b(?:memory_find_facts_visible(?:_ex|_lexical_fallback)?|db2_memory_find_facts_like)\b(?=\s*\()|"
    r"\b(?:kb_extract_convention_candidates|db2_kb_convention_row_t|db2_kb_documents_list_convention_candidates)\b|"
    r"\b(?:memory_list|db2_rel_types_ensure_seed|memory_ontology_node_kind_to_text)\b|"
    r"\b(?:memory_collect_scopes|memory_primary_scope|memory_scope_visibility_rank|db2_memory_scope_context_rank(?:_batch)?|memory_scope_tag_t|memory_scope_level_t|MEMORY_SCOPE_(?:NONE|GLOBAL|WORKSPACE|PROJECT))\b|"
    r"\b(?:kb_client_relations_schema_list|db2_relation_schema_row_t|graph_walk_entry_t|memory_graph_walk|RELATION_MASK(?:_ALL)?|memory_ontology_relation_(?:from|to)_text|memory_ontology_node_kind_from_text)\b|"
    r"\b(?:memory_lint_issue_t|MEMORY_LINT_\w+)\b|"
    r"\b(?:memory_fact_history|memory_scope_level_name|db2_memory_epistemic_kind)\b(?=\s*\()|"
    r"\bcontext_(?:row_class|result_copy|memory_kind|memory_duplicates_code|text_has_anchor|memory_anchors_code|memory_code_anchor)\b(?=\s*\()|"
    r"\b(?:kb_demote_run|db2_memory_promotion_demote_id|db2_demotion_score|db2_demotion_profile_write|db2_demotion_profile_read|db2_demotion_candidates)\b(?=\s*\()|"
    r"\b(?:ingress_render_block|ingress_preinject_(?:register_confidence_provider|confidence|format_envelope|format_code_block|format_task_context|task_state_reset|recall_unavailable_total))\b(?=\s*\()|"
    r"\b(?:db2_fact_ingest_text(?:_as_actor|_with_evidence)?|db2_typed_fact_ingress|db2_fact_recall_(?:block|in_query)|aimee_db2_register_fact_(?:extract|scan|recall)_provider)\b(?=\s*\()|"
    r"\b(?:db2_kb_service_memory_(?:context_block|facts)_json|kb_handle_memory_(?:context_block|facts)|memory_get_context_block)\b(?=\s*\()|"
    r"\bkb_client_memory_(?:context_block|facts)\b(?=\s*\()|"
    r"\b(?:db2_kb_service_facts_retract_json|kb_handle_facts_retract)\b(?=\s*\()|"
    r"\b(?:db2_fact_retract|db2_fact_mutation_invalidate)\b(?=\s*\()|"
    r"\bkb_client_facts_retract\b(?=\s*\()|"
    r"\baimee_memory_(?:request_encode|response_decode|put_u32|get_u32|put_i64|get_i64|extract_\w+|scan_\w+)\b(?=\s*\()|"
    r"\bdb2_fact_candidates\b(?=\s*\()|"
    r"\b(?:memory_graph_(?:relation_gravity|confidence_factor|edge_score|detect_code_shape|expand_from_seeds|point_id_to_node_key|populate_score_parts|distribute_path_credit)|memory_fusion_\w+|memory_apply_feedback_path)\b(?=\s*\()|"
    r"\bdb2_memory_(?:reject|restore)\b(?=\s*\()|"
    r"\b(?:ir_session_start|ir_stage_persona_instructions|ir_stage_memory|ir_stage_first_turn_shell_block|gw_memory_system_prompt|gw_stage_memory_enabled|gw_stage_memory_recall_gate_\w+)\b(?=\s*\()|"
    r"\bdb1_cognify_job_\w+\b|"
    r"\bmemory_cognify_(?:unit|drain|queue_status|parse_response)\b(?=\s*\()|"
    r"\bkb_client_memory_(?:export_jsonl|decisions_export_jsonl|key_exists)\b(?=\s*\()|"
    r"\bmemory_(?:list_episodes|search_graph|get_entity_profile)\b(?=\s*\()|"
    r"\bkb_client_memory_(?:get_entity_profile|get_entity_edges|search_graph(?:_as_of)?|get_episode)\b(?=\s*\()|"
    r"\b(?:memory_embed|kb_client_memory_(?:embed|reembed_\w+)_json|db2_kb_service_(?:get_active_embedder_version|set_active_embedder_version|collect_reembed_status|mark_reembed_finished|prepare_reembed_start|update_reembed_progress|list_unembedded_memory_ids|list_pending_reembed_memory_ids|count_embeddings_for_version))\b(?=\s*\()|"
    r"\bkb_client_memory_(?:reindex|rebuild|repair|maintenance_run|lint)_json\b(?=\s*\()|"
    r"\bkb_client_memory_(?:scope_visibility_rank|tag_workspace|tag_scope|get_provenance)\b(?=\s*\()|"
    r"\bkb_client_memory_prospective_\w+\b(?=\s*\()|"
    r"\b(?:is_negation_marker|extract_negation_tokens|memory_query_polarity|memory_refresh_derived_metadata|memory_refresh_coref_entities|memory_extract_named_entities|memory_coref_(?:audit_record|has_pronoun|llm_resolve|mode_effective|window_effective|stats|stats_reset)|db2_memory_negation_\w+|db2_memory_list_prior_in_session|db2_memory_coref_audit_insert)\b(?=\s*\()|"
    r"\bdb2_memory_(?:lookup_primary_entity|lookup_time_bounds|relations_delete_for_memory|episodes_delete_for_memory)\b(?=\s*\()|"
    r"\bmemory_(?:refresh_(?:aliases|chunks|event_frames|summaries|entities|unit_embeddings|units_graph|episode_relations)|alias_insert|entity_insert|temporal_insert)\b(?=\s*\()|"
    r"\b(?:db2_kb_service_memory_find_facts_json|db2_memory_summaries_list)\b(?=\s*\()|"
    r"\b(?:memory_get(?:_as_of_result|_result)?|db2_memory_get|db2_memory_provenance_by_id|memory_run_maintenance)\b(?=\s*\()|"
    r"\bmemory_(?:apply_feedback|upsert_workflow|supersede)\b(?=\s*\()|"
    r"\b(?:memory_fold_session|kb_client_memory_fold_session|db2_kb_service_memory_fold_session_json)\b(?=\s*\()|"
    r"\b(?:db2_memory_scene\w+|db2_kb_service_scene_\w+|kb_client_memory_scene_\w+)\b(?=\s*\()|"
    r"\bkb_client_memory_directive_\w+\b(?=\s*\()|"
    r"\b(?:db2_directive_\w+|memory_directive_(?:to_json|from_json))\b(?=\s*\()|"
    r"\b(?:kb_client_memory_verify_json|db2_kb_service_collect_memory_verify|db2_kb_service_collect_verify_snapshot|pgvec_verify_snapshot(?:_cleanup)?|pgvec_schema_version)\b(?=\s*\()|"
    r"\b(?:pgvec_memory_vector_search_record_type|pgvec_kb_service_search_memory_points)\b(?=\s*\()|"
    r"\b(?:(?:kb_client_)?memory_episode_card_(?:generate|parse)|memory_episode_cards_query)\b(?=\s*\()|"
    r"\b(?:memory_repair_vector_index(?:_failed_only)?|db2_kb_service_reset_stuck_vector_ops|db2_kb_service_list_memory_ids_by_updated)\b(?=\s*\()|"
    r"\b(?:kb_client_memory_ask|cmd_memory_ask|memory_answer_evidence_(?:decision|reason)_str)\b(?=\s*\()|"
    r"\b(?:memory_rebuild_derived_indexes|memory_rebuild_vector_index_for_version)\b(?=\s*\()|"
    r"\b(?:memory_fusion_state_is_on|memory_recall_metrics|memory_maintenance_(?:summary_to_json|last_summary|metrics))\b(?=\s*\()|"
    r"\b(?:memory_ask_query(?:_scoped)?|memory_answer_query(?:_scoped)?|memory_explain_match|db2_kb_service_memory_ask_json)\b(?=\s*\()|"
    r"\b(?:memory_embed_texts?|memory_embed_serving_id|memory_embed_command_is_http|memory_embedder_last_result_unauthorized)\b(?=\s*\()|"
    r"\bmemory_profile_pack_\w+\b|"
    r"\b(?:memory_recall_trace_\w+|memory_recall_rejection_t|memory_diagnose_scoped|db2_kb_service_memory_diagnose_scoped_json|db2_kb_service_memory_explain_match_json)\b(?=\s*\()|"
    r"\b(?:anti_pattern_extract_from_feedback|anti_pattern_extract_from_failures|anti_pattern_escalate|memory_learn_style|memory_scan_conversations)\b(?=\s*\()|"
    r"\bdb2_kb_service_(?:anti_pattern_extract_from_feedback|anti_pattern_extract_from_failures|anti_pattern_escalate|memory_learn_style|memory_scan_conversations)_json\b|"
    r"\b(?:memory_fact_gate_check|memory_fact_gate_register_checker|"
    r"memory_fact_gate_checker_fn|memory_extract_patterns|memory_pattern_scan_turn|"
    r"memory_extract_register_extractor|memory_extract_register_turn_scanner|"
    r"memory_pattern_extractor_fn|memory_pattern_turn_scanner_fn|"
    r"pattern_triple_t|memory_pattern_turn_t|assemble_texts_near_duplicate|"
    r"memory_pii_(?:register_\w+|turn_requests_sensitive|rel_sensitivity(?:_batch)?|should_inject)|"
    r"memory_ontology_(?:validate|rules|rule_t)|memory_update_content_as|memory_delete_as|"
    r"db2_memory_(?:workspace_tag_insert|search_facts_patterns_by_keyword)|db2_kb_service_relations_schema_list_json|"
    r"gate_check_sensitive|gate_check_ephemeral|gate_has_evidence_markers|memory_scan_content|"
    r"platform_memory_background_embed(?:_set_suppressed)?|"
    r"assemble_token_bits|context_recency_from_age_days|context_apply_recency|"
    r"context_xml_tag_for_header|memory_recall_activated|db2_kb_service_memory_recall_json|"
    r"db2_kb_service_memory_prospective_\w+|"
    r"db2_memory_export_(?:alloc_all|row_free|row_t)|db2_memory_decisions_export_jsonl|memory_search(?=\s*\()|"
    r"db2_kb_service_memory_insert(?:_ex|_epistemic_ex)?_json|"
    r"db2_kb_service_memory_(?:delete|update|touch|reject|restore|upsert_workflow|supersede)_json|"
    r"db2_kb_service_memory_(?:briefing|alerts|assemble_context|compact_windows|query_edges|check_drift|episode_card_generate|export_jsonl|decisions_export_jsonl|search)_json|"
    r"db2_kb_service_memory_(?:find_facts_visible|find_facts_scoped|get|list|fact_history|top_l2_facts|load_eval_corpus|list_session_scope_priority|list_session_scope_priority_like|search_facts_patterns_by_keyword|scope_visibility_rank|tag_workspace|tag_scope)_json|"
    r"db2_kb_service_memory_(?:key_exists|find_id_by_key_kind|list_low_effectiveness|list_unused_l2|list_superseded_keys|review_list|set_artifact|effectiveness_stats)_json|"
    r"db2_kb_service_memory_(?:lint|maintenance_run|entity_profile|entity_edges|search_graph|search_graph_as_of|get_episode|get_provenance|link_query|link_create|link_delete|list_conflicts|query_health|stats)_json|"
    r"db2_kb_service_directive_(?:create|resolve|suppress|sweep_expired|list_json)|"
    r"memory_activation_(?:t|row_t|load|last_loaded|last_turn|in_cooldown|is_sticky|is_delayed|record))\b"
)

FORBIDDEN_STORE_CALLS = (
    "db2_conn(",
    "aimee_pg_",
)

EXTERNAL_CONNECTION_C: set[str] = set()

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
