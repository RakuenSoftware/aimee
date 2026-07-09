/* test_delegate_dispatch_reliability.c: unit tests for delegate dispatch
 * reliability improvements (Phase 2):
 *   1. delegate_extract_named_paths — multi-file scope detection
 *   2. delegate_inject_code_context — context injection via code index */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cmd_agent_delegate_impl.h"
#include "index.h"

/* ── stub: kb_client_index_code_search ──────────────────────────────────── */

static int g_stub_hit_count = 0;
static code_search_hit_t g_stub_hits[6];

int kb_client_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max)
{
   (void)query;
   (void)project;
   int n = g_stub_hit_count < max ? g_stub_hit_count : max;
   for (int i = 0; i < n; i++)
      out[i] = g_stub_hits[i];
   return n;
}

static void stub_hits_reset(void)
{
   g_stub_hit_count = 0;
   memset(g_stub_hits, 0, sizeof(g_stub_hits));
}

static void stub_hits_add(const char *file_path, const char *snippet)
{
   int i = g_stub_hit_count++;
   snprintf(g_stub_hits[i].file_path, sizeof(g_stub_hits[i].file_path), "%s", file_path);
   snprintf(g_stub_hits[i].snippet, sizeof(g_stub_hits[i].snippet), "%s", snippet);
}

/* ── stubs: the structural blast-radius resolver's kb_client calls ──────────── */

static int g_br_rc = -1; /* kb_client_index_blast_radius: 0 = filled, else fail-open */
static blast_radius_t g_br;

int kb_client_index_list(project_info_t *out, int max)
{
   if (max < 1 || !out)
      return 0;
   memset(&out[0], 0, sizeof(out[0]));
   snprintf(out[0].name, sizeof(out[0].name), "p");
   snprintf(out[0].root, sizeof(out[0].root), "/repo"); /* abs paths under /repo match */
   return 1;
}

int kb_client_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out)
{
   (void)project;
   (void)file_path;
   if (g_br_rc != 0 || !out)
      return -1;
   *out = g_br;
   return 0;
}

static void set_blast_radius(const char *const *deps, int ndeps, const char *const *dependencies,
                             int ndependencies)
{
   memset(&g_br, 0, sizeof(g_br));
   g_br.dependent_count = ndeps;
   for (int i = 0; i < ndeps && i < 64; i++)
      snprintf(g_br.dependents[i], sizeof(g_br.dependents[i]), "%s", deps[i]);
   g_br.dependency_count = ndependencies;
   for (int i = 0; i < ndependencies && i < 64; i++)
      snprintf(g_br.dependencies[i], sizeof(g_br.dependencies[i]), "%s", dependencies[i]);
   g_br_rc = 0;
}

/* Drive the real config_load via a temp AIMEE_HOME/aimee.yaml (config.o is linked,
 * so config_load can't be stubbed). AIMEE_NO_CACHE defeats the config cache. */
static char g_graph_home[256];
static void graph_flag_setup(void)
{
   snprintf(g_graph_home, sizeof(g_graph_home), "/tmp/aimee-gctx-%d", (int)getpid());
   mkdir(g_graph_home, 0700);
   setenv("AIMEE_HOME", g_graph_home, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);
}
static void graph_flag_write(int on)
{
   char p[512];
   snprintf(p, sizeof(p), "%s/aimee.yaml", g_graph_home);
   FILE *f = fopen(p, "w");
   if (f)
   {
      fprintf(f, "delegate_graph_context_enabled: %s\n", on ? "true" : "false");
      fclose(f);
   }
}

/* ── 1. delegate_extract_named_paths tests ──────────────────────────────── */

static void test_extract_no_paths(void)
{
   char paths[16][256];
   int n = delegate_extract_named_paths("just some text with no file references", paths, 16);
   assert(n == 0);
   printf("  PASS: test_extract_no_paths\n");
}

static void test_extract_single_path(void)
{
   char paths[16][256];
   int n = delegate_extract_named_paths("Edit src/foo.c to add error handling.", paths, 16);
   assert(n == 1);
   assert(strstr(paths[0], "src/foo.c") != NULL);
   printf("  PASS: test_extract_single_path\n");
}

static void test_extract_multi_path(void)
{
   char paths[16][256];
   int n = delegate_extract_named_paths(
       "Update src/server/server.c and src/headers/server.h with the new field.", paths, 16);
   assert(n == 2);
   printf("  PASS: test_extract_multi_path (count=%d)\n", n);
}

static void test_extract_deduplicates(void)
{
   char paths[16][256];
   int n = delegate_extract_named_paths("Edit src/foo.c — src/foo.c has the function you need.",
                                        paths, 16);
   assert(n == 1);
   printf("  PASS: test_extract_deduplicates\n");
}

static void test_extract_skips_system_includes(void)
{
   char paths[16][256];
   /* <sys/stat.h> should be skipped (angle-bracket system include) */
   int n =
       delegate_extract_named_paths("#include <sys/stat.h> — edit src/real.c instead.", paths, 16);
   assert(n == 1);
   assert(strstr(paths[0], "src/real.c") != NULL);
   printf("  PASS: test_extract_skips_system_includes\n");
}

/* ── 2. delegate_inject_code_context tests ──────────────────────────────── */

static void test_inject_returns_null_when_no_hits(void)
{
   stub_hits_reset();
   char *ctx = delegate_inject_code_context("find foo_function in the codebase");
   assert(ctx == NULL);
   printf("  PASS: test_inject_returns_null_when_no_hits\n");
}

static void test_inject_returns_context_block_with_hits(void)
{
   stub_hits_reset();
   stub_hits_add("src/foo.c", "int foo_function(void) { return 42; }");

   char *ctx = delegate_inject_code_context("implement foo_function");
   assert(ctx != NULL);
   assert(strstr(ctx, "## Context") != NULL);
   assert(strstr(ctx, "src/foo.c") != NULL);
   assert(strstr(ctx, "foo_function") != NULL);
   free(ctx);
   printf("  PASS: test_inject_returns_context_block_with_hits\n");
}

static void test_inject_handles_multiple_hits(void)
{
   stub_hits_reset();
   stub_hits_add("src/alpha.c", "void alpha(void) {}");
   stub_hits_add("src/beta.c", "void beta(void) {}");

   char *ctx = delegate_inject_code_context("use alpha and beta");
   assert(ctx != NULL);
   assert(strstr(ctx, "src/alpha.c") != NULL);
   assert(strstr(ctx, "src/beta.c") != NULL);
   free(ctx);
   printf("  PASS: test_inject_handles_multiple_hits\n");
}

static void test_inject_null_prompt(void)
{
   stub_hits_reset();
   char *ctx = delegate_inject_code_context(NULL);
   assert(ctx == NULL);
   printf("  PASS: test_inject_null_prompt\n");
}

/* ── 2b. delegate_inject_graph_context (§7 graph-informed delegation) ─────── */

/* Flag on + a referenced file with structural edges → a "## Structural context"
 * block naming the callers and dependencies. */
static void test_graph_ctx_block_with_edges(void)
{
   graph_flag_setup();
   graph_flag_write(1);
   const char *deps[] = {"src/caller_a.c", "src/caller_b.c"};
   const char *dependencies[] = {"src/dep_x.c"};
   set_blast_radius(deps, 2, dependencies, 1);

   char *ctx = delegate_inject_graph_context("please fix the bug in src/foo.c handler", "/repo");
   assert(ctx != NULL);
   assert(strstr(ctx, "## Structural context") != NULL);
   assert(strstr(ctx, "src/foo.c") != NULL); /* the referenced file */
   assert(strstr(ctx, "callers:") != NULL);
   assert(strstr(ctx, "src/caller_a.c") != NULL && strstr(ctx, "src/caller_b.c") != NULL);
   assert(strstr(ctx, "depends on:") != NULL && strstr(ctx, "src/dep_x.c") != NULL);
   free(ctx);
   printf("  PASS: test_graph_ctx_block_with_edges\n");
}

/* Flag OFF → NULL even with referenced files + edges (opt-in). */
static void test_graph_ctx_disabled_is_null(void)
{
   graph_flag_setup();
   graph_flag_write(0);
   const char *deps[] = {"src/caller_a.c"};
   set_blast_radius(deps, 1, NULL, 0);
   char *ctx = delegate_inject_graph_context("fix src/foo.c", "/repo");
   assert(ctx == NULL);
   printf("  PASS: test_graph_ctx_disabled_is_null\n");
}

/* Flag on but the prompt references no file path → NULL. */
static void test_graph_ctx_no_paths_is_null(void)
{
   graph_flag_setup();
   graph_flag_write(1);
   const char *deps[] = {"src/caller_a.c"};
   set_blast_radius(deps, 1, NULL, 0);
   char *ctx = delegate_inject_graph_context("just refactor the thing, no files named", "/repo");
   assert(ctx == NULL);
   printf("  PASS: test_graph_ctx_no_paths_is_null\n");
}

/* Flag on + file referenced but the resolver fails (kb down / unindexed) →
 * NULL (fail-open). */
static void test_graph_ctx_resolver_fail_open(void)
{
   graph_flag_setup();
   graph_flag_write(1);
   g_br_rc = -1; /* resolver returns "no data" for every path */
   char *ctx = delegate_inject_graph_context("fix src/foo.c", "/repo");
   assert(ctx == NULL);
   printf("  PASS: test_graph_ctx_resolver_fail_open\n");
}

/* ── 3. delegate_worktree_has_changes tests ─────────────────────────────── */

static void test_worktree_has_changes_empty_input(void)
{
   /* NULL and empty string return 0 without invoking git. */
   assert(delegate_worktree_has_changes(NULL) == 0);
   assert(delegate_worktree_has_changes("") == 0);
   printf("  PASS: test_worktree_has_changes_empty_input\n");
}

static void test_worktree_has_changes_nonexistent_path(void)
{
   /* Path that does not exist or is not a git repo: drift_git_diff returns
    * empty stdout, helper returns 0. We use /tmp/aimee-no-such-path-XYZ to
    * avoid clobbering anything; no setup required. */
   assert(delegate_worktree_has_changes("/tmp/aimee-no-such-path-XYZ-test") == 0);
   printf("  PASS: test_worktree_has_changes_nonexistent_path\n");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
   printf("delegate_dispatch_reliability:\n");

   test_extract_no_paths();
   test_extract_single_path();
   test_extract_multi_path();
   test_extract_deduplicates();
   test_extract_skips_system_includes();

   test_inject_returns_null_when_no_hits();
   test_inject_returns_context_block_with_hits();
   test_inject_handles_multiple_hits();
   test_inject_null_prompt();
   test_graph_ctx_block_with_edges();
   test_graph_ctx_disabled_is_null();
   test_graph_ctx_no_paths_is_null();
   test_graph_ctx_resolver_fail_open();

   test_worktree_has_changes_empty_input();
   test_worktree_has_changes_nonexistent_path();

   printf("ok\n");
   return 0;
}
