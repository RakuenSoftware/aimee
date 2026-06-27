/* test_cross_repo_deps_orch.c: S4a orchestration over the sqlite shim. Unit-tests
 * the pure manifest parser, the graceful-empty path, and a seeded end-to-end
 * positive case (moonlight-qt -> moonlight-common-c via LiStartConnection). */
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

   /* but it WAS surfaced to the review queue. */
   xrepo_review_row_t rows[16];
   int rn = db2_cross_repo_review_list("moonlight-qt", "open", rows, 16, NULL);
   int found = 0;
   for (int i = 0; i < rn; i++)
      if (strcmp(rows[i].symbol, "AmbiguousThing") == 0)
         found = 1;
   assert(found);
   printf("ok\n");
}

int main(void)
{
   test_parse_module_id();
   db2_test_shim_open();
   test_empty_graceful();
   test_end_to_end();
   test_ambiguous_to_review();
   printf("cross_repo_deps_orch: all tests passed\n");
   return 0;
}
