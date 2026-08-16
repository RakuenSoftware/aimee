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

#include "modules/db2/c/db2_test_shim.h"
#include "../modules/db2/c/db_postgres.h"
#include "../modules/db2/c/db2_internal.h"
#include "../modules/db2/c/artifacts.h"
#include "feature_rows.h"
#include "../kb_ranker.h"
#include "../kb_ranker_fit.h"
#include "config.h"
#include <cJSON.h>

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}
static void close_db(void)
{
   db2_test_shim_close();
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
   int rc = db2_feature_row_upsert(subj, "kb_document", "", "", "v1", f, NULL);
   assert(rc == 0);
}

static void write_exec(const char *path, const char *content)
{
   FILE *fp = fopen(path, "wb");
   assert(fp);
   fputs(content, fp);
   fclose(fp);
   chmod(path, 0755);
}

static void write_file(const char *path, const char *content)
{
   FILE *fp = fopen(path, "wb");
   assert(fp);
   fputs(content, fp);
   fclose(fp);
}

static int count_kind(const char *kind)
{
   void *conn = db2_conn();
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT COUNT(*) FROM artifacts WHERE kind = ?1", err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", kind);
   int n = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c = aimee_pg_column_text(st, 0);
      n = c ? atoi(c) : 0;
   }
   aimee_pg_finalize(st);
   return n;
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
   int64_t docs[2] = {100, 101};
   insert_feat(100, 0.9, 0.8, 0.9);
   insert_feat(101, 0.2, 0.1, 0.3);

   /* Mint a kb_hybrid event for the "query", then attribute per-doc outcomes to
    * it — exactly what an outcome-reporting caller does after a search. */
   char ev[64] = "";
   assert(kb_ranker_emit_event(docs, 2, "fp-query", ev, sizeof(ev)) == 0);
   assert(ev[0] != '\0');
   assert(kb_ranker_outcome_write(ev, 100, "accepted", 1.0) == 0);
   assert(kb_ranker_outcome_write(ev, 101, "contradicted", 1.0) == 0);

   cJSON *rows = NULL;
   int ng = 0, nr = 0, np = 0;
   assert(kb_ranker_training_view("kb_document", "v1", &rows, &ng, &nr, &np) == 0);
   assert(nr == 2); /* both candidates joined to their feature rows */
   assert(np == 1); /* one accepted */
   assert(ng == 1); /* the emitted event groups both outcomes */
   cJSON *r0 = cJSON_GetArrayItem(rows, 0);
   assert(strcmp(sstr(r0, "group"), ev) == 0); /* grouped by the emitted event id */
   cJSON_Delete(rows);
   close_db();
   printf("  closed_loop_capture: ok\n");
}

/* ---- 3. fit disabled ---- */
static void test_fit_disabled(void)
{
   open_db();
   static config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_snapshot_init(&cfg);
   char *rep = NULL;
   int rc = kb_ranker_fit_run(NULL, 0, &rep);
   assert(rc == 1);
   assert(rep);
   cJSON *o = cJSON_Parse(rep);
   free(rep);
   assert(strcmp(sstr(o, "status"), "disabled") == 0);
   cJSON_Delete(o);
   close_db();
   printf("  fit_disabled: ok\n");
}

/* ---- 4. below floor ---- */
static void test_fit_below_floor(void)
{
   open_db();
   insert_feat(100, 0.9, 0.8, 0.9);
   insert_feat(101, 0.2, 0.1, 0.3);
   insert_attr("e1", 100, "accepted");
   insert_attr("e1", 101, "contradicted");

   static config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_ranker_fit_enabled = 1;
   cfg.kb_ranker_fit_min_groups = 8;
   snprintf(cfg.kb_ranker_fit_command, sizeof(cfg.kb_ranker_fit_command), "/bin/true");

   config_snapshot_init(&cfg);
   char *rep = NULL;
   int rc = kb_ranker_fit_run(NULL, 0, &rep);
   assert(rc == 1);
   cJSON *o = cJSON_Parse(rep);
   free(rep);
   assert(strcmp(sstr(o, "reason"), "below_floor") == 0);
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(o, "diagnostic")));
   cJSON_Delete(o);
   close_db();
   printf("  fit_below_floor: ok\n");
}

/* Seed two event groups, each with an accepted + a contradicted candidate — the
 * training view is non-degenerate and above a floor of 2. */
static void seed_two_groups(void)
{
   insert_feat(100, 0.9, 0.8, 0.9);
   insert_feat(101, 0.2, 0.1, 0.3);
   insert_feat(102, 0.85, 0.75, 0.8);
   insert_feat(103, 0.15, 0.2, 0.25);
   insert_attr("e1", 100, "accepted");
   insert_attr("e1", 101, "contradicted");
   insert_attr("e2", 102, "accepted");
   insert_attr("e2", 103, "contradicted");
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

/* Concatenate `a` repeated na times then `b` repeated nb times into a JSON array.
 * Returns a malloc'd string the caller frees. */
static char *build_fixture(const char *a, int na, const char *b, int nb)
{
   size_t cap = (strlen(a) + 2) * (size_t)na + (strlen(b) + 2) * (size_t)nb + 4;
   char *buf = malloc(cap);
   assert(buf);
   size_t off = 0;
   off += (size_t)snprintf(buf + off, cap - off, "[");
   for (int i = 0; i < na; i++)
      off += (size_t)snprintf(buf + off, cap - off, "%s%s", i ? "," : "", a);
   for (int i = 0; i < nb; i++)
      off += (size_t)snprintf(buf + off, cap - off, "%s%s", (na || i) ? "," : "", b);
   snprintf(buf + off, cap - off, "]");
   return buf;
}

/* 24 lex-wins + 8 dense-wins = 32 queries. A lex-only model takes 24 wins to 8
 * losses (a real majority) with mean lift ~+0.185. */
static char *fixture_lex_majority(void)
{
   return build_fixture(Q_LEX_WINS, 24, Q_DENSE_WINS, 8);
}

/* 12 win-big + 20 lose-small = 32 queries. A lex-only model gains ~+0.0998 mean
 * NDCG while LOSING on 20 of 32 queries — the overfitting shape a mean-only gate
 * cannot see. */
static char *fixture_minority_gain(void)
{
   return build_fixture(Q_WIN_BIG, 12, Q_LOSE_SMALL, 20);
}

/* One query — below the gate's minimum, whatever its content. */
static const char *FIXTURE_UNDERPOWERED = "[" Q_LEX_WINS "]";

static char *stub_weights(const char *w)
{
   static char buf[512];
   snprintf(buf, sizeof(buf),
            "#!/bin/sh\ncat >/dev/null\nprintf '%%s' '{\"status\":\"ok\",\"weights\":%s,"
            "\"fit_metrics\":{\"n_groups\":2}}'\n",
            w);
   return buf;
}

/* ---- 5. commit on lift + round-trip + benchmark_trace ---- */
static void test_gate_commit_on_lift(void)
{
   open_db();
   seed_two_groups();
   write_exec(
       "/tmp/rf_stub_win.sh",
       stub_weights("{\"dense.cos\":0.0,\"lex.cos\":1.0,\"temp.recency\":0.0,"
                    "\"sketch.frequency_kind_scope\":0.0,\"sketch.distinct_sources_hll\":0.0}"));
   char *fix = fixture_lex_majority();
   write_file("/tmp/rf_fix.json", fix);
   free(fix);

   static config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_ranker_fit_enabled = 1;
   cfg.kb_ranker_fit_min_groups = 2;
   cfg.kb_ranker_fit_bench_k = 5;
   snprintf(cfg.kb_ranker_fit_command, sizeof(cfg.kb_ranker_fit_command), "/tmp/rf_stub_win.sh");
   snprintf(cfg.kb_ranker_fit_benchmark, sizeof(cfg.kb_ranker_fit_benchmark), "/tmp/rf_fix.json");

   config_snapshot_init(&cfg);
   char id[64] = "";
   char *rep = NULL;
   int rc = kb_ranker_fit_run(id, sizeof(id), &rep);
   assert(rep);
   cJSON *o = cJSON_Parse(rep);
   free(rep);
   assert(rc == 0);
   assert(strcmp(sstr(o, "status"), "committed") == 0);
   cJSON *gate = cJSON_GetObjectItemCaseSensitive(o, "gate");
   assert(strcmp(sstr(gate, "result"), "commit") == 0);
   assert(snum(gate, "ndcg_candidate") > snum(gate, "ndcg_incumbent"));
   /* Both gate conditions are visible and were genuinely met, not just the mean. */
   assert((int)snum(gate, "n_queries") == 32);
   assert((int)snum(gate, "wins") == 24);
   assert((int)snum(gate, "losses") == 8);
   /* Round-trip: the committed model loads and parses. */
   assert(kb_ranker_model_load() == 0);
   /* Evidence trail recorded. */
   assert(count_kind("benchmark_trace") >= 1);
   assert(id[0] != '\0');
   cJSON_Delete(o);
   close_db();
   printf("  gate_commit_on_lift: ok\n");
}

/* ---- 6. hold proposed when no lift ---- */
static void test_gate_hold_on_no_lift(void)
{
   open_db();
   seed_two_groups();
   /* dense-only weights keep the incumbent's (wrong) ordering → no lift. */
   write_exec(
       "/tmp/rf_stub_lose.sh",
       stub_weights("{\"dense.cos\":1.0,\"lex.cos\":0.0,\"temp.recency\":0.0,"
                    "\"sketch.frequency_kind_scope\":0.0,\"sketch.distinct_sources_hll\":0.0}"));
   char *fix = fixture_lex_majority();
   write_file("/tmp/rf_fix.json", fix);
   free(fix);

   static config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_ranker_fit_enabled = 1;
   cfg.kb_ranker_fit_min_groups = 2;
   snprintf(cfg.kb_ranker_fit_command, sizeof(cfg.kb_ranker_fit_command), "/tmp/rf_stub_lose.sh");
   snprintf(cfg.kb_ranker_fit_benchmark, sizeof(cfg.kb_ranker_fit_benchmark), "/tmp/rf_fix.json");

   config_snapshot_init(&cfg);
   char *rep = NULL;
   int rc = kb_ranker_fit_run(NULL, 0, &rep);
   cJSON *o = cJSON_Parse(rep);
   free(rep);
   assert(rc == 1);
   assert(strcmp(sstr(o, "status"), "proposed") == 0);
   assert(strcmp(sstr(o, "reason"), "no_lift") == 0);
   /* No committed model → the {0.6,0.4} default keeps serving. */
   assert(kb_ranker_model_load() == -1);
   cJSON_Delete(o);
   close_db();
   printf("  gate_hold_on_no_lift: ok\n");
}

/* ---- 6b. an underpowered fixture refuses to promote at all ----
 * The regression this guards: the shipped default fixture holds 5 queries and the
 * old gate promoted on it with a 1e-6 epsilon. Content is irrelevant here — the
 * model would WIN this query outright; the gate must still refuse on size. */
static void test_gate_refuses_underpowered_benchmark(void)
{
   open_db();
   seed_two_groups();
   write_exec(
       "/tmp/rf_stub_win.sh",
       stub_weights("{\"dense.cos\":0.0,\"lex.cos\":1.0,\"temp.recency\":0.0,"
                    "\"sketch.frequency_kind_scope\":0.0,\"sketch.distinct_sources_hll\":0.0}"));
   write_file("/tmp/rf_fix.json", FIXTURE_UNDERPOWERED);

   static config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_ranker_fit_enabled = 1;
   cfg.kb_ranker_fit_min_groups = 2;
   cfg.kb_ranker_fit_bench_k = 5;
   snprintf(cfg.kb_ranker_fit_command, sizeof(cfg.kb_ranker_fit_command), "/tmp/rf_stub_win.sh");
   snprintf(cfg.kb_ranker_fit_benchmark, sizeof(cfg.kb_ranker_fit_benchmark), "/tmp/rf_fix.json");

   config_snapshot_init(&cfg);
   char *rep = NULL;
   int rc = kb_ranker_fit_run(NULL, 0, &rep);
   cJSON *o = cJSON_Parse(rep);
   free(rep);
   assert(rc == 1);
   assert(strcmp(sstr(o, "status"), "proposed") == 0);
   assert(strcmp(sstr(o, "reason"), "benchmark_underpowered") == 0);
   cJSON *gate = cJSON_GetObjectItemCaseSensitive(o, "gate");
   assert((int)snum(gate, "n_queries") == 1);
   /* The candidate DID win the query — refusal is on power, not on merit. */
   assert((int)snum(gate, "wins") == 1);
   assert(snum(gate, "ndcg_candidate") > snum(gate, "ndcg_incumbent"));
   /* Nothing promoted. */
   assert(kb_ranker_model_load() == -1);
   cJSON_Delete(o);
   close_db();
   printf("  gate_refuses_underpowered_benchmark: ok\n");
}

/* ---- 6c. mean lift concentrated in a minority of queries is refused ----
 * The overfitting shape: +0.0998 mean NDCG built from 12 large wins against 20
 * smaller losses. The old mean-only gate would have committed this. */
static void test_gate_refuses_minority_gain(void)
{
   open_db();
   seed_two_groups();
   write_exec(
       "/tmp/rf_stub_win.sh",
       stub_weights("{\"dense.cos\":0.0,\"lex.cos\":1.0,\"temp.recency\":0.0,"
                    "\"sketch.frequency_kind_scope\":0.0,\"sketch.distinct_sources_hll\":0.0}"));
   char *fix = fixture_minority_gain();
   write_file("/tmp/rf_fix.json", fix);
   free(fix);

   static config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_ranker_fit_enabled = 1;
   cfg.kb_ranker_fit_min_groups = 2;
   cfg.kb_ranker_fit_bench_k = 5;
   snprintf(cfg.kb_ranker_fit_command, sizeof(cfg.kb_ranker_fit_command), "/tmp/rf_stub_win.sh");
   snprintf(cfg.kb_ranker_fit_benchmark, sizeof(cfg.kb_ranker_fit_benchmark), "/tmp/rf_fix.json");

   config_snapshot_init(&cfg);
   char *rep = NULL;
   int rc = kb_ranker_fit_run(NULL, 0, &rep);
   cJSON *o = cJSON_Parse(rep);
   free(rep);
   assert(rc == 1);
   assert(strcmp(sstr(o, "status"), "proposed") == 0);
   assert(strcmp(sstr(o, "reason"), "no_paired_majority") == 0);
   cJSON *gate = cJSON_GetObjectItemCaseSensitive(o, "gate");
   /* The mean genuinely rose — that is exactly why a mean-only gate was unsafe. */
   assert(snum(gate, "ndcg_candidate") > snum(gate, "ndcg_incumbent"));
   assert((int)snum(gate, "n_queries") == 32);
   assert((int)snum(gate, "wins") == 12);
   assert((int)snum(gate, "losses") == 20);
   assert(kb_ranker_model_load() == -1);
   cJSON_Delete(o);
   close_db();
   printf("  gate_refuses_minority_gain: ok\n");
}

/* ---- 7. sidecar refusal is surfaced, nothing committed ---- */
static void test_sidecar_refusal(void)
{
   open_db();
   seed_two_groups();
   write_exec("/tmp/rf_stub_refuse.sh",
              "#!/bin/sh\ncat >/dev/null\nprintf '%s' "
              "'{\"status\":\"refused\",\"reason\":\"version_mismatch\"}'\n");

   static config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_ranker_fit_enabled = 1;
   cfg.kb_ranker_fit_min_groups = 2;
   snprintf(cfg.kb_ranker_fit_command, sizeof(cfg.kb_ranker_fit_command), "/tmp/rf_stub_refuse.sh");

   config_snapshot_init(&cfg);
   char *rep = NULL;
   int rc = kb_ranker_fit_run(NULL, 0, &rep);
   cJSON *o = cJSON_Parse(rep);
   free(rep);
   assert(rc == 1);
   assert(strcmp(sstr(o, "reason"), "version_mismatch") == 0);
   assert(kb_ranker_model_load() == -1);
   cJSON_Delete(o);
   close_db();
   printf("  sidecar_refusal: ok\n");
}

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
   test_fit_disabled();
   test_fit_below_floor();
   test_gate_commit_on_lift();
   test_gate_hold_on_no_lift();
   test_gate_refuses_underpowered_benchmark();
   test_gate_refuses_minority_gain();
   test_sidecar_refusal();
   test_sidecar_recovers_order();
   test_sidecar_pairwise();
   printf("test_ranker_fit: all passed\n");
   return 0;
}
