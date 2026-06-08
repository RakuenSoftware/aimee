/* kb_bandit.c: Contextual bandit arm selection and reward feedback.
 * See docs/proposals/accepted/contextual-bandits-and-counterfactual-replay.md
 */

#include "kb_bandit.h"
#include "db2/artifacts.h"
#include "db2/bandit.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"
#include "headers/platform_process.h"

#include <cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ---- arm registration ---- */

int kb_bandit_arm_register(const char *decision_point, const char *arm_id, const char *variant_json,
                           const char *reward_schema_json)
{
   if (!decision_point || !arm_id)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Composite scope_id encodes both decision_point and arm_id, enabling
    * a JSON-operator-free lookup that works with the SQLite test shim. */
   char composite_scope_id[256];
   snprintf(composite_scope_id, sizeof(composite_scope_id), "%s:%s", decision_point, arm_id);

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT id FROM artifacts"
                                          " WHERE kind = 'policy_arm'"
                                          "   AND scope_kind = 'bandit_arm'"
                                          "   AND scope_id   = ?1"
                                          " LIMIT 1",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", composite_scope_id);

   char existing_id[64] = "";
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      if (v)
         strncpy(existing_id, v, sizeof(existing_id) - 1);
   }
   aimee_pg_finalize(st);

   /* Build payload JSON. */
   cJSON *payload = cJSON_CreateObject();
   cJSON_AddStringToObject(payload, "decision_point", decision_point);
   cJSON_AddStringToObject(payload, "arm_id", arm_id);

   cJSON *variant = variant_json ? cJSON_Parse(variant_json) : NULL;
   cJSON_AddItemToObject(payload, "variant", variant ? variant : cJSON_CreateObject());

   cJSON *reward_schema = reward_schema_json ? cJSON_Parse(reward_schema_json) : NULL;
   cJSON_AddItemToObject(payload, "reward_schema",
                         reward_schema ? reward_schema : cJSON_CreateObject());

   cJSON *posterior = cJSON_CreateObject();
   cJSON_AddStringToObject(posterior, "kind", "beta");
   cJSON_AddNumberToObject(posterior, "alpha", 1.0);
   cJSON_AddNumberToObject(posterior, "beta", 1.0);
   cJSON_AddItemToObject(payload, "posterior", posterior);

   cJSON_AddFalseToObject(payload, "retired");

   char *payload_str = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   if (!payload_str)
      return -1;

   int rc = 0;
   if (existing_id[0])
   {
      /* Update existing artifact payload; no ::jsonb cast for SQLite compat. */
      aimee_pg_stmt_t *upd = aimee_pg_prepare(
          conn, "UPDATE artifacts SET payload = ?1 WHERE id = ?2", err, sizeof(err));
      if (upd)
      {
         aimee_pg_bind_text(upd, "?1", payload_str);
         aimee_pg_bind_text(upd, "?2", existing_id);
         aimee_pg_step(upd, err, sizeof(err));
         aimee_pg_finalize(upd);
      }
      else
      {
         rc = -1;
      }
   }
   else
   {
      char id[64];
      db2_artifact_gen_id(id, sizeof(id));
      rc = db2_artifact_write(id, "policy_arm", "proposed", "bandit_arm", composite_scope_id,
                              "system", 1.0, payload_str);
   }

   free(payload_str);
   return rc;
}

/* ---- Thompson sampling ---- */

/* Simple djb2 hash for context fingerprinting. */
static void _context_hash(const char *s, char *out, size_t out_len)
{
   unsigned long h = 5381;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      h = h * 33 + *p;
   snprintf(out, out_len, "%08lx", h & 0xffffffffUL);
}

int kb_bandit_sample(const config_t *cfg, const char *decision_point, const char *context_json,
                     const char (*arm_ids)[KB_BANDIT_MAX_ARM_ID], int n_arms, char *decision_id_out)
{
   if (!cfg || !cfg->bandit_optimize_command[0])
      return -1;
   if (!decision_point || !arm_ids || n_arms <= 0)
      return -1;

   /* Budget Gate: refuse exploration when the observed window fraction is
    * already at or above the configured budget. Below a minimum sample count
    * the Gate stands down (the per-decision random draw still provides
    * exploration on cold-start). */
   double budget = cfg->bandit_exploration_fraction;
   int window_seconds = cfg->bandit_exploration_window_seconds;
   if (window_seconds <= 0)
      window_seconds = 7 * 24 * 3600;

   long long n_explore = 0, n_total = 0;
   db2_bandit_explore_stats(decision_point, window_seconds, &n_explore, &n_total);

   int gate_refuses = 0;
   if (n_total >= 20 && budget > 0.0)
   {
      double observed = (double)n_explore / (double)n_total;
      if (observed >= budget)
         gate_refuses = 1;
   }

   double u = (double)rand() / ((double)RAND_MAX + 1.0);
   int allow_explore = gate_refuses ? 0 : (u < budget);

   /* Read current arm posteriors from DB2. */
   cJSON *arms_arr = cJSON_CreateArray();
   for (int i = 0; i < n_arms; i++)
   {
      db2_bandit_arm_stats_t stats;
      memset(&stats, 0, sizeof(stats));
      db2_bandit_arm_stats_read(decision_point, arm_ids[i], &stats);

      cJSON *arm = cJSON_CreateObject();
      cJSON_AddStringToObject(arm, "arm_id", arm_ids[i]);

      cJSON *post = cJSON_CreateObject();
      cJSON_AddStringToObject(post, "kind", "beta");
      cJSON_AddNumberToObject(post, "alpha", stats.posterior_alpha);
      cJSON_AddNumberToObject(post, "beta", stats.posterior_beta);
      cJSON_AddItemToObject(arm, "posterior", post);

      cJSON_AddItemToArray(arms_arr, arm);
   }

   /* Build sidecar request. */
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "version", 1);
   cJSON_AddStringToObject(req, "role", "optimize");
   cJSON_AddStringToObject(req, "model_version", "linear-ts-v1");
   cJSON_AddStringToObject(req, "prompt_version", "beta-bernoulli-v1");

   cJSON *inputs = cJSON_CreateObject();
   cJSON_AddStringToObject(inputs, "decision_point", decision_point);

   cJSON *ctx_obj = context_json ? cJSON_Parse(context_json) : NULL;
   cJSON_AddItemToObject(inputs, "context", ctx_obj ? ctx_obj : cJSON_CreateObject());
   cJSON_AddBoolToObject(inputs, "allow_explore", allow_explore);
   cJSON_AddItemToObject(inputs, "arms", arms_arr);
   cJSON_AddItemToObject(req, "inputs", inputs);

   char *req_str = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!req_str)
      return -1;

   /* Call sidecar. */
   char *out = NULL;
   size_t out_len = 0;
   platform_exec_pipe(cfg->bandit_optimize_command, req_str, strlen(req_str), &out, &out_len);
   free(req_str);

   if (!out)
      return -1;

   /* Parse response. */
   cJSON *resp = cJSON_ParseWithLength(out, (int)out_len);
   free(out);
   if (!resp)
      return -1;

   const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(resp, "status"));
   if (!status || strcmp(status, "ok") != 0)
   {
      cJSON_Delete(resp);
      return -1;
   }

   const char *selected = cJSON_GetStringValue(cJSON_GetObjectItem(resp, "selected_arm"));
   cJSON *prop_item = cJSON_GetObjectItem(resp, "propensity");
   double propensity = prop_item ? prop_item->valuedouble : 1.0;
   cJSON_Delete(resp);

   /* Find selected arm index; fall back to 0 on unknown. */
   int arm_idx = 0;
   if (selected)
   {
      for (int i = 0; i < n_arms; i++)
      {
         if (strcmp(arm_ids[i], selected) == 0)
         {
            arm_idx = i;
            break;
         }
      }
   }

   /* Log decision to DB2. */
   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   char ctx_hash[16] = "";
   if (context_json)
      _context_hash(context_json, ctx_hash, sizeof(ctx_hash));

   db2_bandit_decision_insert(id, decision_point, arm_ids[arm_idx], ctx_hash, propensity,
                              allow_explore);

   if (decision_id_out)
      strncpy(decision_id_out, id, KB_BANDIT_MAX_DECISION - 1);

   return arm_idx;
}

/* ---- reward feedback ---- */

int kb_bandit_reward(const config_t *cfg, const char *decision_point, const char *decision_id,
                     const char *arm_id, double reward)
{
   (void)cfg;
   if (!decision_point || !decision_id || !arm_id)
      return -1;

   /* Close the decision log entry. */
   if (db2_bandit_decision_close(decision_id, reward) != 0)
      return -1;

   /* Read current arm stats to get the posterior. */
   db2_bandit_arm_stats_t stats;
   memset(&stats, 0, sizeof(stats));
   db2_bandit_arm_stats_read(decision_point, arm_id, &stats);

   /* Beta-Bernoulli posterior update: alpha += reward, beta += (1 - reward).
    * For bounded rewards in [0,1] this approximates a Bernoulli observation. */
   double new_alpha = stats.posterior_alpha + reward;
   double new_beta = stats.posterior_beta + (1.0 - reward);

   return db2_bandit_arm_stats_update(decision_point, arm_id, reward, new_alpha, new_beta);
}

double kb_bandit_recall_sufficiency_reward(int n_results, int limit)
{
   if (n_results <= 0)
      return 0.0;
   if (limit > 0 && n_results >= limit)
      return 0.5; /* truncated at the cap: a larger limit might have helped */
   return 1.0;    /* sufficient recall without truncation */
}

/* ---- replay evidence ---- */

int kb_bandit_record_replay_evidence(const char *decision_point, const char *result_json,
                                     char *id_out, size_t id_out_len)
{
   if (!decision_point || !decision_point[0] || !result_json || !result_json[0])
      return -1;

   /* Parse to discover estimator + status; we wrap the original payload so the
    * downstream consumer keeps full detail (variance, ci, n_*, lift, z…). */
   cJSON *parsed = cJSON_Parse(result_json);
   if (!parsed || !cJSON_IsObject(parsed))
   {
      cJSON_Delete(parsed);
      return -1;
   }

   const char *estimator = cJSON_GetStringValue(cJSON_GetObjectItem(parsed, "estimator"));
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(parsed, "status"));
   if (!estimator)
      estimator = "unknown";
   if (!status)
      status = "unknown";

   cJSON *wrap = cJSON_CreateObject();
   cJSON_AddStringToObject(wrap, "source", "bandit_replay");
   cJSON_AddStringToObject(wrap, "decision_point", decision_point);
   cJSON_AddStringToObject(wrap, "estimator", estimator);
   cJSON_AddStringToObject(wrap, "status", status);
   cJSON_AddItemToObject(wrap, "result", cJSON_Duplicate(parsed, 1));
   cJSON_Delete(parsed);

   char *payload = cJSON_PrintUnformatted(wrap);
   cJSON_Delete(wrap);
   if (!payload)
      return -1;

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   /* Replay evidence is durable, system-scoped: scope_kind="bandit_replay",
    * scope_id=<decision_point>. Confidence reflects estimator status:
    * ok → 1.0, insufficient_data → 0.5, else → 0.5. */
   double confidence = (strcmp(status, "ok") == 0) ? 1.0 : 0.5;
   int rc = db2_artifact_write(id, "benchmark_trace", "committed", "bandit_replay", decision_point,
                               "", confidence, payload);
   free(payload);
   if (rc != 0)
      return -1;

   if (id_out && id_out_len > 0)
   {
      strncpy(id_out, id, id_out_len - 1);
      id_out[id_out_len - 1] = '\0';
   }
   return 0;
}
