/* test_cross_repo_deps_orch.c: S4a orchestration over the sqlite shim. Unit-tests
 * the pure manifest parser, the graceful-empty path, and a seeded end-to-end
 * positive case (moonlight-qt -> moonlight-common-c via LiStartConnection). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../db2/cross_repo_deps.h"
#include "../db2/cross_repo_identity.h"
#include "../db2/cross_repo_review.h"
#include "../db2/cross_repo_route.h"
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

static void test_parse_module_id(void)
{
   printf("test_parse_module_id... ");
   char out[128];
   assert(xrepo_parse_module_id("go.mod", "module example.com/foo\n\ngo 1.21\n", out, sizeof(out)));
   assert(strcmp(out, "example.com/foo") == 0);
   assert(xrepo_parse_module_id(
       "Cargo.toml", "[package]\nname = \"serde_helpers\"\nversion=\"1\"\n", out, sizeof(out)));
   assert(strcmp(out, "serde_helpers") == 0);
   assert(xrepo_parse_module_id("package.json", "{\n  \"name\": \"@scope/widgets\",\n  \"v\":1}\n",
                                out, sizeof(out)));
   assert(strcmp(out, "@scope/widgets") == 0);
   assert(xrepo_parse_module_id("pyproject.toml", "[project]\nname = \"requests_ext\"\n", out,
                                sizeof(out)));
   assert(strcmp(out, "requests_ext") == 0);
   assert(xrepo_parse_module_id("go.mod", "no module here\n", out, sizeof(out)) == 0);
   printf("ok\n");
}

static void test_empty_graceful(void)
{
   printf("test_empty_graceful... ");
   xrepo_deps_opts_t opts = {.direction = XREPO_DIR_OUT, .min_tier = XREPO_TIER_MEDIUM};
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 99;
   int trunc = 9;
   int rc = canonical_index_cross_repo_deps("nope", &opts, &edges, &n, &trunc);
   assert(rc == 0 && n == 0 && edges == NULL && trunc == 0);
   /* NULL args -> -1, never crash. */
   assert(canonical_index_cross_repo_deps(NULL, &opts, &edges, &n, &trunc) == -1);
   printf("ok\n");
}

/* Seed moonlight-qt (A, 5 files) -> moonlight-common-c (B): A imports Limelight.h
 * and calls LiStartConnection in 1 of its 5 files; B defines + exports it and
 * indexes include/Limelight.h. Import route + distinctive -> HIGH edge. */
static void seed_moonlight(void)
{
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES "
     "('moonlight-qt','/q','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES "
     "('moonlight-common-c','/c','t','trusted')");
   /* A: 5 files so the call lands in 20% of files (distinctive, not local-method). */
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'src/main.cpp','t' FROM projects "
     "WHERE name='moonlight-qt'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'src/a.cpp','t' FROM projects WHERE "
     "name='moonlight-qt'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'src/b.cpp','t' FROM projects WHERE "
     "name='moonlight-qt'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'src/c.cpp','t' FROM projects WHERE "
     "name='moonlight-qt'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'src/d.cpp','t' FROM projects WHERE "
     "name='moonlight-qt'");
   /* B: header (for include resolution) + def file. */
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'include/Limelight.h','t' FROM "
     "projects WHERE name='moonlight-common-c'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'src/connection.c','t' FROM "
     "projects WHERE name='moonlight-common-c'");
   /* B defines + exports LiStartConnection. */
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'LiStartConnection','definition' FROM files "
     "WHERE path='src/connection.c'");
   X("INSERT INTO file_exports (file_id,name) SELECT id,'LiStartConnection' FROM files WHERE "
     "path='src/connection.c'");
   /* A imports the header + calls the symbol in one file. */
   X("INSERT INTO file_imports (file_id,name) SELECT id,'Limelight.h' FROM files WHERE "
     "path='src/main.cpp'");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'LiStartConnection' FROM files WHERE "
     "path='src/main.cpp'");
   /* H1 structural-edge gate: the precomputed inter-repo route (H0d) that makes
    * this a real dependency rather than a bare name match. Without it the gate
    * holds the edge at LOW-unresolved (not emitted). */
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('moonlight-qt','moonlight-common-c','import_header','medium','Limelight.h')");
}

static void test_end_to_end(void)
{
   printf("test_end_to_end... ");
   seed_moonlight();
   xrepo_deps_opts_t opts = {.direction = XREPO_DIR_OUT, .min_tier = XREPO_TIER_MEDIUM};
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   int trunc = 0;
   int rc = canonical_index_cross_repo_deps("moonlight-qt", &opts, &edges, &n, &trunc);
   assert(rc == 0);
   /* exactly one edge: moonlight-qt -> moonlight-common-c, import-corroborated HIGH. */
   assert(n == 1);
   assert(strcmp(edges[0].caller_repo, "moonlight-qt") == 0);
   assert(strcmp(edges[0].definer_repo, "moonlight-common-c") == 0);
   assert(edges[0].tier == XREPO_TIER_HIGH);
   assert(edges[0].import_corroborated == 1);
   assert(strcmp(edges[0].example_symbol, "LiStartConnection") == 0);
   assert(edges[0].resolver_version == XREPO_RESOLVER_VERSION);
   free(edges);

   /* min_tier HIGH still includes it; a non-existent project yields nothing. */
   opts.min_tier = XREPO_TIER_HIGH;
   rc = canonical_index_cross_repo_deps("moonlight-qt", &opts, &edges, &n, &trunc);
   assert(rc == 0 && n == 1);
   free(edges);
   printf("ok\n");
}

/* A symbol defined in two trusted repos that A imports NEITHER of, called in A:
 * multi-definer without corroboration -> AMBIGUOUS -> review queue (no edge). */
static void test_ambiguous_to_review(void)
{
   printf("test_ambiguous_to_review... ");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('lib-x','/x','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('lib-y','/y','t','trusted')");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'x.c','t' FROM projects WHERE "
     "name='lib-x'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'y.c','t' FROM projects WHERE "
     "name='lib-y'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'AmbiguousThing','definition' FROM files "
     "WHERE path='x.c'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'AmbiguousThing','definition' FROM files "
     "WHERE path='y.c'");
   /* called in 1 of moonlight-qt's 5 files (20% -> distinctive); A imports neither. */
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'AmbiguousThing' FROM files WHERE "
     "path='src/a.cpp'");
   /* H1: genuine route-backed ambiguity — A has a structural route to BOTH definers,
    * so the multi-definer collision is real (which one?) and worth review (vs the
    * no-route name-noise case, asserted not-surfaced in acceptance). */
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('moonlight-qt','lib-x','import_header','medium','ambig.h')");
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('moonlight-qt','lib-y','import_header','medium','ambig.h')");

   xrepo_deps_opts_t opts = {
       .direction = XREPO_DIR_OUT, .min_tier = XREPO_TIER_MEDIUM, .include_review = 1};
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   int trunc = 0;
   assert(canonical_index_cross_repo_deps("moonlight-qt", &opts, &edges, &n, &trunc) == 0);
   /* still just the moonlight edge; AmbiguousThing did NOT emit an edge. */
   for (size_t i = 0; i < n; i++)
      assert(strcmp(edges[i].example_symbol, "AmbiguousThing") != 0);
   free(edges);

   /* but it WAS surfaced to the review queue (route-backed ambiguity). */
   xrepo_review_row_t rows[16];
   int rn = db2_cross_repo_review_list("moonlight-qt", "open", rows, 16, NULL);
   int found = 0;
   for (int i = 0; i < rn; i++)
      if (strcmp(rows[i].symbol, "AmbiguousThing") == 0)
         found = 1;
   assert(found);
   printf("ok\n");
}

/* H1 structural-edge gate in isolation: an otherwise-HIGH candidate (distinctive,
 * single trusted definer, import + call) is NOT emitted while no cross_repo_route
 * exists; inserting the route flips it to an edge. The route is the ONLY variable. */
static void test_structural_edge_gate(void)
{
   printf("test_structural_edge_gate... ");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES "
     "('gate-app','/ga','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES "
     "('gate-lib','/gl','t','trusted')");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'gsrc/0.cpp','t' FROM projects "
     "WHERE name='gate-app'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'gsrc/1.cpp','t' FROM projects "
     "WHERE name='gate-app'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'gsrc/2.cpp','t' FROM projects "
     "WHERE name='gate-app'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'gsrc/3.cpp','t' FROM projects "
     "WHERE name='gate-app'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'gsrc/4.cpp','t' FROM projects "
     "WHERE name='gate-app'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'include/Gate.h','t' FROM projects "
     "WHERE name='gate-lib'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'src/gate.c','t' FROM projects "
     "WHERE name='gate-lib'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'GateDistinctiveSym','definition' FROM files "
     "WHERE path='src/gate.c'");
   X("INSERT INTO file_exports (file_id,name) SELECT id,'GateDistinctiveSym' FROM files WHERE "
     "path='src/gate.c'");
   X("INSERT INTO file_imports (file_id,name) SELECT id,'Gate.h' FROM files WHERE "
     "path='gsrc/0.cpp'");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'GateDistinctiveSym' FROM files WHERE "
     "path='gsrc/0.cpp'");

   xrepo_deps_opts_t opts = {.direction = XREPO_DIR_OUT, .min_tier = XREPO_TIER_MEDIUM};
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   int trunc = 0;

   /* No route yet: the candidate is structurally unbacked -> no gate-lib edge. */
   assert(canonical_index_cross_repo_deps("gate-app", &opts, &edges, &n, &trunc) == 0);
   for (size_t i = 0; i < n; i++)
      assert(strcmp(edges[i].definer_repo, "gate-lib") != 0);
   free(edges);
   edges = NULL;

   /* Insert the structural route -> the same candidate now emits an edge. */
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('gate-app','gate-lib','import_header','medium','Gate.h')");
   assert(canonical_index_cross_repo_deps("gate-app", &opts, &edges, &n, &trunc) == 0);
   int found = 0;
   for (size_t i = 0; i < n; i++)
      if (strcmp(edges[i].definer_repo, "gate-lib") == 0)
         found = 1;
   assert(found);
   free(edges);
   printf("ok\n");
}

/* Cold-start end-to-end: the curator drain's rebuild path (db2_cross_repo_rebuild_
 * identities + rebuild_routes) populates cross_repo_route from raw file_imports/
 * files with NO hand-seeded route, and the resolver's gate then accepts the edge.
 * Proves the rebuild produces gate-acceptable routes (the cold-start backfill in
 * kb_curator_drain.c) and that an empty route table drops the edge (the cliff the
 * backfill fixes). Header-route variant: C caller #include -> non-vendored
 * definer file basename. */
static void test_cold_start_rebuild_to_gate(void)
{
   printf("test_cold_start_rebuild_to_gate... ");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('cs-app','/ca','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('cs-lib','/cl','t','trusted')");
   for (int i = 0; i < 5; i++)
   {
      char sql[256];
      snprintf(
          sql, sizeof(sql),
          "INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT id,'src/%d.c',"
          "'t','c',0 FROM projects WHERE name='cs-app'",
          i);
      X(sql);
   }
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'include/CsWidget.h','t','c',0 FROM projects WHERE name='cs-lib'");
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'src/cs.c','t','c',0 "
     "FROM projects WHERE name='cs-lib'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'CsDistinctiveSym','definition' FROM files "
     "WHERE path='src/cs.c'");
   X("INSERT INTO file_exports (file_id,name) SELECT id,'CsDistinctiveSym' FROM files WHERE "
     "path='src/cs.c'");
   X("INSERT INTO file_imports (file_id,name) SELECT id,'CsWidget.h' FROM files WHERE "
     "path='src/0.c' AND project_id=(SELECT id FROM projects WHERE name='cs-app')");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'CsDistinctiveSym' FROM files WHERE "
     "path='src/0.c' AND project_id=(SELECT id FROM projects WHERE name='cs-app')");

   xrepo_deps_opts_t opts = {.direction = XREPO_DIR_OUT, .min_tier = XREPO_TIER_MEDIUM};
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   int trunc = 0;

   /* Empty route table (cold start, no backfill): the cliff — edge dropped. */
   assert(canonical_index_cross_repo_deps("cs-app", &opts, &edges, &n, &trunc) == 0);
   for (size_t i = 0; i < n; i++)
      assert(strcmp(edges[i].definer_repo, "cs-lib") != 0);
   free(edges);
   edges = NULL;

   /* The drain's rebuild sequence (the cold-start backfill) populates routes. */
   assert(db2_cross_repo_rebuild_identities() >= 0);
   assert(db2_cross_repo_rebuild_routes() >= 1);

   /* Same resolver call now emits the edge — the real route satisfies the gate. */
   assert(canonical_index_cross_repo_deps("cs-app", &opts, &edges, &n, &trunc) == 0);
   int found = 0;
   for (size_t i = 0; i < n; i++)
      if (strcmp(edges[i].definer_repo, "cs-lib") == 0)
         found = 1;
   assert(found);
   free(edges);
   printf("ok\n");
}

/* §4 vendor canonical-preference: (a) when a symbol is defined in BOTH a
 * non-vendored repo and a vendored copy, the vendored candidate is dropped so the
 * pair resolves to the canonical repo (not a false AMBIGUOUS); (b) a lone vendored
 * definer with a route is still emitted (header-only-library exemption). */
static void test_vendor_canonical_preference(void)
{
   printf("test_vendor_canonical_preference... ");
   /* (a) canonical vs vendored duplicate. */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('vc-app','/va','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES "
     "('vc-canon','/vc','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('vc-vend','/vv','t','trusted')");
   for (int i = 0; i < 5; i++)
   {
      char sql[256];
      snprintf(sql, sizeof(sql),
               "INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
               "id,'src/%d.c','t','c',0 FROM projects WHERE name='vc-app'",
               i);
      X(sql);
   }
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'include/Canon.h','t','c',0 FROM projects WHERE name='vc-canon'");
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT id,'src/canon.c','t',"
     "'c',0 FROM projects WHERE name='vc-canon'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'VendorCanonSym','definition' FROM files "
     "WHERE path='src/canon.c'");
   X("INSERT INTO file_exports (file_id,name) SELECT id,'VendorCanonSym' FROM files WHERE "
     "path='src/canon.c'");
   /* vc-vend defines the SAME symbol but only in a vendored file. */
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'third_party/dup.c','t','c',1 FROM projects WHERE name='vc-vend'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'VendorCanonSym','definition' FROM files "
     "WHERE path='third_party/dup.c'");
   X("INSERT INTO file_imports (file_id,name) SELECT id,'Canon.h' FROM files WHERE path='src/0.c' "
     "AND project_id=(SELECT id FROM projects WHERE name='vc-app')");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'VendorCanonSym' FROM files WHERE "
     "path='src/0.c' AND project_id=(SELECT id FROM projects WHERE name='vc-app')");
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('vc-app','vc-canon','import_header','medium','Canon.h')");

   xrepo_deps_opts_t opts = {
       .direction = XREPO_DIR_OUT, .min_tier = XREPO_TIER_MEDIUM, .include_review = 1};
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   int trunc = 0;
   assert(canonical_index_cross_repo_deps("vc-app", &opts, &edges, &n, &trunc) == 0);
   int canon = 0, vend = 0;
   for (size_t i = 0; i < n; i++)
   {
      if (strcmp(edges[i].definer_repo, "vc-canon") == 0)
         canon = 1;
      if (strcmp(edges[i].definer_repo, "vc-vend") == 0)
         vend = 1;
   }
   /* vendored duplicate dropped -> resolves to canonical, NOT AMBIGUOUS, no vend edge. */
   assert(canon && !vend);
   free(edges);
   edges = NULL;
   /* VendorCanonSym is NOT in the review queue (not ambiguous after canonical pass). */
   xrepo_review_row_t rows[16];
   int rn = db2_cross_repo_review_list("vc-app", "open", rows, 16, NULL);
   for (int i = 0; i < rn; i++)
      assert(strcmp(rows[i].symbol, "VendorCanonSym") != 0);

   /* (b) lone vendored definer + route -> still emitted (header-only exemption). */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('vo-app','/oa','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('vo-lib','/ol','t','trusted')");
   for (int i = 0; i < 5; i++)
   {
      char sql[256];
      snprintf(sql, sizeof(sql),
               "INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
               "id,'s/%d.c','t','c',0 FROM projects WHERE name='vo-app'",
               i);
      X(sql);
   }
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'third_party/Vo.h','t','c',1 FROM projects WHERE name='vo-lib'");
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'third_party/vo.c','t','c',1 FROM projects WHERE name='vo-lib'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'VendoredOnlySym','definition' FROM files "
     "WHERE path='third_party/vo.c'");
   X("INSERT INTO file_exports (file_id,name) SELECT id,'VendoredOnlySym' FROM files WHERE "
     "path='third_party/vo.c'");
   X("INSERT INTO file_imports (file_id,name) SELECT id,'Vo.h' FROM files WHERE path='s/0.c' "
     "AND project_id=(SELECT id FROM projects WHERE name='vo-app')");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'VendoredOnlySym' FROM files WHERE "
     "path='s/0.c' AND project_id=(SELECT id FROM projects WHERE name='vo-app')");
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('vo-app','vo-lib','import_header','medium','Vo.h')");
   assert(canonical_index_cross_repo_deps("vo-app", &opts, &edges, &n, &trunc) == 0);
   int found = 0;
   for (size_t i = 0; i < n; i++)
      if (strcmp(edges[i].definer_repo, "vo-lib") == 0)
         found = 1;
   assert(found); /* lone vendored definer kept (exemption) */
   free(edges);
   printf("ok\n");
}

/* §4 canonical-preference edge cases: (c) a single repo with BOTH a vendored and a
 * non-vendored def of one symbol is treated as CANONICAL (MIN(vendored)=0), so a
 * competing pure-vendored repo is dropped; (d) no-route-to-canonical — when the
 * non-vendored definer is NOT route-reachable but a vendored copy IS, the vendored
 * candidate is KEPT and emits (the recall fix: don't drop the copy the caller
 * actually uses). */
static void test_vendor_canonical_edge_cases(void)
{
   printf("test_vendor_canonical_edge_cases... ");
   /* (c) mixed repo (canonical) vs pure-vendored repo. */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('mr-app','/ma','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('mr-mix','/mm','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('mr-vend','/mv','t','trusted')");
   for (int i = 0; i < 5; i++)
   {
      char sql[256];
      snprintf(sql, sizeof(sql),
               "INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
               "id,'m/%d.c','t','c',0 FROM projects WHERE name='mr-app'",
               i);
      X(sql);
   }
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'include/Mix.h','t','c',0 FROM projects WHERE name='mr-mix'");
   /* mr-mix defines MixedSym in BOTH a canonical and a vendored file -> MIN=0. */
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'src/m.c','t','c',0 "
     "FROM projects WHERE name='mr-mix'");
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'third_party/m2.c','t','c',1 FROM projects WHERE name='mr-mix'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'MixedSym','definition' FROM files WHERE "
     "path='src/m.c'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'MixedSym','definition' FROM files WHERE "
     "path='third_party/m2.c'");
   X("INSERT INTO file_exports (file_id,name) SELECT id,'MixedSym' FROM files WHERE "
     "path='src/m.c'");
   /* mr-vend is a pure-vendored duplicate. */
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'third_party/mv.c','t','c',1 FROM projects WHERE name='mr-vend'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'MixedSym','definition' FROM files WHERE "
     "path='third_party/mv.c'");
   X("INSERT INTO file_imports (file_id,name) SELECT id,'Mix.h' FROM files WHERE path='m/0.c' AND "
     "project_id=(SELECT id FROM projects WHERE name='mr-app')");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'MixedSym' FROM files WHERE path='m/0.c' "
     "AND "
     "project_id=(SELECT id FROM projects WHERE name='mr-app')");
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('mr-app','mr-mix','import_header','medium','Mix.h')");

   xrepo_deps_opts_t opts = {
       .direction = XREPO_DIR_OUT, .min_tier = XREPO_TIER_MEDIUM, .include_review = 1};
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   int trunc = 0;
   assert(canonical_index_cross_repo_deps("mr-app", &opts, &edges, &n, &trunc) == 0);
   int mix = 0, mvend = 0;
   for (size_t i = 0; i < n; i++)
   {
      if (strcmp(edges[i].definer_repo, "mr-mix") == 0)
         mix = 1;
      if (strcmp(edges[i].definer_repo, "mr-vend") == 0)
         mvend = 1;
   }
   assert(mix && !mvend); /* mixed repo = canonical (survives); pure-vendored dropped */
   free(edges);
   edges = NULL;

   /* (d) no-route-to-canonical: canonical unreachable, vendored copy reachable. */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('nr-app','/na','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES "
     "('nr-canon','/nc','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('nr-vend','/nv','t','trusted')");
   for (int i = 0; i < 5; i++)
   {
      char sql[256];
      snprintf(sql, sizeof(sql),
               "INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
               "id,'n/%d.c','t','c',0 FROM projects WHERE name='nr-app'",
               i);
      X(sql);
   }
   /* nr-canon: non-vendored def, but NO route from nr-app. */
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'src/n.c','t','c',0 "
     "FROM projects WHERE name='nr-canon'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'NoRouteCanonSym','definition' FROM files "
     "WHERE path='src/n.c'");
   /* nr-vend: vendored def + header the app imports + a route. */
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'third_party/Nv.h','t','c',1 FROM projects WHERE name='nr-vend'");
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'third_party/nv.c','t','c',1 FROM projects WHERE name='nr-vend'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'NoRouteCanonSym','definition' FROM files "
     "WHERE path='third_party/nv.c'");
   X("INSERT INTO file_exports (file_id,name) SELECT id,'NoRouteCanonSym' FROM files WHERE "
     "path='third_party/nv.c'");
   X("INSERT INTO file_imports (file_id,name) SELECT id,'Nv.h' FROM files WHERE path='n/0.c' AND "
     "project_id=(SELECT id FROM projects WHERE name='nr-app')");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'NoRouteCanonSym' FROM files WHERE "
     "path='n/0.c' AND project_id=(SELECT id FROM projects WHERE name='nr-app')");
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('nr-app','nr-vend','import_header','medium','Nv.h')");
   assert(canonical_index_cross_repo_deps("nr-app", &opts, &edges, &n, &trunc) == 0);
   int canon = 0, vend = 0;
   for (size_t i = 0; i < n; i++)
   {
      if (strcmp(edges[i].definer_repo, "nr-canon") == 0)
         canon = 1;
      if (strcmp(edges[i].definer_repo, "nr-vend") == 0)
         vend = 1;
   }
   /* vendored copy kept + emitted (canonical was unreachable); canonical not emitted
    * (no route). Without the route-aware drop this edge would be lost (recall bug). */
   assert(vend && !canon);
   free(edges);
   printf("ok\n");
}

/* Seed one definer repo `lib` exporting/defining `sym` (def_kind) reachable from
 * ke-app via #include of `hdrbase` (in include/ or third_party/ when vendored) +
 * a route, with the call placed in ke-app's `appfile`. */
static void mk_ke_def(const char *lib, const char *hdrbase, const char *sym, const char *def_kind,
                      int exported, int vendored, const char *appfile)
{
   char s[640];
   const char *dir = vendored ? "third_party" : "include";
   snprintf(s, sizeof(s),
            "INSERT INTO projects (name, root, scanned_at, trust) VALUES ('%s','/k','t','trusted')",
            lib);
   X(s);
   snprintf(s, sizeof(s),
            "INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
            "id,'%s/%s','t','c',%d FROM projects WHERE name='%s'",
            dir, hdrbase, vendored, lib);
   X(s);
   snprintf(s, sizeof(s),
            "INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
            "id,'%s/def.c','t','c',%d FROM projects WHERE name='%s'",
            dir, vendored, lib);
   X(s);
   snprintf(s, sizeof(s),
            "INSERT INTO terms (file_id,name,kind,def_kind) SELECT id,'%s','definition','%s' FROM "
            "files WHERE path='%s/def.c' AND project_id=(SELECT id FROM projects WHERE name='%s')",
            sym, def_kind, dir, lib);
   X(s);
   if (exported)
   {
      snprintf(s, sizeof(s),
               "INSERT INTO file_exports (file_id,name) SELECT id,'%s' FROM files WHERE "
               "path='%s/def.c' AND project_id=(SELECT id FROM projects WHERE name='%s')",
               sym, dir, lib);
      X(s);
   }
   snprintf(s, sizeof(s),
            "INSERT INTO file_imports (file_id,name) SELECT id,'%s' FROM files WHERE path='%s' AND "
            "project_id=(SELECT id FROM projects WHERE name='ke-app')",
            hdrbase, appfile);
   X(s);
   snprintf(s, sizeof(s),
            "INSERT INTO code_calls (file_id,callee) SELECT id,'%s' FROM files WHERE path='%s' AND "
            "project_id=(SELECT id FROM projects WHERE name='ke-app')",
            sym, appfile);
   X(s);
   snprintf(
       s, sizeof(s),
       "INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
       "VALUES ('ke-app','%s','import_header','medium','%s')",
       lib, hdrbase);
   X(s);
}

/* §5 symbol-kind eligibility + §6 export-booster + §4 lone-vendored ceiling: an
 * import-corroborated edge is HIGH for a function or an EXPORTED macro, but capped
 * at MEDIUM for a non-exported macro (SDK-style §5) and for a lone vendored definer
 * (§4 ceiling) even when exported. */
static void test_kind_eligibility_and_vendored_ceiling(void)
{
   printf("test_kind_eligibility_and_vendored_ceiling... ");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('ke-app','/ka','t','trusted')");
   for (int i = 0; i < 5; i++)
   {
      char s[256];
      snprintf(s, sizeof(s),
               "INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
               "id,'k/%d.c','t','c',0 FROM projects WHERE name='ke-app'",
               i);
      X(s);
   }
   mk_ke_def("ke-func", "KeFunc.h", "KeFuncSym", "function", 0, 0, "k/0.c");  /* HIGH */
   mk_ke_def("ke-macro", "KeMacro.h", "KeMacroSym", "macro", 0, 0, "k/1.c");  /* §5 -> MEDIUM */
   mk_ke_def("ke-emac", "KeEmac.h", "KeExpMacroSym", "macro", 1, 0, "k/2.c"); /* exported -> HIGH */
   mk_ke_def("ke-vend", "KeVend.h", "KeVendSym", "function", 1, 1,
             "k/3.c"); /* §4 ceiling -> MEDIUM */

   xrepo_deps_opts_t opts = {.direction = XREPO_DIR_OUT, .min_tier = XREPO_TIER_LOW};
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   int trunc = 0;
   assert(canonical_index_cross_repo_deps("ke-app", &opts, &edges, &n, &trunc) == 0);
   int seen_func = 0, seen_macro = 0, seen_emac = 0, seen_vend = 0;
   for (size_t i = 0; i < n; i++)
   {
      const char *d = edges[i].definer_repo;
      if (strcmp(d, "ke-func") == 0)
      {
         seen_func = 1;
         assert(edges[i].tier == XREPO_TIER_HIGH); /* function + import -> HIGH */
      }
      else if (strcmp(d, "ke-macro") == 0)
      {
         seen_macro = 1;
         assert(edges[i].tier == XREPO_TIER_MEDIUM); /* §5: non-exported macro capped */
      }
      else if (strcmp(d, "ke-emac") == 0)
      {
         seen_emac = 1;
         assert(edges[i].tier == XREPO_TIER_HIGH); /* §6: exported macro allowed HIGH */
      }
      else if (strcmp(d, "ke-vend") == 0)
      {
         seen_vend = 1;
         assert(edges[i].tier == XREPO_TIER_MEDIUM); /* §4 ceiling: lone vendored capped */
      }
   }
   assert(seen_func && seen_macro && seen_emac && seen_vend);
   free(edges);
   printf("ok\n");
}

/* R2c: build-declared deps merge into the deps output as a separate evidence class.
 * build-only (no symbol) -> MEDIUM build_declared (high-parse) / dropped at MEDIUM
 * min_tier (low-parse); build + an existing symbol edge -> both, promoted to HIGH. */
static void test_build_declared_merge(void)
{
   printf("test_build_declared_merge... ");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('bdo-app','/bo','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('bdo-lib','/bl','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('bdo-low','/bw','t','trusted')");
   /* build-only high-parse -> MEDIUM build_declared; low-parse -> LOW (excluded at MEDIUM). */
   X("INSERT INTO cross_repo_build_dep (caller_project,definer_project,build_kind,parse_confidence,"
     "evidence) VALUES ('bdo-app','bdo-lib','fetchcontent','high','https://h/o/bdo-lib.git')");
   X("INSERT INTO cross_repo_build_dep (caller_project,definer_project,build_kind,parse_confidence,"
     "evidence) VALUES ('bdo-app','bdo-low','submodule','low','${VAR}/bdo-low.git')");

   xrepo_deps_opts_t opts = {.direction = XREPO_DIR_OUT, .min_tier = XREPO_TIER_MEDIUM};
   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   int trunc = 0;
   assert(canonical_index_cross_repo_deps("bdo-app", &opts, &edges, &n, &trunc) == 0);
   int lib = 0, low = 0;
   for (size_t i = 0; i < n; i++)
   {
      if (strcmp(edges[i].definer_repo, "bdo-lib") == 0)
      {
         lib = 1;
         assert(edges[i].tier == XREPO_TIER_MEDIUM);
         assert(strcmp(edges[i].evidence_type, "build_declared") == 0);
         assert(strcmp(edges[i].build_kind, "fetchcontent") == 0);
      }
      if (strcmp(edges[i].definer_repo, "bdo-low") == 0)
         low = 1;
   }
   assert(lib && !low); /* high-parse emitted MEDIUM; low-parse excluded at min_tier MEDIUM */
   free(edges);

   /* both: seed a self-contained symbol-resolved edge (bdb-app -> bdb-lib via a
    * distinctive imported symbol, like seed_moonlight) so the merge has a symbol
    * edge to fold into; adding a high-parse build dep marks it evidence_type=both
    * and promotes the tier to HIGH. (Self-contained rather than reusing moonlight,
    * whose seeded edge does not survive intervening tests' shared-shim state.) */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('bdb-app','/ba','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('bdb-lib','/bb','t','trusted')");
   for (int i = 0; i < 5; i++)
   {
      char sql[160];
      snprintf(sql, sizeof(sql),
               "INSERT INTO files (project_id,path,scanned_at) SELECT id,'src/f%d.cpp','t' FROM "
               "projects WHERE name='bdb-app'",
               i);
      X(sql);
   }
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'include/Bdb.h','t' FROM projects "
     "WHERE name='bdb-lib'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'src/bdb.c','t' FROM projects WHERE "
     "name='bdb-lib'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'BdbConnectSession','definition' FROM files "
     "WHERE path='src/bdb.c'");
   X("INSERT INTO file_exports (file_id,name) SELECT id,'BdbConnectSession' FROM files WHERE "
     "path='src/bdb.c'");
   X("INSERT INTO file_imports (file_id,name) SELECT id,'Bdb.h' FROM files WHERE "
     "path='src/f0.cpp'");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'BdbConnectSession' FROM files WHERE "
     "path='src/f0.cpp'");
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('bdb-app','bdb-lib','import_header','medium','Bdb.h')");
   X("INSERT INTO cross_repo_build_dep (caller_project,definer_project,build_kind,parse_confidence,"
     "evidence) VALUES ('bdb-app','bdb-lib','submodule','high','https://h/o/bdb-lib.git')");
   edges = NULL;
   assert(canonical_index_cross_repo_deps("bdb-app", &opts, &edges, &n, &trunc) == 0);
   int found = 0;
   for (size_t i = 0; i < n; i++)
      if (strcmp(edges[i].definer_repo, "bdb-lib") == 0)
      {
         found = 1;
         assert(edges[i].tier == XREPO_TIER_HIGH);
         assert(strcmp(edges[i].evidence_type, "both") == 0);
         assert(strcmp(edges[i].build_kind, "submodule") == 0);
      }
   assert(found);
   free(edges);

   /* order-independence regression: a symbol-bearing pair (bdo2-app -> bdo2-lib)
    * with a low-parse build row inserted BEFORE a high-parse one must still reach
    * HIGH (the high-parse row promotes regardless of merge-iteration order). */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES "
     "('bdo2-app','/o2a','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES "
     "('bdo2-lib','/o2b','t','trusted')");
   for (int i = 0; i < 5; i++)
   {
      char sql[160];
      snprintf(sql, sizeof(sql),
               "INSERT INTO files (project_id,path,scanned_at) SELECT id,'src/g%d.cpp','t' FROM "
               "projects WHERE name='bdo2-app'",
               i);
      X(sql);
   }
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'include/Bdo2.h','t' FROM projects "
     "WHERE name='bdo2-lib'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'src/bdo2.c','t' FROM projects "
     "WHERE "
     "name='bdo2-lib'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'Bdo2OpenChannel','definition' FROM files "
     "WHERE path='src/bdo2.c'");
   X("INSERT INTO file_exports (file_id,name) SELECT id,'Bdo2OpenChannel' FROM files WHERE "
     "path='src/bdo2.c'");
   X("INSERT INTO file_imports (file_id,name) SELECT id,'Bdo2.h' FROM files WHERE "
     "path='src/g0.cpp'");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'Bdo2OpenChannel' FROM files WHERE "
     "path='src/g0.cpp'");
   X("INSERT INTO cross_repo_route (caller_project,definer_project,kind,confidence,evidence) "
     "VALUES ('bdo2-app','bdo2-lib','import_header','medium','Bdo2.h')");
   /* low-parse FIRST, high-parse SECOND (distinct build_kind so both rows persist). */
   X("INSERT INTO cross_repo_build_dep (caller_project,definer_project,build_kind,parse_confidence,"
     "evidence) VALUES ('bdo2-app','bdo2-lib','fetchcontent','low','${VAR}/bdo2-lib.git')");
   X("INSERT INTO cross_repo_build_dep (caller_project,definer_project,build_kind,parse_confidence,"
     "evidence) VALUES ('bdo2-app','bdo2-lib','submodule','high','https://h/o/bdo2-lib.git')");
   edges = NULL;
   assert(canonical_index_cross_repo_deps("bdo2-app", &opts, &edges, &n, &trunc) == 0);
   found = 0;
   for (size_t i = 0; i < n; i++)
      if (strcmp(edges[i].definer_repo, "bdo2-lib") == 0)
      {
         found = 1;
         assert(edges[i].tier == XREPO_TIER_HIGH);
         assert(strcmp(edges[i].evidence_type, "both") == 0);
         /* ORDER BY parse_confidence DESC -> the high-parse 'submodule' row wins
          * build_kind over the earlier-inserted low-parse 'fetchcontent' row. */
         assert(strcmp(edges[i].build_kind, "submodule") == 0);
      }
   assert(found);
   free(edges);
   printf("ok\n");
}

int main(void)
{
   test_parse_module_id();
   db2_test_shim_open();
   test_empty_graceful();
   test_end_to_end();
   test_ambiguous_to_review();
   test_structural_edge_gate();
   test_cold_start_rebuild_to_gate();
   test_vendor_canonical_preference();
   test_vendor_canonical_edge_cases();
   test_kind_eligibility_and_vendored_ceiling();
   test_build_declared_merge();
   printf("cross_repo_deps_orch: all tests passed\n");
   return 0;
}
