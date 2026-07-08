/* kb_intel_payload.c: shared JSON payloads for intelligence readiness/export. */

#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "db2/bandit.h"
#include "db2/calibration.h"
#include "db2/demotion.h"
#include "db2/memory_query.h"
#include "kb_bandit.h"
#include "kb_bandit_registry.h"
#include "kb_intel_payload.h"
#include "kb_ranker_fit.h"
#include "memory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cJSON *kb_intel_calibrate_readiness_response(void)
{
   int min_rows = 200;
   int n = db2_calibration_surfaces_with_data(min_rows);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "ready", n > 0 ? 1 : 0);
   cJSON_AddNumberToObject(resp, "surfaces_with_data", n < 0 ? 0 : n);
   cJSON_AddNumberToObject(resp, "min_rows_required", min_rows);
   return resp;
}

#define KB_INTEL_DEMOTE_DRY_MAX 4096

cJSON *kb_intel_demote_check_response(void)
{
   config_t cfg;
   config_load(&cfg);

   db2_demotion_candidate_t *candidates =
       calloc(KB_INTEL_DEMOTE_DRY_MAX, sizeof(db2_demotion_candidate_t));
   if (!candidates)
      return NULL;

   int n_candidates =
       db2_demotion_candidates(cfg.demotion_n_min, candidates, KB_INTEL_DEMOTE_DRY_MAX);
   if (n_candidates < 0)
      n_candidates = 0;

   typedef struct
   {
      char kind[64];
      double score;
   } scored_t;

   scored_t *rows = calloc((size_t)(n_candidates > 0 ? n_candidates : 1), sizeof(scored_t));
   if (!rows)
   {
      free(candidates);
      return NULL;
   }

   int n_scored = 0;
   for (int i = 0; i < n_candidates; i++)
   {
      double score = db2_demotion_score(candidates[i].row_id, cfg.demotion_window,
                                        cfg.demotion_half_life_days, cfg.demotion_n_min);
      if (isnan(score))
         continue;
      memory_t mem;
      memset(&mem, 0, sizeof(mem));
      if (db2_memory_get(candidates[i].row_id, &mem) != 0 || !mem.kind[0])
         continue;
      snprintf(rows[n_scored].kind, sizeof(rows[n_scored].kind), "%s", mem.kind);
      rows[n_scored].score = score;
      n_scored++;
   }
   free(candidates);

   int would_demote = 0;
   cJSON *by_kind = cJSON_CreateArray();
   for (int i = 0; i < n_scored; i++)
   {
      int seen = 0;
      for (int j = 0; j < i; j++)
      {
         if (strcmp(rows[j].kind, rows[i].kind) == 0)
         {
            seen = 1;
            break;
         }
      }
      if (seen)
         continue;

      char pbuf[2048];
      double p10 = 0.0;
      if (db2_demotion_profile_read(rows[i].kind, "global", "", pbuf, sizeof(pbuf)) == 0)
      {
         cJSON *pj = cJSON_ParseWithLength(pbuf, strlen(pbuf));
         cJSON *percs = pj ? cJSON_GetObjectItemCaseSensitive(pj, "score_percentiles") : NULL;
         cJSON *p10j = percs ? cJSON_GetObjectItemCaseSensitive(percs, "p10") : NULL;
         p10 = cJSON_IsNumber(p10j) ? p10j->valuedouble : 0.0;
         cJSON_Delete(pj);
      }

      int kind_scored = 0;
      int below = 0;
      for (int j = i; j < n_scored; j++)
      {
         if (strcmp(rows[j].kind, rows[i].kind) != 0)
            continue;
         kind_scored++;
         if (rows[j].score < p10)
            below++;
      }
      would_demote += below;

      cJSON *entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "kind", rows[i].kind);
      cJSON_AddNumberToObject(entry, "scored", kind_scored);
      cJSON_AddNumberToObject(entry, "would_demote", below);
      cJSON_AddNumberToObject(entry, "p10", p10);
      cJSON_AddItemToArray(by_kind, entry);
   }
   free(rows);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      cJSON_Delete(by_kind);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "candidates", n_candidates);
   cJSON_AddNumberToObject(resp, "scored", n_scored);
   cJSON_AddNumberToObject(resp, "would_demote", would_demote);
   cJSON_AddNumberToObject(resp, "demotion_enabled", cfg.demotion_enabled);
   cJSON_AddItemToObject(resp, "by_kind", by_kind);
   return resp;
}

#define KB_INTEL_BANDIT_EXPORT_LIMIT 500
#define KB_INTEL_BANDIT_EXPORT_BUFSZ (256 * 1024)

/* Build one {decision_point, decisions, arm_stats} object for a decision point
 * that is actually present in the bandit decision log.  Arms are discovered from
 * the log (so they appear even before any reward has been observed). */
static cJSON *intel_bandit_point_obj(const char *decision_point)
{
   cJSON *pt = cJSON_CreateObject();
   if (!pt)
      return NULL;
   cJSON_AddStringToObject(pt, "decision_point", decision_point);

   /* Closed decisions for offline replay. */
   char *buf = malloc(KB_INTEL_BANDIT_EXPORT_BUFSZ);
   cJSON *decisions = NULL;
   if (buf)
   {
      buf[0] = '\0';
      db2_bandit_decisions_export(decision_point, KB_INTEL_BANDIT_EXPORT_LIMIT, buf,
                                  KB_INTEL_BANDIT_EXPORT_BUFSZ);
      decisions = cJSON_ParseWithLength(buf, strlen(buf));
      free(buf);
   }
   cJSON_AddItemToObject(pt, "decisions", decisions ? decisions : cJSON_CreateArray());

   /* Per-arm stats, over the arms observed in the log for this point. */
   cJSON *arm_stats_arr = cJSON_CreateArray();
   char arms_buf[8192];
   arms_buf[0] = '\0';
   db2_bandit_arms_list(decision_point, arms_buf, sizeof(arms_buf));
   cJSON *arms = cJSON_ParseWithLength(arms_buf, strlen(arms_buf));
   if (cJSON_IsArray(arms))
   {
      cJSON *arm;
      cJSON_ArrayForEach(arm, arms)
      {
         if (!cJSON_IsString(arm) || !arm->valuestring[0])
            continue;
         db2_bandit_arm_stats_t stats;
         memset(&stats, 0, sizeof(stats));
         db2_bandit_arm_stats_read(decision_point, arm->valuestring, &stats);

         cJSON *entry = cJSON_CreateObject();
         cJSON_AddStringToObject(entry, "arm_id", arm->valuestring);
         cJSON_AddNumberToObject(entry, "n_decisions", (double)stats.n_decisions);
         cJSON_AddNumberToObject(entry, "n_rewards", (double)stats.n_rewards);
         cJSON_AddNumberToObject(entry, "sum_reward", stats.sum_reward);
         cJSON_AddNumberToObject(entry, "posterior_alpha", stats.posterior_alpha);
         cJSON_AddNumberToObject(entry, "posterior_beta", stats.posterior_beta);
         cJSON_AddItemToArray(arm_stats_arr, entry);
      }
   }
   cJSON_Delete(arms);
   cJSON_AddItemToObject(pt, "arm_stats", arm_stats_arr);
   return pt;
}

/* Declared decision points from the registry (source of truth) — present even
 * when a point has no logged decisions yet.  Lets `aimee optimize` show what is
 * tunable, with each point's arm set and reward function. */
static cJSON *intel_bandit_registry_array(void)
{
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return NULL;
   for (int i = 0; i < kb_bandit_registry_count(); i++)
   {
      const kb_bandit_decision_point_t *dp = kb_bandit_registry_at(i);
      if (!dp)
         continue;
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "decision_point", dp->id);
      cJSON_AddStringToObject(e, "description", dp->description);
      cJSON_AddStringToObject(e, "reward_fn", dp->reward_fn);
      cJSON_AddStringToObject(e, "status", dp->status);
      cJSON *arms = cJSON_CreateArray();
      for (int a = 0; a < dp->n_arms; a++)
         cJSON_AddItemToArray(arms, cJSON_CreateString(dp->arms[a]));
      cJSON_AddItemToObject(e, "arms", arms);
      cJSON_AddItemToArray(arr, e);
   }
   return arr;
}

/* Export bandit state for every decision point that has logged decisions.
 *
 * Data-driven: the set of points and their arms is read from the decision log
 * (db2_bandit_decision_points_list / db2_bandit_arms_list), not hard-coded, so
 * introspection reflects what is actually sampled at runtime.  The top-level
 * `decision_point` mirrors the primary (most-recent) point for backward
 * compatibility; `points` carries the full per-point breakdown; `registry`
 * lists every declared decision point (even those not yet sampled). */
cJSON *kb_intel_bandit_export_response(void)
{
   char points_buf[8192];
   points_buf[0] = '\0';
   db2_bandit_decision_points_list(points_buf, sizeof(points_buf));
   cJSON *names = cJSON_ParseWithLength(points_buf, strlen(points_buf));

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      cJSON_Delete(names);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   cJSON *points = cJSON_CreateArray();
   const char *primary = "";
   if (cJSON_IsArray(names))
   {
      cJSON *name;
      cJSON_ArrayForEach(name, names)
      {
         if (!cJSON_IsString(name) || !name->valuestring[0])
            continue;
         if (!primary[0])
            primary = name->valuestring;
         cJSON *pt = intel_bandit_point_obj(name->valuestring);
         if (pt)
            cJSON_AddItemToArray(points, pt);
      }
   }
   /* Back-compat: a single primary point at the top level (string is copied). */
   cJSON_AddStringToObject(resp, "decision_point", primary);
   cJSON_AddItemToObject(resp, "points", points);
   cJSON_AddItemToObject(resp, "registry", intel_bandit_registry_array());
   cJSON_Delete(names);
   return resp;
}

static cJSON *intel_bandit_replay_err(const char *msg)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "error");
   cJSON_AddStringToObject(resp, "message", msg ? msg : "bad request");
   return resp;
}

cJSON *kb_intel_bandit_replay_record_response(const char *body_json, int body_len)
{
   if (!body_json || body_len <= 0)
      return intel_bandit_replay_err("missing body");

   cJSON *body = cJSON_ParseWithLength(body_json, (size_t)body_len);
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return intel_bandit_replay_err("body must be a JSON object");
   }

   const char *dp = cJSON_GetStringValue(cJSON_GetObjectItem(body, "decision_point"));
   cJSON *result = cJSON_GetObjectItem(body, "result");
   if (!dp || !dp[0])
   {
      cJSON_Delete(body);
      return intel_bandit_replay_err("decision_point is required");
   }
   if (!result || !cJSON_IsObject(result))
   {
      cJSON_Delete(body);
      return intel_bandit_replay_err("result is required (replay-tool output object)");
   }

   char *result_str = cJSON_PrintUnformatted(result);
   if (!result_str)
   {
      cJSON_Delete(body);
      return intel_bandit_replay_err("failed to serialize result");
   }

   char artifact_id[64] = "";
   int rc = kb_bandit_record_replay_evidence(dp, result_str, artifact_id, sizeof(artifact_id));
   free(result_str);
   cJSON_Delete(body);

   if (rc != 0)
      return intel_bandit_replay_err("failed to record benchmark_trace");

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "artifact_id", artifact_id);
   cJSON_AddStringToObject(resp, "kind", "benchmark_trace");
   return resp;
}

int kb_intel_bandit_replay_record_http(const char *body, int body_len, char *out_buf, int out_cap)
{
   if (!out_buf || out_cap <= 0)
      return 500;
   cJSON *resp = kb_intel_bandit_replay_record_response(body, body_len);
   char *json = resp ? cJSON_PrintUnformatted(resp) : NULL;
   cJSON_Delete(resp);
   if (!json)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"status\":\"error\",\"message\":\"out of memory\"}");
      return 500;
   }
   size_t n = strlen(json);
   if (n >= (size_t)out_cap)
   {
      free(json);
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"error\",\"message\":\"response too large\"}");
      return 500;
   }
   memcpy(out_buf, json, n + 1);
   free(json);
   return 200;
}

/* Serialize a response object into out_buf; returns the HTTP status. */
static int intel_bandit_emit_http(cJSON *resp, char *out_buf, int out_cap)
{
   char *json = resp ? cJSON_PrintUnformatted(resp) : NULL;
   cJSON_Delete(resp);
   if (!json)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"status\":\"error\",\"message\":\"out of memory\"}");
      return 500;
   }
   size_t n = strlen(json);
   if (n >= (size_t)out_cap)
   {
      free(json);
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"error\",\"message\":\"response too large\"}");
      return 500;
   }
   memcpy(out_buf, json, n + 1);
   free(json);
   return 200;
}

/* bandit.sample: select an arm for a decision point (Thompson via the optimize
 * sidecar), log the decision, and return {arm, decision_id}. Server-side
 * decision points (e.g. delegate_routing) reach the DB2 bandit through this. */
cJSON *kb_intel_bandit_sample_response(const char *body_json, int body_len)
{
   if (!body_json || body_len <= 0)
      return intel_bandit_replay_err("missing body");
   cJSON *body = cJSON_ParseWithLength(body_json, (size_t)body_len);
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return intel_bandit_replay_err("body must be a JSON object");
   }
   const char *dp = cJSON_GetStringValue(cJSON_GetObjectItem(body, "decision_point"));
   cJSON *arms = cJSON_GetObjectItem(body, "arms");
   if (!dp || !dp[0])
   {
      cJSON_Delete(body);
      return intel_bandit_replay_err("decision_point is required");
   }
   if (!cJSON_IsArray(arms) || cJSON_GetArraySize(arms) < 1)
   {
      cJSON_Delete(body);
      return intel_bandit_replay_err("arms (non-empty array) is required");
   }
   int total = cJSON_GetArraySize(arms);
   char arm_ids[KB_BANDIT_MAX_ARMS][KB_BANDIT_MAX_ARM_ID];
   int na = 0;
   for (int i = 0; i < total && na < KB_BANDIT_MAX_ARMS; i++)
   {
      cJSON *a = cJSON_GetArrayItem(arms, i);
      if (cJSON_IsString(a) && a->valuestring[0])
         snprintf(arm_ids[na++], KB_BANDIT_MAX_ARM_ID, "%s", a->valuestring);
   }
   cJSON *ctx = cJSON_GetObjectItem(body, "context");
   char *ctx_str = (ctx && cJSON_IsObject(ctx)) ? cJSON_PrintUnformatted(ctx) : NULL;

   config_t cfg;
   config_load(&cfg);
   char decision_id[KB_BANDIT_MAX_DECISION] = {0};
   int idx = (na >= 1) ? kb_bandit_sample(&cfg, dp, ctx_str, arm_ids, na, decision_id) : -1;
   free(ctx_str);
   cJSON_Delete(body);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (idx < 0 || idx >= na)
   {
      /* Sampling disabled (no optimize command) or failed — caller falls back to
       * its default routing; no decision is logged. */
      cJSON_AddStringToObject(resp, "status", "disabled");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "arm", arm_ids[idx]);
   cJSON_AddStringToObject(resp, "decision_id", decision_id);
   return resp;
}

/* bandit.close: close a logged decision with its observed reward [0,1] and update
 * the arm posterior. */
cJSON *kb_intel_bandit_close_response(const char *body_json, int body_len)
{
   if (!body_json || body_len <= 0)
      return intel_bandit_replay_err("missing body");
   cJSON *body = cJSON_ParseWithLength(body_json, (size_t)body_len);
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return intel_bandit_replay_err("body must be a JSON object");
   }
   const char *dp = cJSON_GetStringValue(cJSON_GetObjectItem(body, "decision_point"));
   const char *did = cJSON_GetStringValue(cJSON_GetObjectItem(body, "decision_id"));
   const char *arm = cJSON_GetStringValue(cJSON_GetObjectItem(body, "arm_id"));
   cJSON *rj = cJSON_GetObjectItem(body, "reward");
   if (!dp || !dp[0] || !did || !did[0] || !arm || !arm[0])
   {
      cJSON_Delete(body);
      return intel_bandit_replay_err("decision_point, decision_id and arm_id are required");
   }
   double reward = cJSON_IsNumber(rj) ? rj->valuedouble : 0.0;
   if (reward < 0.0)
      reward = 0.0;
   if (reward > 1.0)
      reward = 1.0;

   config_t cfg;
   config_load(&cfg);
   int rc = kb_bandit_reward(&cfg, dp, did, arm, reward);
   cJSON_Delete(body);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   if (rc != 0)
      cJSON_AddStringToObject(resp, "message", "failed to close decision");
   return resp;
}

int kb_intel_bandit_sample_http(const char *body, int body_len, char *out_buf, int out_cap)
{
   if (!out_buf || out_cap <= 0)
      return 500;
   return intel_bandit_emit_http(kb_intel_bandit_sample_response(body, body_len), out_buf, out_cap);
}

int kb_intel_bandit_close_http(const char *body, int body_len, char *out_buf, int out_cap)
{
   if (!out_buf || out_cap <= 0)
      return 500;
   return intel_bandit_emit_http(kb_intel_bandit_close_response(body, body_len), out_buf, out_cap);
}

/* bandit.promote: persist {decision_point, arm} as the production-default arm
 * (consumers honour it when live sampling is off), recording the prior default
 * as rollback. Returns {status, rollback_arm}. */
cJSON *kb_intel_bandit_promote_response(const char *body_json, int body_len)
{
   if (!body_json || body_len <= 0)
      return intel_bandit_replay_err("missing body");
   cJSON *body = cJSON_ParseWithLength(body_json, (size_t)body_len);
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return intel_bandit_replay_err("body must be a JSON object");
   }
   const char *dp = cJSON_GetStringValue(cJSON_GetObjectItem(body, "decision_point"));
   const char *arm = cJSON_GetStringValue(cJSON_GetObjectItem(body, "arm"));
   if (!dp || !dp[0] || !arm || !arm[0])
   {
      cJSON_Delete(body);
      return intel_bandit_replay_err("decision_point and arm are required");
   }

   char rollback[KB_BANDIT_MAX_ARM_ID] = "";
   db2_bandit_promotion_get(dp, rollback, sizeof(rollback)); /* prior default, if any */
   int rc = db2_bandit_promotion_set(dp, arm, rollback);
   cJSON_Delete(body);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   if (rc == 0)
      cJSON_AddStringToObject(resp, "rollback_arm", rollback);
   else
      cJSON_AddStringToObject(resp, "message", "failed to persist promotion");
   return resp;
}

int kb_intel_bandit_promote_http(const char *body, int body_len, char *out_buf, int out_cap)
{
   if (!out_buf || out_cap <= 0)
      return 500;
   return intel_bandit_emit_http(kb_intel_bandit_promote_response(body, body_len), out_buf,
                                 out_cap);
}

cJSON *kb_intel_ranker_export_view_response(void)
{
   char *json = kb_ranker_export_view_json("kb_document", NULL);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
   {
      resp = cJSON_CreateObject();
      if (resp)
      {
         cJSON_AddStringToObject(resp, "status", "error");
         cJSON_AddStringToObject(resp, "error", "training view unavailable");
      }
   }
   return resp;
}

int kb_intel_ranker_fit_http(const char *body, int body_len, char *out_buf, int out_cap)
{
   (void)body;
   (void)body_len;
   if (!out_buf || out_cap <= 0)
      return 500;
   config_t cfg;
   config_load(&cfg);
   char *report = NULL;
   /* rc distinguishes committed(0)/refused(1)/error(-1); the report carries the
    * detail. HTTP 200 for any well-formed report (the fit ran, even if it
    * refused/held) — a fit that declines to promote is a success, not a 5xx. */
   int rc = kb_ranker_fit_run(&cfg, NULL, 0, &report);
   if (!report)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"status\":\"error\",\"error\":\"fit produced no report\"}");
      return 500;
   }
   snprintf(out_buf, (size_t)out_cap, "%s", report);
   free(report);
   return rc < 0 ? 500 : 200;
}
