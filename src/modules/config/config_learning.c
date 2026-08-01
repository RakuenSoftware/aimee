#include "aimee.h"
#include "config_learning.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>

static void apply_tau_value(config_t *cfg, const char *surface, const char *mode, double value)
{
   if (!cfg || !surface || !mode || value < 0.0 || value > 1.0)
      return;
   if (strcmp(surface, "memory") == 0)
   {
      if (strcmp(mode, "auto") == 0)
         cfg->calibration_tau_memory_auto = value;
      else if (strcmp(mode, "flag") == 0)
         cfg->calibration_tau_memory_flag = value;
   }
   else if (strcmp(surface, "working_profile") == 0)
   {
      if (strcmp(mode, "auto") == 0)
         cfg->calibration_tau_working_profile_auto = value;
      else if (strcmp(mode, "flag") == 0)
         cfg->calibration_tau_working_profile_flag = value;
   }
}

static void apply_tau_object(config_t *cfg, cJSON *tau)
{
   if (!cJSON_IsObject(tau))
      return;

   cJSON *child = NULL;
   cJSON_ArrayForEach(child, tau)
   {
      if (!child->string)
         continue;

      const char *dot = strchr(child->string, '.');
      if (dot)
      {
         char surface[64];
         char mode[32];
         size_t n = (size_t)(dot - child->string);
         if (n >= sizeof(surface))
            continue;
         memcpy(surface, child->string, n);
         surface[n] = '\0';
         snprintf(mode, sizeof(mode), "%s", dot + 1);
         if (cJSON_IsNumber(child))
            apply_tau_value(cfg, surface, mode, child->valuedouble);
         continue;
      }

      if (cJSON_IsObject(child))
      {
         cJSON *mode_item = NULL;
         cJSON_ArrayForEach(mode_item, child)
         {
            if (mode_item->string && cJSON_IsNumber(mode_item))
               apply_tau_value(cfg, child->string, mode_item->string, mode_item->valuedouble);
         }
      }
   }
}

void config_learning_defaults(config_t *cfg)
{
   cfg->learning_synthesize_enabled = 0;
   cfg->learning_synthesize_command[0] = '\0';
   cfg->learning_synthesize_max_tokens = 2048;
   cfg->learning_synthesize_k = 8;
   snprintf(cfg->learning_embed_model_version, sizeof(cfg->learning_embed_model_version), "v1");
   snprintf(cfg->learning_synthesize_prompt_version,
            sizeof(cfg->learning_synthesize_prompt_version), "v1");
}

void config_apply_learning_settings(config_t *cfg, cJSON *root)
{
   cJSON *item;
   cJSON *learning_cfg = cJSON_GetObjectItemCaseSensitive(root, "learning");
   if (!cJSON_IsObject(learning_cfg))
      return;

   cJSON *router_cfg = cJSON_GetObjectItemCaseSensitive(learning_cfg, "router");
   if (cJSON_IsObject(router_cfg))
   {
      item = cJSON_GetObjectItemCaseSensitive(router_cfg, "enabled");
      if (cJSON_IsBool(item))
         cfg->learning_router_enabled = cJSON_IsTrue(item) ? 1 : 0;

      item = cJSON_GetObjectItemCaseSensitive(router_cfg, "ttl_days");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->learning_proposal_ttl_days = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(router_cfg, "max_commits_per_week");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->learning_max_commits_per_week = (int)item->valuedouble;
   }

   cJSON *synth_cfg = cJSON_GetObjectItemCaseSensitive(learning_cfg, "synthesize");
   if (cJSON_IsObject(synth_cfg))
   {
      item = cJSON_GetObjectItemCaseSensitive(synth_cfg, "enabled");
      if (cJSON_IsBool(item))
         cfg->learning_synthesize_enabled = cJSON_IsTrue(item) ? 1 : 0;
      else if (cJSON_IsNumber(item))
         cfg->learning_synthesize_enabled = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(synth_cfg, "command");
      if (cJSON_IsString(item) && item->valuestring)
         snprintf(cfg->learning_synthesize_command, sizeof(cfg->learning_synthesize_command), "%s",
                  item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(synth_cfg, "max_tokens");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->learning_synthesize_max_tokens = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(synth_cfg, "k");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->learning_synthesize_k = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(synth_cfg, "prompt_version");
      if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
         snprintf(cfg->learning_synthesize_prompt_version,
                  sizeof(cfg->learning_synthesize_prompt_version), "%s", item->valuestring);
   }

   cJSON *embed_cfg = cJSON_GetObjectItemCaseSensitive(learning_cfg, "embed");
   if (cJSON_IsObject(embed_cfg))
   {
      item = cJSON_GetObjectItemCaseSensitive(embed_cfg, "model_version");
      if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
         snprintf(cfg->learning_embed_model_version, sizeof(cfg->learning_embed_model_version),
                  "%s", item->valuestring);
   }

   /* learning.implicit.*: per-heuristic implicit-signal detector toggles. These
    * were CLI-settable (config_fields) but had no file parse/save, so the value
    * never persisted across a save/restart — only the config.c default applied.
    * Parse them here so an operator override survives. citation_repair /
    * citation_continuation default on (the detector is wired + graded PASS); the
    * three stateful heuristics default off. */
   cJSON *implicit_cfg = cJSON_GetObjectItemCaseSensitive(learning_cfg, "implicit");
   if (cJSON_IsObject(implicit_cfg))
   {
      item = cJSON_GetObjectItemCaseSensitive(implicit_cfg, "citation_repair");
      if (cJSON_IsBool(item))
         cfg->learning_implicit_citation_repair = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(implicit_cfg, "citation_continuation");
      if (cJSON_IsBool(item))
         cfg->learning_implicit_citation_continuation = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(implicit_cfg, "repeat_question");
      if (cJSON_IsBool(item))
         cfg->learning_implicit_repeat_question = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(implicit_cfg, "repeated_correction");
      if (cJSON_IsBool(item))
         cfg->learning_implicit_repeated_correction = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(implicit_cfg, "workflow_repetition");
      if (cJSON_IsBool(item))
         cfg->learning_implicit_workflow_repetition = cJSON_IsTrue(item) ? 1 : 0;
   }
}

void config_apply_calibration_settings(config_t *cfg, cJSON *root)
{
   /* Default values. calibration_enabled defaults to 1 (shadow): write
    * calibration profiles from outcomes but never enforce them — observe-only,
    * no behaviour change. 0 = off, 2 = A/B/enforce (opt-in). */
   cfg->calibration_enabled = 1;
   cfg->calibration_command[0] = '\0';
   cfg->calibration_buckets = 10;
   cfg->calibration_prior_alpha0 = 2.0;
   cfg->calibration_prior_beta0 = 1.0;
   cfg->calibration_credible_delta = 0.10;
   cfg->calibration_conformal_window = 500;
   cfg->calibration_conformal_epsilon = 0.05;
   cfg->calibration_tau_memory_auto = 0.70;
   cfg->calibration_tau_memory_flag = 0.55;
   cfg->calibration_tau_working_profile_auto = 0.80;
   cfg->calibration_tau_working_profile_flag = 0.65;
   snprintf(cfg->calibration_prompt_version, sizeof(cfg->calibration_prompt_version), "v1");
   snprintf(cfg->calibration_model_version, sizeof(cfg->calibration_model_version),
            "beta-binomial-v1");

   cJSON *intel = cJSON_GetObjectItemCaseSensitive(root, "intelligence");
   if (!cJSON_IsObject(intel))
      return;

   cJSON *cal = cJSON_GetObjectItemCaseSensitive(intel, "calibrate");
   if (!cJSON_IsObject(cal))
      return;

   cJSON *item;

   item = cJSON_GetObjectItemCaseSensitive(cal, "enabled");
   if (cJSON_IsNumber(item))
      cfg->calibration_enabled = (int)item->valuedouble;
   else if (cJSON_IsBool(item))
      cfg->calibration_enabled = cJSON_IsTrue(item) ? 1 : 0;

   item = cJSON_GetObjectItemCaseSensitive(cal, "command");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->calibration_command, sizeof(cfg->calibration_command), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(cal, "buckets");
   if (cJSON_IsNumber(item) && item->valuedouble >= 2 && item->valuedouble <= 100)
      cfg->calibration_buckets = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(cal, "prior_alpha0");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->calibration_prior_alpha0 = item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(cal, "prior_beta0");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->calibration_prior_beta0 = item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(cal, "credible_delta");
   if (cJSON_IsNumber(item) && item->valuedouble > 0 && item->valuedouble < 1)
      cfg->calibration_credible_delta = item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(cal, "conformal_window");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->calibration_conformal_window = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(cal, "conformal_epsilon");
   if (cJSON_IsNumber(item) && item->valuedouble > 0 && item->valuedouble < 1)
      cfg->calibration_conformal_epsilon = item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(cal, "prompt_version");
   if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
      snprintf(cfg->calibration_prompt_version, sizeof(cfg->calibration_prompt_version), "%s",
               item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(cal, "model_version");
   if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
      snprintf(cfg->calibration_model_version, sizeof(cfg->calibration_model_version), "%s",
               item->valuestring);

   apply_tau_object(cfg, cJSON_GetObjectItemCaseSensitive(cal, "tau"));
}

void config_apply_demotion_settings(config_t *cfg, cJSON *root)
{
   /* Default 1 = shadow: kb_demote_run computes effectiveness scores + fits the
    * per-class demotion profiles but does NOT demote (live suppression only at
    * >= 2). Flipped on as the proven-safe rollout stage — the poison-gate
    * harness shows the score boundary suppresses only closed-outcome poison
    * rows, never clean ones, and gate criterion #4 requires shadow-before-acting.
    * Bump to 2 (live) after reviewing real-recall shadow data. */
   cfg->demotion_enabled = 1;
   cfg->demotion_window = 64;
   cfg->demotion_half_life_days = 30.0;
   cfg->demotion_n_min = 5;

   cJSON *intel = cJSON_GetObjectItemCaseSensitive(root, "intelligence");
   if (!cJSON_IsObject(intel))
      return;

   cJSON *dem = cJSON_GetObjectItemCaseSensitive(intel, "demotion");
   if (!cJSON_IsObject(dem))
      return;

   cJSON *item;

   item = cJSON_GetObjectItemCaseSensitive(dem, "enabled");
   if (cJSON_IsNumber(item))
      cfg->demotion_enabled = (int)item->valuedouble;
   else if (cJSON_IsBool(item))
      cfg->demotion_enabled = cJSON_IsTrue(item) ? 1 : 0;

   item = cJSON_GetObjectItemCaseSensitive(dem, "window");
   if (cJSON_IsNumber(item) && item->valuedouble >= 1)
      cfg->demotion_window = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(dem, "half_life_days");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->demotion_half_life_days = item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(dem, "n_min");
   if (cJSON_IsNumber(item) && item->valuedouble >= 1)
      cfg->demotion_n_min = (int)item->valuedouble;
}

void config_apply_ranking_settings(config_t *cfg, cJSON *root)
{
   snprintf(cfg->kb_fusion_mode, sizeof(cfg->kb_fusion_mode), "rrf");
   cfg->kb_fusion_static_alpha = 0.5;
   cfg->kb_ranker_enabled = 0;
   cfg->ranker_fuse_command[0] = '\0';
   cfg->drift_detect_shadow_enabled = 0;
   cfg->kb_ranker_fit_enabled = 0;
   cfg->kb_ranker_fit_command[0] = '\0';
   cfg->kb_ranker_fit_min_groups = 0;
   cfg->kb_ranker_fit_benchmark[0] = '\0';
   cfg->kb_ranker_fit_bench_k = 0;
   cfg->kb_ranker_fit_objective[0] = '\0';

   cJSON *intel = cJSON_GetObjectItemCaseSensitive(root, "intelligence");
   if (!cJSON_IsObject(intel))
      return;

   cJSON *item;

   item = cJSON_GetObjectItemCaseSensitive(intel, "ranker_fuse_command");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->ranker_fuse_command, sizeof(cfg->ranker_fuse_command), "%s", item->valuestring);

   cJSON *kb_sec = cJSON_GetObjectItemCaseSensitive(intel, "kb");
   if (cJSON_IsObject(kb_sec))
   {
      item = cJSON_GetObjectItemCaseSensitive(kb_sec, "fusion_mode");
      if (cJSON_IsString(item) && item->valuestring)
         snprintf(cfg->kb_fusion_mode, sizeof(cfg->kb_fusion_mode), "%s", item->valuestring);
      item = cJSON_GetObjectItemCaseSensitive(kb_sec, "fusion_static_alpha");
      if (cJSON_IsNumber(item))
         cfg->kb_fusion_static_alpha = item->valuedouble;
   }

   cJSON *rank = cJSON_GetObjectItemCaseSensitive(intel, "ranking");
   if (!cJSON_IsObject(rank))
      return;

   item = cJSON_GetObjectItemCaseSensitive(rank, "kb_ranker_enabled");
   if (cJSON_IsNumber(item))
      cfg->kb_ranker_enabled = (int)item->valuedouble;
   else if (cJSON_IsBool(item))
      cfg->kb_ranker_enabled = cJSON_IsTrue(item) ? 1 : 0;

   item = cJSON_GetObjectItemCaseSensitive(rank, "drift_detect_shadow_enabled");
   if (cJSON_IsNumber(item))
      cfg->drift_detect_shadow_enabled = (int)item->valuedouble;
   else if (cJSON_IsBool(item))
      cfg->drift_detect_shadow_enabled = cJSON_IsTrue(item) ? 1 : 0;

   /* intelligence.ranking.fit.*: the learning-to-rank weight fitter. */
   cJSON *fit = cJSON_GetObjectItemCaseSensitive(rank, "fit");
   if (cJSON_IsObject(fit))
   {
      item = cJSON_GetObjectItemCaseSensitive(fit, "enabled");
      if (cJSON_IsNumber(item))
         cfg->kb_ranker_fit_enabled = (int)item->valuedouble;
      else if (cJSON_IsBool(item))
         cfg->kb_ranker_fit_enabled = cJSON_IsTrue(item) ? 1 : 0;

      item = cJSON_GetObjectItemCaseSensitive(fit, "command");
      if (cJSON_IsString(item) && item->valuestring)
         snprintf(cfg->kb_ranker_fit_command, sizeof(cfg->kb_ranker_fit_command), "%s",
                  item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(fit, "min_groups");
      if (cJSON_IsNumber(item))
         cfg->kb_ranker_fit_min_groups = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(fit, "benchmark");
      if (cJSON_IsString(item) && item->valuestring)
         snprintf(cfg->kb_ranker_fit_benchmark, sizeof(cfg->kb_ranker_fit_benchmark), "%s",
                  item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(fit, "bench_k");
      if (cJSON_IsNumber(item))
         cfg->kb_ranker_fit_bench_k = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(fit, "objective");
      if (cJSON_IsString(item) && item->valuestring)
         snprintf(cfg->kb_ranker_fit_objective, sizeof(cfg->kb_ranker_fit_objective), "%s",
                  item->valuestring);
   }
}

void config_apply_reasoning_settings(config_t *cfg, cJSON *root)
{
   cfg->reasoning_datalog_command[0] = '\0';
   cfg->reasoning_row_budget = 0;
   cfg->reasoning_time_limit_ms = 0;

   if (!root)
      return;
   cJSON *intel = cJSON_GetObjectItemCaseSensitive(root, "intelligence");
   if (!cJSON_IsObject(intel))
      return;

   cJSON *item = cJSON_GetObjectItemCaseSensitive(intel, "reasoning_datalog_command");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->reasoning_datalog_command, sizeof(cfg->reasoning_datalog_command), "%s",
               item->valuestring);

   cJSON *r = cJSON_GetObjectItemCaseSensitive(intel, "reasoning");
   if (!cJSON_IsObject(r))
      return;

   item = cJSON_GetObjectItemCaseSensitive(r, "row_budget");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->reasoning_row_budget = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(r, "time_limit_ms");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->reasoning_time_limit_ms = (int)item->valuedouble;
}

void config_apply_bandit_settings(config_t *cfg, cJSON *root)
{
   cfg->bandit_optimize_command[0] = '\0';
   cfg->bandit_exploration_fraction = 0.05;
   cfg->bandit_ipw_weight_cap = 10.0;
   cfg->bandit_live_decision_enabled = 0;
   cfg->bandit_exploration_window_seconds = 7 * 24 * 3600;

   if (!root)
      return;
   cJSON *intel = cJSON_GetObjectItemCaseSensitive(root, "intelligence");
   if (!cJSON_IsObject(intel))
      return;

   cJSON *item = cJSON_GetObjectItemCaseSensitive(intel, "bandit_optimize_command");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->bandit_optimize_command, sizeof(cfg->bandit_optimize_command), "%s",
               item->valuestring);

   cJSON *b = cJSON_GetObjectItemCaseSensitive(intel, "bandit");
   if (!cJSON_IsObject(b))
      return;

   item = cJSON_GetObjectItemCaseSensitive(b, "default_exploration_fraction");
   if (cJSON_IsNumber(item) && item->valuedouble >= 0.0)
      cfg->bandit_exploration_fraction = item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(b, "ipw_weight_cap");
   if (cJSON_IsNumber(item) && item->valuedouble > 0.0)
      cfg->bandit_ipw_weight_cap = item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(b, "live_decision_enabled");
   /* cJSON_IsTrue for the bool form: a parsed `true` node carries its value in
    * `type`, and leaves valueint at 0 -- so `bandit_live_decision_enabled: true`
    * read back as FALSE. The numeric form still uses valueint. */
   if (cJSON_IsBool(item))
      cfg->bandit_live_decision_enabled = cJSON_IsTrue(item) ? 1 : 0;
   else if (cJSON_IsNumber(item))
      cfg->bandit_live_decision_enabled = item->valueint;

   item = cJSON_GetObjectItemCaseSensitive(b, "exploration_window_seconds");
   if (cJSON_IsNumber(item) && item->valuedouble > 0.0)
      cfg->bandit_exploration_window_seconds = (int)item->valuedouble;
}

void config_apply_planner_settings(config_t *cfg, cJSON *root)
{
   cfg->planner_search_command[0] = '\0';
   cfg->constraint_solver_command[0] = '\0';
   cfg->planner_budget_default = 32;
   cfg->planner_exploration_constant = 1.41;

   if (!root)
      return;
   cJSON *intel = cJSON_GetObjectItemCaseSensitive(root, "intelligence");
   if (!cJSON_IsObject(intel))
      return;

   cJSON *item = cJSON_GetObjectItemCaseSensitive(intel, "planner_search_command");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->planner_search_command, sizeof(cfg->planner_search_command), "%s",
               item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(intel, "constraint_solver_command");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->constraint_solver_command, sizeof(cfg->constraint_solver_command), "%s",
               item->valuestring);

   cJSON *p = cJSON_GetObjectItemCaseSensitive(intel, "planner");
   if (!cJSON_IsObject(p))
      return;

   item = cJSON_GetObjectItemCaseSensitive(p, "budget_default");
   if (cJSON_IsNumber(item) && item->valuedouble > 0.0)
      cfg->planner_budget_default = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(p, "exploration_constant");
   if (cJSON_IsNumber(item) && item->valuedouble > 0.0)
      cfg->planner_exploration_constant = item->valuedouble;
}

void config_apply_mdl_settings(config_t *cfg, cJSON *root)
{
   cfg->kb_mdl_tiebreak_enabled = 1;
   cfg->kb_mdl_bump_drift_alert = 0.30;
   cfg->kb_synthesize_n_attempts = 3;
   cfg->kb_synthesize_command[0] = '\0';
   cfg->kb_reflection_synthesis_shadow = 0;

   if (!root)
      return;
   cJSON *intel = cJSON_GetObjectItemCaseSensitive(root, "intelligence");
   if (!cJSON_IsObject(intel))
      return;

   cJSON *s = cJSON_GetObjectItemCaseSensitive(intel, "synthesize");
   if (!cJSON_IsObject(s))
      return;

   cJSON *item = cJSON_GetObjectItemCaseSensitive(s, "mdl_tiebreak_enabled");
   /* cJSON_IsTrue for the bool form: a parsed `true` node carries its value in
    * `type`, and leaves valueint at 0 -- so `kb_mdl_tiebreak_enabled: true`
    * read back as FALSE. The numeric form still uses valueint. */
   if (cJSON_IsBool(item))
      cfg->kb_mdl_tiebreak_enabled = cJSON_IsTrue(item) ? 1 : 0;
   else if (cJSON_IsNumber(item))
      cfg->kb_mdl_tiebreak_enabled = item->valueint;

   item = cJSON_GetObjectItemCaseSensitive(s, "mdl_bump_drift_alert");
   if (cJSON_IsNumber(item) && item->valuedouble >= 0.0)
      cfg->kb_mdl_bump_drift_alert = item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(s, "synthesize_command");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->kb_synthesize_command, sizeof(cfg->kb_synthesize_command), "%s",
               item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(s, "synthesize_n_attempts");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->kb_synthesize_n_attempts = (int)item->valuedouble;

   /* Shadow gate for idle-reflection synthesis (proposal §4): when on, the pass
    * scores and logs its winner but writes no durable candidate. Fail-closed. */
   item = cJSON_GetObjectItemCaseSensitive(s, "reflection_shadow");
   /* cJSON_IsTrue for the bool form: a parsed `true` node carries its value in
    * `type`, and leaves valueint at 0 -- so `kb_reflection_synthesis_shadow: true`
    * read back as FALSE. The numeric form still uses valueint. */
   if (cJSON_IsBool(item))
      cfg->kb_reflection_synthesis_shadow = cJSON_IsTrue(item) ? 1 : 0;
   else if (cJSON_IsNumber(item))
      cfg->kb_reflection_synthesis_shadow = item->valueint;
}

/* Read an integer-valued JSON number in [lo, INT_MAX] into *out. Rejects
 * non-numbers, non-finite, out-of-range (avoids UB on the double->int cast),
 * and fractional values (30.5 is a config typo, not 30). Returns 1 on success. */
static int review_int_field(const cJSON *item, int lo, int *out)
{
   if (!cJSON_IsNumber(item))
      return 0;
   double d = item->valuedouble;
   if (!(d >= (double)lo) || d > (double)INT_MAX) /* !(>=) also rejects NaN */
      return 0;
   if (d != (double)(long)d) /* integer-valued only */
      return 0;
   *out = (int)d;
   return 1;
}

/* learning.review.*: idle-reflection scheduler knobs. Defaults live in config.c;
 * this only overrides when a key is present, so an operator can turn the
 * (default-on) scheduler off, tune the idle window, or drop the session cooldown
 * (e.g. to 0 for tests / high-throughput dogfood) without a rebuild. */
void config_apply_review_settings(config_t *cfg, cJSON *root)
{
   if (!cfg || !root)
      return;
   cJSON *learning_cfg = cJSON_GetObjectItemCaseSensitive(root, "learning");
   if (!cJSON_IsObject(learning_cfg))
      return;
   cJSON *rv = cJSON_GetObjectItemCaseSensitive(learning_cfg, "review");
   if (!cJSON_IsObject(rv))
      return;

   /* scheduler_enabled is boolean — normalise any bool/number to 0/1. */
   cJSON *item = cJSON_GetObjectItemCaseSensitive(rv, "scheduler_enabled");
   if (cJSON_IsBool(item))
      cfg->review_scheduler_enabled = cJSON_IsTrue(item) ? 1 : 0;
   else if (cJSON_IsNumber(item))
      cfg->review_scheduler_enabled = item->valuedouble != 0.0 ? 1 : 0;

   int v;
   if (review_int_field(cJSON_GetObjectItemCaseSensitive(rv, "idle_trigger_minutes"), 1, &v))
      cfg->review_idle_trigger_minutes = v; /* minutes > 0 */
   if (review_int_field(cJSON_GetObjectItemCaseSensitive(rv, "session_cooldown_hours"), 0, &v))
      cfg->review_session_cooldown_hours = v; /* whole hours; 0 disables the cooldown */
   if (review_int_field(cJSON_GetObjectItemCaseSensitive(rv, "batch_cap"), 1, &v))
      cfg->review_batch_cap = v; /* cap > 0 */
}

/* Inverse of the intelligence.* parsers (calibrate/demotion/bandit) so config_save
 * does not drop these tuning gates on the next save. Emits a sub-object only when
 * it differs from defaults; ints that carry modes (calibrate/demotion enabled)
 * are written as numbers to preserve non-0/1 values. */
void config_save_intelligence(const config_t *cfg, cJSON *root)
{
   if (!cfg || !root)
      return;
   /* calibration_enabled defaults to 1 (shadow); emit when it differs from the
    * default so the off (0) and A/B (2) states survive a save round-trip. */
   int cal_any =
       cfg->calibration_enabled != 1 || cfg->calibration_command[0] ||
       cfg->calibration_buckets != 10 || cfg->calibration_prior_alpha0 != 2.0 ||
       cfg->calibration_prior_beta0 != 1.0 || cfg->calibration_credible_delta != 0.10 ||
       cfg->calibration_conformal_window != 500 || cfg->calibration_conformal_epsilon != 0.05 ||
       cfg->calibration_tau_memory_auto != 0.70 || cfg->calibration_tau_memory_flag != 0.55 ||
       cfg->calibration_tau_working_profile_auto != 0.80 ||
       cfg->calibration_tau_working_profile_flag != 0.65;
   /* demotion_enabled defaults to 1 (shadow); emit when it differs from the
    * default so the off (0) and live (2) states survive a save round-trip. */
   int dem_any = cfg->demotion_enabled != 1 || cfg->demotion_window != 64 ||
                 cfg->demotion_half_life_days != 30.0 || cfg->demotion_n_min != 5;
   int bandit_any = cfg->bandit_optimize_command[0] || cfg->bandit_exploration_fraction != 0.05 ||
                    cfg->bandit_ipw_weight_cap != 10.0 || cfg->bandit_live_decision_enabled ||
                    cfg->bandit_exploration_window_seconds != 7 * 24 * 3600;
   if (!cal_any && !dem_any && !bandit_any)
      return;

   cJSON *intel = cJSON_GetObjectItemCaseSensitive(root, "intelligence");
   if (!intel)
      intel = cJSON_AddObjectToObject(root, "intelligence");
   if (!intel)
      return;

   if (cal_any)
   {
      cJSON *cal = cJSON_AddObjectToObject(intel, "calibrate");
      if (cal)
      {
         cJSON_AddNumberToObject(cal, "enabled", cfg->calibration_enabled);
         if (cfg->calibration_command[0])
            cJSON_AddStringToObject(cal, "command", cfg->calibration_command);
         cJSON_AddNumberToObject(cal, "buckets", cfg->calibration_buckets);
         cJSON_AddNumberToObject(cal, "prior_alpha0", cfg->calibration_prior_alpha0);
         cJSON_AddNumberToObject(cal, "prior_beta0", cfg->calibration_prior_beta0);
         cJSON_AddNumberToObject(cal, "credible_delta", cfg->calibration_credible_delta);
         cJSON_AddNumberToObject(cal, "conformal_window", cfg->calibration_conformal_window);
         cJSON_AddNumberToObject(cal, "conformal_epsilon", cfg->calibration_conformal_epsilon);
         cJSON_AddStringToObject(cal, "prompt_version", cfg->calibration_prompt_version);
         cJSON_AddStringToObject(cal, "model_version", cfg->calibration_model_version);
         cJSON *tau = cJSON_AddObjectToObject(cal, "tau");
         if (tau)
         {
            cJSON *mem = cJSON_AddObjectToObject(tau, "memory");
            if (mem)
            {
               cJSON_AddNumberToObject(mem, "auto", cfg->calibration_tau_memory_auto);
               cJSON_AddNumberToObject(mem, "flag", cfg->calibration_tau_memory_flag);
            }
            cJSON *wp = cJSON_AddObjectToObject(tau, "working_profile");
            if (wp)
            {
               cJSON_AddNumberToObject(wp, "auto", cfg->calibration_tau_working_profile_auto);
               cJSON_AddNumberToObject(wp, "flag", cfg->calibration_tau_working_profile_flag);
            }
         }
      }
   }
   if (dem_any)
   {
      cJSON *dem = cJSON_AddObjectToObject(intel, "demotion");
      if (dem)
      {
         cJSON_AddNumberToObject(dem, "enabled", cfg->demotion_enabled);
         cJSON_AddNumberToObject(dem, "window", cfg->demotion_window);
         cJSON_AddNumberToObject(dem, "half_life_days", cfg->demotion_half_life_days);
         cJSON_AddNumberToObject(dem, "n_min", cfg->demotion_n_min);
      }
   }
   if (bandit_any)
   {
      if (cfg->bandit_optimize_command[0])
         cJSON_AddStringToObject(intel, "bandit_optimize_command", cfg->bandit_optimize_command);
      cJSON *b = cJSON_AddObjectToObject(intel, "bandit");
      if (b)
      {
         cJSON_AddNumberToObject(b, "default_exploration_fraction",
                                 cfg->bandit_exploration_fraction);
         cJSON_AddNumberToObject(b, "ipw_weight_cap", cfg->bandit_ipw_weight_cap);
         cJSON_AddNumberToObject(b, "live_decision_enabled", cfg->bandit_live_decision_enabled);
         cJSON_AddNumberToObject(b, "exploration_window_seconds",
                                 cfg->bandit_exploration_window_seconds);
      }
   }
}
