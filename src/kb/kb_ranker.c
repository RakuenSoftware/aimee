/* kb_ranker.c: linear in-process ranker for KB-hybrid retrieval.
 * See
 * docs/proposals/accepted/statistical-decision-systems-for-ranking-calibration-and-experiments.md
 */

#include "kb_ranker.h"
#include "kb_features.h"
#include "db2/artifacts.h"

#define KB_RANKER_FEATURE_SET_VERSION KB_FEATURE_SET_VERSION
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"
#include "aimee.h"
#include "log.h"

#include <cJSON.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cached linear weights for the active ranker model. */
static struct
{
   double w_dense;
   double w_lex;
   double w_recency;
   double w_sketch_frequency;
   double w_sketch_distinct;
   int loaded;
} g_weights = {0.6, 0.4, 0.0, 0.0, 0.0, 0};

static double score_candidate(double lex, double dense, double age_days,
                              const kb_sketch_features_t *sketch)
{
   double recency = (age_days > 0.0) ? 1.0 / (1.0 + age_days / 30.0) : 1.0;
   double sketch_frequency = sketch ? sketch->frequency_kind_scope : 0.0;
   double sketch_distinct = sketch ? sketch->distinct_sources_hll : 0.0;
   return g_weights.w_dense * dense + g_weights.w_lex * lex + g_weights.w_recency * recency +
          g_weights.w_sketch_frequency * sketch_frequency +
          g_weights.w_sketch_distinct * sketch_distinct;
}

int kb_ranker_rerank(const config_t *cfg, const int64_t *doc_ids, const double *lex_scores,
                     const double *dense_scores, const double *age_days, int n, int64_t *ranked_out,
                     double *scores_out)
{
   return kb_ranker_rerank_with_sketch(cfg, doc_ids, lex_scores, dense_scores, age_days, NULL, n,
                                       ranked_out, scores_out);
}

int kb_ranker_rerank_with_sketch(const config_t *cfg, const int64_t *doc_ids,
                                 const double *lex_scores, const double *dense_scores,
                                 const double *age_days,
                                 const kb_sketch_features_t *sketch_features, int n,
                                 int64_t *ranked_out, double *scores_out)
{
   (void)cfg;
   if (n <= 0 || !doc_ids || !lex_scores || !dense_scores || !age_days || !ranked_out)
      return 0;
   if (!g_weights.loaded)
      return 0;

   typedef struct
   {
      int64_t id;
      double score;
      int orig_idx;
   } entry_t;

   entry_t *entries = malloc((size_t)n * sizeof(entry_t));
   if (!entries)
      return 0;

   for (int i = 0; i < n; i++)
   {
      entries[i].id = doc_ids[i];
      entries[i].score = score_candidate(lex_scores[i], dense_scores[i], age_days[i],
                                         sketch_features ? &sketch_features[i] : NULL);
      entries[i].orig_idx = i;
   }

   /* Insertion sort (n is small — default <= 50 candidates). */
   for (int i = 1; i < n; i++)
   {
      entry_t tmp = entries[i];
      int j = i - 1;
      while (j >= 0 && entries[j].score < tmp.score)
      {
         entries[j + 1] = entries[j];
         j--;
      }
      entries[j + 1] = tmp;
   }

   for (int i = 0; i < n; i++)
   {
      ranked_out[i] = entries[i].id;
      if (scores_out)
         scores_out[i] = entries[i].score;
   }
   free(entries);
   return n;
}

int kb_ranker_model_write(const char *weights_json, char *id_out, int id_out_len)
{
   if (!weights_json)
      return -1;

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   /* Build payload: model_kind=linear, surface=kb_hybrid, weights blob. */
   char payload[1024];
   snprintf(payload, sizeof(payload),
            "{\"surface\":\"kb_hybrid\",\"objective\":\"pointwise\","
            "\"feature_set_version\":\"%s\",\"model_kind\":\"linear\","
            "\"weights\":%s}",
            KB_RANKER_FEATURE_SET_VERSION, weights_json);

   int rc = db2_artifact_write(id, "ranker_model", "proposed", "system", "", "", 1.0, payload);
   if (rc != 0)
      return -1;

   /* Stamp target_surface and committed_at. */
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE artifacts"
                                          " SET target_surface = 'kb_hybrid',"
                                          "     state = 'committed',"
                                          "     committed_at = ?1"
                                          " WHERE id = ?2",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", ts);
   aimee_pg_bind_text(st, "?2", id);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);

   if (id_out && id_out_len > 0)
      snprintf(id_out, (size_t)id_out_len, "%s", id);

   aimee_log(LOG_DEBUG, "kb_ranker", "wrote ranker_model artifact id=%s", id);
   return 0;
}

int kb_ranker_model_write_proposed(const char *weights_json, const char *fit_metrics_json,
                                   char *id_out, int id_out_len)
{
   if (!weights_json)
      return -1;

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   /* Same payload shape kb_ranker_model_load parses, plus a fit_metrics blob for
    * provenance. Lands 'proposed' — the benchmark gate (kb_ranker_fit_run) must
    * pass before kb_ranker_model_commit promotes it. */
   char payload[2048];
   snprintf(payload, sizeof(payload),
            "{\"surface\":\"kb_hybrid\",\"objective\":\"pointwise\","
            "\"feature_set_version\":\"%s\",\"model_kind\":\"linear\","
            "\"weights\":%s,\"fit_metrics\":%s}",
            KB_RANKER_FEATURE_SET_VERSION, weights_json,
            (fit_metrics_json && fit_metrics_json[0]) ? fit_metrics_json : "{}");

   int rc = db2_artifact_write(id, "ranker_model", "proposed", "system", "", "", 1.0, payload);
   if (rc != 0)
      return -1;

   /* Stamp target_surface so kb_ranker_model_load's WHERE clause can find it once
    * committed — but leave state='proposed' / committed_at empty (gate pending). */
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE artifacts SET target_surface = 'kb_hybrid' WHERE id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", id);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);

   if (id_out && id_out_len > 0)
      snprintf(id_out, (size_t)id_out_len, "%s", id);

   aimee_log(LOG_DEBUG, "kb_ranker", "wrote proposed ranker_model artifact id=%s", id);
   return 0;
}

int kb_ranker_model_commit(const char *id)
{
   if (!id || !id[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE artifacts"
                                          " SET state = 'committed',"
                                          "     target_surface = 'kb_hybrid',"
                                          "     committed_at = ?1"
                                          " WHERE id = ?2 AND kind = 'ranker_model'",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", ts);
   aimee_pg_bind_text(st, "?2", id);
   int step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_DONE)
      return -1;

   aimee_log(LOG_DEBUG, "kb_ranker", "committed ranker_model artifact id=%s", id);
   return 0;
}

int kb_ranker_model_load(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT payload FROM artifacts"
                                          " WHERE kind = 'ranker_model'"
                                          "   AND target_surface = 'kb_hybrid'"
                                          "   AND state = 'committed'"
                                          " ORDER BY committed_at DESC"
                                          " LIMIT 1",
                                          err, sizeof(err));
   if (!st)
      return -1;

   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *payload_str = aimee_pg_column_text(st, 0);
      if (payload_str)
      {
         cJSON *p = cJSON_Parse(payload_str);
         if (p)
         {
            cJSON *w = cJSON_GetObjectItemCaseSensitive(p, "weights");
            if (cJSON_IsObject(w))
            {
               g_weights.w_dense = 0.6;
               g_weights.w_lex = 0.4;
               g_weights.w_recency = 0.0;
               g_weights.w_sketch_frequency = 0.0;
               g_weights.w_sketch_distinct = 0.0;
               cJSON *wd = cJSON_GetObjectItemCaseSensitive(w, "dense.cos");
               cJSON *wl = cJSON_GetObjectItemCaseSensitive(w, "lex.cos");
               cJSON *wr = cJSON_GetObjectItemCaseSensitive(w, "temp.recency");
               cJSON *wsf = cJSON_GetObjectItemCaseSensitive(w, "sketch.frequency_kind_scope");
               cJSON *wsd = cJSON_GetObjectItemCaseSensitive(w, "sketch.distinct_sources_hll");
               if (cJSON_IsNumber(wd))
                  g_weights.w_dense = wd->valuedouble;
               if (cJSON_IsNumber(wl))
                  g_weights.w_lex = wl->valuedouble;
               if (cJSON_IsNumber(wr))
                  g_weights.w_recency = wr->valuedouble;
               if (cJSON_IsNumber(wsf))
                  g_weights.w_sketch_frequency = wsf->valuedouble;
               if (cJSON_IsNumber(wsd))
                  g_weights.w_sketch_distinct = wsd->valuedouble;
               g_weights.loaded = 1;
               found = 1;
               aimee_log(LOG_DEBUG, "kb_ranker",
                         "loaded ranker_model w_dense=%.3f w_lex=%.3f w_recency=%.3f "
                         "w_sketch_frequency=%.3f w_sketch_distinct=%.3f",
                         g_weights.w_dense, g_weights.w_lex, g_weights.w_recency,
                         g_weights.w_sketch_frequency, g_weights.w_sketch_distinct);
            }
            cJSON_Delete(p);
         }
      }
   }
   aimee_pg_finalize(st);
   return found ? 0 : -1;
}
