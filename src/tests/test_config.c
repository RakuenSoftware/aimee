#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "config_database.h"
#include "server.h" /* SERVER_REMOTE_WRITES_* */
#include "aimee_home.h"
#include "platform_path.h"
#include "platform_test_util.h"

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

int main(void)
{
   printf("config: ");

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
      assert(strcmp(cfg.provider, "claude") == 0);
      assert(strcmp(cfg.guardrail_mode, "approve") == 0);
      assert(cfg.db1_path[0] != '\0');
      assert(cfg.guardrails_semantic_advisory_only == 1);
      assert(cfg.skills_review_nudge_interval == 10);
      assert(cfg.skills_curator_interval_hours == 168);
      assert(cfg.skills_stale_after_days == 30);
      assert(cfg.skills_archive_after_days == 90);
      assert(cfg.skills_min_idle_minutes == 30);
      assert(cfg.skills_manage_enabled == 0);
      assert(cfg.skills_dispatch_enabled == 1);
      assert(cfg.skills_dispatch_max_index == 24);
      assert(cfg.skills_dispatch_advisory == 0);
      assert(cfg.skills_capability_autostub == 0);
      assert(cfg.skills_eval_gate_enabled == 0);
      assert(fabs(cfg.skills_eval_threshold - 0.01) < 0.0001);
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
   }

   /* --- config_save + config_load round-trip --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      snprintf(cfg.provider, sizeof(cfg.provider), "gemini");
      snprintf(cfg.claude_model, sizeof(cfg.claude_model), "claude-sonnet-4-6");
      snprintf(cfg.codex_model, sizeof(cfg.codex_model), "gpt-5.4");
      snprintf(cfg.model_reasoning_effort, sizeof(cfg.model_reasoning_effort), "high");
      snprintf(cfg.memory_rerank_mode, sizeof(cfg.memory_rerank_mode), "slow");
      snprintf(cfg.kb_client_url, sizeof(cfg.kb_client_url), "https://kb.example:4010");
      snprintf(cfg.kb_client_bearer_token, sizeof(cfg.kb_client_bearer_token), "tok-abc123");
      cfg.server_api_http_port = 8910;
      snprintf(cfg.server_api_bearer_token, sizeof(cfg.server_api_bearer_token), "tok-api-xyz");
      cfg.server_api_rate_limit_per_min = 60;
      cfg.server_api_max_event_streams = 512;
      snprintf(cfg.server_api_client_transport, sizeof(cfg.server_api_client_transport), "http");
      cfg.server_api_remote_writes = SERVER_REMOTE_WRITES_FULL;
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
      snprintf(cfg.workspace_vcs_head[2], sizeof(cfg.workspace_vcs_head[2]), "abc123def456");
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
      cfg.guardrails_semantic_enabled = 1;
      cfg.guardrails_semantic_dry_run = 0;
      cfg.guardrails_semantic_advisory_only = 0;
      snprintf(cfg.guardrails_semantic_command, sizeof(cfg.guardrails_semantic_command),
               "semantic-sidecar --json");
      cfg.guardrails_semantic_warn_threshold = 0.35;
      cfg.guardrails_semantic_prompt_threshold = 0.65;
      cfg.guardrails_semantic_block_threshold = 0.95;
      cfg.guardrails_semantic_allow_ml_only_block = 1;
      cfg.skills_review_nudge_interval = 12;
      cfg.skills_curator_interval_hours = 240;
      cfg.skills_stale_after_days = 45;
      cfg.skills_archive_after_days = 120;
      cfg.skills_min_idle_minutes = 20;
      cfg.skills_manage_enabled = 1;
      cfg.skills_dispatch_enabled = 0;
      cfg.skills_dispatch_max_index = 7;
      cfg.skills_dispatch_advisory = 1;
      cfg.skills_capability_autostub = 1;
      cfg.skills_eval_gate_enabled = 1;
      cfg.skills_eval_threshold = 0.25;
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
      snprintf(cfg.kb_curator_judge_command, sizeof(cfg.kb_curator_judge_command), "judge --json");
      snprintf(cfg.kb_curator_synthesize_command, sizeof(cfg.kb_curator_synthesize_command),
               "synth --json");
      cfg.kb_evidence_embed_enabled = 0;
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
      cfg.model_meta_capability_routing = 1;
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
      config_save(&cfg);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(strcmp(cfg2.provider, "gemini") == 0);
      assert(strcmp(cfg2.claude_model, "claude-sonnet-4-6") == 0);
      assert(strcmp(cfg2.codex_model, "gpt-5.4") == 0);
      assert(strcmp(cfg2.model_reasoning_effort, "high") == 0);
      assert(strcmp(cfg2.memory_rerank_mode, "slow") == 0);
      assert(strcmp(cfg2.kb_client_bearer_token, "tok-abc123") == 0);
      assert(cfg2.server_api_http_port == 8910);
      assert(strcmp(cfg2.server_api_bearer_token, "tok-api-xyz") == 0);
      assert(cfg2.server_api_rate_limit_per_min == 60);
      assert(cfg2.server_api_max_event_streams == 512);
      assert(strcmp(cfg2.server_api_client_transport, "http") == 0);
      /* regression: remote_writes used to be parsed but never written by config_save,
       * so any save silently reset it to off. */
      assert(cfg2.server_api_remote_writes == SERVER_REMOTE_WRITES_FULL);
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
      assert(strcmp(cfg2.workspace_vcs_head[2], "abc123def456") == 0);
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
      assert(cfg2.guardrails_semantic_enabled == 1);
      assert(cfg2.guardrails_semantic_dry_run == 0);
      assert(cfg2.guardrails_semantic_advisory_only == 0);
      assert(strcmp(cfg2.guardrails_semantic_command, "semantic-sidecar --json") == 0);
      assert(fabs(cfg2.guardrails_semantic_warn_threshold - 0.35) < 0.0001);
      assert(fabs(cfg2.guardrails_semantic_prompt_threshold - 0.65) < 0.0001);
      assert(fabs(cfg2.guardrails_semantic_block_threshold - 0.95) < 0.0001);
      assert(cfg2.guardrails_semantic_allow_ml_only_block == 1);
      assert(cfg2.skills_review_nudge_interval == 12);
      assert(cfg2.skills_curator_interval_hours == 240);
      assert(cfg2.skills_stale_after_days == 45);
      assert(cfg2.skills_archive_after_days == 120);
      assert(cfg2.skills_min_idle_minutes == 20);
      assert(cfg2.skills_manage_enabled == 1);
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
      assert(strcmp(cfg2.kb_curator_judge_command, "judge --json") == 0);
      assert(strcmp(cfg2.kb_curator_synthesize_command, "synth --json") == 0);
      assert(cfg2.kb_evidence_embed_enabled == 0);
      assert(cfg2.memory_profile_cards_enabled == 0);
      assert(cfg2.memory_improve_dedupe_enabled == 0);
      assert(cfg2.memory_improve_summarise_enabled == 1);
      assert(cfg2.memory_improve_min_cluster == 5);
      assert(fabs(cfg2.memory_improve_max_confidence - 0.42) < 0.0001);
      assert(cfg2.memory_directives_enabled == 0);
      assert(cfg2.css_style_graph_enabled == 0); /* opt-out survives save/reload */
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
      assert(strcmp(cfg2.trigger_auth_token, "trig-tok") == 0 && cfg2.trigger_max_concurrent == 4);
      assert(cfg2.trigger_rule_count == 1);
      assert(strcmp(cfg2.trigger_rules[0].source, "github-webhook") == 0);
      assert(strcmp(cfg2.trigger_rules[0].event, "push:main") == 0);
      assert(strcmp(cfg2.trigger_rules[0].pipeline_template, "review") == 0);
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
      assert(strcmp(cfg2.proxy_token, "ptok") == 0);
      assert(cfg2.max_background_processes == 9);
      assert(cfg2.model_meta_refresh_minutes == 15 && cfg2.model_meta_capability_routing == 1);
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
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      const char *mode = config_guardrail_mode(&cfg);
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

   /* --- schema validation: valid config passes strict mode --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nuse_builtin_cli: true\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 1;
      int rc = config_load(&cfg);
      assert(rc == 0); /* all keys valid */
      assert(strcmp(cfg.provider, "claude") == 0);
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
      fprintf(f, "provider: claude\nguardrails:\n  semantic:\n    advisory_only: false\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      platform_setenv("AIMEE_NO_CACHE", "1");
      assert(config_load(&cfg) == 0);
      assert(cfg.guardrails_semantic_advisory_only == 0);

      f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\nguardrails:\n  semantic:\n    enabled: true\n");
      fclose(f);
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      assert(cfg.guardrails_semantic_enabled == 1);
      assert(cfg.guardrails_semantic_advisory_only == 1);
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

   /* --- memory_rerank: defaults --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc2 = config_load(&cfg);
      assert(rc2 == 0);
      /* Reranking (pipeline stage 3) is default-ON with the rerank-remote.py
       * command; it degrades to plain hybrid ordering if the service is absent. */
      assert(cfg.memory_rerank_enabled == 1);
      assert(strcmp(cfg.memory_rerank_command, "python3 /opt/aimee/scripts/rerank-remote.py") == 0);
      assert(cfg.memory_rerank_top_k == 0);
      assert(cfg.memory_rerank_mix == 0.0);
      assert(cfg.memory_query_expansion_mode[0] == '\0');
      assert(cfg.memory_query_expansion_k == 0);
   }

   /* --- memory_rerank: parsed from config --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "provider: claude\n"
                 "memory_rerank:\n"
                 "  enabled: true\n"
                 "  command: /usr/local/bin/cross-encoder\n"
                 "  top_k: 50\n"
                 "  mix: 0.7\n"
                 "memory_query_expansion:\n"
                 "  mode: semantic\n"
                 "  k: 5\n");
      fclose(f);

      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      int rc2 = config_load(&cfg);
      assert(rc2 == 0);
      assert(cfg.memory_rerank_enabled == 1);
      assert(strcmp(cfg.memory_rerank_command, "/usr/local/bin/cross-encoder") == 0);
      assert(cfg.memory_rerank_top_k == 50);
      assert(cfg.memory_rerank_mix >= 0.69 && cfg.memory_rerank_mix <= 0.71);
      assert(strcmp(cfg.memory_query_expansion_mode, "semantic") == 0);
      assert(cfg.memory_query_expansion_k == 5);
   }

   /* --- memory_rerank: round-trip save/load --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.memory_rerank_enabled = 1;
      snprintf(cfg.memory_rerank_command, sizeof(cfg.memory_rerank_command), "/opt/ce/score.py");
      cfg.memory_rerank_top_k = 30;
      cfg.memory_rerank_mix = 0.6;
      snprintf(cfg.memory_query_expansion_mode, sizeof(cfg.memory_query_expansion_mode),
               "semantic");
      cfg.memory_query_expansion_k = 8;
      config_save(&cfg);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.memory_rerank_enabled == 1);
      assert(strcmp(cfg2.memory_rerank_command, "/opt/ce/score.py") == 0);
      assert(cfg2.memory_rerank_top_k == 30);
      assert(cfg2.memory_rerank_mix >= 0.59 && cfg2.memory_rerank_mix <= 0.61);
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

   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(config_load(&cfg) == 0);
      snprintf(cfg.db2_url, sizeof(cfg.db2_url), "postgres:///aimee_shared");
      cfg.db2_pool_size = 16;
      assert(config_save(&cfg) == 0);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      platform_unsetenv("AIMEE_NO_CACHE");
      assert(config_load(&cfg2) == 0);
      assert(strcmp(cfg2.db2_url, "postgres:///aimee_shared") == 0);
      assert(cfg2.db2_pool_size == 16);

      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/aimee.yaml", tmpdir);
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

   /* --- AIMEE_DB2_URL env overrides a cached config-file db2_url ---
    * Regression for the kb IP-drift outage: when Postgres is recreated on a new
    * bridge IP the runtime injects the current address via AIMEE_DB2_URL, which
    * must win over the stale value persisted in aimee.yaml. */
   {
      char *old = getenv("AIMEE_DB2_URL");
      char *saved = old ? strdup(old) : NULL;

      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      snprintf(cfg.db2_url, sizeof(cfg.db2_url),
               "postgresql://aimee:aimee@10.0.0.9:5432/aimee_shared");

      /* env set -> overrides the cached file value, returns 1 (applied) */
      platform_setenv("AIMEE_DB2_URL", "postgresql://aimee:aimee@10.0.0.16:5432/aimee_shared");
      assert(config_apply_db2_url_env_override(&cfg) == 1);
      assert(strcmp(cfg.db2_url, "postgresql://aimee:aimee@10.0.0.16:5432/aimee_shared") == 0);

      /* env unset -> leaves the existing value untouched, returns 0 */
      platform_unsetenv("AIMEE_DB2_URL");
      assert(config_apply_db2_url_env_override(&cfg) == 0);
      assert(strcmp(cfg.db2_url, "postgresql://aimee:aimee@10.0.0.16:5432/aimee_shared") == 0);

      /* empty env -> treated as unset, returns 0 */
      platform_setenv("AIMEE_DB2_URL", "");
      assert(config_apply_db2_url_env_override(&cfg) == 0);
      assert(strcmp(cfg.db2_url, "postgresql://aimee:aimee@10.0.0.16:5432/aimee_shared") == 0);

      /* NULL cfg -> no crash, returns 0 */
      assert(config_apply_db2_url_env_override(NULL) == 0);

      if (saved)
      {
         platform_setenv("AIMEE_DB2_URL", saved);
         free(saved);
      }
      else
         platform_unsetenv("AIMEE_DB2_URL");
   }

   /* AIMEE_EMBEDDING_DIM env override (config_resolve_embedding_dim) */
   {
      char *saved = getenv("AIMEE_EMBEDDING_DIM");
      if (saved)
         saved = strdup(saved);

      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.embedding_dim = 2560;

      /* unset -> returns the cfg value */
      platform_unsetenv("AIMEE_EMBEDDING_DIM");
      assert(config_resolve_embedding_dim(&cfg) == 2560);

      /* valid env -> overrides */
      platform_setenv("AIMEE_EMBEDDING_DIM", "1024");
      assert(config_resolve_embedding_dim(&cfg) == 1024);

      /* empty -> treated as unset, falls back to cfg */
      platform_setenv("AIMEE_EMBEDDING_DIM", "");
      assert(config_resolve_embedding_dim(&cfg) == 2560);

      /* non-numeric / out-of-range -> rejected, falls back to cfg */
      platform_setenv("AIMEE_EMBEDDING_DIM", "abc");
      assert(config_resolve_embedding_dim(&cfg) == 2560);
      platform_setenv("AIMEE_EMBEDDING_DIM", "999999");
      assert(config_resolve_embedding_dim(&cfg) == 2560);

      /* NULL cfg with no env -> 0 (no crash) */
      platform_unsetenv("AIMEE_EMBEDDING_DIM");
      assert(config_resolve_embedding_dim(NULL) == 0);

      if (saved)
      {
         platform_setenv("AIMEE_EMBEDDING_DIM", saved);
         free(saved);
      }
      else
         platform_unsetenv("AIMEE_EMBEDDING_DIM");
   }

   printf("all tests passed\n");
   return 0;
}
