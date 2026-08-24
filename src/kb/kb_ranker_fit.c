/* kb_ranker_fit.c: the Calibrate half of the KB-hybrid ranking substrate.
 * Reads the joined feature/outcome training view, runs the fitter sidecar
 * (scripts/rank-fit.py), benchmark-gates the result, and promotes a
 * ranker_model artifact only on measured lift.
 * See docs/proposals/done/learning-to-rank-weight-fitting.md */

#include "kb_ranker_fit.h"
#include "kb_ranker.h"
#include "kb_features.h" /* KB_FEATURE_SET_VERSION */
#include "modules/db2/c/artifacts.h"
#include "modules/db2/c/feature_rows.h" /* db2_feature_row_read */
#include "modules/db2/c/demotion.h"     /* DEMOTION_VERDICT_ACCEPTED */
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"
#include "aimee.h"
#include "log.h"
#include "platform_process.h"

#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RANK_FIT_DEFAULT_MIN_GROUPS 8
#define RANK_FIT_DEFAULT_BENCHMARK  "benchmarks/rank/kb_hybrid/queries.json"
#define RANK_FIT_DEFAULT_BENCH_K    5
/* A fitted model must beat the incumbent by more than noise to promote.
 *
 * This was 1e-6, which is not "more than noise" — it is "not exactly equal". On a
 * small fixture NDCG@k is quantised to a handful of discrete values, so a single
 * query's reordering cleared it. 1e-3 is below any lift worth shipping (the
 * retrieval campaign's real effects were 1e-2 and up) while being far above the
 * floating-point noise the old value was actually measuring. */
#define RANK_FIT_LIFT_EPSILON 1e-3

/* Minimum gate queries for a promotion decision to carry information.
 *
 * The default fixture (benchmarks/rank/kb_hybrid/queries.json) holds 5 queries of
 * 2-4 candidates each; the gate tests use 1. At that size the gate cannot detect a
 * regression, and it silently promoted anything with a non-zero mean lift. Below
 * this floor the gate now REFUSES rather than promotes: an underpowered gate must
 * fail closed, because a promotion is a change to what production serves.
 *
 * 30 is not a magic number — it is the point where the paired win/loss majority
 * below stops being decidable by one or two queries. A real gate fixture should be
 * far larger; set kb_ranker_fit_benchmark to point at one. */
#define RANK_FIT_MIN_BENCH_QUERIES 30

/* Paired candidate-vs-incumbent evaluation over the gate fixture. */
typedef struct
{
   double mean_candidate;
   double mean_incumbent;
   int n_queries; /* queries that scored under BOTH weight vectors */
   int wins;      /* candidate strictly better on this query */
   int losses;    /* incumbent strictly better */
   int ties;
} rank_gate_eval_t;

/* The v1 feature set, in the order the ranker and the sidecar agree on. These
 * keys are the contract with kb_ranker_model_load and scripts/rank-fit.py. */
static const char *const FEATURE_KEYS[] = {
    "dense.cos",
    "lex.cos",
    "temp.recency",
    "sketch.frequency_kind_scope",
    "sketch.distinct_sources_hll",
};
#define N_FEATURES ((int)(sizeof(FEATURE_KEYS) / sizeof(FEATURE_KEYS[0])))

/* ---- Option B: kb_hybrid outcome capture --------------------------------- */

/* Mint a kb_hybrid retrieval_event for one search and record the surfaced
 * kb_document candidates in its payload (for audit + as the grouping key that
 * ties a query's outcomes together). Returns the event id in id_out; 0 ok / -1. */
int kb_ranker_emit_event(const int64_t *doc_ids, int n, const char *query_fingerprint, char *id_out,
                         int id_out_len)
{
   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   cJSON *p = cJSON_CreateObject();
   if (!p)
      return -1;
   cJSON_AddStringToObject(p, "surface", "kb_hybrid");
   cJSON_AddStringToObject(p, "query_fingerprint", query_fingerprint ? query_fingerprint : "");
   cJSON *arr = cJSON_AddArrayToObject(p, "surfaced_doc_ids");
   for (int i = 0; arr && doc_ids && i < n; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)doc_ids[i]));
   char *payload = cJSON_PrintUnformatted(p);
   cJSON_Delete(p);
   if (!payload)
      return -1;

   int rc = db2_artifact_write(id, "retrieval_event", "proposed", "system", "", "", 1.0, payload);
   free(payload);
   if (rc != 0)
      return -1;

   /* Tag target_surface so kb_hybrid events are separable from memory-recall
    * retrieval_events in audit/trace queries. */
   void *conn = db2_conn();
   if (conn)
   {
      char err[256] = "";
      aimee_pg_stmt_t *st =
          aimee_pg_prepare(conn, "UPDATE artifacts SET target_surface = 'kb_hybrid' WHERE id = ?1",
                           err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_text(st, "?1", id);
         aimee_pg_step(st, err, sizeof(err));
         aimee_pg_finalize(st);
      }
   }

   if (id_out && id_out_len > 0)
      snprintf(id_out, (size_t)id_out_len, "%s", id);
   return 0;
}

/* Record one outcome verdict for a surfaced kb_document candidate as a dedicated
 * `ranker_outcome` artifact (NOT retrieval_attribution — that kind is the memory
 * demotion surface). This is the label source the training view consumes. */
int kb_ranker_outcome_write(const char *event_id, int64_t doc_id, const char *verdict,
                            double weight)
{
   if (!event_id || !event_id[0] || !verdict || !verdict[0])
      return -1;

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   char scope_id[32];
   snprintf(scope_id, sizeof(scope_id), "%lld", (long long)doc_id);

   char payload[512];
   snprintf(payload, sizeof(payload),
            "{\"retrieval_event_id\":\"%s\",\"surfaced_row_id\":%lld,\"verdict\":\"%s\","
            "\"weight\":%.6f,\"subject_kind\":\"kb_document\"}",
            event_id, (long long)doc_id, verdict, weight);

   return db2_artifact_write(id, "ranker_outcome", "proposed", "kb_hybrid", scope_id, "", 1.0,
                             payload);
}

/* ---- Phase 1: the training view ------------------------------------------ */

static int ranker_subject_kind_matches(const char *outcome_kind, const char *ranker_kind)
{
   if (!outcome_kind || !outcome_kind[0])
      return 1; /* Legacy ranker_outcome artifacts predate the discriminator. */
   if (strcmp(outcome_kind, ranker_kind) == 0)
      return 1;
   return strcmp(outcome_kind, "document") == 0 && strcmp(ranker_kind, "kb_document") == 0;
}

/* Join one normalized outcome to its feature vector and append a training row.
 * Returns 1 when appended, 0 when the outcome is outside this feature space or
 * has no feature row, and -1 on allocation failure. */
static int ranker_training_append(cJSON *rows, cJSON *seen_groups, const char *ranker_kind,
                                  const char *feature_set_version, const char *group,
                                  const char *subject_id, const char *outcome_kind,
                                  const char *verdict, double weight, int *n_groups,
                                  int *n_positive)
{
   if (!group || !subject_id || !verdict || !ranker_subject_kind_matches(outcome_kind, ranker_kind))
      return 0;

   char feat_buf[1024];
   if (db2_feature_row_read(subject_id, ranker_kind, feature_set_version, feat_buf,
                            sizeof(feat_buf)) != 0)
      return 0;

   cJSON *feat = cJSON_Parse(feat_buf);
   if (!cJSON_IsObject(feat))
   {
      cJSON_Delete(feat);
      feat = cJSON_CreateObject();
   }
   cJSON *row = cJSON_CreateObject();
   if (!feat || !row)
   {
      cJSON_Delete(feat);
      cJSON_Delete(row);
      return -1;
   }

   int label =
       (strcmp(verdict, DEMOTION_VERDICT_ACCEPTED) == 0 || strcmp(verdict, "useful") == 0) ? 1 : 0;
   cJSON_AddStringToObject(row, "group", group);
   cJSON_AddStringToObject(row, "subject_id", subject_id);
   cJSON_AddItemToObject(row, "features", feat);
   cJSON_AddNumberToObject(row, "label", label);
   cJSON_AddStringToObject(row, "verdict", verdict);
   cJSON_AddNumberToObject(row, "weight", weight);
   cJSON_AddItemToArray(rows, row);

   *n_positive += label;
   if (group[0] && !cJSON_GetObjectItemCaseSensitive(seen_groups, group))
   {
      cJSON_AddBoolToObject(seen_groups, group, 1);
      (*n_groups)++;
   }
   return 1;
}

int kb_ranker_training_view(const char *subject_kind, const char *feature_set_version,
                            cJSON **rows_out, int *n_groups_out, int *n_rows_out,
                            int *n_positive_out)
{
   if (!rows_out)
      return -1;
   *rows_out = NULL;
   if (n_groups_out)
      *n_groups_out = 0;
   if (n_rows_out)
      *n_rows_out = 0;
   if (n_positive_out)
      *n_positive_out = 0;

   const char *sk = (subject_kind && subject_kind[0]) ? subject_kind : "kb_document";
   const char *fsv = (feature_set_version && feature_set_version[0]) ? feature_set_version
                                                                     : KB_FEATURE_SET_VERSION;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Enumerate the legacy ranker_outcome artifacts and the canonical P5
    * work_outcomes table separately, then join each candidate to its feature
    * vector in C via db2_feature_row_read. Separate simple queries preserve
    * postgres/sqlite parity without backend-specific JSON constructors. One
    * row is emitted per (retrieval_event, candidate) that has BOTH a feature
    * vector and an outcome verdict.
    *
    * ranker_outcome is a surface-dedicated kind (kb_hybrid / kb_document ids),
    * distinct from the memory surface's retrieval_attribution — so this join
    * never collides with a memory row id that happens to equal a doc_id, and the
    * memory demotion scorer (which reads retrieval_attribution) is untouched. */
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT payload FROM artifacts WHERE kind='ranker_outcome' ORDER BY created_at", err,
       sizeof(err));
   if (!st)
      return -1;

   cJSON *rows = cJSON_CreateArray();
   cJSON *seen_groups = cJSON_CreateObject(); /* group-id set for distinct count */
   if (!rows || !seen_groups)
   {
      cJSON_Delete(rows);
      cJSON_Delete(seen_groups);
      aimee_pg_finalize(st);
      return -1;
   }

   int n_rows = 0, n_positive = 0, n_groups = 0;

   int step;
   while ((step = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      const char *payload_s = aimee_pg_column_text(st, 0);
      cJSON *p = payload_s ? cJSON_Parse(payload_s) : NULL;
      if (!p)
         continue;

      cJSON *ev = cJSON_GetObjectItemCaseSensitive(p, "retrieval_event_id");
      cJSON *rid = cJSON_GetObjectItemCaseSensitive(p, "surfaced_row_id");
      if (!rid)
         rid = cJSON_GetObjectItemCaseSensitive(p, "subject_id");
      cJSON *subject_kind_j = cJSON_GetObjectItemCaseSensitive(p, "subject_kind");
      cJSON *vj = cJSON_GetObjectItemCaseSensitive(p, "verdict");
      cJSON *wj = cJSON_GetObjectItemCaseSensitive(p, "weight");
      const char *group = cJSON_IsString(ev) ? ev->valuestring : "";
      const char *verdict = cJSON_IsString(vj) ? vj->valuestring : "";
      const char *outcome_kind = cJSON_IsString(subject_kind_j) ? subject_kind_j->valuestring : "";

      char subject_id[256];
      if (cJSON_IsNumber(rid))
         snprintf(subject_id, sizeof(subject_id), "%lld", (long long)rid->valuedouble);
      else if (cJSON_IsString(rid))
         snprintf(subject_id, sizeof(subject_id), "%s", rid->valuestring);
      else
      {
         cJSON_Delete(p);
         continue;
      }

      int appended = ranker_training_append(
          rows, seen_groups, sk, fsv, group, subject_id, outcome_kind, verdict,
          cJSON_IsNumber(wj) ? wj->valuedouble : 1.0, &n_groups, &n_positive);
      cJSON_Delete(p);
      if (appended < 0)
      {
         step = AIMEE_PG_ERR;
         break;
      }
      n_rows += appended;
   }
   aimee_pg_finalize(st);
   if (step == AIMEE_PG_ERR)
   {
      cJSON_Delete(rows);
      cJSON_Delete(seen_groups);
      return -1;
   }

   st = aimee_pg_prepare(conn,
                         "SELECT retrieval_event_id,subject_id,subject_kind,outcome"
                         " FROM work_outcomes ORDER BY occurred_at",
                         err, sizeof(err));
   if (!st)
   {
      cJSON_Delete(rows);
      cJSON_Delete(seen_groups);
      return -1;
   }
   while ((step = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      int appended = ranker_training_append(
          rows, seen_groups, sk, fsv, aimee_pg_column_text(st, 0), aimee_pg_column_text(st, 1),
          aimee_pg_column_text(st, 2), aimee_pg_column_text(st, 3), 1.0, &n_groups, &n_positive);
      if (appended < 0)
      {
         step = AIMEE_PG_ERR;
         break;
      }
      n_rows += appended;
   }
   aimee_pg_finalize(st);
   cJSON_Delete(seen_groups);
   if (step == AIMEE_PG_ERR)
   {
      cJSON_Delete(rows);
      return -1;
   }

   *rows_out = rows;
   if (n_groups_out)
      *n_groups_out = n_groups;
   if (n_rows_out)
      *n_rows_out = n_rows;
   if (n_positive_out)
      *n_positive_out = n_positive;
   return 0;
}

/* Attach the structured wiring-gap diagnostic the operator needs when the view
 * is empty — never silently return zero rows (must-not-break constraint #5). */
static void add_empty_view_diagnostic(cJSON *out, const char *subject_kind)
{
   cJSON *d = cJSON_CreateObject();
   cJSON_AddStringToObject(d, "reason", "empty_training_view");
   cJSON_AddStringToObject(d, "detail",
                           "No (retrieval_event, candidate) rows have BOTH a v1 feature vector and "
                           "an outcome verdict.");
   cJSON_AddStringToObject(
       d, "subject_space_mismatch",
       "ranker features use feature_rows.subject_kind='kb_document'; canonical document outcomes "
       "map to that space, while memory/assertion outcomes remain intentionally disjoint.");
   cJSON_AddStringToObject(
       d, "missing_grouping_key",
       "each outcome needs a retrieval_event_id and a subject_id with a matching "
       "feature row; feature rows alone cannot establish query groups.");
   cJSON_AddStringToObject(
       d, "prerequisite",
       "record ranker_outcome artifacts or canonical work_outcomes for surfaced document ids, "
       "grouped by retrieval_event_id, before live-data promotion is possible.");
   cJSON_AddStringToObject(d, "joined_subject_kind", subject_kind);
   cJSON_AddItemToObject(out, "diagnostic", d);
}

char *kb_ranker_export_view_json(const char *subject_kind, const char *feature_set_version)
{
   const char *sk = (subject_kind && subject_kind[0]) ? subject_kind : "kb_document";
   const char *fsv = (feature_set_version && feature_set_version[0]) ? feature_set_version
                                                                     : KB_FEATURE_SET_VERSION;

   cJSON *rows = NULL;
   int n_groups = 0, n_rows = 0, n_positive = 0;
   int rc = kb_ranker_training_view(sk, fsv, &rows, &n_groups, &n_rows, &n_positive);

   cJSON *out = cJSON_CreateObject();
   if (rc != 0)
   {
      cJSON_AddStringToObject(out, "status", "error");
      cJSON_AddStringToObject(out, "error", "training view query failed");
      cJSON_Delete(rows);
      char *s = cJSON_PrintUnformatted(out);
      cJSON_Delete(out);
      return s;
   }

   cJSON_AddStringToObject(out, "status", "ok");
   cJSON_AddStringToObject(out, "subject_kind", sk);
   cJSON_AddStringToObject(out, "feature_set_version", fsv);
   cJSON_AddNumberToObject(out, "n_groups", n_groups);
   cJSON_AddNumberToObject(out, "n_rows", n_rows);
   cJSON_AddNumberToObject(out, "n_positive", n_positive);
   int fittable =
       (n_groups >= RANK_FIT_DEFAULT_MIN_GROUPS && n_positive > 0 && n_positive < n_rows);
   cJSON_AddBoolToObject(out, "fittable", fittable);
   if (n_rows == 0)
      add_empty_view_diagnostic(out, sk);
   cJSON_AddItemToObject(out, "rows", rows ? rows : cJSON_CreateArray());

   char *s = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   return s;
}

/* ---- Phase 2/3: fit → gate → promote ------------------------------------- */

/* Slurp a file into a malloc'd NUL-terminated buffer (caller frees), or NULL. */
static char *read_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   if (sz < 0 || sz > 32 * 1024 * 1024)
   {
      fclose(f);
      return NULL;
   }
   fseek(f, 0, SEEK_SET);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t got = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[got] = '\0';
   return buf;
}

/* Linear score of one candidate's features under a weight vector (missing
 * features count as 0.0) — the same dot product score_candidate computes. */
static double score_features(const cJSON *features, const double *weights)
{
   double s = 0.0;
   for (int j = 0; j < N_FEATURES; j++)
   {
      const cJSON *v = cJSON_GetObjectItemCaseSensitive(features, FEATURE_KEYS[j]);
      if (cJSON_IsNumber(v))
         s += weights[j] * v->valuedouble;
   }
   return s;
}

/* NDCG@k for ONE query's candidate array under a weight vector. Graded gains
 * (linear relevance), standard log2 discount. Returns -1.0 if the query carries
 * no usable candidates, so the caller can skip it without counting it. */
static double query_ndcg(const cJSON *cands, int k, const double *weights)
{
   if (!cJSON_IsArray(cands))
      return -1.0;
   int n = cJSON_GetArraySize(cands);
   if (n <= 0)
      return -1.0;

   double *score = malloc((size_t)n * sizeof(double));
   double *rel = malloc((size_t)n * sizeof(double));
   int *idx = malloc((size_t)n * sizeof(int));
   if (!score || !rel || !idx)
   {
      free(score);
      free(rel);
      free(idx);
      return -1.0;
   }
   for (int i = 0; i < n; i++)
   {
      cJSON *c = cJSON_GetArrayItem((cJSON *)cands, i);
      cJSON *feats = cJSON_GetObjectItemCaseSensitive(c, "features");
      cJSON *r = cJSON_GetObjectItemCaseSensitive(c, "relevance");
      score[i] = score_features(feats, weights);
      rel[i] = cJSON_IsNumber(r) ? r->valuedouble : 0.0;
      idx[i] = i;
   }

   /* Rank by model score (desc) — insertion sort, n is tiny. */
   for (int a = 1; a < n; a++)
   {
      int t = idx[a];
      int b = a - 1;
      while (b >= 0 && score[idx[b]] < score[t])
      {
         idx[b + 1] = idx[b];
         b--;
      }
      idx[b + 1] = t;
   }

   int kk = (k < n) ? k : n;
   double dcg = 0.0;
   for (int p = 0; p < kk; p++)
      dcg += rel[idx[p]] / (log(p + 2.0) / log(2.0));

   /* Ideal DCG: relevances sorted desc. */
   double *rel_sorted = malloc((size_t)n * sizeof(double));
   double idcg = 0.0;
   if (rel_sorted)
   {
      for (int i = 0; i < n; i++)
         rel_sorted[i] = rel[i];
      for (int a = 1; a < n; a++)
      {
         double t = rel_sorted[a];
         int b = a - 1;
         while (b >= 0 && rel_sorted[b] < t)
         {
            rel_sorted[b + 1] = rel_sorted[b];
            b--;
         }
         rel_sorted[b + 1] = t;
      }
      for (int p = 0; p < kk; p++)
         idcg += rel_sorted[p] / (log(p + 2.0) / log(2.0));
      free(rel_sorted);
   }

   double ndcg = (idcg > 0.0) ? dcg / idcg : 0.0;
   free(score);
   free(rel);
   free(idx);
   return ndcg;
}

/* PAIRED comparison of candidate against incumbent over the gate fixture.
 *
 * Both weight vectors score the SAME query before moving on, which is what makes
 * the per-query win/loss record meaningful: a mean lift alone cannot distinguish
 * "better on most queries" from "much better on one and worse on the rest", and
 * the latter is what overfitting a small fixture looks like.
 *
 * Returns 0 on success, -1 if the fixture cannot be read or parsed. */
static int benchmark_compare(const char *fixture_path, int k, const double *cand_w,
                             const double *incumbent_w, rank_gate_eval_t *out)
{
   memset(out, 0, sizeof(*out));
   char *raw = read_file(fixture_path);
   if (!raw)
      return -1;
   cJSON *queries = cJSON_Parse(raw);
   free(raw);
   if (!cJSON_IsArray(queries))
   {
      cJSON_Delete(queries);
      return -1;
   }

   double cand_sum = 0.0, incumbent_sum = 0.0;
   cJSON *q;
   cJSON_ArrayForEach(q, queries)
   {
      cJSON *cands = cJSON_GetObjectItemCaseSensitive(q, "candidates");
      double nc = query_ndcg(cands, k, cand_w);
      double ni = query_ndcg(cands, k, incumbent_w);
      if (nc < 0.0 || ni < 0.0)
         continue; /* unusable query — not counted toward gate power */

      cand_sum += nc;
      incumbent_sum += ni;
      out->n_queries++;
      if (nc > ni)
         out->wins++;
      else if (nc < ni)
         out->losses++;
      else
         out->ties++;
   }
   cJSON_Delete(queries);

   if (out->n_queries <= 0)
      return -1;
   out->mean_candidate = cand_sum / out->n_queries;
   out->mean_incumbent = incumbent_sum / out->n_queries;
   return 0;
}

/* Read the weights of the most recently committed kb_hybrid ranker_model into
 * `out` (5-vector, FEATURE_KEYS order). Falls back to the {0.6,0.4,0,0,0}
 * default when no model is committed. */
static void load_incumbent_weights(double *out)
{
   out[0] = 0.6; /* dense.cos */
   out[1] = 0.4; /* lex.cos */
   out[2] = 0.0; /* temp.recency */
   out[3] = 0.0; /* sketch.frequency_kind_scope */
   out[4] = 0.0; /* sketch.distinct_sources_hll */

   void *conn = db2_conn();
   if (!conn)
      return;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT payload FROM artifacts"
                                          " WHERE kind = 'ranker_model'"
                                          "   AND target_surface = 'kb_hybrid'"
                                          "   AND state = 'committed'"
                                          " ORDER BY committed_at DESC LIMIT 1",
                                          err, sizeof(err));
   if (!st)
      return;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *payload = aimee_pg_column_text(st, 0);
      cJSON *p = payload ? cJSON_Parse(payload) : NULL;
      cJSON *w = p ? cJSON_GetObjectItemCaseSensitive(p, "weights") : NULL;
      if (cJSON_IsObject(w))
      {
         for (int j = 0; j < N_FEATURES; j++)
         {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(w, FEATURE_KEYS[j]);
            if (cJSON_IsNumber(v))
               out[j] = v->valuedouble;
         }
      }
      cJSON_Delete(p);
   }
   aimee_pg_finalize(st);
}

static void weights_obj_to_vec(const cJSON *weights, double *out)
{
   for (int j = 0; j < N_FEATURES; j++)
   {
      cJSON *v = cJSON_GetObjectItemCaseSensitive(weights, FEATURE_KEYS[j]);
      out[j] = cJSON_IsNumber(v) ? v->valuedouble : 0.0;
   }
}

/* Record the gate decision as a benchmark_trace artifact — the evidence trail.
 *
 * Records the gate's POWER (n_queries, win/loss/tie split) alongside the metric,
 * not just the two means. A reader of this artifact must be able to tell a decision
 * backed by a real fixture from one backed by five hand-authored queries; the
 * previous payload could not express the difference. `eval` may be NULL when the
 * fixture never scored (benchmark_unavailable). */
static void write_benchmark_trace(const char *model_id, const rank_gate_eval_t *eval, int k,
                                  const char *decision)
{
   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   double cand = eval ? eval->mean_candidate : -1.0;
   double inc = eval ? eval->mean_incumbent : -1.0;
   char payload[768];
   snprintf(payload, sizeof(payload),
            "{\"surface\":\"kb_hybrid\",\"track\":\"recall\",\"metric\":\"ndcg@%d\","
            "\"model_id\":\"%s\",\"ndcg_candidate\":%.6f,\"ndcg_incumbent\":%.6f,"
            "\"delta\":%.6f,\"n_queries\":%d,\"wins\":%d,\"losses\":%d,\"ties\":%d,"
            "\"min_queries\":%d,\"lift_epsilon\":%g,\"decision\":\"%s\"}",
            k, model_id ? model_id : "", cand, inc, eval ? cand - inc : 0.0,
            eval ? eval->n_queries : 0, eval ? eval->wins : 0, eval ? eval->losses : 0,
            eval ? eval->ties : 0, RANK_FIT_MIN_BENCH_QUERIES, RANK_FIT_LIFT_EPSILON,
            decision ? decision : "");
   db2_artifact_write(id, "benchmark_trace", "committed", "global", "", "", 1.0, payload);
}

static int finish_refused(cJSON *report, const char *reason, char **report_out, cJSON *rows)
{
   cJSON_AddStringToObject(report, "status", "refused");
   cJSON_AddStringToObject(report, "reason", reason);
   if (report_out)
      *report_out = cJSON_PrintUnformatted(report);
   cJSON_Delete(report);
   cJSON_Delete(rows);
   return 1;
}

int kb_ranker_fit_run(char *id_out, int id_out_len, char **report_out)
{
   if (report_out)
      *report_out = NULL;
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';

   cJSON *report = cJSON_CreateObject();
   cJSON_AddStringToObject(report, "surface", "kb_hybrid");
   cJSON_AddStringToObject(report, "feature_set_version", KB_FEATURE_SET_VERSION);

   if (!config_kb_ranker_fit_enabled())
   {
      cJSON_AddStringToObject(report, "status", "disabled");
      if (report_out)
         *report_out = cJSON_PrintUnformatted(report);
      cJSON_Delete(report);
      return 1;
   }

   int min_groups = config_kb_ranker_fit_min_groups() > 0 ? config_kb_ranker_fit_min_groups()
                                                          : RANK_FIT_DEFAULT_MIN_GROUPS;

   cJSON *rows = NULL;
   int n_groups = 0, n_rows = 0, n_positive = 0;
   if (kb_ranker_training_view("kb_document", KB_FEATURE_SET_VERSION, &rows, &n_groups, &n_rows,
                               &n_positive) != 0)
   {
      cJSON_Delete(rows);
      cJSON_AddStringToObject(report, "status", "error");
      cJSON_AddStringToObject(report, "error", "training view failed");
      if (report_out)
         *report_out = cJSON_PrintUnformatted(report);
      cJSON_Delete(report);
      return -1;
   }
   cJSON_AddNumberToObject(report, "n_groups", n_groups);
   cJSON_AddNumberToObject(report, "n_rows", n_rows);
   cJSON_AddNumberToObject(report, "n_positive", n_positive);

   /* Short-circuit the thin/absent log before spawning the sidecar; attach the
    * wiring-gap diagnostic so the idle loop is legible, not silent. */
   if (n_groups < min_groups || n_positive == 0 || n_positive == n_rows)
   {
      add_empty_view_diagnostic(report, "kb_document");
      cJSON_AddNumberToObject(report, "min_groups", min_groups);
      return finish_refused(report, "below_floor", report_out, rows);
   }

   if (!config_kb_ranker_fit_command()[0])
      return finish_refused(report, "no_fitter_command", report_out, rows);

   /* Build the sidecar request: {feature_set_version, objective, min_groups, rows}.
    * Objective is operator-selectable (pointwise default | pairwise). The rows
    * already carry per-candidate `weight`, so an IPW/confidence weight flows to
    * the sidecar unchanged. */
   const char *objective =
       config_kb_ranker_fit_objective()[0] ? config_kb_ranker_fit_objective() : "pointwise";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "feature_set_version", KB_FEATURE_SET_VERSION);
   cJSON_AddStringToObject(req, "objective", objective);
   cJSON_AddNumberToObject(req, "min_groups", min_groups);
   /* Hand the rows to the sidecar (detach from `rows` so we can free it). */
   cJSON *req_rows = cJSON_Duplicate(rows, 1);
   cJSON_AddItemToObject(req, "rows", req_rows);
   char *req_str = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   cJSON_Delete(rows);
   rows = NULL;
   if (!req_str)
      return finish_refused(report, "internal_error", report_out, NULL);

   char *out = NULL;
   size_t out_len = 0;
   int rc =
       platform_exec_pipe(config_kb_ranker_fit_command(), req_str, strlen(req_str), &out, &out_len);
   free(req_str);
   if (rc != 0 || !out)
   {
      free(out);
      return finish_refused(report, "sidecar_failed", report_out, NULL);
   }

   cJSON *resp = cJSON_ParseWithLength(out, out_len);
   free(out);
   if (!resp)
      return finish_refused(report, "sidecar_bad_json", report_out, NULL);

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *weights = cJSON_GetObjectItemCaseSensitive(resp, "weights");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 ||
       !cJSON_IsObject(weights))
   {
      cJSON *r = cJSON_GetObjectItemCaseSensitive(resp, "reason");
      const char *reason = cJSON_IsString(r) ? r->valuestring : "sidecar_refused";
      cJSON_AddItemToObject(report, "sidecar", cJSON_Duplicate(resp, 1));
      int rv = finish_refused(report, reason, report_out, NULL);
      cJSON_Delete(resp);
      return rv;
   }

   char *weights_json = cJSON_PrintUnformatted(weights);
   cJSON *metrics = cJSON_GetObjectItemCaseSensitive(resp, "fit_metrics");
   char *metrics_json = metrics ? cJSON_PrintUnformatted(metrics) : NULL;
   cJSON_AddItemToObject(report, "weights", cJSON_Duplicate(weights, 1));
   if (metrics)
      cJSON_AddItemToObject(report, "fit_metrics", cJSON_Duplicate(metrics, 1));

   /* ---- Benchmark gate: score candidate vs incumbent on the fixture. ---- */
   double cand_w[N_FEATURES];
   double incumbent_w[N_FEATURES];
   weights_obj_to_vec(weights, cand_w);
   load_incumbent_weights(incumbent_w);

   const char *bench_path = config_kb_ranker_fit_benchmark()[0] ? config_kb_ranker_fit_benchmark()
                                                                : RANK_FIT_DEFAULT_BENCHMARK;
   int k = config_kb_ranker_fit_bench_k() > 0 ? config_kb_ranker_fit_bench_k()
                                              : RANK_FIT_DEFAULT_BENCH_K;
   rank_gate_eval_t eval;
   int eval_rc = benchmark_compare(bench_path, k, cand_w, incumbent_w, &eval);

   /* Write the fitted model as proposed regardless — it is the record of the fit.
    * Only a benchmark win promotes it. */
   char model_id[64] = "";
   int wrc = kb_ranker_model_write_proposed(weights_json, metrics_json, model_id, sizeof(model_id));
   free(weights_json);
   free(metrics_json);
   cJSON_Delete(resp);
   if (wrc != 0)
      return finish_refused(report, "model_write_failed", report_out, NULL);
   if (id_out && id_out_len > 0)
      snprintf(id_out, (size_t)id_out_len, "%s", model_id);
   cJSON_AddStringToObject(report, "model_id", model_id);

   cJSON *gate = cJSON_CreateObject();
   cJSON_AddNumberToObject(gate, "k", k);
   cJSON_AddStringToObject(gate, "benchmark", bench_path);
   cJSON_AddItemToObject(report, "gate", gate);

   if (eval_rc != 0)
   {
      /* Fixture unreadable → cannot prove lift → keep the default, hold proposed. */
      cJSON_AddStringToObject(gate, "result", "benchmark_unavailable");
      write_benchmark_trace(model_id, NULL, k, "benchmark_unavailable");
      cJSON_AddStringToObject(report, "status", "proposed");
      cJSON_AddStringToObject(report, "reason", "benchmark_unavailable");
      if (report_out)
         *report_out = cJSON_PrintUnformatted(report);
      cJSON_Delete(report);
      return 1;
   }

   cJSON_AddNumberToObject(gate, "ndcg_candidate", eval.mean_candidate);
   cJSON_AddNumberToObject(gate, "ndcg_incumbent", eval.mean_incumbent);
   cJSON_AddNumberToObject(gate, "delta", eval.mean_candidate - eval.mean_incumbent);
   cJSON_AddNumberToObject(gate, "n_queries", eval.n_queries);
   cJSON_AddNumberToObject(gate, "wins", eval.wins);
   cJSON_AddNumberToObject(gate, "losses", eval.losses);
   cJSON_AddNumberToObject(gate, "ties", eval.ties);
   cJSON_AddNumberToObject(gate, "min_queries", RANK_FIT_MIN_BENCH_QUERIES);

   /* Underpowered gate → refuse. A gate that cannot detect a regression must not be
    * allowed to authorise one; hold the model proposed and say why. */
   if (eval.n_queries < RANK_FIT_MIN_BENCH_QUERIES)
   {
      cJSON_AddStringToObject(gate, "result", "benchmark_underpowered");
      write_benchmark_trace(model_id, &eval, k, "benchmark_underpowered");
      cJSON_AddStringToObject(report, "status", "proposed");
      cJSON_AddStringToObject(report, "reason", "benchmark_underpowered");
      if (report_out)
         *report_out = cJSON_PrintUnformatted(report);
      cJSON_Delete(report);
      return 1;
   }

   /* Two independent conditions, both required:
    *   mean lift  — the aggregate metric actually moved by a shippable margin;
    *   paired majority — it moved because more queries improved than regressed,
    *                     not because one query improved enormously.
    * Either alone is satisfiable by an overfitted model. */
   int has_lift = eval.mean_candidate > eval.mean_incumbent + RANK_FIT_LIFT_EPSILON;
   int has_majority = eval.wins > eval.losses;

   if (has_lift && has_majority)
   {
      int crc = kb_ranker_model_commit(model_id);
      const char *decision = (crc == 0) ? "commit" : "commit_failed";
      cJSON_AddStringToObject(gate, "result", decision);
      write_benchmark_trace(model_id, &eval, k, decision);
      cJSON_AddStringToObject(report, "status", crc == 0 ? "committed" : "error");
      if (report_out)
         *report_out = cJSON_PrintUnformatted(report);
      int rv = (crc == 0) ? 0 : -1;
      cJSON_Delete(report);
      return rv;
   }

   /* No lift — the fitted model stays proposed; the {0.6,0.4} default keeps serving.
    * Distinguish the two failure shapes so the operator can tell "no real gain" from
    * "gain concentrated in a minority of queries". */
   const char *reason = !has_lift ? "no_lift" : "no_paired_majority";
   cJSON_AddStringToObject(gate, "result", reason);
   write_benchmark_trace(model_id, &eval, k, reason);
   cJSON_AddStringToObject(report, "status", "proposed");
   cJSON_AddStringToObject(report, "reason", reason);
   if (report_out)
      *report_out = cJSON_PrintUnformatted(report);
   cJSON_Delete(report);
   return 1;
}
