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

#include "db2_test_shim.h"
#include "../db2/db_postgres.h"
#include "../db2/db2_internal.h"
#include "../db2/artifacts.h"
#include "feature_rows.h"
#include "../kb_ranker.h"
#include "../kb_ranker_fit.h"
#include "../headers/config.h"
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
   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   char payload[512];
   snprintf(payload, sizeof(payload),
            "{\"retrieval_event_id\":\"%s\",\"surfaced_row_id\":%lld,\"verdict\":\"%s\","
            "\"weight\":1.0}",
            event_id, surfaced, verdict);
   int rc = db2_artifact_write(id, "retrieval_attribution", "proposed", "memory", "", "", 1.0,
                               payload);
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

/* ---- 3. fit disabled ---- */
static void test_fit_disabled(void)
{
   open_db();
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   char *rep = NULL;
   int rc = kb_ranker_fit_run(&cfg, NULL, 0, &rep);
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

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_ranker_fit_enabled = 1;
   cfg.kb_ranker_fit_min_groups = 8;
   snprintf(cfg.kb_ranker_fit_command, sizeof(cfg.kb_ranker_fit_command), "/bin/true");

   char *rep = NULL;
   int rc = kb_ranker_fit_run(&cfg, NULL, 0, &rep);
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

/* Fixture where the incumbent {0.6,0.4} mis-ranks: the relevant doc (a) scores
 * lower than the irrelevant doc (b) under a dense-leaning blend. A lex-only
 * model ranks a first → higher NDCG. */
static const char *FIXTURE =
    "[{\"query\":\"q\",\"candidates\":["
    "{\"subject_id\":\"a\",\"relevance\":1.0,\"features\":{\"dense.cos\":0.5,\"lex.cos\":0.5}},"
    "{\"subject_id\":\"b\",\"relevance\":0.0,\"features\":{\"dense.cos\":0.9,\"lex.cos\":0.1}}]}]";

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
   write_exec("/tmp/rf_stub_win.sh",
              stub_weights("{\"dense.cos\":0.0,\"lex.cos\":1.0,\"temp.recency\":0.0,"
                           "\"sketch.frequency_kind_scope\":0.0,\"sketch.distinct_sources_hll\":0.0}"));
   write_file("/tmp/rf_fix.json", FIXTURE);

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_ranker_fit_enabled = 1;
   cfg.kb_ranker_fit_min_groups = 2;
   cfg.kb_ranker_fit_bench_k = 5;
   snprintf(cfg.kb_ranker_fit_command, sizeof(cfg.kb_ranker_fit_command), "/tmp/rf_stub_win.sh");
   snprintf(cfg.kb_ranker_fit_benchmark, sizeof(cfg.kb_ranker_fit_benchmark), "/tmp/rf_fix.json");

   char id[64] = "";
   char *rep = NULL;
   int rc = kb_ranker_fit_run(&cfg, id, sizeof(id), &rep);
   assert(rep);
   cJSON *o = cJSON_Parse(rep);
   free(rep);
   assert(rc == 0);
   assert(strcmp(sstr(o, "status"), "committed") == 0);
   cJSON *gate = cJSON_GetObjectItemCaseSensitive(o, "gate");
   assert(strcmp(sstr(gate, "result"), "commit") == 0);
   assert(snum(gate, "ndcg_candidate") > snum(gate, "ndcg_incumbent"));
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
   write_exec("/tmp/rf_stub_lose.sh",
              stub_weights("{\"dense.cos\":1.0,\"lex.cos\":0.0,\"temp.recency\":0.0,"
                           "\"sketch.frequency_kind_scope\":0.0,\"sketch.distinct_sources_hll\":0.0}"));
   write_file("/tmp/rf_fix.json", FIXTURE);

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_ranker_fit_enabled = 1;
   cfg.kb_ranker_fit_min_groups = 2;
   snprintf(cfg.kb_ranker_fit_command, sizeof(cfg.kb_ranker_fit_command), "/tmp/rf_stub_lose.sh");
   snprintf(cfg.kb_ranker_fit_benchmark, sizeof(cfg.kb_ranker_fit_benchmark), "/tmp/rf_fix.json");

   char *rep = NULL;
   int rc = kb_ranker_fit_run(&cfg, NULL, 0, &rep);
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

/* ---- 7. sidecar refusal is surfaced, nothing committed ---- */
static void test_sidecar_refusal(void)
{
   open_db();
   seed_two_groups();
   write_exec("/tmp/rf_stub_refuse.sh",
              "#!/bin/sh\ncat >/dev/null\nprintf '%s' "
              "'{\"status\":\"refused\",\"reason\":\"version_mismatch\"}'\n");

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_ranker_fit_enabled = 1;
   cfg.kb_ranker_fit_min_groups = 2;
   snprintf(cfg.kb_ranker_fit_command, sizeof(cfg.kb_ranker_fit_command), "/tmp/rf_stub_refuse.sh");

   char *rep = NULL;
   int rc = kb_ranker_fit_run(&cfg, NULL, 0, &rep);
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

int main(void)
{
   printf("test_ranker_fit:\n");
   test_empty_view_diagnostic();
   test_training_view_join();
   test_fit_disabled();
   test_fit_below_floor();
   test_gate_commit_on_lift();
   test_gate_hold_on_no_lift();
   test_sidecar_refusal();
   test_sidecar_recovers_order();
   printf("test_ranker_fit: all passed\n");
   return 0;
}
