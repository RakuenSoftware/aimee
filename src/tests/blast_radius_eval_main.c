/* blast_radius_eval_main.c: deterministic validation harness for the Bucket-3
 * code-graph flags (guardrails_blast_radius_advisory, delegate_graph_context).
 *
 * Loads a corpus fixture (a small code graph: files + exports + imports) into an
 * ISOLATED throwaway schema in a DISPOSABLE Postgres (via db2_eval_open_temp_store,
 * gated on AIMEE_DB2_EVAL_URL), runs the REAL db2_code_index_blast_radius, and scores
 * precision/recall of the advised dependents/dependencies against the corpus's
 * ground truth. delegate_graph reuses the same computation: its expected_neighbours
 * are exactly blast_radius(referenced_file)'s dependents + dependencies.
 *
 * Usage: AIMEE_DB2_EVAL_URL=postgres://... aimee-blast-radius-eval \
 *          <blast_radius_corpus.json> <delegate_graph_corpus.json>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "cJSON.h"
#include "index.h"            /* blast_radius_t */
#include "db2/code_index.h"   /* db2_code_index_blast_radius */
#include "db2/eval_support.h" /* db2_eval_open_temp_store / close */
#include "db2/db2.h"          /* db2_conn */
#include "db2/db_postgres.h"  /* aimee_pg_exec */

static char g_err[512];

static int xexec(const char *sql)
{
   return aimee_pg_exec(db2_conn(), sql, g_err, sizeof(g_err));
}

static char *slurp(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (n <= 0)
   {
      fclose(f);
      return NULL;
   }
   char *b = malloc((size_t)n + 1);
   size_t r = fread(b, 1, (size_t)n, f);
   fclose(f);
   b[r] = '\0';
   return b;
}

/* Populate projects/files/file_exports/file_imports for one fixture. The corpus is
 * a trusted in-repo file with no quote chars, so straight interpolation is safe. */
static void load_fixture(cJSON *fx)
{
   const char *proj = cJSON_GetStringValue(cJSON_GetObjectItem(fx, "project"));
   char sql[2048];
   snprintf(sql, sizeof(sql),
            "INSERT INTO projects(name,root,scanned_at) VALUES('%s','/fixture','2026-01-01') "
            "ON CONFLICT(name) DO NOTHING",
            proj);
   xexec(sql);
   cJSON *file = NULL;
   cJSON_ArrayForEach(file, cJSON_GetObjectItem(fx, "files"))
   {
      const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(file, "path"));
      snprintf(sql, sizeof(sql),
               "INSERT INTO files(project_id,path,scanned_at) SELECT id,'%s','2026-01-01' "
               "FROM projects WHERE name='%s'",
               path, proj);
      xexec(sql);
      cJSON *e = NULL;
      cJSON_ArrayForEach(e, cJSON_GetObjectItem(file, "exports"))
      {
         snprintf(sql, sizeof(sql),
                  "INSERT INTO file_exports(file_id,name) SELECT f.id,'%s' FROM files f "
                  "JOIN projects p ON p.id=f.project_id WHERE p.name='%s' AND f.path='%s'",
                  e->valuestring, proj, path);
         xexec(sql);
      }
      cJSON *im = NULL;
      cJSON_ArrayForEach(im, cJSON_GetObjectItem(file, "imports"))
      {
         snprintf(sql, sizeof(sql),
                  "INSERT INTO file_imports(file_id,name) SELECT f.id,'%s' FROM files f "
                  "JOIN projects p ON p.id=f.project_id WHERE p.name='%s' AND f.path='%s'",
                  im->valuestring, proj, path);
         xexec(sql);
      }
   }
}

static int set_contains(char list[][MAX_PATH_LEN], int n, const char *v)
{
   for (int i = 0; i < n; i++)
      if (strcmp(list[i], v) == 0)
         return 1;
   return 0;
}

/* Score one edited-file case: tally TP/FP/FN of `got` (from blast_radius) vs the
 * `expected` JSON array. Accumulates into the running totals. */
static void score_set(char got[][MAX_PATH_LEN], int ngot, cJSON *expected, int *tp, int *fp,
                      int *fn)
{
   int nexp = cJSON_GetArraySize(expected);
   for (int i = 0; i < ngot; i++)
   {
      int match = 0;
      cJSON *ev = NULL;
      cJSON_ArrayForEach(ev, expected) if (strcmp(ev->valuestring, got[i]) == 0) match = 1;
      if (match)
         (*tp)++;
      else
         (*fp)++;
   }
   cJSON *ev = NULL;
   cJSON_ArrayForEach(ev, expected) if (!set_contains(got, ngot, ev->valuestring))(*fn)++;
   (void)nexp;
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "usage: aimee-blast-radius-eval <blast_radius_corpus.json> "
                      "[<delegate_graph_corpus.json>]\n");
      return 2;
   }

   int dep_tp = 0, dep_fp = 0, dep_fn = 0; /* dependents */
   int dic_tp = 0, dic_fp = 0, dic_fn = 0; /* dependencies */
   int cases = 0, fail = 0;

   /* --- blast_radius corpus --- */
   {
      char *raw = slurp(argv[1]);
      if (!raw)
      {
         fprintf(stderr, "FAIL: cannot read %s\n", argv[1]);
         return 1;
      }
      cJSON *root = cJSON_Parse(raw);
      free(raw);
      if (!root)
      {
         fprintf(stderr, "FAIL: bad JSON in %s\n", argv[1]);
         return 1;
      }
      cJSON *fx = NULL;
      cJSON_ArrayForEach(fx, cJSON_GetObjectItem(root, "fixtures"))
      {
         if (db2_eval_open_temp_store() != 0)
         {
            fprintf(stderr, "FAIL: temp store open\n");
            return 1;
         }
         load_fixture(fx);
         const char *proj = cJSON_GetStringValue(cJSON_GetObjectItem(fx, "project"));
         cJSON *c = NULL;
         cJSON_ArrayForEach(c, cJSON_GetObjectItem(fx, "cases"))
         {
            const char *edited = cJSON_GetStringValue(cJSON_GetObjectItem(c, "edited"));
            blast_radius_t br;
            memset(&br, 0, sizeof(br));
            db2_code_index_blast_radius(proj, edited, &br);
            int before_fn = dep_fn, before_fp = dep_fp;
            score_set(br.dependents, br.dependent_count,
                      cJSON_GetObjectItem(c, "expected_dependents"), &dep_tp, &dep_fp, &dep_fn);
            score_set(br.dependencies, br.dependency_count,
                      cJSON_GetObjectItem(c, "expected_dependencies"), &dic_tp, &dic_fp, &dic_fn);
            int cfail = (dep_fn > before_fn); /* any missed dependent = recall miss */
            printf("  [%s] %s: dependents=%d deps=%d%s\n", proj, edited, br.dependent_count,
                   br.dependency_count,
                   cfail ? "  <-- MISSED a dependent" : (dep_fp > before_fp ? "  (spurious)" : ""));
            cases++;
            if (cfail)
               fail++;
         }
         db2_eval_close_temp_store();
      }
      cJSON_Delete(root);
   }

   /* --- delegate_graph corpus: expected_neighbours must all appear in
    * blast_radius(referenced).dependents + .dependencies; fail-open cases (no
    * referenced path or no edges) must yield an empty neighbour set. --- */
   int dg_ok = 0, dg_cases = 0;
   if (argc >= 3)
   {
      char *raw = slurp(argv[2]);
      cJSON *root = raw ? cJSON_Parse(raw) : NULL;
      free(raw);
      cJSON *dg_fixtures = root ? cJSON_GetObjectItem(root, "fixtures") : NULL;
      cJSON *fx = NULL;
      cJSON_ArrayForEach(fx, dg_fixtures)
      {
         if (db2_eval_open_temp_store() != 0)
            return 1;
         load_fixture(fx);
         const char *proj = cJSON_GetStringValue(cJSON_GetObjectItem(fx, "project"));
         cJSON *c = NULL;
         cJSON_ArrayForEach(c, cJSON_GetObjectItem(fx, "cases"))
         {
            dg_cases++;
            cJSON *refj = cJSON_GetObjectItem(c, "references");
            int expect_block = cJSON_IsTrue(cJSON_GetObjectItem(c, "expect_block"));
            blast_radius_t br;
            memset(&br, 0, sizeof(br));
            if (cJSON_IsString(refj))
               db2_code_index_blast_radius(proj, refj->valuestring, &br);
            int total = br.dependent_count + br.dependency_count;
            int ok = 1;
            if (!expect_block)
               ok = (total == 0); /* fail-open: no edges -> no block */
            else
            {
               cJSON *nb = NULL;
               cJSON_ArrayForEach(nb, cJSON_GetObjectItem(c, "expected_neighbours"))
               {
                  int found = set_contains(br.dependents, br.dependent_count, nb->valuestring) ||
                              set_contains(br.dependencies, br.dependency_count, nb->valuestring);
                  if (!found)
                     ok = 0;
               }
               cJSON *mnc = NULL;
               cJSON_ArrayForEach(
                   mnc,
                   cJSON_GetObjectItem(
                       c, "must_not_contain")) if (set_contains(br.dependents, br.dependent_count,
                                                                mnc->valuestring)) ok = 0;
            }
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(c, "id"));
            printf("  [dg %s] neighbours=%d %s\n", id ? id : "?", total, ok ? "ok" : "<-- FAIL");
            if (ok)
               dg_ok++;
         }
         db2_eval_close_temp_store();
      }
      cJSON_Delete(root);
   }

   double dep_recall = (dep_tp + dep_fn) ? (double)dep_tp / (dep_tp + dep_fn) : 1.0;
   double dep_prec = (dep_tp + dep_fp) ? (double)dep_tp / (dep_tp + dep_fp) : 1.0;
   printf("\nblast_radius: %d cases | dependents recall=%.3f precision=%.3f | "
          "dependencies tp=%d fp=%d fn=%d\n",
          cases, dep_recall, dep_prec, dic_tp, dic_fp, dic_fn);
   printf("delegate_graph: %d/%d cases correct\n", dg_ok, dg_cases);

   int pass = (dep_recall >= 0.999) && (dep_prec >= 0.8) && (dic_fn == 0) && (fail == 0) &&
              (dg_ok == dg_cases);
   printf("PASS BAR (dependents recall=1.0, precision>=0.8, no dependency miss, delegate 100%%): "
          "%s\n",
          pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
