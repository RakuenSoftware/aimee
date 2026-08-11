/* test_config_field_eligibility.c: pins the flat-vs-non-flat eligibility inventory
 * for the config_fields[] descriptor table (Proposal A, step 0 —
 * docs/proposals/done/config-field-descriptor-table.md).
 *
 * "flat" = a top-level YAML scalar whose config_load parse is a PURE type-guarded
 * assignment (no value guard, coercion, enum mapping, cross-field derivation, or
 * computed default) — i.e. safe to drive from offset/size/type alone. Everything
 * else is "non-flat" and keeps its bespoke parser. The classification is
 * conservative: a field is FLAT only when its parse was verified pure; anything
 * unverified (guarded, enum, section-parsed, or set-only) is NON-flat, which is
 * always the safe direction (a non-flat field merely keeps today's handler).
 *
 * This test enforces the inventory stays EXHAUSTIVE against the live
 * config_fields[] table: adding, removing, or renaming any descriptor fails the
 * build until it is classified here. It is the source-of-truth the later
 * table-driven parse/save (Proposal A) and the generated accessors
 * (Proposal B, config-invariable-accessor-surface.md) key off. Zero runtime
 * behavior change — this table is referenced only by this test.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config_fields.h"

typedef struct
{
   const char *key;
   int flat; /* 1 = table-drivable pure typed scalar; 0 = keeps bespoke handler */
} elig_row_t;

/* THE INVENTORY. One row per config_fields[] descriptor. Keep sorted-as-declared. */
static const elig_row_t g_eligibility[] = {
    {"db2_url", 1},
    {"provider", 1},
    {"default_persona", 1},
    {"claude_model", 1},
    {"openai_endpoint", 1},
    {"openai_model", 1},
    {"openai_key_cmd", 1},
    {"guardrail_mode", 1},
    {"embedder_command", 1},
    {"embedder_model", 1},
    {"embedder_url", 1},
    {"embedder_api_key", 1},
    {"embedder_dims", 0},
    {"kb_mode", 0},
    {"aimee_with_llamacpp", 1},
    {"aimee_synthesis_model", 1},
    {"kb_client_url", 1},
    {"kb_client_bearer_token", 1},
    {"synthesis_endpoint", 0},
    {"synthesis_model", 0},
    {"synthesis_api_key", 1},
    {"synthesis_thinking", 1},
    {"memory_coref_mode", 0},
    {"memory_coref_window", 0},
    {"memory_rerank_mode", 1},
    {"ingress_preinject_enabled", 1},
    {"code_context_mode", 1},
    {"ingress_preinject_anthropic_enabled", 1},
    {"ingress_compress_enabled", 1},
    {"ingress_cache_placement_enabled", 0},
    {"ingress_compress_min_chars", 0},
    {"delegates_enabled", 0},
    {"prompt_manager_block_enabled", 0},
    {"prompt_manager_review_enabled", 0},
    {"gateway_prevent_subagents", 1},
    {"gateway_pin_model", 1},
    {"ingress_preinject_assembly_budget", 0},
    {"ingress_max_raw_scans", 0},
    {"code_span_max_lines", 0},
    {"tool_output_max_bytes", 0},
    {"require_session_worktree", 1},
    {"session_worktree_base", 0},
    {"require_aimee_memory", 1},
    {"require_aimee_git", 1},
    {"subagent_ban_enabled", 1},
    {"delegate_sandbox_package_access", 0},
    {"delegate_sandbox_require_isolation", 1},
    {"delegate_sandbox_learn_packages", 1},
    {"typed_facts_enabled", 1},
    {"kb_pdf_tier", 0},
    {"kb_pdf_ingest_enabled", 1},
    {"kb_pdf_vector_enabled", 1},
    {"kb_pdf_tsr_enabled", 1},
    {"tsr_command", 1},
    {"kb_pdf_assets_enabled", 1},
    {"kb_pdf_blob_dir", 1},
    {"kb_pdf_blob_recon_secs", 1},
    {"kb_pdf_blob_orphan_alarm_mb", 1},
    {"kb_pdf_ocr_enabled", 1},
    {"ocr_command", 1},
    {"css_style_graph_enabled", 1},
    {"code_cochange_git_enabled", 1},
    {"wfe_live_forge_enabled", 1},
    {"wfe_proposals_autoscan_enabled", 1},
    {"client_integrations_enabled", 1},
    {"audit_action_enabled", 1},
    {"audit_worm_enabled", 1},
    {"css_render_command", 1},
    {"vault.custody", 0},
    {"vault.tpm2.blob_path", 0},
    {"vault.tpm2.tcti", 0},
    {"vault.tpm2.nv_index", 0},
    {"kb_evidence_emit_enabled", 1},
    {"fidelity_check_enabled", 1},
    {"memory_query_expansion_mode", 0},
    {"memory_query_expansion_k", 0},
    {"kb_fusion_mode", 0},
    {"kb_fusion_static_alpha", 0},
    {"autonomous", 1},
    {"cross_verify", 1},
    {"verify_cmd", 1},
    {"verify_role", 1},
    {"verify_prompt", 1},
    {"max_iterations", 1},
    {"max_iterations_delegate", 1},
    {"memory_maintenance_trigger_inserts", 0},
    {"memory_maintenance_trigger_secs", 0},
    {"memory_improve_dedupe_enabled", 0},
    {"memory_improve_summarise_enabled", 0},
    {"memory_profile_cards_enabled", 0},
    {"memory_profile_cards_min_obs", 0},
    {"memory_profile_cards_stale_secs", 0},
    {"memory_rewrite_enabled", 0},
    {"memory_rewrite_command", 0},
    {"memory_rewrite_hyde", 0},
    {"memory_rewrite_decompose", 0},
    {"memory_rewrite_max_subqueries", 0},
    {"memory_window_radius", 0},
    {"kb_search_max_results", 0},
    {"memory_negation_enabled", 0},
    {"memory_scenes_enabled", 0},
    {"memory_bm25_weight", 0},
    {"code_hybrid_weight_code", 0},
    {"code_hybrid_weight_graph", 0},
    {"code_hybrid_weight_vector", 0},
    {"code_hybrid_weight_memory", 0},
    {"code_hybrid_rrf_k", 0},
    {"code_trust_actuation_enabled", 0},
    {"code_surprising_precision_floor", 0},
    {"memory_semantic_weight", 0},
    {"memory_semantic_floor_scale", 0},
    {"memory_fetch_budget_enabled", 0},
    {"memory_fetch_budget_base", 0},
    {"memory_fetch_budget_shape_aware", 0},
    {"memory_abstain_enabled", 0},
    {"memory_abstain_gate", 0},
    {"memory_chunk_min_confidence", 0},
    {"memory_hard_negative_log", 0},
    {"dogfood_enabled", 0},
    {"dogfood_log_dir", 0},
    {"dogfood_commit_raw", 0},
    {"dogfood_inline_tagging", 0},
    {"dogfood_autolabel_repair", 0},
    {"dogfood_autolabel_continuation", 0},
    {"dogfood_autolabel_repeat_question", 0},
    {"learning_router_enabled", 0},
    {"learning_proposal_ttl_days", 0},
    {"learning_max_commits_per_week", 0},
    {"learning_implicit_citation_repair", 0},
    {"learning_implicit_citation_continuation", 0},
    {"learning_implicit_repeat_question", 0},
    {"learning_implicit_repeated_correction", 0},
    {"learning_implicit_workflow_repetition", 0},
    {"learning_implicit_retrieval_outcome", 0},
    {"identity_working_profile_injection_enabled", 0},
    {"integrity_enabled", 0},
    {"integrity_dry_run", 0},
    {"virtual_context_enabled", 0},
    {"virtual_context_assembly_budget", 0},
    {"cache_aware_rewrite_enabled", 0},
    {"transport.kb_pool_enabled", 0},
    {"transport.server_keepalive_enabled", 0},
    {"transport.thinclient_gzip_enabled", 0},
    {"transport.kb_gzip_enabled", 0},
    {"cost_reward_enabled", 0},
    {"cost_reward_lambda_pct", 0},
    {"cost_reward_ref_usd_milli", 0},
    {"reasoning_cap_enabled", 0},
    {"dedup_enabled", 0},
    {"cache_shaping_enabled", 0},
    /* Nested under an "extended_thinking" object, so bespoke like cache_shaping
     * above rather than a table-drivable flat scalar. The companion
     * budget_tokens key is gone: the shape it fed was removed by Anthropic. */
    {"extended_thinking_enabled", 0},
    {"ingress_usage_accounting_enabled", 0},
    {"ingress_audit_async", 0},
    {"ingress_trusted_proxy_secret", 0},
    {"dedup_window_seconds", 0},
    {"cache_min_chars", 0},
    {"guardrails_semantic_mode", 0},
    {"guardrails_blast_radius_advisory_enabled", 0},
    {"guardrails_semantic_command", 0},
    {"guardrails_semantic_warn_threshold", 0},
    {"guardrails_semantic_prompt_threshold", 0},
    {"guardrails_semantic_block_threshold", 0},
    {"kb_api_http_port", 0},
    {"kb_api_bearer_token", 0},
    {"telemetry.metrics_token", 0},
    {"kb_mining_enabled", 0},
    {"kb_mining_min_poll_s", 0},
    {"verify_enabled", 1},
    {"delegate_graph_context_enabled", 1},
    {"roundtable.replay_verify_enabled", 0},
    {"roundtable.chair_synthesis", 0},
    {"roundtable.require_evidence", 0},
    {"verify_cross_project", 1},
    {"roundtable.max_rounds", 0},
    {"roundtable.converge_threshold", 0},
    {"roundtable.deadline_ms", 0},
    {"roundtable.turns", 0},
    {"roundtable.default", 0},
    {"roundtable.pipeline_done_bar", 0},
    {"roundtable.pipeline_max_passes", 0},
    {"roundtable.pipeline_max_attempts_per_pass", 0},
    {"roundtable.pipeline_max_cost_usd", 0},
    {"roundtable.pipeline_max_total_cost_usd", 0},
    {"roundtable.pipeline_gate_ttl_h", 0},
    {"roundtable.pipeline_parked_releases_slot", 0},
    {"roundtable.pipeline_unknown_context_tokens", 0},
    {"trigger.max_concurrent", 0},
    {"economizer.mode", 0},
    {"autonomy.skeptics", 0},
    {"autonomy.fanout", 0},
    {"autonomy.unit_retry", 0},
    {"autonomy.unit_max", 0},
    {"autonomy.ci_retry_max", 0},
    {"autonomy.max_turns", 0},
    {"autonomy.max_wall_secs", 0},
    {"autonomy.stale_abandon_secs", 0},
    {"autonomy.concurrency", 0},
    {"autonomy.auto_resume_cap_parks", 0},
    {"autonomy.max_resumes", 0},
    {"kb_curator_tier", 0},
    {"kb_curator_extract_docs_enabled", 0},
    {"kb_curator_extract_docs_workers", 0},
    {"kb_curator_stage_order", 0},
    {"kb_curator_user_presets", 0},
    {"kb_curator_custom_stages", 0},
    {"kb_curator_extract_code_enabled", 0},
    {"kb_curator_extract_code_workers", 0},
    {"kb_curator_resolve_entities_enabled", 0},
    {"kb_curator_index_narrative_enabled", 0},
    {"kb_curator_index_claims_enabled", 0},
    {"kb_curator_detect_contradictions_enabled", 0},
    {"kb_curator_index_code_unit_enabled", 0},
    {"kb_curator_link_artifacts_enabled", 0},
    {"kb_curator_projection_graph_enabled", 0},
    {"kb_curator_synthesize_enabled", 0},
    {"kb_curator_promote_entity_enabled", 0},
    {"kb_curator_cross_repo_graph_enabled", 0},
    {"kb_evidence_embed_enabled", 0},
};
static const int g_eligibility_n = (int)(sizeof(g_eligibility) / sizeof(g_eligibility[0]));

static const elig_row_t *elig_lookup(const char *key)
{
   for (int i = 0; i < g_eligibility_n; i++)
      if (strcmp(g_eligibility[i].key, key) == 0)
         return &g_eligibility[i];
   return NULL;
}

int main(void)
{
   int runtime_n = 0, flat = 0, nonflat = 0, missing = 0, stale = 0, bad_flat = 0;

   /* 1) Exhaustiveness: every live config_fields[] descriptor is classified. */
   for (const config_field_t *f = config_fields; f->key; f++)
   {
      runtime_n++;
      const elig_row_t *e = elig_lookup(f->key);
      if (!e)
      {
         missing++;
         fprintf(stderr, "  UNCLASSIFIED config_fields[] key: \"%s\" — add it to the inventory\n",
                 f->key);
         continue;
      }
      if (e->flat)
      {
         flat++;
         /* 2) A FLAT field must be a real scalar type, never the enum-mapped one. */
         if (f->type == CFG_ECON_MODE)
         {
            bad_flat++;
            fprintf(stderr, "  FLAT key \"%s\" is CFG_ECON_MODE (enum-mapped) — must be non-flat\n",
                    f->key);
         }
      }
      else
         nonflat++;
   }

   /* 3) No stale inventory rows: every classified key still exists in the table. */
   for (int i = 0; i < g_eligibility_n; i++)
   {
      if (!config_field_lookup(g_eligibility[i].key))
      {
         stale++;
         fprintf(stderr, "  STALE inventory key: \"%s\" — no longer in config_fields[]\n",
                 g_eligibility[i].key);
      }
   }

   printf("config_fields eligibility: runtime=%d classified=%d flat=%d non-flat=%d\n", runtime_n,
          g_eligibility_n, flat, nonflat);
   assert(missing == 0 && "every config_fields[] descriptor must be classified");
   assert(stale == 0 && "inventory must not reference removed descriptors");
   assert(bad_flat == 0 && "CFG_ECON_MODE fields cannot be flat");
   assert(runtime_n == g_eligibility_n && "inventory and config_fields[] must be 1:1");
   printf("  PASS: config_fields eligibility inventory is exhaustive and consistent\n");
   return 0;
}
