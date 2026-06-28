/* test_cross_repo_acceptance.c: S8 end-to-end acceptance harness for the
 * cross-repo dependency graph, run over the sqlite shim. The pure resolver /
 * classifier strata are exhaustively unit-tested in test_cross_repo_deps.c; this
 * file drives the SAME enumerated cases through the real DB orchestration
 * (canonical_index_cross_repo_deps + the review queue) so the full pipeline is
 * validated, and it mirrors the live acceptance procedure S9 runs against the
 * .254 corpus (docs/proposals/pending/cross-repo-dependency-graph.md §9).
 *
 * Stratified positives + enumerated negatives (acceptance #2-#3, shim tier). Each
 * no-edge case cites the rule it exercises (crd_flush in db2/cross_repo_deps.c /
 * the pure suite in test_cross_repo_deps.c):
 *   - import-resolvable, trusted        -> HIGH   (LiStartConnection-style)
 *   - import-corroborated, UNTRUSTED caller -> capped MEDIUM (§0 caller cap)
 *   - UNTRUSTED definer (trusted caller imports it) -> NO edge (§0: an untrusted
 *                                                   repo can't originate an edge)
 *   - call-site-only, no corroboration  -> NO edge (no import route + lone definer
 *                                                   -> crd_flush sets no target)
 *   - bare-name / common collision      -> no edge (distinctiveness gate)
 *   - multi-definer, no corroboration   -> AMBIGUOUS -> review queue (no edge)
 *
 * Live-only gates (precision N=50, recall floors, p50/p95 latency) need the real
 * Postgres corpus and are measured in S9, not here. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../db2/cross_repo_deps.h"
#include "../db2/cross_repo_review.h"
#include "../db2/db2.h"
#include "../db2/db_postgres.h"

static void X(const char *sql)
{
   char err[256] = "";
   int rc = aimee_pg_exec(db2_conn(), sql, err, sizeof(err));
   if (rc != 0)
      fprintf(stderr, "seed failed: %s\n  sql: %s\n", err, sql);
   assert(rc == 0);
}

/* Add a project with explicit trust. */
static void mk_project(const char *name, const char *trust)
{
   char sql[256];
   snprintf(sql, sizeof(sql),
            "INSERT INTO projects (name, root, scanned_at, trust) VALUES ('%s','/%s','t','%s')",
            name, name, trust);
   X(sql);
}

/* Add a file to a project. */
static void mk_file(const char *project, const char *path)
{
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO files (project_id,path,scanned_at) SELECT id,'%s','t' FROM projects WHERE "
            "name='%s'",
            path, project);
   X(sql);
}

static void mk_def(const char *file, const char *name)
{
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO terms (file_id,name,kind) SELECT id,'%s','definition' FROM files WHERE "
            "path='%s'",
            name, file);
   X(sql);
}

static void mk_export(const char *file, const char *name)
{
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO file_exports (file_id,name) SELECT id,'%s' FROM files WHERE path='%s'",
            name, file);
   X(sql);
}

static void mk_import(const char *file, const char *header)
{
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO file_imports (file_id,name) SELECT id,'%s' FROM files WHERE path='%s'",
            header, file);
   X(sql);
}

static void mk_call(const char *file, const char *callee)
{
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO code_calls (file_id,callee) SELECT id,'%s' FROM files WHERE path='%s'",
            callee, file);
   X(sql);
}

/* Find the emitted edge to `definer` (NULL if none). */
static const xrepo_dep_edge_t *find_edge(const xrepo_dep_edge_t *edges, size_t n,
                                         const char *definer, const char *symbol)
{
   for (size_t i = 0; i < n; i++)
      if (strcmp(edges[i].definer_repo, definer) == 0 &&
          (!symbol || strcmp(edges[i].example_symbol, symbol) == 0))
         return &edges[i];
   return NULL;
}

/* The whole corpus, seeded once. `app` is a trusted 5-file caller; `ext` an
 * untrusted 5-file caller. Definer repos each export one distinctive symbol. */
static void seed_corpus(void)
{
   mk_project("app", "trusted");
   mk_project("ext", "untrusted");
   mk_project("lib-high", "trusted"); /* import-resolvable HIGH target */
   mk_project("lib-med", "trusted");  /* call-site-only, no corroboration -> no edge */
   mk_project("dup-a", "trusted");    /* multi-definer (AMBIGUOUS) */
   mk_project("dup-b", "trusted");

   for (int i = 0; i < 5; i++)
   {
      char p[32];
      snprintf(p, sizeof(p), "src/app%d.c", i);
      mk_file("app", p);
      snprintf(p, sizeof(p), "src/ext%d.c", i);
      mk_file("ext", p);
   }

   /* lib-high: defines + exports LiStartConnection in include/Limelight.h-style. */
   mk_file("lib-high", "include/Limelight.h");
   mk_file("lib-high", "src/connection.c");
   mk_def("src/connection.c", "LiStartConnection");
   mk_export("src/connection.c", "LiStartConnection");

   /* lib-med: defines a distinctive symbol, no header app imports. */
   mk_file("lib-med", "src/widget.c");
   mk_def("src/widget.c", "WidgetComputeLayout");

   /* dup-a / dup-b: both define AmbiguousThing (multi-definer, no corroboration). */
   mk_file("dup-a", "a.c");
   mk_file("dup-b", "b.c");
   mk_def("a.c", "AmbiguousThing");
   mk_def("b.c", "AmbiguousThing");

   /* app: imports Limelight.h + calls LiStartConnection (import-resolvable HIGH);
    * calls WidgetComputeLayout with NO import (call-site-only -> no edge); calls
    * AmbiguousThing (multi-definer AMBIGUOUS); calls a bare-name "init" that is
    * defined nowhere distinctive. Each call lands in 1 of 5 files (distinctive). */
   mk_import("src/app0.c", "Limelight.h");
   mk_call("src/app0.c", "LiStartConnection");
   mk_call("src/app1.c", "WidgetComputeLayout");
   mk_call("src/app2.c", "AmbiguousThing");
   mk_call("src/app3.c", "init"); /* bare name: short / non-distinctive */

   /* ext (untrusted): imports Limelight.h + calls LiStartConnection. Import
    * corroboration rooted in an untrusted caller must cap the edge at MEDIUM. */
   mk_import("src/ext0.c", "Limelight.h");
   mk_call("src/ext0.c", "LiStartConnection");

   /* lib-uexp (UNTRUSTED): defines + exports VendoredApiCall and ships its header,
    * and trusted `app` imports + calls it. With an identical TRUSTED definer this
    * is a HIGH edge (the import route corroborates); because lib-uexp is untrusted
    * it must originate NO edge at all (§0). */
   mk_project("lib-uexp", "untrusted");
   mk_file("lib-uexp", "include/Vendored.h");
   mk_file("lib-uexp", "u.c");
   mk_def("u.c", "VendoredApiCall");
   mk_export("u.c", "VendoredApiCall");
   mk_import("src/app4.c", "Vendored.h");
   mk_call("src/app4.c", "VendoredApiCall");

   /* H1 structural-edge routes (H0d) for the import-corroborated edges: app->lib-high
    * and the untrusted-caller ext->lib-high (capped MEDIUM). app->lib-uexp gets a
    * route too so the test proves the untrusted-DEFINER §0 rule (not the route gate)
    * is what blocks that edge. lib-med/dup-* have no import, hence no route. */
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('app','lib-high','import_header','medium','Limelight.h')");
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('ext','lib-high','import_header','medium','Limelight.h')");
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('app','lib-uexp','import_header','medium','Vendored.h')");
}

static void run(const char *caller, xrepo_dep_edge_t **edges, size_t *n)
{
   xrepo_deps_opts_t opts = {
       .direction = XREPO_DIR_OUT, .min_tier = XREPO_TIER_LOW, .include_review = 1};
   int trunc = 0;
   assert(canonical_index_cross_repo_deps(caller, &opts, edges, n, &trunc) == 0);
}

static void test_import_resolvable_high(void)
{
   printf("test_import_resolvable_high... ");
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   run("app", &edges, &n);
   const xrepo_dep_edge_t *e = find_edge(edges, n, "lib-high", "LiStartConnection");
   assert(e && e->tier == XREPO_TIER_HIGH && e->import_corroborated == 1);
   free(edges);
   printf("ok\n");
}

static void test_call_site_only_no_edge(void)
{
   printf("test_call_site_only_no_edge... ");
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   run("app", &edges, &n);
   /* WidgetComputeLayout: distinctive, single trusted definer, but `app` does NOT
    * import lib-med. Grounded in crd_flush (db2/cross_repo_deps.c): an edge gets a
    * target -- and is emitted -- ONLY via (a) an import route the caller has, or
    * (b) the dominant definer among MULTIPLE definers. A lone definer with no
    * caller import has neither route, so no edge of any tier. Core precision rule. */
   assert(find_edge(edges, n, "lib-med", "WidgetComputeLayout") == NULL);
   free(edges);
   printf("ok\n");
}

static void test_bare_name_no_edge(void)
{
   printf("test_bare_name_no_edge... ");
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   run("app", &edges, &n);
   /* "init" fails the distinctiveness gate (length < len_min and a ubiquitous
    * identifier), so xrepo_name_distinctive returns false and crd_flush never
    * emits it; cf. test_cross_repo_deps.c test_distinctiveness. No edge, any tier. */
   for (size_t i = 0; i < n; i++)
      assert(strcmp(edges[i].example_symbol, "init") != 0);
   free(edges);
   printf("ok\n");
}

static void test_multi_definer_ambiguous(void)
{
   printf("test_multi_definer_ambiguous... ");
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   run("app", &edges, &n);
   /* AmbiguousThing emits no edge ... */
   for (size_t i = 0; i < n; i++)
      assert(strcmp(edges[i].example_symbol, "AmbiguousThing") != 0);
   free(edges);
   /* ... and is NOT surfaced to review either: dup-a/dup-b both define it but `app`
    * has no structural route to either (no import), so under the H1 invariant it is
    * bare name noise, not a review-worthy cross-repo ambiguity. (The route-backed
    * ambiguity-IS-surfaced case is covered in test_cross_repo_deps_orch.) */
   xrepo_review_row_t rows[16];
   int rn = db2_cross_repo_review_list("app", "open", rows, 16, NULL);
   int found = 0;
   for (int i = 0; i < rn; i++)
      if (strcmp(rows[i].symbol, "AmbiguousThing") == 0)
         found = 1;
   assert(!found);
   printf("ok\n");
}

static void test_untrusted_import_capped_medium(void)
{
   printf("test_untrusted_import_capped_medium... ");
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   run("ext", &edges, &n);
   const xrepo_dep_edge_t *e = find_edge(edges, n, "lib-high", "LiStartConnection");
   /* §0 untrusted-CALLER cap: same import corroboration as `app`, but the caller is
    * untrusted -> capped at MEDIUM (HIGH requires a trusted-rooted signal); cf.
    * test_cross_repo_deps.c "Untrusted caller caps the import route at MEDIUM". */
   assert(e && e->tier == XREPO_TIER_MEDIUM);
   free(edges);
   printf("ok\n");
}

static void test_untrusted_definer_no_edge(void)
{
   printf("test_untrusted_definer_no_edge... ");
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   run("app", &edges, &n);
   /* §0 untrusted-DEFINER: `app` (trusted) imports lib-uexp's header and calls
    * VendoredApiCall, defined+exported ONLY by lib-uexp (untrusted). The trusted
    * caller's import would corroborate a trusted definer to HIGH (see the
    * lib-uexp=trusted probe), but an untrusted definer cannot originate a
    * cross-repo edge at all -- realizing "a planted/forged export in an untrusted
    * repo can neither create nor suppress an edge". Conservative (stricter than the
    * literal §0 export-cap-to-MEDIUM, applied here to the import route too). */
   assert(find_edge(edges, n, "lib-uexp", "VendoredApiCall") == NULL);
   free(edges);
   printf("ok\n");
}

int main(void)
{
   db2_test_shim_open();
   seed_corpus();
   test_import_resolvable_high();
   test_call_site_only_no_edge();
   test_bare_name_no_edge();
   test_multi_definer_ambiguous();
   test_untrusted_import_capped_medium();
   test_untrusted_definer_no_edge();
   printf("cross_repo_acceptance: all tests passed\n");
   return 0;
}
