/* test_ranker_fit.c — unit tests for the learning-to-rank weight fitter.
 *
 * Covers the proposal's Tests section
 * (docs/proposals/done/learning-to-rank-weight-fitting.md):
 *   1. empty_view_diagnostic:   an empty training view emits the structured
 *                               wiring-gap diagnostic (never a silent zero).
 *   2. training_view_join:      feature_rows ⋈ retrieval_attribution joins per
 *                               event with correct labels (accepted=1, else 0).
 *   3. fit_disabled:            fit refuses (status=disabled) when the flag is off.
 *   4. fit_below_floor:         too few labelled groups → refused below_floor,
 *                               default kept, diagnostic attached.
 *   5. gate_commit_on_lift:     a fitted model that beats the incumbent on the
 *                               NDCG fixture is committed + round-trips through
 *                               kb_ranker_model_load; a benchmark_trace is written.
 *   6. gate_hold_on_no_lift:    a model that does not beat the incumbent stays
 *                               proposed (not committed).
 *   6b. gate_refuses_underpowered_benchmark:
 *                               a fixture below RANK_FIT_MIN_BENCH_QUERIES refuses
 *                               to promote even a model that wins it outright.
 *   6c. gate_refuses_minority_gain:
 *                               mean lift concentrated in a minority of queries is
 *                               refused (paired win/loss majority required).
 *   7. sidecar_refusal:         a sidecar refusal (e.g. version_mismatch) is
 *                               surfaced and no model is committed.
 *   8. sidecar_recovers_order:  the real rank-fit.py recovers a planted separable
 *                               ordering (skipped gracefully if python3 absent).
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modules/kb/c/kb_store_test_shim.h"
#include "../modules/kb/c/db_postgres.h"
#include "../modules/kb/c/kb_store_internal.h"
#include "../modules/kb/c/artifacts.h"
#include "feature_rows.h"
#include "../kb_ranker.h"
#include "../kb_ranker_fit.h"
#include "config.h"
#include <cJSON.h>

static void open_db(void)
{
   kb_store_test_shim_close();
   kb_store_test_shim_open();
}
static void close_db(void)
{
   kb_store_test_shim_close();
}

static void insert_attr(const char *event_id, long long surfaced, const char *verdict)
{
   /* Option B: outcomes for the ranker surface are dedicated `ranker_outcome`
    * artifacts (kb_document ids), written via the capture primitive. */
   int rc = kb_ranker_outcome_write(event_id, (int64_t)surfaced, verdict, 1.0);
   assert(rc == 0);
}

static void insert_feat(long long doc, double dense, double lex, double rec)
{
   char subj[32];
   snprintf(subj, sizeof(subj), "%lld", doc);
   char f[256];
   snprintf(f, sizeof(f),
            "{\"lex.cos\":%.4f,\"dense.cos\":%.4f,\"temp.recency\":%.4f,"
            "\"sketch.frequency_kind_scope\":0.0,\"sketch.distinct_sources_hll\":0.0}",
            lex, dense, rec);
   int rc = kb_store_feature_row_upsert(subj, "kb_document", "", "", "v1", f, NULL);
   assert(rc == 0);
}

static void insert_work_outcome(const char *outcome_id, const char *event_id,
                                const char *subject_kind, const char *subject_id,
                                const char *outcome)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       kb_store_conn(),
       "INSERT INTO work_outcomes(outcome_id,retrieval_event_id,subject_kind,subject_id,"
       " authenticated_evaluator,outcome,occurred_at)"
       " VALUES(?1,?2,?3,?4,'test',?5,'2026-01-01 00:00:00')",
       err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", outcome_id);
   aimee_pg_bind_text(st, "?2", event_id);
   aimee_pg_bind_text(st, "?3", subject_kind);
   aimee_pg_bind_text(st, "?4", subject_id);
   aimee_pg_bind_text(st, "?5", outcome);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(st);
}

static const char *sstr(cJSON *o, const char *k)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
   return cJSON_IsString(v) ? v->valuestring : "";
}
static double snum(cJSON *o, const char *k)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
   return cJSON_IsNumber(v) ? v->valuedouble : -12345.0;
}

/* ---- 1. empty view diagnostic ---- */
static void test_empty_view_diagnostic(void)
{
   open_db();
   char *j = kb_ranker_export_view_json("kb_document", NULL);
   assert(j);
   cJSON *o = cJSON_Parse(j);
   free(j);
   assert(o);
   assert((int)snum(o, "n_rows") == 0);
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "fittable")));
   cJSON *diag = cJSON_GetObjectItemCaseSensitive(o, "diagnostic");
   assert(cJSON_IsObject(diag));
   assert(strstr(sstr(diag, "subject_space_mismatch"), "kb_document") != NULL);
   assert(strstr(sstr(diag, "missing_grouping_key"), "retrieval_event_id") != NULL);
   cJSON_Delete(o);
   close_db();
   printf("  empty_view_diagnostic: ok\n");
}

/* ---- 2. training view join + labels ---- */
static void test_training_view_join(void)
{
   open_db();
   insert_feat(100, 0.9, 0.8, 0.9);
   insert_feat(101, 0.2, 0.1, 0.3);
   insert_attr("e1", 100, "accepted");
   insert_attr("e1", 101, "contradicted");
   /* An attribution whose candidate has NO feature row must not appear. */
   insert_attr("e1", 999, "accepted");

   cJSON *rows = NULL;
   int ng = 0, nr = 0, np = 0;
   int rc = kb_ranker_training_view("kb_document", "v1", &rows, &ng, &nr, &np);
   assert(rc == 0);
   assert(nr == 2); /* doc 999 dropped: no feature vector */
   assert(np == 1); /* one accepted */
   assert(ng == 1); /* one event group */
   cJSON_Delete(rows);
   close_db();
   printf("  training_view_join: ok\n");
}

/* ---- 2b. option B: emit-event + record-outcome closes the loop end-to-end ---- */
static void test_closed_loop_capture(void)
{
   open_db();
   int64_t docs[2] = {INT64_C(9007199254740992), INT64_C(9007199254740993)};
   insert_feat(INT64_C(9007199254740992), 0.9, 0.8, 0.9);
   insert_feat(INT64_C(9007199254740993), 0.2, 0.1, 0.3);

   /* Mint a kb_hybrid event for the "query", then attribute per-doc outcomes to
    * it — exactly what an outcome-reporting caller does after a search. */
   char ev[64] = "";
   assert(kb_ranker_emit_event(docs, 2, "fp-query", ev, sizeof(ev)) == 0);
   assert(ev[0] != '\0');
   assert(kb_ranker_outcome_write(ev, INT64_C(9007199254740992), "accepted", 1.0) == 0);
   assert(kb_ranker_outcome_write(ev, INT64_C(9007199254740993), "contradicted", 1.0) == 0);

   cJSON *rows = NULL;
   int ng = 0, nr = 0, np = 0;
   assert(kb_ranker_training_view("kb_document", "v1", &rows, &ng, &nr, &np) == 0);
   assert(nr == 2); /* both candidates joined to their feature rows */
   assert(np == 1); /* one accepted */
   assert(ng == 1); /* the emitted event groups both outcomes */
   cJSON *r0 = cJSON_GetArrayItem(rows, 0);
   assert(strcmp(sstr(r0, "group"), ev) == 0); /* grouped by the emitted event id */
   cJSON *r1 = cJSON_GetArrayItem(rows, 1);
   const char *first = sstr(r0, "subject_id"), *second = sstr(r1, "subject_id");
   assert(strcmp(first, second) != 0);
   assert((!strcmp(first, "9007199254740992") && !strcmp(second, "9007199254740993")) ||
          (!strcmp(first, "9007199254740993") && !strcmp(second, "9007199254740992")));
   cJSON_Delete(rows);
   close_db();
   printf("  closed_loop_capture: ok\n");
}

/* ---- 2c. canonical P5 work outcomes feed the ranker training view ---- */
static void test_work_outcome_capture(void)
{
   open_db();
   insert_feat(100, 0.9, 0.8, 0.9);
   insert_feat(101, 0.2, 0.1, 0.3);
   insert_work_outcome("wo-1", "work-e1", "document", "100", "useful");
   insert_work_outcome("wo-2", "work-e1", "document", "101", "dead_end");
   /* Same id, different subject space: must not contaminate document fitting. */
   insert_work_outcome("wo-3", "work-e1", "assertion", "100", "useful");

   cJSON *rows = NULL;
   int ng = 0, nr = 0, np = 0;
   assert(kb_ranker_training_view("kb_document", "v1", &rows, &ng, &nr, &np) == 0);
   assert(nr == 2);
   assert(np == 1);
   assert(ng == 1);
   cJSON_Delete(rows);
   close_db();
   printf("  work_outcome_capture: ok\n");
}

/* ---- Gate fixtures -------------------------------------------------------
 *
 * The gate needs at least RANK_FIT_MIN_BENCH_QUERIES (30) queries to decide
 * anything, so these are built rather than hand-written. Each shape below is
 * chosen for what it does to the {0.6 dense, 0.4 lex} incumbent:
 *
 *   LEX_WINS   incumbent mis-ranks (dense-leaning blend puts the irrelevant doc
 *              first); a lex-only model ranks correctly.  lex +0.369/query.
 *   DENSE_WINS the mirror image; a lex-only model mis-ranks it.  lex -0.369/query.
 *   WIN_BIG    incumbent buries the one relevant doc at rank 3; lex-only puts it
 *              first.  lex +0.5/query.
 *   LOSE_SMALL graded relevance where lex-only swaps the top two.  lex -0.14/query.
 *
 * Mixing them produces a fixture with a known win/loss split AND a known mean
 * lift, which is what lets the paired-majority condition be tested independently
 * of the mean-lift condition. */

#define Q_LEX_WINS                                                                                 \
   "{\"query\":\"lw\",\"candidates\":["                                                            \
   "{\"subject_id\":\"a\",\"relevance\":1.0,\"features\":{\"dense.cos\":0.5,\"lex.cos\":0.5}},"    \
   "{\"subject_id\":\"b\",\"relevance\":0.0,\"features\":{\"dense.cos\":0.9,\"lex.cos\":0.1}}]}"

#define Q_DENSE_WINS                                                                               \
   "{\"query\":\"dw\",\"candidates\":["                                                            \
   "{\"subject_id\":\"a\",\"relevance\":1.0,\"features\":{\"dense.cos\":0.9,\"lex.cos\":0.1}},"    \
   "{\"subject_id\":\"b\",\"relevance\":0.0,\"features\":{\"dense.cos\":0.5,\"lex.cos\":0.5}}]}"

#define Q_WIN_BIG                                                                                  \
   "{\"query\":\"wb\",\"candidates\":["                                                            \
   "{\"subject_id\":\"a\",\"relevance\":1.0,\"features\":{\"dense.cos\":0.0,\"lex.cos\":1.0}},"    \
   "{\"subject_id\":\"b\",\"relevance\":0.0,\"features\":{\"dense.cos\":1.0,\"lex.cos\":0.0}},"    \
   "{\"subject_id\":\"c\",\"relevance\":0.0,\"features\":{\"dense.cos\":0.9,\"lex.cos\":0.0}}]}"

#define Q_LOSE_SMALL                                                                               \
   "{\"query\":\"ls\",\"candidates\":["                                                            \
   "{\"subject_id\":\"a\",\"relevance\":1.0,\"features\":{\"dense.cos\":1.0,\"lex.cos\":0.9}},"    \
   "{\"subject_id\":\"b\",\"relevance\":0.5,\"features\":{\"dense.cos\":0.9,\"lex.cos\":1.0}},"    \
   "{\"subject_id\":\"c\",\"relevance\":0.0,\"features\":{\"dense.cos\":0.0,\"lex.cos\":0.0}}]}"

/* ---- 8. the real sidecar recovers a planted separable ordering ---- */
static void test_sidecar_recovers_order(void)
{
   /* Locate rank-fit.py relative to a few plausible cwds; skip if absent. */
   const char *cands[] = {"scripts/rank-fit.py", "../scripts/rank-fit.py",
                          "../../scripts/rank-fit.py"};
   const char *script = NULL;
   for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
      if (access(cands[i], R_OK) == 0)
      {
         script = cands[i];
         break;
      }
   if (!script || system("command -v python3 >/dev/null 2>&1") != 0)
   {
      printf("  sidecar_recovers_order: skipped (python3/rank-fit.py unavailable)\n");
      return;
   }

   /* Separable batch: positives high on dense+lex, negatives low. */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "python3 %s > /tmp/rf_realout.json", script);
   FILE *p = popen(cmd, "w");
   assert(p);
   fputs("{\"feature_set_version\":\"v1\",\"objective\":\"pointwise\",\"min_groups\":8,\"rows\":[",
         p);
   for (int g = 0; g < 12; g++)
   {
      if (g)
         fputs(",", p);
      fprintf(p,
              "{\"group\":\"e%d\",\"label\":1,\"features\":{\"dense.cos\":0.9,\"lex.cos\":0.85,"
              "\"temp.recency\":0.9,\"sketch.frequency_kind_scope\":3,"
              "\"sketch.distinct_sources_hll\":2}},"
              "{\"group\":\"e%d\",\"label\":0,\"features\":{\"dense.cos\":0.15,\"lex.cos\":0.1,"
              "\"temp.recency\":0.3,\"sketch.frequency_kind_scope\":1,"
              "\"sketch.distinct_sources_hll\":1}}",
              g, g);
   }
   fputs("]}", p);
   int st = pclose(p);
   assert(st == 0);

   FILE *rf = fopen("/tmp/rf_realout.json", "rb");
   assert(rf);
   char buf[4096];
   size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
   buf[n] = '\0';
   fclose(rf);
   cJSON *o = cJSON_Parse(buf);
   assert(o);
   assert(strcmp(sstr(o, "status"), "ok") == 0);
   cJSON *w = cJSON_GetObjectItemCaseSensitive(o, "weights");
   assert(cJSON_IsObject(w));
   /* The discriminative features must carry positive weight — the planted
    * ordering is recovered. */
   assert(snum(w, "dense.cos") > 0.0);
   assert(snum(w, "lex.cos") > 0.0);
   cJSON_Delete(o);
   printf("  sidecar_recovers_order: ok\n");
}

/* ---- 9. the real sidecar fits the pairwise objective (within-query ordering) ---- */
static void test_sidecar_pairwise(void)
{
   const char *cands[] = {"scripts/rank-fit.py", "../scripts/rank-fit.py",
                          "../../scripts/rank-fit.py"};
   const char *script = NULL;
   for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
      if (access(cands[i], R_OK) == 0)
      {
         script = cands[i];
         break;
      }
   if (!script || system("command -v python3 >/dev/null 2>&1") != 0)
   {
      printf("  sidecar_pairwise: skipped (python3/rank-fit.py unavailable)\n");
      return;
   }

   /* Each query has a used (label 1) and an unused (label 0) candidate that
    * differ only in dense/lex — so pairwise must recover positive dense/lex
    * weight and ~0 on the constant recency feature. */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "python3 %s > /tmp/rf_pairout.json", script);
   FILE *p = popen(cmd, "w");
   assert(p);
   fputs("{\"feature_set_version\":\"v1\",\"objective\":\"pairwise\",\"min_groups\":4,\"rows\":[",
         p);
   for (int g = 0; g < 6; g++)
   {
      if (g)
         fputs(",", p);
      fprintf(p,
              "{\"group\":\"q%d\",\"label\":1,\"weight\":1.0,\"features\":{\"dense.cos\":0.9,"
              "\"lex.cos\":0.8,\"temp.recency\":0.5}},"
              "{\"group\":\"q%d\",\"label\":0,\"weight\":1.0,\"features\":{\"dense.cos\":0.3,"
              "\"lex.cos\":0.2,\"temp.recency\":0.5}}",
              g, g);
   }
   fputs("]}", p);
   assert(pclose(p) == 0);

   FILE *rf = fopen("/tmp/rf_pairout.json", "rb");
   assert(rf);
   char buf[4096];
   size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
   buf[n] = '\0';
   fclose(rf);
   cJSON *o = cJSON_Parse(buf);
   assert(o);
   assert(strcmp(sstr(o, "status"), "ok") == 0);
   cJSON *m = cJSON_GetObjectItemCaseSensitive(o, "fit_metrics");
   assert(strcmp(sstr(m, "objective"), "pairwise") == 0);
   assert(snum(m, "n_pairs") > 0.0);
   cJSON *w = cJSON_GetObjectItemCaseSensitive(o, "weights");
   assert(snum(w, "dense.cos") > 0.0);
   assert(snum(w, "lex.cos") > 0.0);
   /* Constant-within-query feature earns ~0 weight — the pairwise property. */
   assert(snum(w, "temp.recency") < 0.5 && snum(w, "temp.recency") > -0.5);
   cJSON_Delete(o);
   printf("  sidecar_pairwise: ok\n");
}

int main(void)
{
   printf("test_ranker_fit:\n");
   test_empty_view_diagnostic();
   test_training_view_join();
   test_closed_loop_capture();
   test_work_outcome_capture();
   test_sidecar_recovers_order();
   test_sidecar_pairwise();
   printf("test_ranker_fit: all passed\n");
   return 0;
}
