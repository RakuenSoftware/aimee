/* test_config_surface.c: characterization net for config_load's parse surface.
 * AUTO-DERIVED from config.c. Two YAML fixtures (A/B) set every config_load-parsed
 * field to distinct, in-range values; asserting cfgA != cfgB proves each field is
 * read from aimee.yaml, pinning the surface so config_load can be split into
 * config_parse_* section helpers without silently dropping a field. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "aimee.h"
#include "platform_path.h"
#include "platform_test_util.h"

static void write_cfg(const char *home, const char *yaml)
{
   char p1[600], dir[600], path[700];
   snprintf(p1, sizeof(p1), "%s/.config", home);
   mkdir(p1, 0755);
   snprintf(dir, sizeof(dir), "%s/.config/aimee", home);
   mkdir(dir, 0755);
   snprintf(path, sizeof(path), "%s/aimee.yaml", dir);
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(yaml, f);
   fclose(f);
}

static const char *FIXTURE_A =
    "db1_path: ZZA_val\nguardrail_mode: ZZA_val\nprovider: ZZA_val\nopenai_endpoint: "
    "ZZA_val\nopenai_model: ZZA_val\nopenai_key_cmd: ZZA_val\nclaude_model: ZZA_val\ncodex_model: "
    "ZZA_val\nautonomous: true\nverify_enabled: true\nverify_cross_project: "
    "true\ningress_preinject_enabled: true\ncode_context_mode: "
    "on\ningress_preinject_anthropic_enabled: "
    "true\ningress_compress_enabled: "
    "true\ningress_cache_placement_enabled: true\ngateway_prevent_subagents: "
    "true\ncss_style_graph_enabled: "
    "true\n"
    "ingress_preinject_assembly_budget: 1\n"
    "code_span_max_lines: 7\n"
    "tool_output_max_bytes: 4096\n"
    "ingress_max_raw_scans: 3\nembedder_command: ZZA_val\nembedder_model: "
    "ZZA_val\nembedder_url: ZZA_val\nembedder_dims: 1\nmemory_rerank_mode: "
    "ZZA_val\nmemory_rewrite:\n  enabled: true\n  hyde: true\n  decompose: true\n  max_subqueries: "
    "1\nmemory_negation:\n  enabled: true\n"
    "memory_query_expansion:\n  k: 1\nmemory_recall_lanes:\n  enabled: true\n  k_summary: "
    "1\n  k_fact: 1\n  floor_summary: 3\n  floor_fact: 3\nmemory_window:\n  radius: 3\n  "
    "kb_neighbour_expand: true\nkb:\n  search_max_results: 1\n  api:\n    http_port: 3\n  "
    "background_ingest:\n    enabled: true\n    interval_hours: 3\n    watch_enabled: true\n    "
    "watch_debounce_secs: 3\n  mining:\n    enabled: true\n    min_poll_s: "
    "1\nmemory_maintenance:\n  trigger_inserts: 3\n  trigger_secs: 3\n  enabled: true\n  "
    "interval_seconds: 3\n  summarize_enabled: true\nmemory:\n  salience:\n    enabled: true\n    "
    "weight: 0.01\n    window_size: 1\n    surprise_enabled: true\n    surprise_weight: 0.01\n  "
    "pagerank:\n    enabled: true\n    iterations: 1\n    weight: 0.01\n  coref:\n    window: 1\n  "
    "citations:\n    reprompt_on_miss: true\n    strip_unverified: true\n  cognify:\n    async:\n  "
    "    enabled: true\n    enabled: true\n  improve:\n    dedupe_enabled: true\n    "
    "summarise_enabled: true\n    min_cluster: 1\n    max_confidence: 0.01\n  scenes:\n    "
    "enabled: true\n    min_cluster_size: 1\n    top_m: 1\n    global_escape_ratio: 0.01\n  "
    "episode_summaries:\n    enabled: true\n  derive_facts:\n    enabled: true\n  "
    "failure_detection:\n    enabled: true\n    threshold: 0.01\n  abstain:\n    enabled: "
    "true\n    gate: 0.01\n    chunk_min_confidence: 0.01\n  "
    "profile_cards:\n    enabled: "
    "true\n    min_observations: 1\n    stale_secs: 1\n  context_budget:\n    enabled: true\n    "
    "tokens: 1\n  routing:\n    enabled: true\n  bm25_weight: 0.01\n  semantic_weight: 0.01\n  "
    "fetch_budget:\n    enabled: true\n    base: 32\n    shape_aware: true\ncross_verify:\n  "
    "enabled: true\n  verify_cmd: ZZA_val\n  role: ZZA_val\n  prompt: ZZA_val\nretry:\n  "
    "max_attempts: 3\n  base_ms: 3\n  max_ms: 3\nmax_iterations: 3\nmax_iterations_delegate: "
    "3\nmax_delegation_depth: 3\nmax_delegation_spawns: 3\nmax_background_processes: "
    "1\nworker_threads: 1\nsession_threads: 1\nconcurrency:\n  default: 1\n  preempt:\n    "
    "enabled: true\n    single_slot_only: true\n    requeue_max: 3\nsearch:\n  backend: ZZA_val\n  "
    "max_results: 1\ndogfood:\n  enabled: true\n  log_dir: ZZA_val\n  commit_raw: true\n  "
    "inline_tagging: true\nidentity:\n  working_profile_injection:\n    enabled: true\ncompact:\n  "
    "enabled: true\n  threshold: 1\n  head_bytes: 1\n  tail_bytes: 1\nsessions:\n  "
    "stale_threshold_secs: 1\n  max_sessions: 1\n  max_worktrees: 1\nprompt_tier: "
    "ZZA_val\nprompt_file: ZZA_val\nmcp:\n  osv:\n    enabled: true\n    offline: "
    "true\n    enforce: true\n    endpoint: ZZA_val\nrewind:\n  auto_snapshot: true\notel:\n  "
    "endpoint: ZZA_val\n  service_name: ZZA_val\nintegrity:\n  enabled: true\n  dry_run: "
    "true\nsession:\n  virtual_context:\n    enabled: true\n    assembly_budget: 1\ntransport:\n  "
    "cache_aware_rewrite:\n    enabled: true\n    min_savings_tokens: 3\n    "
    "hard_context_threshold: 1.0\n    max_defer_turns: 3\n    segment_check_turns: "
    "3\nguardrails:\n  semantic:\n    enabled: true\n    dry_run: true\n    advisory_only: true\n  "
    "  warn_threshold: 0.01\n    prompt_threshold: 0.01\n    block_threshold: 0.01\n    "
    "allow_ml_only_block: true\nauxiliary:\n  enabled: true\n  default_model: ZZA_val\n  "
    "default_max_tokens: 1\nmodel_meta:\n  refresh_minutes: 1\n  capability_routing: "
    "true\nensemble:\n  enabled: true\n  min_successful: 1\n  max_cost_usd: 1.0\n"
    "modules:\n  memory: true\n  governance: true\n  delegates: true\n  workflows: true\n  "
    "roundtable: true\n  "
    "economizer: true";
static const char *FIXTURE_B =
    "db1_path: ZZB_val\nguardrail_mode: ZZB_val\nprovider: ZZB_val\nopenai_endpoint: "
    "ZZB_val\nopenai_model: ZZB_val\nopenai_key_cmd: ZZB_val\nclaude_model: ZZB_val\ncodex_model: "
    "ZZB_val\nautonomous: false\nverify_enabled: false\nverify_cross_project: "
    "false\ningress_preinject_enabled: false\ncode_context_mode: "
    "off\ningress_preinject_anthropic_enabled: "
    "false\ningress_compress_enabled: "
    "false\ningress_cache_placement_enabled: false\ngateway_prevent_subagents: "
    "false\ncss_style_graph_enabled: "
    "false\n"
    "ingress_preinject_assembly_budget: 4096\n"
    "code_span_max_lines: 4096\n"
    "tool_output_max_bytes: 8192\n"
    "ingress_max_raw_scans: 4096\nembedder_command: ZZB_val\nembedder_model: "
    "ZZB_val\nembedder_url: ZZB_val\nembedder_dims: 4096\nmemory_rerank_mode: "
    "ZZB_val\nmemory_rewrite:\n  enabled: false\n  hyde: false\n  decompose: false\n  "
    "max_subqueries: 4096\nmemory_negation:\n  enabled: false\n"
    "memory_query_expansion:\n  k: 4096\nmemory_recall_lanes:\n  "
    "enabled: false\n  k_summary: 4096\n  k_fact: 4096\n  floor_summary: 4096\n  floor_fact: "
    "4096\nmemory_window:\n  radius: 4096\n  kb_neighbour_expand: false\nkb:\n  "
    "search_max_results: 4096\n  api:\n    http_port: 4096\n  background_ingest:\n    enabled: "
    "false\n    interval_hours: 4096\n    watch_enabled: false\n    watch_debounce_secs: 4096\n  "
    "mining:\n    enabled: false\n    min_poll_s: 4096\nmemory_maintenance:\n  trigger_inserts: "
    "4096\n  trigger_secs: 4096\n  enabled: false\n  interval_seconds: 4096\n  summarize_enabled: "
    "false\nmemory:\n  salience:\n    enabled: false\n    weight: 0.99\n    window_size: 4096\n    "
    "surprise_enabled: false\n    surprise_weight: 0.99\n  pagerank:\n    enabled: false\n    "
    "iterations: 4096\n    weight: 0.99\n  coref:\n    window: 4096\n  citations:\n    "
    "reprompt_on_miss: false\n    strip_unverified: false\n  cognify:\n    async:\n      enabled: "
    "false\n    enabled: false\n  improve:\n    dedupe_enabled: false\n    summarise_enabled: "
    "false\n    min_cluster: 4096\n    max_confidence: 0.99\n  scenes:\n    enabled: false\n    "
    "min_cluster_size: 4096\n    top_m: 4096\n    global_escape_ratio: 0.99\n  "
    "episode_summaries:\n    enabled: false\n  derive_facts:\n    enabled: false\n  "
    "failure_detection:\n    enabled: false\n    threshold: 0.99\n  abstain:\n    enabled: "
    "false\n    gate: 0.99\n    chunk_min_confidence: 0.99\n  "
    "profile_cards:\n    enabled: "
    "false\n    min_observations: 4096\n    stale_secs: 4096\n  context_budget:\n    enabled: "
    "false\n    tokens: 4096\n  routing:\n    enabled: false\n  bm25_weight: 0.99\n  "
    "semantic_weight: 0.99\n  fetch_budget:\n    enabled: false\n    base: 512\n    shape_aware: "
    "false\ncross_verify:\n  enabled: false\n  verify_cmd: ZZB_val\n  role: ZZB_val\n  prompt: "
    "ZZB_val\nretry:\n  max_attempts: 4096\n  base_ms: 4096\n  max_ms: 4096\nmax_iterations: "
    "4096\nmax_iterations_delegate: 4096\nmax_delegation_depth: 4096\nmax_delegation_spawns: "
    "4096\nmax_background_processes: 4096\nworker_threads: 4096\nsession_threads: "
    "4096\nconcurrency:\n  default: 4096\n  preempt:\n    enabled: false\n    single_slot_only: "
    "false\n    requeue_max: 4096\nsearch:\n  backend: ZZB_val\n  max_results: 4096\ndogfood:\n  "
    "enabled: false\n  log_dir: ZZB_val\n  commit_raw: false\n  inline_tagging: false\nidentity:\n "
    " working_profile_injection:\n    enabled: false\ncompact:\n  enabled: false\n  threshold: "
    "4096\n  head_bytes: 4096\n  tail_bytes: 4096\nsessions:\n  stale_threshold_secs: 4096\n  "
    "max_sessions: 4096\n  max_worktrees: 4096\nprompt_tier: ZZB_val\nprompt_file: "
    "ZZB_val\nmcp:\n  osv:\n    enabled: false\n    offline: false\n    enforce: "
    "false\n    endpoint: ZZB_val\nrewind:\n  auto_snapshot: false\notel:\n  endpoint: ZZB_val\n  "
    "service_name: ZZB_val\nintegrity:\n  enabled: false\n  dry_run: false\nsession:\n  "
    "virtual_context:\n    enabled: false\n    assembly_budget: 4096\ntransport:\n  "
    "cache_aware_rewrite:\n    enabled: false\n    min_savings_tokens: 4096\n    "
    "hard_context_threshold: 0.99\n    max_defer_turns: 4096\n    segment_check_turns: "
    "4096\nguardrails:\n  semantic:\n    enabled: false\n    dry_run: false\n    advisory_only: "
    "false\n    warn_threshold: 0.99\n    prompt_threshold: 0.99\n    block_threshold: 0.99\n    "
    "allow_ml_only_block: false\nauxiliary:\n  enabled: false\n  default_model: ZZB_val\n  "
    "default_max_tokens: 4096\nmodel_meta:\n  refresh_minutes: 4096\n  capability_routing: "
    "false\nensemble:\n  enabled: false\n  min_successful: 4096\n  max_cost_usd: 0.99\n"
    "modules:\n  memory: false\n  governance: false\n  delegates: false\n  workflows: false\n  "
    "roundtable: false\n  "
    "economizer: false";

int main(void)
{
   printf("config_surface: ");
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-cfgsurf-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   platform_setenv("HOME", tmpdir);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");

   static config_t cfgA, cfgB;
   write_cfg(tmpdir, FIXTURE_A);
   memset(&cfgA, 0, sizeof(cfgA));
   config_load(&cfgA);
   write_cfg(tmpdir, FIXTURE_B);
   memset(&cfgB, 0, sizeof(cfgB));
   config_load(&cfgB);

   assert(strcmp(cfgA.db1_path, cfgB.db1_path) != 0);
   assert(strcmp(cfgA.guardrail_mode, cfgB.guardrail_mode) != 0);
   assert(strcmp(cfgA.provider, cfgB.provider) != 0);
   assert(strcmp(cfgA.openai_endpoint, cfgB.openai_endpoint) != 0);
   assert(strcmp(cfgA.openai_model, cfgB.openai_model) != 0);
   assert(strcmp(cfgA.openai_key_cmd, cfgB.openai_key_cmd) != 0);
   assert(strcmp(cfgA.claude_model, cfgB.claude_model) != 0);
   assert(strcmp(cfgA.codex_model, cfgB.codex_model) != 0);
   assert(cfgA.autonomous == 1 && cfgB.autonomous == 0);
   assert(cfgA.verify_enabled == 1 && cfgB.verify_enabled == 0);
   assert(cfgA.verify_cross_project == 1 && cfgB.verify_cross_project == 0);
   assert(cfgA.ingress_preinject_enabled == 1 && cfgB.ingress_preinject_enabled == 0);
   assert(strcmp(cfgA.code_context_mode, "on") == 0);
   assert(strcmp(cfgB.code_context_mode, "off") == 0);
   assert(cfgA.ingress_preinject_anthropic_enabled == 1 &&
          cfgB.ingress_preinject_anthropic_enabled == 0);
   assert(cfgA.ingress_compress_enabled == 1 && cfgB.ingress_compress_enabled == 0);
   assert(cfgA.ingress_cache_placement_enabled == 1 && cfgB.ingress_cache_placement_enabled == 0);
   assert(cfgA.gateway_prevent_subagents == 1 && cfgB.gateway_prevent_subagents == 0);
   assert(cfgA.css_style_graph_enabled == 1 && cfgB.css_style_graph_enabled == 0);
   assert(cfgA.ingress_preinject_assembly_budget != cfgB.ingress_preinject_assembly_budget);
   assert(cfgA.code_span_max_lines != cfgB.code_span_max_lines);
   assert(cfgA.tool_output_max_bytes != cfgB.tool_output_max_bytes);
   assert(cfgA.ingress_max_raw_scans != cfgB.ingress_max_raw_scans);
   assert(strcmp(cfgA.embedder_command, cfgB.embedder_command) != 0);
   assert(strcmp(cfgA.embedder_model, cfgB.embedder_model) != 0);
   assert(strcmp(cfgA.embedder_url, cfgB.embedder_url) != 0);
   assert(cfgA.embedder_dims != cfgB.embedder_dims);
   assert(strcmp(cfgA.memory_rerank_mode, cfgB.memory_rerank_mode) != 0);
   assert(cfgA.memory_rewrite_enabled == 1 && cfgB.memory_rewrite_enabled == 0);
   assert(cfgA.memory_rewrite_hyde == 1 && cfgB.memory_rewrite_hyde == 0);
   assert(cfgA.memory_rewrite_decompose == 1 && cfgB.memory_rewrite_decompose == 0);
   assert(cfgA.memory_rewrite_max_subqueries != cfgB.memory_rewrite_max_subqueries);
   assert(cfgA.memory_negation_enabled == 1 && cfgB.memory_negation_enabled == 0);
   assert(cfgA.memory_query_expansion_k != cfgB.memory_query_expansion_k);
   assert(cfgA.memory_recall_lanes_enabled == 1 && cfgB.memory_recall_lanes_enabled == 0);
   assert(cfgA.memory_recall_lanes_k_summary != cfgB.memory_recall_lanes_k_summary);
   assert(cfgA.memory_recall_lanes_k_fact != cfgB.memory_recall_lanes_k_fact);
   assert(cfgA.memory_recall_lanes_floor_summary != cfgB.memory_recall_lanes_floor_summary);
   assert(cfgA.memory_recall_lanes_floor_fact != cfgB.memory_recall_lanes_floor_fact);
   assert(cfgA.memory_window_radius != cfgB.memory_window_radius);
   assert(cfgA.kb_search_max_results != cfgB.kb_search_max_results);
   assert(cfgA.memory_maintenance_trigger_inserts != cfgB.memory_maintenance_trigger_inserts);
   assert(cfgA.memory_maintenance_trigger_secs != cfgB.memory_maintenance_trigger_secs);
   assert(cfgA.memory_maintenance_enabled == 1 && cfgB.memory_maintenance_enabled == 0);
   assert(cfgA.memory_maintenance_interval_seconds != cfgB.memory_maintenance_interval_seconds);
   assert(cfgA.memory_maintenance_summarize_enabled == 1 &&
          cfgB.memory_maintenance_summarize_enabled == 0);
   assert(cfgA.memory_salience_enabled == 1 && cfgB.memory_salience_enabled == 0);
   assert(cfgA.memory_salience_weight != cfgB.memory_salience_weight);
   assert(cfgA.memory_salience_window_size != cfgB.memory_salience_window_size);
   assert(cfgA.memory_surprise_enabled == 1 && cfgB.memory_surprise_enabled == 0);
   assert(cfgA.memory_surprise_weight != cfgB.memory_surprise_weight);
   assert(cfgA.memory_pagerank_enabled == 1 && cfgB.memory_pagerank_enabled == 0);
   assert(cfgA.memory_pagerank_iterations != cfgB.memory_pagerank_iterations);
   assert(cfgA.memory_pagerank_weight != cfgB.memory_pagerank_weight);
   assert(cfgA.memory_coref_window != cfgB.memory_coref_window);
   assert(cfgA.memory_citations_reprompt_on_miss == 1 &&
          cfgB.memory_citations_reprompt_on_miss == 0);
   assert(cfgA.memory_citations_strip_unverified == 1 &&
          cfgB.memory_citations_strip_unverified == 0);
   assert(cfgA.memory_cognify_async_enabled == 1 && cfgB.memory_cognify_async_enabled == 0);
   assert(cfgA.memory_cognify_enabled == 1 && cfgB.memory_cognify_enabled == 0);
   assert(cfgA.memory_improve_dedupe_enabled == 1 && cfgB.memory_improve_dedupe_enabled == 0);
   assert(cfgA.memory_improve_summarise_enabled == 1 && cfgB.memory_improve_summarise_enabled == 0);
   assert(cfgA.memory_improve_min_cluster != cfgB.memory_improve_min_cluster);
   assert(cfgA.memory_improve_max_confidence != cfgB.memory_improve_max_confidence);
   assert(cfgA.memory_scenes_enabled == 1 && cfgB.memory_scenes_enabled == 0);
   assert(cfgA.memory_scenes_global_escape_ratio != cfgB.memory_scenes_global_escape_ratio);
   assert(cfgA.memory_episode_summaries_enabled == 1 && cfgB.memory_episode_summaries_enabled == 0);
   assert(cfgA.memory_derive_facts_enabled == 1 && cfgB.memory_derive_facts_enabled == 0);
   assert(cfgA.memory_failure_detection_enabled == 1 && cfgB.memory_failure_detection_enabled == 0);
   assert(cfgA.memory_failure_detection_threshold != cfgB.memory_failure_detection_threshold);
   assert(cfgA.memory_abstain_enabled == 1 && cfgB.memory_abstain_enabled == 0);
   assert(cfgA.memory_abstain_gate != cfgB.memory_abstain_gate);
   assert(cfgA.memory_chunk_min_confidence != cfgB.memory_chunk_min_confidence);
   assert(cfgA.memory_profile_cards_enabled == 1 && cfgB.memory_profile_cards_enabled == 0);
   assert(cfgA.memory_profile_cards_min_obs != cfgB.memory_profile_cards_min_obs);
   assert(cfgA.memory_profile_cards_stale_secs != cfgB.memory_profile_cards_stale_secs);
   assert(cfgA.memory_context_budget_enabled == 1 && cfgB.memory_context_budget_enabled == 0);
   assert(cfgA.memory_context_budget_tokens != cfgB.memory_context_budget_tokens);
   assert(cfgA.memory_routing_enabled == 1 && cfgB.memory_routing_enabled == 0);
   assert(cfgA.memory_bm25_weight != cfgB.memory_bm25_weight);
   assert(cfgA.memory_semantic_weight != cfgB.memory_semantic_weight);
   assert(cfgA.memory_fetch_budget_enabled == 1 && cfgB.memory_fetch_budget_enabled == 0);
   assert(cfgA.memory_fetch_budget_base != cfgB.memory_fetch_budget_base);
   assert(cfgA.memory_fetch_budget_shape_aware == 1 && cfgB.memory_fetch_budget_shape_aware == 0);
   assert(cfgA.cross_verify == 1 && cfgB.cross_verify == 0);
   assert(strcmp(cfgA.verify_cmd, cfgB.verify_cmd) != 0);
   assert(strcmp(cfgA.verify_role, cfgB.verify_role) != 0);
   assert(strcmp(cfgA.verify_prompt, cfgB.verify_prompt) != 0);
   assert(cfgA.retry_max_attempts != cfgB.retry_max_attempts);
   assert(cfgA.retry_base_ms != cfgB.retry_base_ms);
   assert(cfgA.retry_max_ms != cfgB.retry_max_ms);
   assert(cfgA.max_iterations != cfgB.max_iterations);
   assert(cfgA.max_iterations_delegate != cfgB.max_iterations_delegate);
   assert(cfgA.max_delegation_depth != cfgB.max_delegation_depth);
   assert(cfgA.max_delegation_spawns != cfgB.max_delegation_spawns);
   assert(cfgA.max_background_processes != cfgB.max_background_processes);
   assert(cfgA.compute_threads != cfgB.compute_threads);
   assert(cfgA.session_threads != cfgB.session_threads);
   assert(cfgA.concurrency_default != cfgB.concurrency_default);
   assert(cfgA.concurrency_preempt_enabled == 1 && cfgB.concurrency_preempt_enabled == 0);
   assert(cfgA.concurrency_preempt_single_slot_only == 1 &&
          cfgB.concurrency_preempt_single_slot_only == 0);
   assert(cfgA.concurrency_preempt_requeue_max != cfgB.concurrency_preempt_requeue_max);
   assert(strcmp(cfgA.search_backend, cfgB.search_backend) != 0);
   assert(cfgA.search_max_results != cfgB.search_max_results);
   assert(cfgA.dogfood_enabled == 1 && cfgB.dogfood_enabled == 0);
   assert(strcmp(cfgA.dogfood_log_dir, cfgB.dogfood_log_dir) != 0);
   assert(cfgA.dogfood_commit_raw == 1 && cfgB.dogfood_commit_raw == 0);
   assert(cfgA.dogfood_inline_tagging == 1 && cfgB.dogfood_inline_tagging == 0);
   assert(cfgA.identity_working_profile_injection_enabled == 1 &&
          cfgB.identity_working_profile_injection_enabled == 0);
   assert(cfgA.compact_enabled == 1 && cfgB.compact_enabled == 0);
   assert(cfgA.compact_threshold != cfgB.compact_threshold);
   assert(cfgA.compact_head_bytes != cfgB.compact_head_bytes);
   assert(cfgA.compact_tail_bytes != cfgB.compact_tail_bytes);
   assert(cfgA.worktree_stale_secs != cfgB.worktree_stale_secs);
   assert(cfgA.max_sessions != cfgB.max_sessions);
   assert(cfgA.max_worktrees != cfgB.max_worktrees);
   assert(strcmp(cfgA.prompt_tier, cfgB.prompt_tier) != 0);
   assert(strcmp(cfgA.prompt_file, cfgB.prompt_file) != 0);
   assert(cfgA.mcp_osv_enabled == 1 && cfgB.mcp_osv_enabled == 0);
   assert(cfgA.mcp_osv_offline == 1 && cfgB.mcp_osv_offline == 0);
   assert(cfgA.mcp_osv_enforce == 1 && cfgB.mcp_osv_enforce == 0);
   assert(strcmp(cfgA.mcp_osv_endpoint, cfgB.mcp_osv_endpoint) != 0);
   assert(cfgA.rewind_auto_snapshot == 1 && cfgB.rewind_auto_snapshot == 0);
   assert(strcmp(cfgA.otel_endpoint, cfgB.otel_endpoint) != 0);
   assert(strcmp(cfgA.otel_service_name, cfgB.otel_service_name) != 0);
   assert(cfgA.integrity_enabled == 1 && cfgB.integrity_enabled == 0);
   assert(cfgA.integrity_dry_run == 1 && cfgB.integrity_dry_run == 0);
   assert(cfgA.virtual_context_enabled == 1 && cfgB.virtual_context_enabled == 0);
   assert(cfgA.virtual_context_assembly_budget != cfgB.virtual_context_assembly_budget);
   assert(cfgA.cache_aware_rewrite_enabled == 1 && cfgB.cache_aware_rewrite_enabled == 0);
   assert(cfgA.cache_aware_rewrite_min_savings_tokens !=
          cfgB.cache_aware_rewrite_min_savings_tokens);
   assert(cfgA.cache_aware_rewrite_hard_context_threshold !=
          cfgB.cache_aware_rewrite_hard_context_threshold);
   assert(cfgA.cache_aware_rewrite_max_defer_turns != cfgB.cache_aware_rewrite_max_defer_turns);
   assert(cfgA.cache_aware_rewrite_segment_check_turns !=
          cfgB.cache_aware_rewrite_segment_check_turns);
   /* Legacy object form maps onto the mode: A {enabled:true,dry_run:true} -> "dry_run";
    * B {enabled:false} -> "off". Exercises the back-compat parse mapping. */
   assert(strcmp(cfgA.guardrails_semantic_mode, "dry_run") == 0);
   assert(strcmp(cfgB.guardrails_semantic_mode, "off") == 0);
   assert(cfgA.guardrails_semantic_warn_threshold != cfgB.guardrails_semantic_warn_threshold);
   assert(cfgA.guardrails_semantic_prompt_threshold != cfgB.guardrails_semantic_prompt_threshold);
   assert(cfgA.guardrails_semantic_block_threshold != cfgB.guardrails_semantic_block_threshold);
   assert(cfgA.kb_api_http_port != cfgB.kb_api_http_port);
   assert(cfgA.kb_bg_ingest_enabled == 1 && cfgB.kb_bg_ingest_enabled == 0);
   assert(cfgA.kb_bg_ingest_interval_hours != cfgB.kb_bg_ingest_interval_hours);
   assert(cfgA.kb_bg_watch_enabled == 1 && cfgB.kb_bg_watch_enabled == 0);
   assert(cfgA.kb_bg_watch_debounce_secs != cfgB.kb_bg_watch_debounce_secs);
   assert(cfgA.kb_mining_enabled == 1 && cfgB.kb_mining_enabled == 0);
   assert(cfgA.kb_mining_min_poll_s != cfgB.kb_mining_min_poll_s);
   assert(cfgA.aux_enabled == 1 && cfgB.aux_enabled == 0);
   assert(strcmp(cfgA.aux_default_model, cfgB.aux_default_model) != 0);
   assert(cfgA.aux_default_max_tokens != cfgB.aux_default_max_tokens);
   assert(cfgA.model_meta_refresh_minutes != cfgB.model_meta_refresh_minutes);
   assert(cfgA.model_meta_capability_routing == 1 && cfgB.model_meta_capability_routing == 0);
   assert(cfgA.ensemble_min_successful != cfgB.ensemble_min_successful);
   assert(cfgA.ensemble_max_cost_usd != cfgB.ensemble_max_cost_usd);

   /* modules.* pluggable-module toggles (tristate; A=true, B=false, distinct from the -1
    * unspecified default so each key is proven read from the `modules:` block). */
   assert(cfgA.module_memory == 1 && cfgB.module_memory == 0);
   assert(cfgA.module_governance == 1 && cfgB.module_governance == 0);
   assert(cfgA.module_delegates == 1 && cfgB.module_delegates == 0);
   assert(cfgA.module_workflows == 1 && cfgB.module_workflows == 0);
   assert(cfgA.module_roundtable == 1 && cfgB.module_roundtable == 0);
   assert(cfgA.module_economizer == 1 && cfgB.module_economizer == 0);

   /* config_module_enabled precedence: an explicit user tristate (0/1) is canonical and wins
    * over the env default; -1 (unspecified) falls back to the env default. */
   assert(config_module_enabled(1, 0) == 1);  /* config ON overrides env OFF */
   assert(config_module_enabled(0, 1) == 0);  /* config OFF overrides env ON  */
   assert(config_module_enabled(-1, 1) == 1); /* unspecified -> env default ON */
   assert(config_module_enabled(-1, 0) == 0); /* unspecified -> env default OFF */
   assert(config_module_enabled(1, 1) == 1 && config_module_enabled(0, 0) == 0);

   printf("all parsed-field tests passed\n");
   return 0;
}
