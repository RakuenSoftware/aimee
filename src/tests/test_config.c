#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "cJSON.h"
#include "config_fields.h"
#include "config_learning.h"
#include "config_database.h"
#include "config_sections.h"
#include "server.h" /* SERVER_REMOTE_WRITES_* */
#include "aimee_home.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "runtime_secret.h"

void config_kb_curator_defaults(config_t *cfg);
int config_parse_kb_curator(config_t *cfg, const cJSON *root);

static char migrated_db2[256];
static char migrated_api_bearer[256];

static int capture_migrated_secret(const char *name, const char *value)
{
   if (strcmp(name, "AIMEE_DB2_URL") == 0)
      snprintf(migrated_db2, sizeof(migrated_db2), "%s", value);
   else if (strcmp(name, "AIMEE_API_BEARER_TOKEN") == 0)
      snprintf(migrated_api_bearer, sizeof(migrated_api_bearer), "%s", value);
   return 0;
}

static int no_migrated_secret_present(const char *name)
{
   (void)name;
   return 0;
}

/* kb_curator preset: the tier drives the 12 stage gates, an explicit per-stage
 * gate still overrides, and "off" disables everything. Pure (no file I/O). */
static void test_kb_curator_tier(void)
{
   /* default -> "full": every stage on (the historical all-on default). */
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_kb_curator_defaults(&cfg);
   assert(strcmp(cfg.kb_curator_tier, "full") == 0);
   assert(cfg.kb_curator_extract_docs_enabled && cfg.kb_curator_synthesize_enabled &&
          cfg.kb_curator_cross_repo_graph_enabled && cfg.kb_curator_detect_contradictions_enabled);

   /* "lite" -> core extract+index on, heavy stages off. */
   {
      cJSON *root = cJSON_CreateObject();
      cJSON *kb = cJSON_AddObjectToObject(root, "kb");
      cJSON *cur = cJSON_AddObjectToObject(kb, "curator");
      cJSON_AddStringToObject(cur, "tier", "lite");
      assert(config_parse_kb_curator(&cfg, root) == 0);
      assert(strcmp(cfg.kb_curator_tier, "lite") == 0);
      assert(cfg.kb_curator_extract_docs_enabled && cfg.kb_curator_extract_code_enabled &&
             cfg.kb_curator_resolve_entities_enabled && cfg.kb_curator_index_narrative_enabled &&
             cfg.kb_curator_index_claims_enabled);
      assert(!cfg.kb_curator_synthesize_enabled && !cfg.kb_curator_detect_contradictions_enabled &&
             !cfg.kb_curator_cross_repo_graph_enabled && !cfg.kb_curator_projection_graph_enabled &&
             !cfg.kb_curator_link_artifacts_enabled && !cfg.kb_curator_promote_entity_enabled &&
             !cfg.kb_curator_index_code_unit_enabled);
      cJSON_Delete(root);
   }

   /* "off" -> every stage off. */
   {
      config_t c2;
      memset(&c2, 0, sizeof(c2));
      config_kb_curator_defaults(&c2);
      cJSON *root = cJSON_CreateObject();
      cJSON *kb = cJSON_AddObjectToObject(root, "kb");
      cJSON *cur = cJSON_AddObjectToObject(kb, "curator");
      cJSON_AddStringToObject(cur, "tier", "off");
      assert(config_parse_kb_curator(&c2, root) == 0);
      assert(!c2.kb_curator_extract_docs_enabled && !c2.kb_curator_synthesize_enabled &&
             !c2.kb_curator_resolve_entities_enabled);
      cJSON_Delete(root);
   }

   /* explicit per-stage gate overrides the tier (applied after it). */
   {
      config_t c3;
      memset(&c3, 0, sizeof(c3));
      config_kb_curator_defaults(&c3);
      cJSON *root = cJSON_CreateObject();
      cJSON *kb = cJSON_AddObjectToObject(root, "kb");
      cJSON *cur = cJSON_AddObjectToObject(kb, "curator");
      cJSON_AddStringToObject(cur, "tier", "lite");
      cJSON *syn = cJSON_AddObjectToObject(cur, "synthesize");
      cJSON_AddBoolToObject(syn, "enabled", 1);
      assert(config_parse_kb_curator(&c3, root) == 0);
      assert(c3.kb_curator_synthesize_enabled == 1);   /* override wins over lite */
      assert(!c3.kb_curator_cross_repo_graph_enabled); /* rest still lite */
      cJSON_Delete(root);
   }
}

static void assert_disposition(const config_t *cfg, int index, const char *name, double value,
                               config_disposition_source_t source)
{
   assert(cfg);
   assert(index >= 0);
   assert(index < cfg->disposition_count);
   assert(strcmp(cfg->dispositions[index].name, name) == 0);
   assert(cfg->dispositions[index].value == value);
   assert(cfg->dispositions[index].source == source);
}

/* A YAML `true` must read back as true. cJSON stores a parsed boolean in its
 * `type` and leaves valueint at 0, so `x = item->valueint` on a bool node yields
 * FALSE for `true`. Three intelligence.synthesize keys did exactly that; the
 * worst was mdl_tiebreak_enabled, which defaults to 1, so writing `true` TURNED
 * IT OFF. reflection_shadow is a fail-closed gate, so `true` failing to enable it
 * meant durable writes an operator had asked to suppress. */
static void test_bool_true_parses_as_true(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));

   cJSON *root = cJSON_CreateObject();
   cJSON *intel = cJSON_AddObjectToObject(root, "intelligence");
   cJSON *syn = cJSON_AddObjectToObject(intel, "synthesize");
   cJSON_AddTrueToObject(syn, "mdl_tiebreak_enabled");
   cJSON_AddTrueToObject(syn, "reflection_shadow");
   config_apply_mdl_settings(&cfg, root);
   assert(cfg.kb_mdl_tiebreak_enabled == 1);
   assert(cfg.kb_reflection_synthesis_shadow == 1);
   cJSON_Delete(root);

   /* `false` must still be false, and the numeric form must still work. */
   root = cJSON_CreateObject();
   intel = cJSON_AddObjectToObject(root, "intelligence");
   syn = cJSON_AddObjectToObject(intel, "synthesize");
   cJSON_AddFalseToObject(syn, "mdl_tiebreak_enabled");
   cJSON_AddNumberToObject(syn, "reflection_shadow", 1);
   config_apply_mdl_settings(&cfg, root);
   assert(cfg.kb_mdl_tiebreak_enabled == 0);
   assert(cfg.kb_reflection_synthesis_shadow == 1);
   cJSON_Delete(root);

   printf("bool-true ");
}

/* A bundled embedder reaches the process as EMBEDDER_URL, never as the stored
 * embedder_command: the kb entrypoint exports the URL when it starts the in-container
 * model, and the wizard writes embedder_model. Anything asking "is an embedder
 * available" must therefore go through the resolver.
 *
 * The curator drain asked the raw field instead, so on every bundled deployment its
 * ingest_docs and embed_code stages saw "no embedder" and never ran -- while query
 * embedding kept working, because that path resolves. The result was a KB that
 * reported healthy, embedded every search query, and returned zero hits forever. */
static void test_embedder_command_resolves_from_env(void)
{
   /* Deliberately NULL rather than a config_t: naming that type is a lint failure
    * (config-encapsulation-check), and the invariant the drain gate depends on is
    * exactly the no-stored-command case anyway -- a bundled deployment never writes
    * embedder_command, so NULL models it faithfully. */
   platform_unsetenv("EMBEDDER_URL");
   /* Nothing configured anywhere: the honest answer is empty, which is what makes
    * the resolver safe to use as an availability gate. */
   assert(config_embedder_command(NULL, NULL)[0] == '\0');

   /* The bundled case: the entrypoint exports EMBEDDER_URL for the model it just
    * started, and that alone must make the resolver non-empty. This is the assertion
    * the curator drain's en_embedder() now relies on. */
   platform_setenv("EMBEDDER_URL", "http://127.0.0.1:8760");
   assert(strcmp(config_embedder_command(NULL, NULL), "http://127.0.0.1:8760") == 0);

   /* A per-call request still outranks the environment. */
   assert(strcmp(config_embedder_command(NULL, "http://other:9"), "http://other:9") == 0);

   platform_unsetenv("EMBEDDER_URL");
   assert(config_embedder_command(NULL, NULL)[0] == '\0');
}

int main(void)
{
   printf("config: ");
   test_kb_curator_tier();
   test_bool_true_parses_as_true();
   test_embedder_command_resolves_from_env();

   /* Use isolated temp HOME */
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-config-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   char *old_aimee_home = getenv("AIMEE_HOME") ? strdup(getenv("AIMEE_HOME")) : NULL;
   char *old_no_cache = getenv("AIMEE_NO_CACHE") ? strdup(getenv("AIMEE_NO_CACHE")) : NULL;
   platform_setenv("HOME", tmpdir);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");

   /* --- config_load: missing file returns defaults --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      /* A fresh install has NO primary provider. It used to default to "claude",
       * which pre-populated a provider nobody chose: the chat path synthesized a
       * claude tmux-CLI agent for it and PINNED the turn to that agent, so on a
       * machine with no claude CLI every turn died with "no agent available for
       * role 'code'" while the operator's own agents sat there ineligible — and
       * completing the setup wizard did not help, because the wizard writes
       * agents.json's default_agent and this pin overrode it. Empty means "not
       * chosen", which lets default_agent decide. */
      assert(cfg.provider[0] == '\0');
      /* Capability routing defaults ON: routing enforces the packet's required
       * capabilities and minimum context window. Pinned explicitly so flipping
       * it is a deliberate, reviewed change rather than an accident — with it
       * off, agent_route_with_caps() degrades to cost-tier-only and capability
       * is never consulted. */
      assert(cfg.model_meta_capability_routing == 1);
      assert(cfg.model_meta_refresh_minutes == 60);
      /* kb.typed_facts.* autonomous reconciliation defaults (§7.2/§8). */
      assert(cfg.kb_typed_facts_auto_promote_enabled == 1);
      assert(cfg.kb_typed_facts_promote_threshold == 3);
      /* Typed-fact extraction is offline, so it defaults ON on every backend. */
      assert(cfg.typed_facts_enabled == 1);
      assert(strcmp(cfg.guardrail_mode, "approve") == 0);
      /* Sub-agent ban is an enforcement dial: defaults ON so a fresh install bans
       * the primary agent's own sub-agents (when delegates exist). */
      assert(cfg.subagent_ban_enabled == 1);
      /* §5 hybrid RRF weights + rank constant default to equal weights / k=60,
       * and the §7 blast-radius advisory is opt-in (off). */
      assert(fabs(cfg.code_hybrid_weight_code - 1.0) < 1e-9);
      assert(fabs(cfg.code_hybrid_weight_graph - 1.0) < 1e-9);
      assert(fabs(cfg.code_hybrid_rrf_k - 60.0) < 1e-9);
      assert(cfg.guardrails_blast_radius_advisory_enabled == 0);
      /* The autonomous live forge (F4) defaults OFF (2026-07-17): it does real
       * git push + PR + merge, so it stays opt-in while the autonomous pipeline is
       * under test. Set wfe_live_forge_enabled true to opt in. The proposals auto-scan
       * likewise defaults off, so pending proposals are filed one at a time by a human. */
      assert(cfg.wfe_live_forge_enabled == 0);
      assert(cfg.wfe_proposals_autoscan_enabled == 0);
      assert(cfg.db1_path[0] != '\0');
      assert(strcmp(cfg.guardrails_semantic_mode, "off") == 0); /* default */
      assert(cfg.skills_review_nudge_interval == 10);
      assert(cfg.skills_stale_after_days == 30);
      assert(cfg.skills_archive_after_days == 90);
      assert(cfg.skills_dispatch_enabled == 1);
      assert(cfg.skills_dispatch_max_index == 24);
      assert(cfg.skills_dispatch_advisory == 0);
      assert(cfg.skills_capability_autostub == 0);
      assert(cfg.skills_eval_gate_enabled == 0);
      assert(fabs(cfg.skills_eval_threshold - 0.01) < 0.0001);
      /* worktree auto-GC defaults on (once/day at session-start, 14d idle). */
      assert(cfg.worktree_gc_enabled == 1);
      assert(cfg.worktree_gc_max_age_days == 14);
      assert(cfg.ingress_max_raw_scans == 0);
      assert(cfg.concurrency_preempt_requeue_max == CONFIG_DEFAULT_CONCURRENCY_PREEMPT_REQUEUE_MAX);
      /* profile-card refresh ran ungated in maintenance before the enable-gate was
       * wired; the flag now defaults on so that behavior is preserved. */
      assert(cfg.memory_profile_cards_enabled == 1);
      /* dedupe likewise ran ungated in the COMPACT pass; default-on preserves it.
       * summarise stays opt-in (default off). */
      assert(cfg.memory_improve_dedupe_enabled == 1);
      assert(cfg.memory_improve_summarise_enabled == 0);
      /* directives auto-create ran ungated before the toggle was wired; default-on
       * preserves it. */
      assert(cfg.memory_directives_enabled == 1);
      /* CSS style graph now defaults on so the read-only css signals/report work
       * out of the box (the indexer populates css_rules/css_declarations). */
      assert(cfg.css_style_graph_enabled == 1);
      /* git co-change backfill defaults on: index scan seeds co_edited edges that
       * blast radius already reads (incremental/idempotent). */
      assert(cfg.code_cochange_git_enabled == 1);
      assert(cfg.transport_kb_pool_enabled == 1);
      assert(cfg.transport_server_keepalive_enabled == 1);
      assert(cfg.transport_thinclient_gzip_enabled == 0);
      assert(cfg.transport_kb_gzip_enabled == 0);
      cJSON *rollback_root = cJSON_CreateObject();
      cJSON *rollback_transport = cJSON_AddObjectToObject(rollback_root, "transport");
      cJSON_AddBoolToObject(rollback_transport, "kb_pool_enabled", 0);
      cJSON_AddBoolToObject(rollback_transport, "server_keepalive_enabled", 0);
      config_parse_transport_section(&cfg, rollback_root);
      assert(cfg.transport_kb_pool_enabled == 0);
      assert(cfg.transport_server_keepalive_enabled == 0);
      cJSON_Delete(rollback_root);
      /* css_render_command defaults to the conventional sidecar curl (inert until
       * the sidecar is up), so render-capture works out of the box on-demand. */
      assert(strcmp(cfg.css_render_command, CONFIG_DEFAULT_CSS_RENDER_COMMAND) == 0);
      /* Cross-repo dependency graph defaults (initial calibration). */
      assert(cfg.kb_curator_cross_repo_graph_enabled == 1);
      assert(cfg.kb_curator_cross_repo_distinctiveness_v == 1);
      assert(cfg.kb_curator_cross_repo_k == 5);
      assert(cfg.kb_curator_cross_repo_m == 8);
      assert(cfg.kb_curator_cross_repo_p_pct == 25);
      assert(cfg.kb_curator_cross_repo_len_min == 4);
      assert(cfg.kb_curator_cross_repo_caller_collision_c == 5);
      assert(cfg.kb_curator_cross_repo_max_candidates == 50000);
      assert(cfg.kb_curator_cross_repo_query_timeout_ms == 5000);
      assert(cfg.kb_curator_cross_repo_review_queue_max == 5000);
   }

   /* --- config_save + config_load round-trip --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      snprintf(cfg.provider, sizeof(cfg.provider), "gemini");
      snprintf(cfg.default_persona, sizeof(cfg.default_persona), "architect");
      snprintf(cfg.claude_model, sizeof(cfg.claude_model), "claude-sonnet-4-6");
      snprintf(cfg.codex_model, sizeof(cfg.codex_model), "gpt-5.4");
      snprintf(cfg.model_reasoning_effort, sizeof(cfg.model_reasoning_effort), "high");
      snprintf(cfg.memory_rerank_mode, sizeof(cfg.memory_rerank_mode), "slow");
      snprintf(cfg.kb_client_url, sizeof(cfg.kb_client_url), "https://kb.example:4010");
      snprintf(cfg.kb_client_bearer_token, sizeof(cfg.kb_client_bearer_token), "tok-abc123");
      /* Setup-wizard page-2 backend record (kb_mode + per-role llm_* fields). */
      snprintf(cfg.kb_mode, sizeof(cfg.kb_mode), "local");
      snprintf(cfg.synthesis_endpoint, sizeof(cfg.synthesis_endpoint), "https://api.example/v1");
      snprintf(cfg.synthesis_model, sizeof(cfg.synthesis_model), "gpt-5.5");
      cfg.server_api_http_port = 8910;
      snprintf(cfg.server_api_bearer_token, sizeof(cfg.server_api_bearer_token), "tok-api-xyz");
      cfg.server_api_bearer_extra_count = 2;
      snprintf(cfg.server_api_bearer_extra[0], sizeof(cfg.server_api_bearer_extra[0]),
               "tok-client-one");
      memset(cfg.server_api_bearer_extra[1], 'x', 180);
      cfg.server_api_bearer_extra[1][180] = '\0';
      cfg.server_api_rate_limit_per_min = 60;
      cfg.server_api_max_event_streams = 512;
      snprintf(cfg.server_api_client_transport, sizeof(cfg.server_api_client_transport), "http");
      cfg.server_api_remote_writes = SERVER_REMOTE_WRITES_FULL;
      cfg.transport_kb_pool_enabled = 1;
      cfg.transport_server_keepalive_enabled = 1;
      cfg.transport_thinclient_gzip_enabled = 1;
      cfg.transport_kb_gzip_enabled = 1;
      cfg.ingress_preinject_assembly_budget = 8192;
      cfg.ingress_max_raw_scans = 2;
      /* Per-workspace provider: a detached entry round-trips as {path,provider};
       * a shared/default entry stays a bare path string; a mirror entry also
       * round-trips its vcs.remote + head in the object. */
      cfg.workspace_count = 3;
      snprintf(cfg.workspaces[0], MAX_PATH_LEN, "/tmp/ws-shared-rt");
      cfg.workspace_providers[0][0] = '\0'; /* default shared */
      snprintf(cfg.workspaces[1], MAX_PATH_LEN, "/tmp/ws-detached-rt");
      snprintf(cfg.workspace_providers[1], sizeof(cfg.workspace_providers[1]), "detached");
      snprintf(cfg.workspaces[2], MAX_PATH_LEN, "/tmp/ws-mirror-rt");
      snprintf(cfg.workspace_providers[2], sizeof(cfg.workspace_providers[2]), "mirror");
      snprintf(cfg.workspace_vcs_remote[2], sizeof(cfg.workspace_vcs_remote[2]),
               "https://example.com/r.git");
      snprintf(cfg.workspace_vcs_head[2], sizeof(cfg.workspace_vcs_head[2]),
               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
      cfg.memory_maintenance_trigger_inserts = 7;
      cfg.memory_maintenance_trigger_secs = 90;
      cfg.memory_cognify_async_enabled = 1;
      snprintf(cfg.memory_citations_mode, sizeof(cfg.memory_citations_mode), "required");
      cfg.memory_citations_reprompt_on_miss = 1;
      cfg.memory_citations_strip_unverified = 1;
      cfg.learning_router_enabled = 0;
      cfg.learning_proposal_ttl_days = 14;
      cfg.learning_max_commits_per_week = 11;
      /* learning.implicit.* now persist (was parse/save gap): citation_repair off
       * (default on → prove the off state round-trips), repeat_question on. */
      cfg.learning_implicit_citation_repair = 0;
      cfg.learning_implicit_repeat_question = 1;
      cfg.cache_aware_rewrite_enabled = 1;
      cfg.cache_aware_rewrite_min_savings_tokens = 321;
      cfg.cache_aware_rewrite_hard_context_threshold = 0.72;
      snprintf(cfg.guardrails_semantic_mode, sizeof(cfg.guardrails_semantic_mode), "enforce");
      snprintf(cfg.guardrails_semantic_command, sizeof(cfg.guardrails_semantic_command),
               "semantic-sidecar --json");
      cfg.guardrails_semantic_warn_threshold = 0.35;
      cfg.guardrails_semantic_prompt_threshold = 0.65;
      cfg.guardrails_semantic_block_threshold = 0.95;
      cfg.skills_review_nudge_interval = 12;
      cfg.skills_stale_after_days = 45;
      cfg.skills_archive_after_days = 120;
      cfg.skills_dispatch_enabled = 0;
      cfg.skills_dispatch_max_index = 7;
      cfg.skills_dispatch_advisory = 1;
      cfg.skills_capability_autostub = 1;
      cfg.skills_eval_gate_enabled = 1;
      cfg.skills_eval_threshold = 0.25;
      /* worktree_gc.* — regression: enabled/max_age_days used to be parse/save
       * gaps, so the documented auto-GC knob was inert. Prove the off state +
       * non-default age round-trip (default enabled=1 → flip to 0 to test). */
      cfg.worktree_gc_enabled = 0;
      cfg.worktree_gc_max_age_days = 21;
      cfg.concurrency_preempt_enabled = 1;
      cfg.concurrency_preempt_single_slot_only = 0;
      cfg.concurrency_preempt_requeue_max = 2;
      snprintf(cfg.dispositions[0].name, sizeof(cfg.dispositions[0].name), "skepticism");
      cfg.dispositions[0].value = 0.8;
      cfg.dispositions[0].source = CONFIG_DISPOSITION_SOURCE_GLOBAL;
      snprintf(cfg.dispositions[1].name, sizeof(cfg.dispositions[1].name), "literalism");
      cfg.dispositions[1].value = 0.5;
      cfg.dispositions[1].source = CONFIG_DISPOSITION_SOURCE_GLOBAL;
      cfg.disposition_count = 2;
      cfg.disposition_globals[0] = cfg.dispositions[0];
      cfg.disposition_globals[1] = cfg.dispositions[1];
      cfg.disposition_global_count = 2;
      /* kb.curator.* gates — must survive config_save (regression: they used to
       * be dropped, so --bootstrap-db2 silently disabled the curator). */
      cfg.kb_curator_resolve_entities_enabled = 1;
      cfg.kb_curator_promote_entity_enabled = 1;
      cfg.kb_curator_promote_min_sources = 5;
      cfg.kb_curator_synthesize_enabled = 1;
      cfg.kb_curator_synthesize_k = 4;
      /* kb.typed_facts.* (§8): non-default values must round-trip (auto_promote
       * defaults on, so set it off; promote_threshold defaults 3, so set 7). */
      cfg.kb_typed_facts_auto_promote_enabled = 0;
      cfg.kb_typed_facts_promote_threshold = 7;
      cfg.typed_facts_enabled = 1; /* KB-owned master gate: persisted as kb.typed_facts.enabled */
      snprintf(cfg.kb_curator_judge_command, sizeof(cfg.kb_curator_judge_command), "judge --json");
      snprintf(cfg.kb_curator_synthesize_command, sizeof(cfg.kb_curator_synthesize_command),
               "synth --json");
      /* kb.curator provider/tier_b (§2) — full provider defs must round-trip. */
      snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
               "http://curator:8080/v1");
      snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "gemma-4-e4b");
      cfg.kb_evidence_embed_enabled = 0;
      /* kb.curator.cross_repo_graph.* — non-default values must round-trip; enabled
       * defaults on so set off to prove the disabled state survives save+reload. */
      cfg.kb_curator_cross_repo_graph_enabled = 0;
      cfg.kb_curator_cross_repo_distinctiveness_v = 3;
      cfg.kb_curator_cross_repo_k = 7;
      cfg.kb_curator_cross_repo_m = 9;
      cfg.kb_curator_cross_repo_p_pct = 40;
      cfg.kb_curator_cross_repo_len_min = 3;
      cfg.kb_curator_cross_repo_caller_collision_c = 6;
      cfg.kb_curator_cross_repo_max_candidates = 12345;
      cfg.kb_curator_cross_repo_query_timeout_ms = 2500;
      cfg.kb_curator_cross_repo_review_queue_max = 999;
      /* profile_cards now defaults on; set it off to prove the disabled state
       * round-trips (regression class: a default-on bool whose save guard only
       * emitted on a truthy value would silently reset back to on). */
      cfg.memory_profile_cards_enabled = 0;
      /* memory.improve.* was parse-only (dropped on save); dedupe defaults on so
       * set it off, summarise on, to prove the whole block now round-trips. */
      cfg.memory_improve_dedupe_enabled = 0;
      cfg.memory_improve_summarise_enabled = 1;
      cfg.memory_improve_min_cluster = 5;
      cfg.memory_improve_max_confidence = 0.42;
      /* directives defaults on; set off to prove the disabled state round-trips
       * (same default-on save-guard regression class as profile_cards). */
      cfg.memory_directives_enabled = 0;
      /* css_style_graph now defaults on; set off to prove the opt-out round-trips
       * (default-on save-guard regression class). */
      cfg.css_style_graph_enabled = 0;
      /* code_cochange_git defaults on; set off to prove the opt-out round-trips
       * (default-on save-guard regression class). */
      cfg.code_cochange_git_enabled = 0;
      /* audit_worm_enabled defaults off; set on to prove the opt-in persists
       * (regression: it was allowlist-settable but had no config_save/load path,
       * so an enabled deployment silently reverted to off on restart). */
      cfg.audit_worm_enabled = 1;
      /* css_render_command defaults non-empty; set empty to prove the disable
       * override round-trips (string default-on save-guard: save must emit the
       * non-default empty, not drop it and silently re-default on reload). */
      cfg.css_render_command[0] = '\0';
      /* kb.maintenance.* — must survive config_save (same drop class as curator). */
      cfg.kb_maintenance_enabled = 1;
      cfg.kb_maintenance_interval_hours = 12;
      cfg.kb_maintenance_min_age_days = 3;
      cfg.kb_maintenance_orphan_days = 30;
      /* charter.* (arrays + scalar) */
      cfg.charter_safety_axioms_count = 2;
      snprintf(cfg.charter_safety_axioms[0], CONFIG_CHARTER_ENTRY_LEN, "do no harm");
      snprintf(cfg.charter_safety_axioms[1], CONFIG_CHARTER_ENTRY_LEN, "ask when unsure");
      cfg.charter_values_count = 1;
      snprintf(cfg.charter_values[0], CONFIG_CHARTER_ENTRY_LEN, "honesty");
      cfg.charter_working_profile_drift_limit = 5;
      /* intelligence.* (calibrate multi-value enabled, demotion, bandit) */
      cfg.calibration_enabled = 2; /* A/B mode -- must survive as a number, not bool */
      cfg.calibration_buckets = 20;
      cfg.calibration_tau_memory_auto = 0.91;
      snprintf(cfg.calibration_command, sizeof(cfg.calibration_command), "calib --json");
      /* demotion_enabled now defaults to 1 (shadow); set 2 (live) to prove the
       * non-default value survives the emit-when-!=1 save guard. */
      cfg.demotion_enabled = 2;
      cfg.demotion_window = 128;
      cfg.bandit_live_decision_enabled = 1;
      cfg.bandit_exploration_fraction = 0.2;
      snprintf(cfg.bandit_optimize_command, sizeof(cfg.bandit_optimize_command), "bopt --json");
      /* dogfood.* (enabled default 1 -> set 0 to prove non-default survives) */
      cfg.dogfood_enabled = 0;
      snprintf(cfg.dogfood_log_dir, sizeof(cfg.dogfood_log_dir), "/tmp/df-rt");
      cfg.dogfood_commit_raw = 1;
      /* integrity.* */
      cfg.integrity_enabled = 1;
      cfg.integrity_dry_run = 0;
      /* ensemble.* (+ reference_models array) */
      snprintf(cfg.ensemble_aggregator, sizeof(cfg.ensemble_aggregator), "synthesizer");
      cfg.ensemble_min_successful = 3;
      cfg.ensemble_reference_count = 2;
      snprintf(cfg.ensemble_reference_models[0], sizeof(cfg.ensemble_reference_models[0]), "m-a");
      snprintf(cfg.ensemble_reference_models[1], sizeof(cfg.ensemble_reference_models[1]), "m-b");
      cfg.ensemble_reference_persona_count = 2;
      snprintf(cfg.ensemble_reference_personas[0], sizeof(cfg.ensemble_reference_personas[0]),
               "security");
      snprintf(cfg.ensemble_reference_personas[1], sizeof(cfg.ensemble_reference_personas[1]),
               "reviewer-constructive");
      /* identity.working_profile_injection.* (+ fields array) */
      cfg.identity_working_profile_injection_enabled = 1;
      cfg.identity_working_profile_injection_fields_count = 1;
      snprintf(cfg.identity_working_profile_injection_fields[0],
               sizeof(cfg.identity_working_profile_injection_fields[0]), "tone");
      /* trigger.* + trigger_rules[] + cron_jobs[] (arrays of nested structs) */
      snprintf(cfg.trigger_auth_token, sizeof(cfg.trigger_auth_token), "trig-tok");
      cfg.trigger_max_concurrent = 4;
      cfg.trigger_rule_count = 1;
      snprintf(cfg.trigger_rules[0].source, sizeof(cfg.trigger_rules[0].source), "github-webhook");
      snprintf(cfg.trigger_rules[0].event, sizeof(cfg.trigger_rules[0].event), "push:main");
      snprintf(cfg.trigger_rules[0].pipeline_template,
               sizeof(cfg.trigger_rules[0].pipeline_template), "review");
      snprintf(cfg.trigger_rules[0].mode, sizeof(cfg.trigger_rules[0].mode), "interactive");
      cfg.trigger_rules[0].max_spend_usd = 2.5;
      cfg.cron_job_count = 1;
      snprintf(cfg.cron_jobs[0].id, sizeof(cfg.cron_jobs[0].id), "nightly");
      snprintf(cfg.cron_jobs[0].schedule, sizeof(cfg.cron_jobs[0].schedule), "0 3 * * *");
      snprintf(cfg.cron_jobs[0].mode, sizeof(cfg.cron_jobs[0].mode), "llm");
      snprintf(cfg.cron_jobs[0].prompt, sizeof(cfg.cron_jobs[0].prompt), "summarize the day");
      cfg.cron_jobs[0].enabled = 1;
      cfg.cron_jobs[0].pre_wake_gate = 1;
      cfg.cron_jobs[0].skill_count = 1;
      snprintf(cfg.cron_jobs[0].skills[0], sizeof(cfg.cron_jobs[0].skills[0]), "deep-research");
      snprintf(cfg.cron_jobs[0].deliver_target, sizeof(cfg.cron_jobs[0].deliver_target), "ntfy:me");
      cfg.cron_jobs[0].deliver_only_if_changed = 1;
      /* niche scalars + auxiliary */
      snprintf(cfg.proxy_url, sizeof(cfg.proxy_url), "http://proxy:3128");
      snprintf(cfg.proxy_token, sizeof(cfg.proxy_token), "ptok");
      cfg.max_background_processes = 9;
      cfg.model_meta_refresh_minutes = 15;
      /* Non-default value, so the round trip proves persistence rather than
       * agreeing with the default by accident (capability_routing defaults ON). */
      cfg.model_meta_capability_routing = 0;
      snprintf(cfg.search_backend, sizeof(cfg.search_backend), "searxng");
      cfg.search_max_results = 7;
      snprintf(cfg.search_searxng_url, sizeof(cfg.search_searxng_url), "http://sx:8888");
      cfg.aux_enabled = 1;
      snprintf(cfg.aux_default_provider, sizeof(cfg.aux_default_provider), "openai");
      cfg.aux_default_max_tokens = 4096;
      cfg.aux_task_count = 1;
      snprintf(cfg.aux_tasks[0].task, CONFIG_AUX_TASK_NAME_LEN, "summarize");
      snprintf(cfg.aux_tasks[0].model, sizeof(cfg.aux_tasks[0].model), "gpt-mini");
      cfg.aux_tasks[0].max_tokens = 512;
      /* lsp_servers[] */
      cfg.lsp_server_count = 1;
      snprintf(cfg.lsp_servers[0].name, sizeof(cfg.lsp_servers[0].name), "clangd");
      snprintf(cfg.lsp_servers[0].command, sizeof(cfg.lsp_servers[0].command), "clangd");
      cfg.lsp_servers[0].arg_count = 1;
      snprintf(cfg.lsp_servers[0].args[0], sizeof(cfg.lsp_servers[0].args[0]),
               "--background-index");
      cfg.lsp_servers[0].extension_count = 1;
      snprintf(cfg.lsp_servers[0].extensions[0], sizeof(cfg.lsp_servers[0].extensions[0]), "c");
      /* require_aimee_git (default ON): an enforcement dial, so a value that does not
       * survive save+reload is a guard silently in the wrong state after every
       * restart. Set it to its OPT-OUT — the direction config_save has to persist
       * explicitly, and the direction that is unsafe to lose. */
      cfg.require_aimee_git = 0;
      cfg.subagent_ban_enabled = 0; /* default-ON dial: opt-out must survive save+reload */
      config_save(&cfg);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(strcmp(cfg2.provider, "gemini") == 0);
      assert(strcmp(cfg2.default_persona, "architect") == 0);
      assert(strcmp(cfg2.claude_model, "claude-sonnet-4-6") == 0);
      assert(strcmp(cfg2.codex_model, "gpt-5.4") == 0);
      assert(strcmp(cfg2.model_reasoning_effort, "high") == 0);
      assert(strcmp(cfg2.memory_rerank_mode, "slow") == 0);
      /* Credential fields never round-trip through aimee.yaml. They are
       * hydrated by the Vault bootstrap in production. */
      assert(cfg2.kb_client_bearer_token[0] == '\0');
      /* Setup-wizard page-2 backend record survives save/load. */
      assert(strcmp(cfg2.kb_mode, "local") == 0);
      assert(strcmp(cfg2.synthesis_endpoint, "https://api.example/v1") == 0);
      assert(strcmp(cfg2.synthesis_model, "gpt-5.5") == 0);
      assert(cfg2.server_api_http_port == 8910);
      assert(cfg2.server_api_bearer_token[0] == '\0');
      assert(cfg2.server_api_bearer_extra_count == 0);
      assert(cfg2.server_api_bearer_extra[0][0] == '\0');
      assert(cfg2.server_api_rate_limit_per_min == 60);
      assert(cfg2.server_api_max_event_streams == 512);
      assert(strcmp(cfg2.server_api_client_transport, "http") == 0);
      assert(cfg2.transport_kb_pool_enabled == 1);
      assert(cfg2.transport_server_keepalive_enabled == 1);
      assert(cfg2.transport_thinclient_gzip_enabled == 1);
      assert(cfg2.transport_kb_gzip_enabled == 1);
      /* regression: remote_writes used to be parsed but never written by config_save,
       * so any save silently reset it to off. */
      assert(cfg2.server_api_remote_writes == SERVER_REMOTE_WRITES_FULL);
      /* regression, the MIRROR of the remote_writes bug above: require_aimee_git was
       * WRITTEN by config_save and never PARSED back, so `require_aimee_git: false`
       * in aimee.yaml did nothing and the dial silently reverted to ON at every
       * restart — while cmd_hooks.c told operators that exact line was the way to
       * turn it off. Save-without-parse and parse-without-save are the same bug from
       * opposite ends; a round-trip is the only thing that catches either. */
      assert(cfg2.require_aimee_git == 0);
      /* Same save-without-parse / parse-without-save class as require_aimee_git:
       * subagent_ban_enabled is written only as the opt-out and must parse back. */
      assert(cfg2.subagent_ban_enabled == 0);
      assert(cfg2.ingress_preinject_assembly_budget == 8192);
      assert(cfg2.ingress_max_raw_scans == 2);
      assert(cfg2.workspace_count == 3);
      assert(strcmp(cfg2.workspaces[0], "/tmp/ws-shared-rt") == 0);
      assert(cfg2.workspace_providers[0][0] == '\0'); /* shared stays default */
      assert(cfg2.workspace_vcs_remote[0][0] == '\0');
      assert(strcmp(cfg2.workspaces[1], "/tmp/ws-detached-rt") == 0);
      assert(strcmp(cfg2.workspace_providers[1], "detached") == 0);
      assert(strcmp(cfg2.workspaces[2], "/tmp/ws-mirror-rt") == 0);
      assert(strcmp(cfg2.workspace_providers[2], "mirror") == 0);
      assert(strcmp(cfg2.workspace_vcs_remote[2], "https://example.com/r.git") == 0);
      assert(strcmp(cfg2.workspace_vcs_head[2],
                    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 0);
      assert(cfg2.worktree_gc_enabled == 0);
      assert(cfg2.worktree_gc_max_age_days == 21);
      assert(cfg2.memory_maintenance_trigger_inserts == 7);
      assert(cfg2.memory_maintenance_trigger_secs == 90);
      assert(cfg2.memory_cognify_async_enabled == 1);
      assert(strcmp(cfg2.memory_citations_mode, "required") == 0);
      assert(cfg2.memory_citations_reprompt_on_miss == 1);
      assert(cfg2.memory_citations_strip_unverified == 1);
      assert(cfg2.learning_router_enabled == 0);
      assert(cfg2.learning_proposal_ttl_days == 14);
      /* implicit overrides persisted; the untouched citation_continuation kept
       * its default-on. */
      assert(cfg2.learning_implicit_citation_repair == 0);
      assert(cfg2.learning_implicit_citation_continuation == 1);
      assert(cfg2.learning_implicit_repeat_question == 1);
      assert(cfg2.learning_max_commits_per_week == 11);
      assert(cfg2.cache_aware_rewrite_enabled == 1);
      assert(cfg2.cache_aware_rewrite_min_savings_tokens == 321);
      assert(fabs(cfg2.cache_aware_rewrite_hard_context_threshold - 0.72) < 0.0001);
      assert(strcmp(cfg2.guardrails_semantic_mode, "enforce") == 0);
      assert(strcmp(cfg2.guardrails_semantic_command, "semantic-sidecar --json") == 0);
      assert(fabs(cfg2.guardrails_semantic_warn_threshold - 0.35) < 0.0001);
      assert(fabs(cfg2.guardrails_semantic_prompt_threshold - 0.65) < 0.0001);
      assert(fabs(cfg2.guardrails_semantic_block_threshold - 0.95) < 0.0001);
      assert(cfg2.skills_review_nudge_interval == 12);
      assert(cfg2.skills_stale_after_days == 45);
      assert(cfg2.skills_archive_after_days == 120);
      assert(cfg2.skills_dispatch_enabled == 0);
      assert(cfg2.skills_dispatch_max_index == 7);
      assert(cfg2.skills_dispatch_advisory == 1);
      assert(cfg2.skills_capability_autostub == 1);
      assert(cfg2.skills_eval_gate_enabled == 1);
      assert(fabs(cfg2.skills_eval_threshold - 0.25) < 0.0001);
      assert(cfg2.concurrency_preempt_enabled == 1);
      assert(cfg2.concurrency_preempt_single_slot_only == 0);
      assert(cfg2.concurrency_preempt_requeue_max == 2);
      assert(cfg2.disposition_count == 2);
      assert(cfg2.disposition_global_count == 2);
      assert_disposition(&cfg2, 0, "skepticism", 0.8, CONFIG_DISPOSITION_SOURCE_GLOBAL);
      assert_disposition(&cfg2, 1, "literalism", 0.5, CONFIG_DISPOSITION_SOURCE_GLOBAL);
      assert(cfg2.kb_curator_resolve_entities_enabled == 1);
      assert(cfg2.kb_curator_promote_entity_enabled == 1);
      assert(cfg2.kb_curator_promote_min_sources == 5);
      assert(cfg2.kb_curator_synthesize_enabled == 1);
      assert(cfg2.kb_curator_synthesize_k == 4);
      assert(cfg2.kb_typed_facts_auto_promote_enabled == 0);
      assert(cfg2.kb_typed_facts_promote_threshold == 7);
      assert(cfg2.typed_facts_enabled == 1);
      assert(strcmp(cfg2.kb_curator_judge_command, "judge --json") == 0);
      assert(strcmp(cfg2.kb_curator_synthesize_command, "synth --json") == 0);
      assert(strcmp(cfg2.kb_curator_provider_base_url, "http://curator:8080/v1") == 0);
      assert(strcmp(cfg2.kb_curator_provider_model, "gemma-4-e4b") == 0);
      assert(cfg2.kb_evidence_embed_enabled == 0);
      assert(cfg2.kb_curator_cross_repo_graph_enabled == 0);
      assert(cfg2.kb_curator_cross_repo_distinctiveness_v == 3);
      assert(cfg2.kb_curator_cross_repo_k == 7);
      assert(cfg2.kb_curator_cross_repo_m == 9);
      assert(cfg2.kb_curator_cross_repo_p_pct == 40);
      assert(cfg2.kb_curator_cross_repo_len_min == 3);
      assert(cfg2.kb_curator_cross_repo_caller_collision_c == 6);
      assert(cfg2.kb_curator_cross_repo_max_candidates == 12345);
      assert(cfg2.kb_curator_cross_repo_query_timeout_ms == 2500);
      assert(cfg2.kb_curator_cross_repo_review_queue_max == 999);
      assert(cfg2.memory_profile_cards_enabled == 0);
      assert(cfg2.memory_improve_dedupe_enabled == 0);
      assert(cfg2.memory_improve_summarise_enabled == 1);
      assert(cfg2.memory_improve_min_cluster == 5);
      assert(fabs(cfg2.memory_improve_max_confidence - 0.42) < 0.0001);
      assert(cfg2.memory_directives_enabled == 0);
      assert(cfg2.css_style_graph_enabled == 0);   /* opt-out survives save/reload */
      assert(cfg2.code_cochange_git_enabled == 0); /* opt-out survives save/reload */
      assert(cfg2.audit_worm_enabled == 1);        /* opt-in survives save/reload */
      assert(cfg2.css_render_command[0] == '\0');  /* disable (empty) survives save/reload */
      /* regression: kb.maintenance.* used to be parsed but never saved -> dropped on save. */
      assert(cfg2.kb_maintenance_enabled == 1);
      assert(cfg2.kb_maintenance_interval_hours == 12);
      assert(cfg2.kb_maintenance_min_age_days == 3);
      assert(cfg2.kb_maintenance_orphan_days == 30);
      /* regression: these whole sections used to be dropped by config_save. */
      assert(cfg2.charter_safety_axioms_count == 2);
      assert(strcmp(cfg2.charter_safety_axioms[1], "ask when unsure") == 0);
      assert(cfg2.charter_values_count == 1 && strcmp(cfg2.charter_values[0], "honesty") == 0);
      assert(cfg2.charter_working_profile_drift_limit == 5);
      assert(cfg2.calibration_enabled == 2); /* multi-value int preserved */
      assert(cfg2.calibration_buckets == 20);
      assert(cfg2.calibration_tau_memory_auto > 0.90 && cfg2.calibration_tau_memory_auto < 0.92);
      assert(strcmp(cfg2.calibration_command, "calib --json") == 0);
      assert(cfg2.demotion_enabled == 2 && cfg2.demotion_window == 128);
      assert(cfg2.bandit_live_decision_enabled == 1);
      assert(cfg2.bandit_exploration_fraction > 0.19 && cfg2.bandit_exploration_fraction < 0.21);
      assert(strcmp(cfg2.bandit_optimize_command, "bopt --json") == 0);
      assert(cfg2.dogfood_enabled == 0 && cfg2.dogfood_commit_raw == 1);
      assert(strcmp(cfg2.dogfood_log_dir, "/tmp/df-rt") == 0);
      assert(cfg2.integrity_enabled == 1 && cfg2.integrity_dry_run == 0);
      assert(cfg2.ensemble_min_successful == 3);
      assert(strcmp(cfg2.ensemble_aggregator, "synthesizer") == 0);
      assert(cfg2.ensemble_reference_count == 2 &&
             strcmp(cfg2.ensemble_reference_models[1], "m-b") == 0);
      assert(cfg2.ensemble_reference_persona_count == 2 &&
             strcmp(cfg2.ensemble_reference_personas[0], "security") == 0 &&
             strcmp(cfg2.ensemble_reference_personas[1], "reviewer-constructive") == 0);
      assert(cfg2.identity_working_profile_injection_enabled == 1);
      assert(cfg2.identity_working_profile_injection_fields_count == 1 &&
             strcmp(cfg2.identity_working_profile_injection_fields[0], "tone") == 0);
      /* regression: trigger/cron (arrays of nested structs) used to be dropped on save. */
      assert(cfg2.trigger_auth_token[0] == '\0' && cfg2.trigger_max_concurrent == 4);
      assert(cfg2.trigger_rule_count == 1);
      assert(strcmp(cfg2.trigger_rules[0].source, "github-webhook") == 0);
      assert(strcmp(cfg2.trigger_rules[0].event, "push:main") == 0);
      assert(strcmp(cfg2.trigger_rules[0].pipeline_template, "review") == 0);
      assert(strcmp(cfg2.trigger_rules[0].mode, "interactive") == 0);
      assert(cfg2.trigger_rules[0].max_spend_usd > 2.4 &&
             cfg2.trigger_rules[0].max_spend_usd < 2.6);
      assert(cfg2.cron_job_count == 1);
      assert(strcmp(cfg2.cron_jobs[0].id, "nightly") == 0);
      assert(strcmp(cfg2.cron_jobs[0].schedule, "0 3 * * *") == 0);
      assert(strcmp(cfg2.cron_jobs[0].mode, "llm") == 0);
      assert(strcmp(cfg2.cron_jobs[0].prompt, "summarize the day") == 0);
      assert(cfg2.cron_jobs[0].enabled == 1 && cfg2.cron_jobs[0].pre_wake_gate == 1);
      assert(cfg2.cron_jobs[0].skill_count == 1 &&
             strcmp(cfg2.cron_jobs[0].skills[0], "deep-research") == 0);
      assert(strcmp(cfg2.cron_jobs[0].deliver_target, "ntfy:me") == 0);
      assert(cfg2.cron_jobs[0].deliver_only_if_changed == 1);
      /* regression: niche scalar + auxiliary sections used to be dropped on save. */
      assert(strcmp(cfg2.proxy_url, "http://proxy:3128") == 0);
      assert(cfg2.proxy_token[0] == '\0');
      assert(cfg2.max_background_processes == 9);
      assert(cfg2.model_meta_refresh_minutes == 15 && cfg2.model_meta_capability_routing == 0);
      assert(strcmp(cfg2.search_backend, "searxng") == 0 && cfg2.search_max_results == 7);
      assert(strcmp(cfg2.search_searxng_url, "http://sx:8888") == 0);
      assert(cfg2.aux_enabled == 1 && strcmp(cfg2.aux_default_provider, "openai") == 0);
      assert(cfg2.aux_default_max_tokens == 4096);
      assert(cfg2.aux_task_count == 1 && strcmp(cfg2.aux_tasks[0].task, "summarize") == 0);
      assert(strcmp(cfg2.aux_tasks[0].model, "gpt-mini") == 0 &&
             cfg2.aux_tasks[0].max_tokens == 512);
      assert(cfg2.lsp_server_count == 1 && strcmp(cfg2.lsp_servers[0].name, "clangd") == 0);
      assert(cfg2.lsp_servers[0].arg_count == 1 &&
             strcmp(cfg2.lsp_servers[0].args[0], "--background-index") == 0);
      assert(cfg2.lsp_servers[0].extension_count == 1 &&
             strcmp(cfg2.lsp_servers[0].extensions[0], "c") == 0);
   }

   /* --- kb_curator preset round-trip: a non-"full" tier persists and re-derives
    *     the 12 stage gates on reload (the persistence subtlety: save records the
    *     tier, parse applies it before any per-stage override). --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cJSON *root = cJSON_CreateObject();
      cJSON *kb = cJSON_AddObjectToObject(root, "kb");
      cJSON *cur = cJSON_AddObjectToObject(kb, "curator");
      cJSON_AddStringToObject(cur, "tier", "lite"); /* drive stages via the parser */
      config_parse_kb_curator(&cfg, root);
      cJSON_Delete(root);
      assert(config_save(&cfg) == 0);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(strcmp(cfg2.kb_curator_tier, "lite") == 0);
      assert(cfg2.kb_curator_extract_docs_enabled && cfg2.kb_curator_index_claims_enabled);
      assert(!cfg2.kb_curator_synthesize_enabled && !cfg2.kb_curator_cross_repo_graph_enabled);
   }

   /* --- kb_pdf preset: default "off" (all gates off), and a non-default tier
    *     persists + re-derives the 5 gates on reload. --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      /* default: structured-PDF off (plain pdftotext). */
      assert(strcmp(cfg.kb_pdf_tier, "off") == 0);
      assert(!cfg.kb_pdf_ingest_enabled && !cfg.kb_pdf_vector_enabled && !cfg.kb_pdf_tsr_enabled &&
             !cfg.kb_pdf_assets_enabled && !cfg.kb_pdf_ocr_enabled);

      /* set the tier (as an operator would in aimee.yaml), save, reload. */
      snprintf(cfg.kb_pdf_tier, sizeof(cfg.kb_pdf_tier), "basic");
      assert(config_save(&cfg) == 0);
      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(strcmp(cfg2.kb_pdf_tier, "basic") == 0);
      assert(cfg2.kb_pdf_ingest_enabled && cfg2.kb_pdf_vector_enabled); /* basic core */
      assert(!cfg2.kb_pdf_tsr_enabled && !cfg2.kb_pdf_assets_enabled &&
             !cfg2.kb_pdf_ocr_enabled); /* heavy stages stay off */
   }

   /* --- install.sh persists provider/openai/kb_client_* as plain top-level
    *     YAML scalars into aimee.yaml (the file config_load reads), matching
    *     aimee's own unquoted emit (cf. db2_url with colons). Regression guard
    *     that the installer's hand-written format parses — these must reach the
    *     server, not land in a config.json it ignores. Uses its own HOME so it
    *     doesn't disturb the shared config the later blocks rely on. --- */
   {
      char kb_home[600];
      snprintf(kb_home, sizeof(kb_home), "%s/kb-yaml-home", tmpdir);
      char kb_dir[700];
      snprintf(kb_dir, sizeof(kb_dir), "%s/.config/aimee", kb_home);
      platform_mkdir_p(kb_dir, 0700);
      char kb_path[800];
      snprintf(kb_path, sizeof(kb_path), "%s/aimee.yaml", kb_dir);
      FILE *fp = fopen(kb_path, "w");
      assert(fp != NULL);
      fputs("provider: openai\n", fp);
      fputs("use_builtin_cli: true\n", fp);
      fputs("openai_endpoint: https://api.openai.com/v1\n", fp);
      fputs("openai_model: gpt-4o\n", fp);
      fputs("kb_client_url: https://kb.example:4010\n", fp);
      fputs("kb_client_bearer_token: tok-xyz-789\n", fp);
      fclose(fp);

      platform_setenv("HOME", kb_home);
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(strcmp(cfg.provider, "openai") == 0);
      assert(strcmp(cfg.openai_endpoint, "https://api.openai.com/v1") == 0);
      assert(strcmp(cfg.openai_model, "gpt-4o") == 0);
      assert(strcmp(cfg.kb_client_url, "https://kb.example:4010") == 0);
      assert(strcmp(cfg.kb_client_bearer_token, "tok-xyz-789") == 0);
      platform_setenv("HOME", tmpdir); /* restore shared test HOME */
   }

   /* --- default db1_path tracks HOME changes and saved defaults stay portable --- */
   {
      char other_home[512];
      char cfgdir[512];
      char src_cfg[512];
      char dst_cfg[512];
      char expected_db[512];
      char buf[4096];

      snprintf(other_home, sizeof(other_home), "%s/aimee-test-config-copy-XXXXXX",
               platform_tmpdir());
      assert(platform_mkdtemp(other_home) != NULL);

      platform_setenv("AIMEE_NO_CACHE", "1");
      platform_setenv("HOME", tmpdir);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      snprintf(expected_db, sizeof(expected_db), "%s/.config/aimee/aimee.db", tmpdir);
      assert(strcmp(cfg.db1_path, expected_db) == 0);
      assert(config_save(&cfg) == 0);

      snprintf(src_cfg, sizeof(src_cfg), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *src = fopen(src_cfg, "r");
      assert(src != NULL);
      size_t nread = fread(buf, 1, sizeof(buf) - 1, src);
      fclose(src);
      buf[nread] = '\0';
      assert(strstr(buf, "db1_path:") == NULL);

      platform_setenv("HOME", other_home);

      static config_t moved_cfg;
      memset(&moved_cfg, 0, sizeof(moved_cfg));
      assert(config_load(&moved_cfg) == 0);
      snprintf(expected_db, sizeof(expected_db), "%s/.config/aimee/aimee.db", other_home);
      assert(strcmp(moved_cfg.db1_path, expected_db) == 0);

      snprintf(cfgdir, sizeof(cfgdir), "%s/.config", other_home);
      assert(platform_test_mkdir(cfgdir, 0700) == 0 || access(cfgdir, F_OK) == 0);
      snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", other_home);
      assert(platform_test_mkdir(cfgdir, 0700) == 0 || access(cfgdir, F_OK) == 0);
      snprintf(dst_cfg, sizeof(dst_cfg), "%s/aimee.yaml", cfgdir);

      FILE *dst = fopen(dst_cfg, "w");
      assert(dst != NULL);
      assert(fwrite(buf, 1, nread, dst) == nread);
      fclose(dst);

      memset(&moved_cfg, 0, sizeof(moved_cfg));
      assert(config_load(&moved_cfg) == 0);
      assert(strcmp(moved_cfg.db1_path, expected_db) == 0);

      platform_setenv("HOME", tmpdir);
      platform_test_rmrf(other_home);
   }

   /* --- explicit db1_path survives save/load --- */
   {
      char custom_db[512];
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);

      snprintf(custom_db, sizeof(custom_db), "%s/custom-aimee.db", tmpdir);
      snprintf(cfg.db1_path, sizeof(cfg.db1_path), "%s", custom_db);
      assert(config_save(&cfg) == 0);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      platform_setenv("AIMEE_NO_CACHE", "1");
      assert(config_load(&cfg2) == 0);
      assert(strcmp(cfg2.db1_path, custom_db) == 0);
   }

   /* --- config_guardrail_mode --- */
   {
      const char *mode = config_guardrail_mode();
      assert(mode != NULL);
      assert(strcmp(mode, "approve") == 0 || strcmp(mode, "prompt") == 0 ||
             strcmp(mode, "deny") == 0);
   }

   /* --- session_id: returns non-empty, stable across calls --- */
   {
      platform_setenv("CLAUDE_SESSION_ID", "test-session-42");
      /* Note: session_id() caches on first call, so this only works
       * if it hasn't been called yet in this process. Since we set the
       * env before any call, it should pick it up. */
   }

   /* --- session_id_refresh: drops cache so a rotated PPID file is picked up.
    * Long-lived MCP processes used to cache session_id forever, so a session
    * rotation (e.g. `aimee session-start` between MCP requests) would leave
    * them reading the previous session's worktree mapping. */
   {
      session_id_clear_override();
      session_id_refresh();
      const char *base = aimee_home();
      assert(base);
      int ppid = (int)platform_getppid();
      assert(ppid > 1);

      char ppid_path[600];
      snprintf(ppid_path, sizeof(ppid_path), "%s/session-ppid-%d", base, ppid);

      FILE *fp = fopen(ppid_path, "w");
      assert(fp);
      fputs("session-aaaa\n", fp);
      fclose(fp);
      const char *sid_a = session_id();
      assert(sid_a && strcmp(sid_a, "session-aaaa") == 0);

      fp = fopen(ppid_path, "w");
      assert(fp);
      fputs("session-bbbb\n", fp);
      fclose(fp);
      /* Without refresh, cached A still wins. */
      assert(strcmp(session_id(), "session-aaaa") == 0);

      session_id_refresh();
      assert(strcmp(session_id(), "session-bbbb") == 0);

      /* Refresh is a no-op when an override is active. */
      session_id_set_override("session-override");
      session_id_refresh();
      assert(strcmp(session_id(), "session-override") == 0);
      session_id_clear_override();

      unlink(ppid_path);
      session_id_refresh();
   }

   /* --- config_default_dir: contains .config/aimee --- */
   {
      const char *dir = config_default_dir();
      assert(dir != NULL);
      assert(strstr(dir, ".config/aimee") != NULL);
   }

   /* --- schema validation: valid config passes --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 0;
      platform_setenv("AIMEE_NO_CACHE", "1"); /* force re-parse */
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(strcmp(cfg.provider, "gemini") == 0); /* from earlier save */
   }

   /* --- schema validation: unknown key produces warning (non-strict) --- */
   {
      /* Write config with unknown key */
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nbogus_key: value\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 0;
      int rc = config_load(&cfg);
      assert(rc == 0); /* warnings only, does not fail */
      assert(strcmp(cfg.provider, "claude") == 0);
   }

   /* --- schema validation: strict mode rejects unknown key --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nbogus_key: value\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 1;
      int rc = config_load(&cfg);
      assert(rc == -1); /* strict mode rejects */
      g_config_strict = 0;
   }

   /* Retired background skill-curator keys remain load-compatible for existing
    * installations, but saving the config must scrub them permanently. */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nskills:\n  curator:\n    enabled: true\n"
                 "  curator_interval_hours: 12\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 1;
      assert(config_load(&cfg) == 0);
      assert(config_save(&cfg) == 0);
      g_config_strict = 0;

      f = fopen(cpath, "r");
      assert(f);
      char saved[65536];
      size_t saved_len = fread(saved, 1, sizeof(saved) - 1, f);
      fclose(f);
      saved[saved_len] = '\0';
      assert(strstr(saved, "curator_interval_hours") == NULL);
      assert(strstr(saved, "skills:\n  curator:") == NULL);
   }

   /* --- schema validation: type mismatch detected --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: 123\n"); /* schema expects string, parser yields integer */
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 1;
      int rc = config_load(&cfg);
      assert(rc == -1); /* type mismatch in strict mode */
      g_config_strict = 0;
   }

   /* schema validation: valid config passes strict mode. Also this PR's fold-key
    * regression: top-level `fold:` is a registered key (loads clean, no "unknown
    * key") AND actually parses (fold_enabled set, not merely allowlisted). */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nuse_builtin_cli: true\nfold:\n  enabled: true\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 1;
      int rc = config_load(&cfg);
      assert(rc == 0); /* all keys valid, incl. the registered top-level fold */
      assert(strcmp(cfg.provider, "claude") == 0);
      assert(cfg.fold_enabled == 1); /* fold section parsed, not merely allowlisted */
      g_config_strict = 0;
   }

   /* --- memory.dispositions: valid nested values parse --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(
          f, "provider: claude\nmemory:\n  dispositions:\n    skepticism: 0.8\n    empathy: 0.3\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      platform_setenv("AIMEE_NO_CACHE", "1");
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.disposition_count == 2);
      assert(cfg.disposition_global_count == 2);
      assert_disposition(&cfg, 0, "skepticism", 0.8, CONFIG_DISPOSITION_SOURCE_GLOBAL);
      assert_disposition(&cfg, 1, "empathy", 0.3, CONFIG_DISPOSITION_SOURCE_GLOBAL);
   }

   /* --- guardrails.semantic: advisory_only parses and defaults true --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      /* legacy quad (enabled + dry_run:false + advisory_only:false + allow_ml_only_block:true)
       * maps onto the "enforce" mode. */
      fprintf(f, "provider: claude\nguardrails:\n  semantic:\n    enabled: true\n    dry_run: "
                 "false\n    advisory_only: false\n    allow_ml_only_block: true\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      platform_setenv("AIMEE_NO_CACHE", "1");
      assert(config_load(&cfg) == 0);
      assert(strcmp(cfg.guardrails_semantic_mode, "enforce") == 0);

      /* legacy `enabled: true` alone -> dry_run (dry_run defaults on when enabled). */
      f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nguardrails:\n  semantic:\n    enabled: true\n");
      fclose(f);
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(strcmp(cfg.guardrails_semantic_mode, "dry_run") == 0);

      /* canonical string form wins. */
      f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nguardrails:\n  semantic:\n    mode: advisory\n");
      fclose(f);
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(strcmp(cfg.guardrails_semantic_mode, "advisory") == 0);
      platform_unsetenv("AIMEE_NO_CACHE");
   }

   /* --- memory.dispositions: scoped overrides merge with source attribution --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nmemory:\n  dispositions:\n    global:\n      skepticism: 0.8\n "
                 "     empathy: 0.3\n    workspace:\n      empathy: 0.6\n    project:\n      "
                 "literalism: 0.5\n      skepticism: 0.2\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      platform_setenv("AIMEE_NO_CACHE", "1");
      assert(config_load(&cfg) == 0);
      assert(cfg.disposition_global_count == 2);
      assert(cfg.disposition_workspace_count == 1);
      assert(cfg.disposition_project_count == 2);
      assert(cfg.disposition_count == 3);
      assert_disposition(&cfg, 0, "skepticism", 0.2, CONFIG_DISPOSITION_SOURCE_PROJECT);
      assert_disposition(&cfg, 1, "empathy", 0.6, CONFIG_DISPOSITION_SOURCE_WORKSPACE);
      assert_disposition(&cfg, 2, "literalism", 0.5, CONFIG_DISPOSITION_SOURCE_PROJECT);
   }

   /* --- config_save preserves scoped dispositions --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.disposition_globals[0].value = 0.8;
      cfg.disposition_globals[0].source = CONFIG_DISPOSITION_SOURCE_GLOBAL;
      snprintf(cfg.disposition_globals[0].name, sizeof(cfg.disposition_globals[0].name), "%s",
               "skepticism");
      cfg.disposition_global_count = 1;
      cfg.disposition_workspaces[0].value = 0.6;
      cfg.disposition_workspaces[0].source = CONFIG_DISPOSITION_SOURCE_WORKSPACE;
      snprintf(cfg.disposition_workspaces[0].name, sizeof(cfg.disposition_workspaces[0].name), "%s",
               "empathy");
      cfg.disposition_workspace_count = 1;
      cfg.disposition_projects[0].value = 0.5;
      cfg.disposition_projects[0].source = CONFIG_DISPOSITION_SOURCE_PROJECT;
      snprintf(cfg.disposition_projects[0].name, sizeof(cfg.disposition_projects[0].name), "%s",
               "literalism");
      cfg.disposition_project_count = 1;
      cfg.dispositions[0] = cfg.disposition_globals[0];
      cfg.dispositions[1] = cfg.disposition_workspaces[0];
      cfg.dispositions[2] = cfg.disposition_projects[0];
      cfg.disposition_count = 3;
      config_save(&cfg);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      platform_setenv("AIMEE_NO_CACHE", "1");
      assert(config_load(&cfg2) == 0);
      assert(cfg2.disposition_global_count == 1);
      assert(cfg2.disposition_workspace_count == 1);
      assert(cfg2.disposition_project_count == 1);
      assert_disposition(&cfg2, 0, "skepticism", 0.8, CONFIG_DISPOSITION_SOURCE_GLOBAL);
      assert_disposition(&cfg2, 1, "empathy", 0.6, CONFIG_DISPOSITION_SOURCE_WORKSPACE);
      assert_disposition(&cfg2, 2, "literalism", 0.5, CONFIG_DISPOSITION_SOURCE_PROJECT);
   }

   /* --- memory.dispositions: strict mode rejects wrong types --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nmemory:\n  dispositions:\n    skepticism: yes\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 1;
      platform_setenv("AIMEE_NO_CACHE", "1");
      int rc = config_load(&cfg);
      assert(rc == -1);
      g_config_strict = 0;
   }

   /* --- memory.dispositions: strict mode rejects out-of-range values --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nmemory:\n  dispositions:\n    skepticism: 1.5\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 1;
      platform_setenv("AIMEE_NO_CACHE", "1");
      int rc = config_load(&cfg);
      assert(rc == -1);
      g_config_strict = 0;
   }

   /* --- kb.curator.cross_repo_graph: strict mode rejects out-of-range knobs --- */
   {
      const char *bad[] = {
          "    p_pct: 101\n", /* > 100 */
          "    k: 0\n",       /* not positive */
          "    query_timeout_ms: 0\n",
          "    max_candidates: -1\n",
          "    len_min: 99999\n", /* > 1024 ceiling */
      };
      for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
      {
         char cpath[512];
         snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
         FILE *f = fopen(cpath, "w");
         assert(f);
         fprintf(f, "provider: claude\nkb:\n  curator:\n    cross_repo_graph:\n%s", bad[i]);
         fclose(f);

         static config_t cfg;
         memset(&cfg, 0, sizeof(cfg));
         g_config_strict = 1;
         platform_setenv("AIMEE_NO_CACHE", "1");
         int rc = config_load(&cfg);
         assert(rc == -1); /* each pathological value must be rejected in strict mode */
         g_config_strict = 0;
      }
   }

   /* --- memory.citations: valid nested values parse --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nmemory:\n  citations:\n    mode: required\n    "
                 "reprompt_on_miss: true\n    strip_unverified: false\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      platform_setenv("AIMEE_NO_CACHE", "1");
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(strcmp(cfg.memory_citations_mode, "required") == 0);
      assert(cfg.memory_citations_reprompt_on_miss == 1);
      assert(cfg.memory_citations_strip_unverified == 0);
   }

   /* --- memory.cognify.async: valid nested flag parses --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nmemory:\n  cognify:\n    async:\n      enabled: true\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      platform_setenv("AIMEE_NO_CACHE", "1");
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.memory_cognify_async_enabled == 1);
   }

   /* --- memory.cognify.async: strict mode rejects wrong types --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nmemory:\n  cognify:\n    async:\n      enabled: maybe\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 1;
      platform_setenv("AIMEE_NO_CACHE", "1");
      int rc = config_load(&cfg);
      assert(rc == -1);
      g_config_strict = 0;
   }

   /* --- memory.citations: strict mode rejects invalid mode --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nmemory:\n  citations:\n    mode: always\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 1;
      platform_setenv("AIMEE_NO_CACHE", "1");
      int rc = config_load(&cfg);
      assert(rc == -1);
      g_config_strict = 0;
   }

   /* --- max_iterations: defaults to 0 (use compile-time default) --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.max_iterations == 0);          /* not set = 0 */
      assert(cfg.max_iterations_delegate == 0); /* not set = 0 */
   }

   /* --- max_iterations: parsed from config --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nmax_iterations: 10\nmax_iterations_delegate: 30\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.max_iterations == 10);
      assert(cfg.max_iterations_delegate == 30);
   }

   /* --- max_iterations: round-trip save/load --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.max_iterations = 5;
      cfg.max_iterations_delegate = 50;
      config_save(&cfg);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.max_iterations == 5);
      assert(cfg2.max_iterations_delegate == 50);
   }

   /* --- max_iterations: effective defaults --- */
   {
      /* When config value is 0, code should use compile-time defaults */
      assert(CONFIG_DEFAULT_MAX_ITERATIONS == 15);
      assert(CONFIG_DEFAULT_MAX_ITERATIONS_DELEGATE == 25);

      /* Simulate the effective calculation used in chat loops */
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int effective = cfg.max_iterations > 0 ? cfg.max_iterations : CONFIG_DEFAULT_MAX_ITERATIONS;
      assert(effective == 15);

      cfg.max_iterations = 8;
      effective = cfg.max_iterations > 0 ? cfg.max_iterations : CONFIG_DEFAULT_MAX_ITERATIONS;
      assert(effective == 8);
   }

   /* --- background_threads: parsed from preferred config key --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nbackground_threads: 6\nsession_threads: 4\n");
      fclose(f);
      platform_setenv("AIMEE_NO_CACHE", "1");

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.compute_threads == 6);
      assert(cfg.session_threads == 4);
      platform_unsetenv("AIMEE_BACKGROUND_THREADS");
      platform_unsetenv("AIMEE_COMPUTE_THREADS");
      platform_unsetenv("AIMEE_SESSION_THREADS");
      assert(aimee_default_compute_threads() == CONFIG_DEFAULT_BACKGROUND_THREADS);
      assert(aimee_default_session_threads() == CONFIG_DEFAULT_SESSION_THREADS);
      assert(aimee_resolve_compute_threads(0) == CONFIG_DEFAULT_BACKGROUND_THREADS);
      assert(aimee_resolve_session_threads(0) == CONFIG_DEFAULT_SESSION_THREADS);
   }

   /* --- delegate_max_inflight: on-demand delegate backstop ceiling --- */
   {
      platform_unsetenv("AIMEE_DELEGATE_MAX_INFLIGHT");
      /* Unconfigured -> default ceiling; configured -> honored. */
      assert(aimee_resolve_delegate_max_inflight(0) == CONFIG_DEFAULT_DELEGATE_MAX_INFLIGHT);
      assert(aimee_resolve_delegate_max_inflight(2048) == 2048);
      /* Env override wins over both. */
      assert(platform_setenv("AIMEE_DELEGATE_MAX_INFLIGHT", "777") == 0);
      assert(aimee_resolve_delegate_max_inflight(0) == 777);
      assert(aimee_resolve_delegate_max_inflight(2048) == 777);
      /* Non-positive / garbage env is ignored (falls back to configured/default). */
      assert(platform_setenv("AIMEE_DELEGATE_MAX_INFLIGHT", "0") == 0);
      assert(aimee_resolve_delegate_max_inflight(2048) == 2048);
      assert(platform_setenv("AIMEE_DELEGATE_MAX_INFLIGHT", "abc") == 0);
      assert(aimee_resolve_delegate_max_inflight(0) == CONFIG_DEFAULT_DELEGATE_MAX_INFLIGHT);
      platform_unsetenv("AIMEE_DELEGATE_MAX_INFLIGHT");
   }

   /* --- background_threads: accepts legacy compute_threads/worker_threads keys --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\ncompute_threads: 5\n");
      fclose(f);
      platform_setenv("AIMEE_NO_CACHE", "1");

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.compute_threads == 5);

      f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nworker_threads: 3\n");
      fclose(f);
      platform_setenv("AIMEE_NO_CACHE", "1");

      memset(&cfg, 0, sizeof(cfg));
      rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.compute_threads == 3);
   }

   /* --- background/session threads: round-trip save uses preferred keys --- */
   {
      char cpath[512];
      char buf[4096];

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.compute_threads = 7;
      cfg.session_threads = 6;
      config_save(&cfg);

      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "r");
      assert(f);
      size_t nread = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      buf[nread] = '\0';
      assert(strstr(buf, "background_threads: 7") != NULL);
      assert(strstr(buf, "session_threads: 6") != NULL);
      assert(strstr(buf, "compute_threads:") == NULL);
      assert(strstr(buf, "worker_threads:") == NULL);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.compute_threads == 7);
      assert(cfg2.session_threads == 6);
   }

   /* --- autonomous: defaults to 0 --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.autonomous == 0);
   }

   /* --- autonomous: parsed from config --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nautonomous: true\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.autonomous == 1);
   }

   /* --- autonomous: round-trip save/load --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.autonomous = 1;
      config_save(&cfg);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.autonomous == 1);

      /* Reset and verify false round-trip */
      cfg2.autonomous = 0;
      config_save(&cfg2);
      static config_t cfg3;
      memset(&cfg3, 0, sizeof(cfg3));
      config_load(&cfg3);
      assert(cfg3.autonomous == 0);
   }

   /* --- sessions: defaults to 0 (use compile-time default) --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.worktree_stale_secs ==
             0); /* unset = 0; effective default is CONFIG_DEFAULT_STALE_SESSION_SECS */
      assert(cfg.max_sessions == 0);
      assert(cfg.max_worktrees == 0);
      /* Verify compile-time default constant */
      assert(CONFIG_DEFAULT_STALE_SESSION_SECS == 14400);
   }

   /* --- sessions: parsed from config --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n"
                 "sessions:\n"
                 "  stale_threshold_secs: 7200\n"
                 "  max_sessions: 5\n"
                 "  max_worktrees: 10\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.worktree_stale_secs == 7200);
      assert(cfg.max_sessions == 5);
      assert(cfg.max_worktrees == 10);
   }

   /* --- sessions: round-trip save/load --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.worktree_stale_secs = 3600;
      cfg.max_sessions = 3;
      cfg.max_worktrees = 6;
      config_save(&cfg);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.worktree_stale_secs == 3600);
      assert(cfg2.max_sessions == 3);
      assert(cfg2.max_worktrees == 6);
   }

   /* --- sessions: effective stale threshold --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int effective = (cfg.worktree_stale_secs > 0) ? cfg.worktree_stale_secs
                                                    : CONFIG_DEFAULT_STALE_SESSION_SECS;
      assert(effective == CONFIG_DEFAULT_STALE_SESSION_SECS);

      cfg.worktree_stale_secs = 1800;
      effective = (cfg.worktree_stale_secs > 0) ? cfg.worktree_stale_secs
                                                : CONFIG_DEFAULT_STALE_SESSION_SECS;
      assert(effective == 1800);
   }

   /* --- sandbox: parsed from config --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n"
                 "sandbox:\n"
                 "  mode: workspace_only\n"
                 "  network: true\n"
                 "  allow_paths:\n"
                 "    - /tmp/alpha\n"
                 "    - /tmp/beta\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.sandbox.mode == SANDBOX_MODE_WORKSPACE_ONLY);
      assert(cfg.sandbox.network_isolated == 1);
      assert(cfg.sandbox.allow_path_count == 2);
      assert(strcmp(cfg.sandbox.allow_paths[0], "/tmp/alpha") == 0);
      assert(strcmp(cfg.sandbox.allow_paths[1], "/tmp/beta") == 0);
   }

   /* --- sandbox: round-trip save/load --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.sandbox.mode = SANDBOX_MODE_ALLOWLIST;
      cfg.sandbox.network_isolated = 1;
      cfg.sandbox.allow_path_count = 2;
      snprintf(cfg.sandbox.allow_paths[0], sizeof(cfg.sandbox.allow_paths[0]), "%s", "/opt/a");
      snprintf(cfg.sandbox.allow_paths[1], sizeof(cfg.sandbox.allow_paths[1]), "%s", "/opt/b");
      config_save(&cfg);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.sandbox.mode == SANDBOX_MODE_ALLOWLIST);
      assert(cfg2.sandbox.network_isolated == 1);
      assert(cfg2.sandbox.allow_path_count == 2);
      assert(strcmp(cfg2.sandbox.allow_paths[0], "/opt/a") == 0);
      assert(strcmp(cfg2.sandbox.allow_paths[1], "/opt/b") == 0);
   }

   /* --- sandbox mode: defaults, opt-out persistence, and bad input ---
    *
    * Read through config_sandbox() rather than a config_t. check-config-encapsulation
    * ratchets config_t exposure in this file DOWNWARD — the baseline is debt, not
    * headroom — so these cases go through the accessor the checker steers callers to.
    * That is also the honest surface: the delegate shell guard in tool_bash() reads
    * config_sandbox(), not a config_t.
    *
    * Deliberately NOT via config_reload(): publishing a snapshot makes every later
    * config_load() in the process return that snapshot instead of the file
    * (config.c:1128), which silently breaks any test after this one. With no snapshot
    * live, config_sandbox() heap-loads from disk, which is what these cases want.
    * AIMEE_NO_CACHE defeats the stat-keyed load cache so successive rewrites of the
    * same path are always re-read. */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      sandbox_config_t sb;
      setenv("AIMEE_NO_CACHE", "1", 1);

      /* DEFAULT-ON with no sandbox section. The guard refuses a delegated shell
       * whenever the mode is OFF, so while this defaulted to the zero value it
       * refused every co-located delegate shell on an unconfigured install. */
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n");
      fclose(f);
      memset(&sb, 0, sizeof(sb));
      config_sandbox(&sb);
      assert(sb.mode == SANDBOX_MODE_WORKSPACE_ONLY);

      /* The explicit opt-out SURVIVES a save. config_save persists the sandbox block
       * only when it differs from the default; with the default flipped, testing that
       * predicate against OFF would drop an operator's "off" on the next save and
       * silently re-enable the sandbox. config_set_* performs a real load/modify/save
       * cycle, so it exercises the persistence path without naming config_t. */
      f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n"
                 "sandbox:\n"
                 "  mode: off\n");
      fclose(f);
      memset(&sb, 0, sizeof(sb));
      config_sandbox(&sb);
      assert(sb.mode == SANDBOX_MODE_OFF); /* opt-out parsed */

      assert(config_set_fold_enabled(0) == 0); /* forces a save of the whole config */
      memset(&sb, 0, sizeof(sb));
      config_sandbox(&sb);
      assert(sb.mode == SANDBOX_MODE_OFF); /* opt-out still honoured after the save */

      /* An unknown mode string keeps the default and never downgrades.
       * sandbox_mode_from_string() maps anything unrecognized to OFF, which would
       * silently disable isolation on a typo now that the default is on. */
      f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n"
                 "sandbox:\n"
                 "  mode: wokspace_only\n");
      fclose(f);
      memset(&sb, 0, sizeof(sb));
      config_sandbox(&sb);
      assert(sb.mode == SANDBOX_MODE_WORKSPACE_ONLY);

      /* The loose leading-character parse is preserved, so configs saying "allow"
       * or "workspace" do not regress into the default. */
      f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n"
                 "sandbox:\n"
                 "  mode: allow\n");
      fclose(f);
      memset(&sb, 0, sizeof(sb));
      config_sandbox(&sb);
      assert(sb.mode == SANDBOX_MODE_ALLOWLIST);

      unsetenv("AIMEE_NO_CACHE");
   }

   /* --- compact config: defaults --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      /* compact_enabled defaults to 1 (on) */
      assert(cfg.compact_enabled == 1);
      /* other fields default to 0 (use built-in compact.h defaults) */
      assert(cfg.compact_threshold == 0);
      assert(cfg.compact_head_bytes == 0);
      assert(cfg.compact_tail_bytes == 0);
      assert(cfg.compact_per_tool_count == 0);
   }

   /* --- compact config: save and load round-trip --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.compact_enabled = 1;
      cfg.compact_threshold = 8192;
      cfg.compact_head_bytes = 256;
      cfg.compact_tail_bytes = 512;
      /* Add a per-tool override */
      snprintf(cfg.compact_per_tool[0], sizeof(cfg.compact_per_tool[0]), "read_file=2048");
      cfg.compact_per_tool_count = 1;
      config_save(&cfg);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.compact_enabled == 1);
      assert(cfg2.compact_threshold == 8192);
      assert(cfg2.compact_head_bytes == 256);
      assert(cfg2.compact_tail_bytes == 512);
      assert(cfg2.compact_per_tool_count == 1);
      assert(strncmp(cfg2.compact_per_tool[0], "read_file=2048", 14) == 0);
   }

   /* --- memory.salience: parse nested config --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "memory:\n"
                 "  salience:\n"
                 "    enabled: true\n"
                 "    weight: 0.75\n"
                 "    window_size: 6\n"
                 "    surprise_enabled: true\n"
                 "    surprise_weight: 1.4\n"
                 "  pagerank:\n"
                 "    enabled: true\n"
                 "    iterations: 7\n"
                 "    weight: 0.45\n"
                 "    relations: depends_on,related_to\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.memory_salience_enabled == 1);
      assert(cfg.memory_salience_window_size == 6);
      assert(cfg.memory_surprise_enabled == 1);
      assert(cfg.memory_pagerank_enabled == 1);
      assert(cfg.memory_pagerank_iterations == 7);
      assert(strcmp(cfg.memory_pagerank_relations, "depends_on,related_to") == 0);
   }

   /* --- memory.salience: round-trip save/load --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.memory_salience_enabled = 1;
      cfg.memory_salience_weight = 0.6;
      cfg.memory_salience_window_size = 10;
      cfg.memory_surprise_enabled = 1;
      cfg.memory_surprise_weight = 0.9;
      cfg.memory_pagerank_enabled = 1;
      cfg.memory_pagerank_iterations = 9;
      cfg.memory_pagerank_weight = 0.5;
      snprintf(cfg.memory_pagerank_relations, sizeof(cfg.memory_pagerank_relations), "%s",
               "depends_on,fixes");
      config_save(&cfg);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.memory_salience_enabled == 1);
      assert(cfg2.memory_salience_window_size == 10);
      assert(cfg2.memory_surprise_enabled == 1);
      assert(cfg2.memory_pagerank_enabled == 1);
      assert(cfg2.memory_pagerank_iterations == 9);
      assert(strcmp(cfg2.memory_pagerank_relations, "depends_on,fixes") == 0);
   }

   /* --- memory_query_expansion: defaults, parse, and save/load round-trip --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(cfg.memory_query_expansion_mode[0] == '\0');
      assert(cfg.memory_query_expansion_k == 0);

      f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n"
                 "memory_query_expansion:\n"
                 "  mode: semantic\n"
                 "  k: 5\n");
      fclose(f);
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(strcmp(cfg.memory_query_expansion_mode, "semantic") == 0);
      assert(cfg.memory_query_expansion_k == 5);

      cfg.memory_query_expansion_k = 8;
      config_save(&cfg);
      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(strcmp(cfg2.memory_query_expansion_mode, "semantic") == 0);
      assert(cfg2.memory_query_expansion_k == 8);
   }

   /* --- mcp_clients: parsed from config --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n"
                 "mcp_clients:\n"
                 "  - name: github\n"
                 "    transport: stdio\n"
                 "    command:\n"
                 "      - github-mcp-server\n"
                 "      - --stdio\n"
                 "    cwd: /tmp/github\n"
                 "  - name: grafana\n"
                 "    transport: sse\n"
                 "    url: https://grafana.example.com/mcp\n"
                 "    bearer_token_env: GRAFANA_TOKEN\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(cfg.mcp_client_count == 2);
      assert(strcmp(cfg.mcp_clients[0].name, "github") == 0);
      assert(cfg.mcp_clients[0].transport == CONFIG_MCP_TRANSPORT_STDIO);
      assert(cfg.mcp_clients[0].command_count == 2);
      assert(strcmp(cfg.mcp_clients[0].command[0], "github-mcp-server") == 0);
      assert(strcmp(cfg.mcp_clients[0].command[1], "--stdio") == 0);
      assert(strcmp(cfg.mcp_clients[0].cwd, "/tmp/github") == 0);
      assert(strcmp(cfg.mcp_clients[1].name, "grafana") == 0);
      assert(cfg.mcp_clients[1].transport == CONFIG_MCP_TRANSPORT_SSE);
      assert(strcmp(cfg.mcp_clients[1].url, "https://grafana.example.com/mcp") == 0);
      assert(strcmp(cfg.mcp_clients[1].bearer_token_env, "GRAFANA_TOKEN") == 0);
   }

   /* --- mcp_clients: round-trip save/load --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.mcp_client_count = 1;
      snprintf(cfg.mcp_clients[0].name, sizeof(cfg.mcp_clients[0].name), "%s", "mock");
      cfg.mcp_clients[0].transport = CONFIG_MCP_TRANSPORT_STDIO;
      cfg.mcp_clients[0].command_count = 2;
      snprintf(cfg.mcp_clients[0].command[0], sizeof(cfg.mcp_clients[0].command[0]), "%s",
               "mock-mcp-server");
      snprintf(cfg.mcp_clients[0].command[1], sizeof(cfg.mcp_clients[0].command[1]), "%s", "happy");
      snprintf(cfg.mcp_clients[0].cwd, sizeof(cfg.mcp_clients[0].cwd), "%s", "/tmp/mock");
      config_save(&cfg);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      assert(config_load(&cfg2) == 0);
      assert(cfg2.mcp_client_count == 1);
      assert(strcmp(cfg2.mcp_clients[0].name, "mock") == 0);
      assert(cfg2.mcp_clients[0].transport == CONFIG_MCP_TRANSPORT_STDIO);
      assert(cfg2.mcp_clients[0].command_count == 2);
      assert(strcmp(cfg2.mcp_clients[0].command[0], "mock-mcp-server") == 0);
      assert(strcmp(cfg2.mcp_clients[0].command[1], "happy") == 0);
      assert(strcmp(cfg2.mcp_clients[0].cwd, "/tmp/mock") == 0);
   }

   /* --- computer_use: parsed from config and round-trip save/load --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "computer_use:\n"
                 "  enabled: true\n"
                 "  default_navigation: block\n"
                 "  redact_sensitive_screenshots: false\n"
                 "  allowed_domains:\n"
                 "    - localhost\n"
                 "    - '*.internal.example'\n");
      fclose(f);
      platform_unsetenv("AIMEE_NO_CACHE");

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(cfg.computer_use_enabled == 1);
      assert(strcmp(cfg.computer_use_default_navigation, "block") == 0);
      assert(cfg.computer_use_redact_sensitive_screenshots == 0);
      assert(cfg.computer_use_allowed_domain_count == 2);
      assert(strcmp(cfg.computer_use_allowed_domains[1], "*.internal.example") == 0);

      config_save(&cfg);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      assert(config_load(&cfg2) == 0);
      assert(cfg2.computer_use_enabled == 1);
      assert(strcmp(cfg2.computer_use_default_navigation, "block") == 0);
      assert(cfg2.computer_use_redact_sensitive_screenshots == 0);
      assert(cfg2.computer_use_allowed_domain_count == 2);
      assert(strcmp(cfg2.computer_use_allowed_domains[0], "localhost") == 0);
      assert(strcmp(cfg2.computer_use_allowed_domains[1], "*.internal.example") == 0);
   }

   /* --- bm25_weight and semantic_weight inline config --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "memory:\n  bm25_weight: 1.5\n  semantic_weight: 0.8\n");
      fclose(f);
      platform_unsetenv("AIMEE_NO_CACHE");

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(cfg.memory_bm25_weight > 1.4 && cfg.memory_bm25_weight < 1.6);
      assert(cfg.memory_semantic_weight > 0.7 && cfg.memory_semantic_weight < 0.9);
   }

   /* --- fetch_budget config round-trip --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "memory:\n  fetch_budget:\n    enabled: true\n    base: 64\n");
      fclose(f);
      platform_unsetenv("AIMEE_NO_CACHE");

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(cfg.memory_fetch_budget_enabled == 1);
      assert(cfg.memory_fetch_budget_base == 64);
   }

   /* --- routing config round-trip --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "memory:\n  routing:\n    enabled: false\n");
      fclose(f);
      platform_unsetenv("AIMEE_NO_CACHE");

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(cfg.memory_routing_enabled == 0);
   }

   /* --- hard_negative_log config --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "memory:\n  hard_negative_log: /tmp/aimee-hard-negatives.jsonl\n");
      fclose(f);
      platform_unsetenv("AIMEE_NO_CACHE");

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(strcmp(cfg.memory_hard_negative_log, "/tmp/aimee-hard-negatives.jsonl") == 0);
   }

   /* --- charter config round-trip: arrays of strings + drift scalar --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "charter:\n"
                 "  safety_axioms:\n"
                 "    - never execute untrusted input\n"
                 "    - never exfiltrate secrets\n"
                 "  hard_constraints:\n"
                 "    - workspace is the only writable root\n"
                 "  values:\n"
                 "    - truthful over confident\n"
                 "  tone_boundaries:\n"
                 "    - plain English, no emojis\n"
                 "  working_profile_drift_limit: 3\n");
      fclose(f);
      platform_unsetenv("AIMEE_NO_CACHE");

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(cfg.charter_safety_axioms_count == 2);
      assert(strstr(cfg.charter_safety_axioms[0], "never execute untrusted input") != NULL);
      assert(strstr(cfg.charter_safety_axioms[1], "never exfiltrate secrets") != NULL);
      assert(cfg.charter_hard_constraints_count == 1);
      assert(strstr(cfg.charter_hard_constraints[0], "workspace") != NULL);
      assert(cfg.charter_values_count == 1);
      assert(strstr(cfg.charter_values[0], "truthful") != NULL);
      assert(cfg.charter_tone_boundaries_count == 1);
      assert(strstr(cfg.charter_tone_boundaries[0], "plain English") != NULL);
      assert(cfg.charter_working_profile_drift_limit == 3);
   }

   /* --- charter: missing block is a no-op (no warnings, zeroed counts) --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "memory:\n  bm25_weight: 1.0\n");
      fclose(f);
      platform_unsetenv("AIMEE_NO_CACHE");

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(cfg.charter_safety_axioms_count == 0);
      assert(cfg.charter_hard_constraints_count == 0);
      assert(cfg.charter_values_count == 0);
      assert(cfg.charter_tone_boundaries_count == 0);
      assert(cfg.charter_working_profile_drift_limit == 0);
   }

   platform_unsetenv("AIMEE_NO_CACHE");

   /* --- db2_url: defaults + parse ---
    * DB2 is the shared relational tier in the explicit three-store architecture. */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      unlink(cpath); /* defaults only */
      platform_unsetenv("AIMEE_NO_CACHE");

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      assert(cfg.db2_url[0] == '\0');
   }

   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "db2_url: db2://user@host:5432/aimee\n");
      fclose(f);
      platform_unsetenv("AIMEE_NO_CACHE");

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(strcmp(cfg.db2_url, "db2://user@host:5432/aimee") == 0);
      unlink(cpath);
   }

   /* Legacy credentials are handed to the injected Vault writer and removed
    * from the file in the same migration. The config module owns its struct
    * layout; the Vault adapter never reaches into config_t. */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "db2_url: postgresql://legacy-user:legacy-pass@db/aimee\n");
      fprintf(f, "aimee:\n  api:\n    bearer_token: legacy-test-bearer\n");
      fclose(f);
      memset(migrated_db2, 0, sizeof(migrated_db2));
      memset(migrated_api_bearer, 0, sizeof(migrated_api_bearer));
      assert(config_migrate_legacy_credentials(capture_migrated_secret,
                                               no_migrated_secret_present) == 1);
      assert(strcmp(migrated_db2, "postgresql://legacy-user:legacy-pass@db/aimee") == 0);
      assert(strcmp(migrated_api_bearer, "legacy-test-bearer") == 0);
      f = fopen(cpath, "r");
      assert(f);
      char persisted[8192];
      size_t persisted_len = fread(persisted, 1, sizeof(persisted) - 1, f);
      persisted[persisted_len] = '\0';
      fclose(f);
      assert(strstr(persisted, "legacy-pass") == NULL);
      assert(strstr(persisted, "legacy-test-bearer") == NULL);
      runtime_secret_wipe(migrated_db2, sizeof(migrated_db2));
      runtime_secret_wipe(migrated_api_bearer, sizeof(migrated_api_bearer));
      unlink(cpath);
   }

   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      snprintf(cfg.db2_url, sizeof(cfg.db2_url), "postgres:///aimee_shared");
      cfg.db2_pool_size = 16;
      assert(config_save(&cfg) == 0);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      platform_setenv("AIMEE_NO_CACHE", "1");
      assert(config_load_file(&cfg2) == 0);
      platform_unsetenv("AIMEE_NO_CACHE");
      assert(cfg2.db2_url[0] == '\0');
      assert(cfg2.db2_pool_size == 16);

      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      unlink(cpath);
   }

   /* --- cache identity: a same-mtime in-place rewrite must NOT serve stale ---
    * The load cache used to key on path+mtime alone, so an in-place rewrite that
    * landed with an equal (or clock-skewed) mtime kept serving the pre-rewrite
    * snapshot forever. This is exactly the shape of `aimee workspace add` on the
    * tiered appliance filesystem: the file grows a `workspaces:` block but the
    * stale empty-workspaces cache is served and a later config_save re-serialises
    * it back to nothing. Size + inode in the cache key make it detectable. */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);

      /* First state: short body. Load with caching on to populate the cache. */
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "db2_url: db2://a\n");
      fclose(f);
      platform_unsetenv("AIMEE_NO_CACHE");

      static config_t cfg1;
      memset(&cfg1, 0, sizeof(cfg1));
      assert(config_load(&cfg1) == 0);
      assert(strcmp(cfg1.db2_url, "db2://a") == 0);

      /* Capture the exact mtime the cache just recorded. */
      struct stat st0;
      assert(stat(cpath, &st0) == 0);

      /* Rewrite in place with a longer, different body (a size change, like an
       * appended workspaces block), then force the mtime back to the cached one
       * so path+mtime alone would still count as a hit. */
      f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "db2_url: db2://a-much-longer-different-value\n");
      fclose(f);
      struct timespec mt0;
#if defined(__APPLE__)
      mt0 = st0.st_mtimespec;
#else
      mt0 = st0.st_mtim;
#endif
      struct timespec times[2];
      times[0] = mt0; /* atime — value irrelevant */
      times[1] = mt0; /* mtime — force the collision */
      assert(utimensat(AT_FDCWD, cpath, times, 0) == 0);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      assert(config_load(&cfg2) == 0);
      /* With the mtime-only cache this returned the stale "db2://a". */
      assert(strcmp(cfg2.db2_url, "db2://a-much-longer-different-value") == 0);

      platform_setenv("AIMEE_NO_CACHE", "1");
      unlink(cpath);
   }

   /* --- delegate_sandbox_package_access: default proxy, round-trip, validation --- */
   {
      assert(config_sandbox_package_access_valid("proxy"));
      assert(config_sandbox_package_access_valid("off"));
      assert(config_sandbox_package_access_valid("gated"));
      assert(config_sandbox_package_access_valid("governance"));
      assert(!config_sandbox_package_access_valid("bogus"));
      assert(!config_sandbox_package_access_valid(NULL));

      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      unlink(cpath);
      platform_setenv("AIMEE_NO_CACHE", "1");

      static config_t def;
      memset(&def, 0, sizeof(def));
      assert(config_load(&def) == 0);
      assert(strcmp(def.delegate_sandbox_package_access, "proxy") == 0); /* default */

      /* Round-trip a non-default value. */
      snprintf(def.delegate_sandbox_package_access, sizeof(def.delegate_sandbox_package_access),
               "gated");
      assert(config_save(&def) == 0);
      static config_t got;
      memset(&got, 0, sizeof(got));
      assert(config_load(&got) == 0);
      assert(strcmp(got.delegate_sandbox_package_access, "gated") == 0);

      /* An unknown value in the file is ignored — the default stands. (The stderr
       * warning itself is not captured here; only the default-preserving behavior.) */
      FILE *f = fopen(cpath, "w");
      assert(f);
      fputs("delegate_sandbox_package_access: nonsense\n", f);
      fclose(f);
      static config_t bad;
      memset(&bad, 0, sizeof(bad));
      assert(config_load(&bad) == 0);
      assert(strcmp(bad.delegate_sandbox_package_access, "proxy") == 0);

      /* An explicitly empty value is also invalid -> keep the default. */
      f = fopen(cpath, "w");
      assert(f);
      fputs("delegate_sandbox_package_access: \"\"\n", f);
      fclose(f);
      static config_t empty;
      memset(&empty, 0, sizeof(empty));
      assert(config_load(&empty) == 0);
      assert(strcmp(empty.delegate_sandbox_package_access, "proxy") == 0);
      unlink(cpath);
   }

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   else
   {
      platform_unsetenv("HOME");
   }
   if (old_aimee_home)
   {
      platform_setenv("AIMEE_HOME", old_aimee_home);
      free(old_aimee_home);
   }
   else
   {
      platform_unsetenv("AIMEE_HOME");
   }
   if (old_no_cache)
   {
      platform_setenv("AIMEE_NO_CACHE", old_no_cache);
      free(old_no_cache);
   }
   else
   {
      platform_unsetenv("AIMEE_NO_CACHE");
   }
   platform_test_rmrf(tmpdir);

   /* --- operating mode: AIMEE_MODE env override resolves the mode --- */
   {
      char *old_mode = getenv("AIMEE_MODE");
      char *saved = old_mode ? strdup(old_mode) : NULL;
      platform_setenv("AIMEE_MODE", "novel");
      assert(config_current_mode() == AIMEE_MODE_NOVEL);
      platform_setenv("AIMEE_MODE", "NOVEL");
      assert(config_current_mode() == AIMEE_MODE_NOVEL); /* case-insensitive */
      platform_setenv("AIMEE_MODE", "engineer");
      assert(config_current_mode() == AIMEE_MODE_ENGINEER);
      platform_setenv("AIMEE_MODE", "nonsense");
      assert(config_current_mode() == AIMEE_MODE_ENGINEER); /* unknown -> default */
      if (saved)
      {
         platform_setenv("AIMEE_MODE", saved);
         free(saved);
      }
      else
         platform_unsetenv("AIMEE_MODE");
   }

   /* --- config_current_persona: the persona injected into the session-start
    * brief is the CONFIGURED one, not a hardcoded 'engineer'. `engineer` is only
    * the DEFAULT; it must be operator-selectable via AIMEE_MODE (or the mode
    * file), including ARBITRARY/custom persona names (not just the built-in enum).
    * Guards the persona-selection side of the session brief from regressing to
    * engineer-only. */
   {
      char *old_mode = getenv("AIMEE_MODE");
      char *saved = old_mode ? strdup(old_mode) : NULL;
      char p[64];
      platform_setenv("AIMEE_MODE", "reviewer");
      config_current_persona(p, sizeof(p));
      assert(strcmp(p, "reviewer") == 0); /* a non-default built-in persona */
      platform_setenv("AIMEE_MODE", "my-custom-persona");
      config_current_persona(p, sizeof(p));
      assert(strcmp(p, "my-custom-persona") == 0); /* arbitrary name, verbatim */
      platform_setenv("AIMEE_MODE", "engineer");
      config_current_persona(p, sizeof(p));
      assert(strcmp(p, "engineer") == 0); /* the default is selectable explicitly too */
      if (saved)
      {
         platform_setenv("AIMEE_MODE", saved);
         free(saved);
      }
      else
         platform_unsetenv("AIMEE_MODE");
   }

   /* --- Vault-hydrated AIMEE_DB2_URL overrides a cached config-file db2_url ---
    * Regression for the kb IP-drift outage: when Postgres is recreated on a new
    * bridge IP the runtime injects the current address via AIMEE_DB2_URL, which
    * must win over the stale value persisted in aimee.yaml. */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      snprintf(cfg.db2_url, sizeof(cfg.db2_url),
               "postgresql://aimee:aimee@10.0.0.9:5432/aimee_shared");

      /* Vault runtime value overrides the cached file value. */
      assert(runtime_secret_store("AIMEE_DB2_URL",
                                  "postgresql://aimee:aimee@10.0.0.16:5432/aimee_shared") == 0);
      assert(config_apply_db2_url_env_override(&cfg) == 1);
      assert(strcmp(cfg.db2_url, "postgresql://aimee:aimee@10.0.0.16:5432/aimee_shared") == 0);

      runtime_secret_remove("AIMEE_DB2_URL");
      assert(config_apply_db2_url_env_override(&cfg) == 0);
      assert(strcmp(cfg.db2_url, "postgresql://aimee:aimee@10.0.0.16:5432/aimee_shared") == 0);

      /* No runtime credential leaves the existing value untouched. */
      assert(config_apply_db2_url_env_override(&cfg) == 0);
      assert(strcmp(cfg.db2_url, "postgresql://aimee:aimee@10.0.0.16:5432/aimee_shared") == 0);

      /* NULL cfg -> no crash, returns 0 */
      assert(config_apply_db2_url_env_override(NULL) == 0);

      runtime_secret_remove("AIMEE_DB2_URL");
   }

   /* learning.review.* config parser (idle-reflection scheduler knobs). */
   {
      /* All four fields overridden, incl. cooldown=0 (disables cooldown). */
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.review_scheduler_enabled = 1; /* seed the config.c defaults */
      cfg.review_idle_trigger_minutes = 30;
      cfg.review_session_cooldown_hours = 24;
      cfg.review_batch_cap = 10;
      cJSON *root = cJSON_Parse("{\"learning\":{\"review\":{\"scheduler_enabled\":0,"
                                "\"idle_trigger_minutes\":5,\"session_cooldown_hours\":0,"
                                "\"batch_cap\":3}}}");
      assert(root);
      config_apply_review_settings(&cfg, root);
      assert(cfg.review_scheduler_enabled == 0);
      assert(cfg.review_idle_trigger_minutes == 5);
      assert(cfg.review_session_cooldown_hours == 0); /* 0 disables the cooldown */
      assert(cfg.review_batch_cap == 3);
      cJSON_Delete(root);

      /* Absent 'review' object leaves all four defaults intact. */
      config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      cfg2.review_scheduler_enabled = 1;
      cfg2.review_idle_trigger_minutes = 30;
      cfg2.review_session_cooldown_hours = 24;
      cfg2.review_batch_cap = 10;
      cJSON *no_review = cJSON_Parse("{\"learning\":{}}");
      assert(no_review);
      config_apply_review_settings(&cfg2, no_review);
      assert(cfg2.review_scheduler_enabled == 1 && cfg2.review_idle_trigger_minutes == 30 &&
             cfg2.review_session_cooldown_hours == 24 && cfg2.review_batch_cap == 10);
      cJSON_Delete(no_review);

      /* Missing 'learning' key entirely, and NULL root: early return, no change,
       * no crash. */
      config_t cfg3;
      memset(&cfg3, 0, sizeof(cfg3));
      cfg3.review_scheduler_enabled = 1;
      cfg3.review_idle_trigger_minutes = 30;
      cfg3.review_session_cooldown_hours = 24;
      cfg3.review_batch_cap = 10;
      cJSON *empty = cJSON_Parse("{}");
      assert(empty);
      config_apply_review_settings(&cfg3, empty);
      config_apply_review_settings(&cfg3, NULL);
      assert(cfg3.review_scheduler_enabled == 1 && cfg3.review_idle_trigger_minutes == 30 &&
             cfg3.review_session_cooldown_hours == 24 && cfg3.review_batch_cap == 10);
      cJSON_Delete(empty);

      /* Robustness: scheduler_enabled normalises to 0/1; fractional/zero positive
       * knobs are rejected (leave defaults); a partial override touches only its
       * own field. */
      config_t cfg4;
      memset(&cfg4, 0, sizeof(cfg4));
      cfg4.review_scheduler_enabled = 0;
      cfg4.review_idle_trigger_minutes = 30;
      cfg4.review_session_cooldown_hours = 24;
      cfg4.review_batch_cap = 10;
      cJSON *robust = cJSON_Parse(
          "{\"learning\":{\"review\":{\"scheduler_enabled\":5,\"idle_trigger_minutes\":30.5,"
          "\"session_cooldown_hours\":0.5,\"batch_cap\":0}}}");
      assert(robust);
      config_apply_review_settings(&cfg4, robust);
      assert(cfg4.review_scheduler_enabled == 1);     /* 5 normalised to 1 */
      assert(cfg4.review_idle_trigger_minutes == 30); /* 30.5 not integer -> rejected */
      assert(cfg4.review_session_cooldown_hours ==
             24);                          /* 0.5h not integer, != disable -> rejected */
      assert(cfg4.review_batch_cap == 10); /* 0 rejected */
      cJSON_Delete(robust);

      /* An explicit whole-number 0 still disables the cooldown. */
      config_t cfg4b;
      memset(&cfg4b, 0, sizeof(cfg4b));
      cfg4b.review_session_cooldown_hours = 24;
      cJSON *disable = cJSON_Parse("{\"learning\":{\"review\":{\"session_cooldown_hours\":0}}}");
      assert(disable);
      config_apply_review_settings(&cfg4b, disable);
      assert(cfg4b.review_session_cooldown_hours == 0);
      cJSON_Delete(disable);

      config_t cfg5;
      memset(&cfg5, 0, sizeof(cfg5));
      cfg5.review_scheduler_enabled = 1;
      cfg5.review_batch_cap = 10;
      cJSON *partial = cJSON_Parse("{\"learning\":{\"review\":{\"batch_cap\":7}}}");
      assert(partial);
      config_apply_review_settings(&cfg5, partial);
      assert(cfg5.review_batch_cap == 7 && cfg5.review_scheduler_enabled == 1);
      cJSON_Delete(partial);
   }

   /* EMBEDDER_DIMS env override (config_resolve_embedder_dims) */
   {
      char *saved = getenv("EMBEDDER_DIMS");
      if (saved)
         saved = strdup(saved);

      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.embedder_dims = 2560;

      /* unset -> returns the cfg value */
      platform_unsetenv("EMBEDDER_DIMS");
      assert(config_resolve_embedder_dims(&cfg) == 2560);

      /* valid env -> overrides */
      platform_setenv("EMBEDDER_DIMS", "1024");
      assert(config_resolve_embedder_dims(&cfg) == 1024);

      /* empty -> treated as unset, falls back to cfg */
      platform_setenv("EMBEDDER_DIMS", "");
      assert(config_resolve_embedder_dims(&cfg) == 2560);

      /* non-numeric / out-of-range -> rejected, falls back to cfg */
      platform_setenv("EMBEDDER_DIMS", "abc");
      assert(config_resolve_embedder_dims(&cfg) == 2560);
      platform_setenv("EMBEDDER_DIMS", "999999");
      assert(config_resolve_embedder_dims(&cfg) == 2560);

      /* NULL cfg with no env -> 0 (no crash) */
      platform_unsetenv("EMBEDDER_DIMS");
      assert(config_resolve_embedder_dims(NULL) == 0);

      /* §2a: config_embedder_dims_is_pinned == (config_resolve_embedder_dims > 0).
       * The env="0"/non-numeric rows are the point — they must NOT count as a pin. */
      platform_unsetenv("EMBEDDER_DIMS");
      assert(config_embedder_dims_is_pinned(&cfg) == 1); /* cfg->embedder_dims=2560 */
      platform_setenv("EMBEDDER_DIMS", "2560");
      assert(config_embedder_dims_is_pinned(&cfg) == 1); /* valid env pin */
      platform_setenv("EMBEDDER_DIMS", "0");
      {
         config_t unset_cfg;
         memset(&unset_cfg, 0, sizeof(unset_cfg));                /* embedding_dim = 0 */
         assert(config_embedder_dims_is_pinned(&unset_cfg) == 0); /* env "0" is not a pin */
         platform_setenv("EMBEDDER_DIMS", "garbage");
         assert(config_embedder_dims_is_pinned(&unset_cfg) == 0); /* non-numeric is not a pin */
         platform_unsetenv("EMBEDDER_DIMS");
         assert(config_embedder_dims_is_pinned(&unset_cfg) == 0); /* unset + cfg=0 -> not pinned */
         assert(config_embedder_dims_is_pinned(&cfg) == 1);       /* unset + cfg=2560 -> pinned */
      }

      if (saved)
      {
         platform_setenv("EMBEDDER_DIMS", saved);
         free(saved);
      }
      else
         platform_unsetenv("EMBEDDER_DIMS");
   }

   /* --- §5 hybrid RRF weights parse from kb.code_hybrid.* --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg); /* valid baseline; the parse below overrides the asserted fields */
      cJSON *root = cJSON_CreateObject();
      cJSON *kb = cJSON_AddObjectToObject(root, "kb");
      cJSON *ch = cJSON_AddObjectToObject(kb, "code_hybrid");
      cJSON_AddNumberToObject(ch, "weight_code", 2.5);
      cJSON_AddNumberToObject(ch, "weight_graph", 0.5);
      cJSON_AddNumberToObject(ch, "rrf_k", 30);
      config_parse_kb_section2(&cfg, root);
      assert(fabs(cfg.code_hybrid_weight_code - 2.5) < 1e-9);
      assert(fabs(cfg.code_hybrid_weight_graph - 0.5) < 1e-9);
      assert(fabs(cfg.code_hybrid_rrf_k - 30.0) < 1e-9);
      /* rrf_k <= 0 is rejected (keeps the prior value). */
      cJSON_ReplaceItemInObjectCaseSensitive(ch, "rrf_k", cJSON_CreateNumber(0));
      config_parse_kb_section2(&cfg, root);
      assert(fabs(cfg.code_hybrid_rrf_k - 30.0) < 1e-9);
      cJSON_Delete(root);
   }

   /* Server API env overrides (config_server_api.c): deploy truth wins over an
    * older persisted config. */
   {
      extern void config_parse_server_api(config_t * cfg, const cJSON *root);
      config_t c;
      memset(&c, 0, sizeof c);
      cJSON *root = cJSON_CreateObject(); /* no "api" block: env is the only source */

      assert(runtime_secret_store("AIMEE_API_BEARER_TOKEN", "env-strong-abc123") == 0);
      config_parse_server_api(&c, root);
      assert(strcmp(c.server_api_bearer_token, "env-strong-abc123") == 0);

      /* An unchanged deployment primary preserves additive client credentials
       * across restart. Changing it is an out-of-band revoke-all. */
      c.server_api_bearer_extra_count = 1;
      snprintf(c.server_api_bearer_extra[0], sizeof c.server_api_bearer_extra[0], "client-one");
      config_parse_server_api(&c, root);
      assert(c.server_api_bearer_extra_count == 1);
      snprintf(c.server_api_bearer_token, sizeof c.server_api_bearer_token, "old-env-token");
      config_parse_server_api(&c, root);
      assert(c.server_api_bearer_extra_count == 0);
      assert(c.server_api_bearer_extra[0][0] == '\0');

      /* Env overrides a pre-existing (e.g. seeded bootstrap) value too. */
      snprintf(c.server_api_bearer_token, sizeof c.server_api_bearer_token,
               "unit-test-legacy-primary");
      config_parse_server_api(&c, root);
      assert(strcmp(c.server_api_bearer_token, "env-strong-abc123") == 0);

      /* Without the env, the existing value is left untouched. */
      runtime_secret_remove("AIMEE_API_BEARER_TOKEN");
      snprintf(c.server_api_bearer_token, sizeof c.server_api_bearer_token, "file-value");
      config_parse_server_api(&c, root);
      assert(strcmp(c.server_api_bearer_token, "file-value") == 0);

      platform_setenv("AIMEE_API_MTLS", "optional");
      config_parse_server_api(&c, root);
      assert(c.server_api_mtls == 1);
      platform_setenv("AIMEE_API_MTLS", "required");
      config_parse_server_api(&c, root);
      assert(c.server_api_mtls == 2);
      platform_setenv("AIMEE_API_MTLS", "off");
      config_parse_server_api(&c, root);
      assert(c.server_api_mtls == 0);
      platform_unsetenv("AIMEE_API_MTLS");

      cJSON_Delete(root);
   }

   /* --- only credentials are Vault-backed --- */
   {
      /* A field with a secret_name is a process-memory view of a Vault record: it is
       * NEVER serialized, config.show and config.get render it as a presence BOOLEAN,
       * and config_set writes it to Vault instead of YAML.
       *
       * That is right for a credential and catastrophic for a setting. Eight ordinary
       * fields were tagged this way -- embedder_model, embedder_url, embedder_dims,
       * synthesis_endpoint, synthesis_model, synthesis_thinking, aimee_synthesis_model,
       * aimee_with_llamacpp -- because the environment-variable name was written into
       * the secret_name slot. The effect was that `aimee config set embedder_model
       * bekko-a25m` reported "= true", `config get` returned false, and the value went
       * into Vault where nothing reads it. A deployment could not select an embedder at
       * all, and the KB logged "no embedder selected" whatever the operator did.
       *
       * secret_name is not an env-var binding and never was: nothing reads getenv()
       * through it. The env vars are consumed by the container entrypoint and
       * embedder-server.py, not by this table.
       *
       * So this asserts the classification directly. A model name, a dimension count
       * and a boolean are not credentials. */
      static const char *const not_secrets[] = {
          "embedder_model",        "embedder_url",        "embedder_dims",
          "synthesis_model",       "synthesis_endpoint",  "synthesis_thinking",
          "aimee_synthesis_model", "aimee_with_llamacpp", NULL};
      for (int i = 0; not_secrets[i]; i++)
      {
         const config_field_t *f = config_field_lookup(not_secrets[i]);
         assert(f && "field must exist");
         assert(config_field_secret_name(f) == NULL);
      }

      /* --- an embedder the image cannot serve is refused --- */
      /* config_set writes YAML, so this needs a home of its own; the block that owns
       * AIMEE_HOME above has already restored the ambient one by here. */
      {
         char home[256];
         snprintf(home, sizeof(home), "/tmp/aimee-cfgset-%d", (int)getpid());
         mkdir(home, 0700);
         platform_setenv("AIMEE_HOME", home);

         /* The two names the images bake are accepted. */
         assert(config_set("embedder_model", "bekko-a25m") == 0);
         assert(config_set("embedder_model", "nomic-embed-text-v2-moe") == 0);

         /* A typo is NOT, while no external endpoint is configured. Unrefused, it
          * deployed the a25m image with EMBEDDER_MODEL=<typo>, started no embedder,
          * and searched lexically while every health surface said ok. */
         assert(config_set("embedder_model", "bekko-a25") != 0);
         assert(config_set("embedder_model", "not-a-model") != 0);

         /* With an external endpoint the name belongs to that endpoint and any value
          * is legitimate -- this is the half a blanket allowlist would have broken. */
         assert(config_set("embedder_url", "https://embed.example/v1") == 0);
         assert(config_set("embedder_model", "text-embedding-3-small") == 0);

         /* Clearing stays allowed: it is how an operator hands the role over. */
         assert(config_set("embedder_model", "") == 0);

         /* api.enable changes the port and rate limit as one transaction. Two
          * generated setters used to load the same published server snapshot;
          * the rate-limit save then silently restored the old port. */
         assert(config_set("provider", "gemini") == 0);
         assert(config_set_api_http_listener(9123, 77) == 0);
         assert(config_server_api_http_port() == 9123);
         assert(config_server_api_rate_limit_per_min() == 77);
         assert(strcmp(config_provider(), "gemini") == 0); /* unrelated disk state survives */

         platform_unsetenv("AIMEE_HOME");
      }

      /* The real credentials stay Vault-backed. This half is what stops the fix above
       * from being applied too broadly. */
      static const char *const secrets[] = {"embedder_api_key", "synthesis_api_key",
                                            "kb_api_bearer_token", "db2_url", NULL};
      for (int i = 0; secrets[i]; i++)
      {
         const config_field_t *f = config_field_lookup(secrets[i]);
         assert(f && "field must exist");
         assert(config_field_secret_name(f) != NULL);
      }
      printf("  config_field secret classification: ok\n");
   }

   /* --- config_emit_deploy_env: page-2 record -> compose env --- */
   {
      char env[2048];

      /* Local kb; embed local(mid), synth external. */
      static config_t cfg;

      /* Local kb, in-container embedder, external synth. There is no inference
       * service to deploy any more, so the kb profile is the whole story and the
       * embedder rides along as a MODEL CHOICE rather than a container placement. */
      memset(&cfg, 0, sizeof(cfg));
      snprintf(cfg.kb_mode, sizeof(cfg.kb_mode), "local");
      snprintf(cfg.embedder_model, sizeof(cfg.embedder_model), "bekko-a25m");
      snprintf(cfg.synthesis_endpoint, sizeof(cfg.synthesis_endpoint), "https://api.x/v1");
      config_emit_deploy_env(&cfg, env, sizeof(env));
      assert(strstr(env, "COMPOSE_PROFILES=kb\n") != NULL);
      /* An EXTERNAL endpoint deploys no sidecar: nothing local serves synthesis. */
      assert(strstr(env, ",llm") == NULL);
      assert(strstr(env, "EMBEDDER_MODEL=bekko-a25m\n") != NULL);
      /* The embedder is BAKED, so the choice picks an image, not just a setting.
       * Emitting the model alone told the kb to start weights its image might not
       * contain -- and `aimee-kb` changed meaning from "bekko" to "no embedder", so
       * that combination was a live regression rather than a missing feature. */
      assert(strstr(env, "AIMEE_KB_VARIANT=a25m\n") != NULL);
      assert(strstr(env, "SYNTHESIS_ENDPOINT=https://api.x/v1\n") != NULL);
      /* No sidecar, so no client identity: SYNTHESIS_CA_FILE REPLACES the system trust
       * store, and pointing it at our CA while the endpoint is external would reject a
       * perfectly valid certificate. */
      assert(strstr(env, "SYNTHESIS_CA_FILE") == NULL);
      assert(strstr(env, "AIMEE_LLM_HOST") == NULL);
      assert(strstr(env, "EMBEDDER_DIMS") == NULL); /* in-container => derived */
      assert(strstr(env, "EMBEDDER_URL") == NULL);  /* no URL => the bundled model */

      /* Synthesis OFF is a supported state, not an error: no endpoint, no emission,
       * and the kb still deploys. This is what llm_synth_backend="off" encoded. */
      memset(&cfg, 0, sizeof(cfg));
      snprintf(cfg.kb_mode, sizeof(cfg.kb_mode), "local");
      config_emit_deploy_env(&cfg, env, sizeof(env));
      assert(strstr(env, "COMPOSE_PROFILES=kb\n") != NULL);
      assert(strstr(env, "SYNTHESIS_ENDPOINT") == NULL);
      assert(strstr(env, "SYNTHESIS_MODEL") == NULL);
      assert(strstr(env, "AIMEE_LLM_HOST") == NULL);
      /* NOTHING SELECTED MUST NOT YIELD THE EMBEDDERLESS IMAGE. aimee-kb carries no
       * weights, so that combination cannot embed and cannot be repaired by setting a
       * key -- it needs a different image. a25m is what `aimee-kb` meant before the
       * axis was split, so an unconfigured deployment keeps what it already had. */
      assert(strstr(env, "AIMEE_KB_VARIANT=a25m\n") != NULL);

      /* A BUNDLED model is a DEPLOYED SIDECAR, not a loopback endpoint. It used to be
       * in-process in the kb; it is aimee-llm-e{2,4}b beside the kb now, reached over
       * mTLS. Every line below is load-bearing: without the profile the service exists
       * in Compose and nothing starts it, without AIMEE_LLM_HOST the kb never mints the
       * identity the sidecar refuses to start without, and without the endpoint the kb
       * has a running sidecar it never calls. Each failure is silent. */
      memset(&cfg, 0, sizeof(cfg));
      snprintf(cfg.kb_mode, sizeof(cfg.kb_mode), "local");
      snprintf(cfg.embedder_model, sizeof(cfg.embedder_model), "nomic-embed-text-v2-moe");
      snprintf(cfg.synthesis_model, sizeof(cfg.synthesis_model), "gemma-4-E4B-it");
      config_emit_deploy_env(&cfg, env, sizeof(env));
      assert(strstr(env, "SYNTHESIS_MODEL=gemma-4-E4B-it\n") != NULL);
      assert(strstr(env, "COMPOSE_PROFILES=kb,llm\n") != NULL);
      assert(strstr(env, "AIMEE_LLM_VARIANT=e4b\n") != NULL);
      assert(strstr(env, "AIMEE_LLM_HOST=aimee-llm\n") != NULL);
      assert(strstr(env, "SYNTHESIS_ENDPOINT=https://aimee-llm:8761/v1\n") != NULL);
      assert(strstr(env, "SYNTHESIS_CA_FILE=/var/lib/aimee/synthesis-tls/ca.pem\n") != NULL);
      assert(strstr(env, "SYNTHESIS_CERT_FILE=/var/lib/aimee/synthesis-tls/client.pem\n") != NULL);
      assert(strstr(env, "SYNTHESIS_KEY_FILE=/var/lib/aimee/synthesis-tls/client.key\n") != NULL);
      /* The embedder axis is independent of the synthesis axis now. */
      assert(strstr(env, "AIMEE_KB_VARIANT=nomic\n") != NULL);

      /* E2B selects the other sidecar tag. */
      memset(&cfg, 0, sizeof(cfg));
      snprintf(cfg.kb_mode, sizeof(cfg.kb_mode), "local");
      snprintf(cfg.synthesis_model, sizeof(cfg.synthesis_model), "gemma-4-E2B-it");
      config_emit_deploy_env(&cfg, env, sizeof(env));
      assert(strstr(env, "AIMEE_LLM_VARIANT=e2b\n") != NULL);
      assert(strstr(env, "COMPOSE_PROFILES=kb,llm\n") != NULL);

      /* Remote kb: connect out, deploy nothing. */
      memset(&cfg, 0, sizeof(cfg));
      snprintf(cfg.kb_mode, sizeof(cfg.kb_mode), "remote");
      snprintf(cfg.kb_client_url, sizeof(cfg.kb_client_url), "https://kb.remote:4010");
      snprintf(cfg.kb_client_bearer_token, sizeof(cfg.kb_client_bearer_token), "tok-x");
      config_emit_deploy_env(&cfg, env, sizeof(env));
      assert(strstr(env, "COMPOSE_PROFILES=\n") != NULL);
      assert(strstr(env, "AIMEE_KB_API_URL=https://kb.remote:4010\n") != NULL);
      assert(strstr(env, "AIMEE_KB_API_BEARER_TOKEN=") == NULL);
      assert(strstr(env, "SYNTHESIS_") == NULL);
      assert(strstr(env, "EMBEDDER_MODEL") == NULL);

      /* An EXTERNAL embedder: the endpoint is passed as the embedder URL the kb's
       * client reads, and a pinned dim comes along because nothing local can derive
       * it from a model we do not serve. */
      memset(&cfg, 0, sizeof(cfg));
      snprintf(cfg.kb_mode, sizeof(cfg.kb_mode), "local");
      snprintf(cfg.embedder_url, sizeof(cfg.embedder_url), "https://emb.x/v1");
      cfg.embedder_dims = 2560;
      snprintf(cfg.embedder_api_key, sizeof(cfg.embedder_api_key), "emb-key");
      snprintf(cfg.synthesis_endpoint, sizeof(cfg.synthesis_endpoint), "https://synth.x/v1");
      snprintf(cfg.synthesis_api_key, sizeof(cfg.synthesis_api_key), "syn-key");
      config_emit_deploy_env(&cfg, env, sizeof(env));
      assert(strstr(env, "COMPOSE_PROFILES=kb\n") != NULL);
      /* An external endpoint is the ONE case the embedderless image is right for. */
      assert(strstr(env, "AIMEE_KB_VARIANT=\n") != NULL);
      assert(strstr(env, "EMBEDDER_URL=https://emb.x/v1\n") != NULL);
      /* Credentials are NEVER emitted into a long-lived service environment:
       * Config.Env persists and shows up in `docker inspect`. They are sealed into
       * Vault by the disposable bootstrap helper (check-vault-only-container-env
       * enforces this, and caught it when this emitted them). */
      assert(strstr(env, "EMBEDDER_API_KEY") == NULL);
      assert(strstr(env, "EMBEDDER_DIMS=2560\n") != NULL);
      assert(strstr(env, "SYNTHESIS_API_KEY") == NULL);
   }

   printf("all tests passed\n");
   return 0;
}
