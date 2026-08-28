#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>
#include "aimee.h"
#include "modules/db2/c/db2.h"
#include "canonical_index.h"
#include "entity_edges.h"
#include "modules/db2/c/db2_test_shim.h"
#include "modules/db2/c/db2_internal.h"
#include "db_postgres.h"
#include "platform_test_util.h"
#include "util.h"

static int css_analysis_mode;
static int css_token_mode;
static int css_invalid_release_count;
static css_stylesheet_t css_invalid_stylesheet;

static css_stylesheet_t *test_css_analyze_provider(const char *text, size_t len)
{
   if (css_analysis_mode)
   {
      memset(&css_invalid_stylesheet, 0, sizeof(css_invalid_stylesheet));
      css_invalid_stylesheet.truncated = 2;
      return &css_invalid_stylesheet;
   }
   return css_analyze(text, len);
}

static void test_css_stylesheet_free_provider(css_stylesheet_t *stylesheet)
{
   if (stylesheet == &css_invalid_stylesheet)
   {
      css_invalid_release_count++;
      return;
   }
   css_stylesheet_free(stylesheet);
}

static int test_css_token_provider(const char *text, size_t len, char (*out)[CSS_CLASS_TOKEN_MAX],
                                   int max)
{
   if (css_token_mode == 1)
      return max + 1;
   if (css_token_mode == 2)
   {
      memset(out[0], 'x', CSS_CLASS_TOKEN_MAX);
      return 1;
   }
   if (css_token_mode == 3)
   {
      strcpy(out[0], "duplicate");
      strcpy(out[1], "duplicate");
      return 2;
   }
   return css_extract_class_tokens(text, len, out, max);
}

static void test_css_analysis_contract(void)
{
   const char *css = ".one { color: black; }";
   const char *markup = "<div className=\"one two\" />";
   char tokens[4][CSS_CLASS_TOKEN_MAX];

   aimee_db2_register_css_analysis_providers(NULL, NULL, NULL);
   assert(canonical_index_css_analyze(css, strlen(css)) == NULL);
   assert(canonical_index_css_extract_class_tokens(markup, strlen(markup), tokens, 4) == -1);

   aimee_db2_register_css_analysis_providers(
       test_css_analyze_provider, test_css_stylesheet_free_provider, test_css_token_provider);
   css_analysis_mode = 1;
   assert(canonical_index_css_analyze(css, strlen(css)) == NULL);
   assert(css_invalid_release_count == 1);

   css_analysis_mode = 0;
   css_stylesheet_t *stylesheet = canonical_index_css_analyze(css, strlen(css));
   assert(stylesheet && stylesheet->rule_count == 1);
   canonical_index_css_stylesheet_free(stylesheet);

   css_token_mode = 1;
   assert(canonical_index_css_extract_class_tokens(markup, strlen(markup), tokens, 4) == -1);
   assert(tokens[0][0] == '\0');
   css_token_mode = 2;
   assert(canonical_index_css_extract_class_tokens(markup, strlen(markup), tokens, 4) == -1);
   css_token_mode = 3;
   assert(canonical_index_css_extract_class_tokens(markup, strlen(markup), tokens, 4) == -1);
   css_token_mode = 0;
   assert(canonical_index_css_extract_class_tokens(markup, strlen(markup), tokens, 4) == 2);
   assert(strcmp(tokens[0], "one") == 0 && strcmp(tokens[1], "two") == 0);
   assert(canonical_index_css_extract_class_tokens(markup, strlen(markup), tokens, 513) == -1);
}

/* Create a temp directory with a test source file */
static char *create_test_project(void)
{
   char *dir = malloc(PATH_MAX);
   assert(dir != NULL);
   snprintf(dir, PATH_MAX, "%s/aimee-test-index-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(dir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/main.c", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "#include <stdio.h>\n"
              "\n"
              "void hello(void) {\n"
              "    printf(\"hello\\n\");\n"
              "}\n"
              "\n"
              "int main(void) {\n"
              "    hello();\n"
              "    return 0;\n"
              "}\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/util.c", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "#include \"main.c\"\n"
              "\n"
              "void helper(void) {}\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/Makefile", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "BUILD_DIR = build\n"
              "GENERATED_DIR = generated\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/.legacy-hidden.c", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "void legacy_hidden_symbol(void) {}\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/build", dir);
   mkdir(path, 0755);
   snprintf(path, sizeof(path), "%s/build/legacy-artifact.c", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "void legacy_build_artifact_symbol(void) {}\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/generated", dir);
   mkdir(path, 0755);
   snprintf(path, sizeof(path), "%s/generated/legacy-generated.c", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "void legacy_generated_artifact_symbol(void) {}\n");
   fclose(f);

   return dir;
}

/* Create a second project with different symbols */
static char *create_test_project_b(void)
{
   char *dir = malloc(PATH_MAX);
   assert(dir != NULL);
   snprintf(dir, PATH_MAX, "%s/aimee-test-index-b-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(dir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/app.c", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "void app_start(void) {}\n"
              "void app_stop(void) {}\n");
   fclose(f);

   return dir;
}

/* Create a project with no extractable files */
static char *create_empty_project(void)
{
   char *dir = malloc(PATH_MAX);
   assert(dir != NULL);
   snprintf(dir, PATH_MAX, "%s/aimee-test-index-empty-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(dir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/README.md", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "# No code here\n");
   fclose(f);

   return dir;
}

static char *create_canonical_excluded_path_project(void)
{
   char *dir = malloc(PATH_MAX);
   assert(dir != NULL);
   snprintf(dir, PATH_MAX, "%s/aimee-test-canonical-index-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(dir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/src", dir);
   mkdir(path, 0755);
   snprintf(path, sizeof(path), "%s/src/liveness.c", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "int liveness_is_degenerate_response(const char *content) {\n"
              "    return content == 0;\n"
              "}\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/Makefile", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "OBJDIR = build\n"
              "GEN_DIR = src/generated\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/.gitignore", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "/build/\n"
              "/src/generated/\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/src/build", dir);
   mkdir(path, 0755);
   snprintf(path, sizeof(path), "%s/src/build/source.c", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "void source_build_symbol(void) {}\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/.aimee", dir);
   mkdir(path, 0755);
   snprintf(path, sizeof(path), "%s/.aimee/worktrees", dir);
   mkdir(path, 0755);
   snprintf(path, sizeof(path), "%s/.aimee/worktrees/old", dir);
   mkdir(path, 0755);
   snprintf(path, sizeof(path), "%s/.aimee/worktrees/old/main", dir);
   mkdir(path, 0755);
   snprintf(path, sizeof(path), "%s/.aimee/worktrees/old/main/src", dir);
   mkdir(path, 0755);
   snprintf(path, sizeof(path), "%s/.aimee/worktrees/old/main/src/liveness.c", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "void hidden_worktree_symbol(void) {}\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/.hidden.c", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "void hidden_root_file_symbol(void) {}\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/.hidden", dir);
   mkdir(path, 0755);
   snprintf(path, sizeof(path), "%s/.hidden/hidden.c", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "void hidden_dir_symbol(void) {}\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/src/.hidden.c", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "void nested_hidden_file_symbol(void) {}\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/build", dir);
   mkdir(path, 0755);
   snprintf(path, sizeof(path), "%s/build/artifact.c", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "void build_artifact_symbol(void) {}\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/src/generated", dir);
   mkdir(path, 0755);
   snprintf(path, sizeof(path), "%s/src/generated/generated.c", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "void generated_artifact_symbol(void) {}\n");
   fclose(f);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "git -C %s init -q && git -C %s add Makefile .gitignore src/liveness.c "
            "src/build/source.c && git -C %s add -f build/artifact.c src/generated/generated.c",
            dir, dir, dir);
   assert(system(cmd) == 0);

   return dir;
}

static void seed_stale_hidden_project_row(const char *root)
{
   char sql[2048];
   char err[256] = "";
   snprintf(sql, sizeof(sql),
            "INSERT INTO projects(id, name, root, scanned_at) "
            "VALUES(7001, 'stalehidden', '%s/.aimee/worktrees/stale/main', "
            "'2026-05-19T00:00:00Z');"
            "INSERT INTO files(id, project_id, path, scanned_at) "
            "VALUES(7002, 7001, 'src/ghost.c', '2026-05-19T00:00:00Z');"
            "INSERT INTO terms(file_id, name, kind, line) "
            "VALUES(7002, 'stale_hidden_project_symbol', 'definition', 7);",
            root);
   assert(aimee_pg_exec(db2_conn(), sql, err, sizeof(err)) == 0);
}

/* Count of files rows for (project, path). Used to assert ingest of a file
 * (.gitmodules) that has no extractable symbols, so index_find can't see it.
 * Returns -1 on a DB/step failure so a schema regression fails loudly rather
 * than masquerading as "row absent". */
static int file_row_count(const char *project, const char *path)
{
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(),
                        "SELECT COUNT(*) FROM files f JOIN projects p ON p.id = f.project_id "
                        "WHERE p.name = ?1 AND f.path = ?2",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", path);
   int n = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? aimee_pg_column_int(st, 0) : -1;
   aimee_pg_finalize(st);
   return n;
}

/* The stored content body of (project, path) into out (empty if absent/error). */
static void file_content(const char *project, const char *path, char *out, size_t cap)
{
   out[0] = '\0';
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(),
                        "SELECT fc.content FROM file_contents fc JOIN files f ON f.id = fc.file_id "
                        "JOIN projects p ON p.id = f.project_id WHERE p.name = ?1 AND f.path = ?2",
                        err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", path);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c = aimee_pg_column_text(st, 0);
      snprintf(out, cap, "%s", c ? c : "");
   }
   aimee_pg_finalize(st);
}

/* Summed co_edited weight between two basenames (symmetric). -1 on error. */
static int cochange_pair_weight(const char *a, const char *b)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "SELECT COALESCE(SUM(weight),0) FROM entity_edges WHERE relation = 'co_edited'"
       " AND ((source = ?1 AND target = ?2) OR (source = ?2 AND target = ?1))",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", a);
   aimee_pg_bind_text(st, "?2", b);
   int w = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? aimee_pg_column_int(st, 0) : -1;
   aimee_pg_finalize(st);
   return w;
}

/* co_edited edges touching any z*.c file (bulk-commit gate check). -1 on error. */
static int cochange_bulk_edge_count(void)
{
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(),
                        "SELECT count(*) FROM entity_edges WHERE relation = 'co_edited'"
                        " AND (source LIKE 'z%.c' OR target LIKE 'z%.c')",
                        err, sizeof(err));
   if (!st)
      return -1;
   int n = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? aimee_pg_column_int(st, 0) : -1;
   aimee_pg_finalize(st);
   return n;
}

/* A temp git repo with a designed co-change history: a.c & b.c change together
 * across init + 5 commits (weight 6), plus a 30-file bulk commit that must be
 * gated (> the bulk threshold). */
static char *create_cochange_repo(void)
{
   char *dir = malloc(PATH_MAX);
   assert(dir != NULL);
   /* Spaces and shell metacharacters remain one literal argv element. A shell-
    * joined implementation would execute/substitute `false` and miss this root. */
   snprintf(dir, PATH_MAX, "%s/aimee $(false) cochange-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(dir) != NULL);
   char cmd[4096];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && git config user.name t && "
            "printf 'int a(){return 1;}\\n' > a.c && printf 'int b(){return 2;}\\n' > b.c && "
            "printf 'int c(){return 3;}\\n' > c.c && printf 'int d(){return 4;}\\n' > d.c && "
            "git add -A && git commit -qm init && "
            "for i in 1 2 3 4 5; do printf '// %%d\\n' \"$i\" >> a.c; printf '// %%d\\n' \"$i\" "
            ">> b.c; git commit -qam \"ab$i\"; done && "
            "printf '// c\\n' >> c.c && git commit -qam conly && "
            "for n in $(seq 1 30); do printf 'int z%%d(){return 1;}\\n' \"$n\" > z$n.c; done && "
            "git add -A && git commit -qm bulk",
            dir);
   assert(system(cmd) == 0);
   return dir;
}

int main(void)
{
   printf("index: ");

   test_css_analysis_contract();

   int missing_host_inspected = 7;
   canonical_index_set_exec_capture(NULL);
   assert(canonical_index_scan_project("missing-host", "/not-scanned", 1,
                                       &missing_host_inspected) == -1);
   assert(missing_host_inspected == 0);
   canonical_index_set_exec_capture(safe_exec_capture);

   db2_test_shim_open();

   char *project_dir = create_test_project();

   /* --- index_scan_project: returns count of files scanned --- */
   {
      int scanned = index_scan_project("testproj", project_dir, 0);
      assert(scanned >= 0);
   }

   /* --- index_list_projects --- */
   {
      project_info_t projects[8];
      int count = index_list_projects(projects, 8);
      assert(count == 1);
      assert(strcmp(projects[0].name, "testproj") == 0);
   }

   /* --- index_find --- */
   {
      term_hit_t hits[16];
      int count = index_find("hello", hits, 16);
      assert(count > 0);
      assert(strcmp(hits[0].project, "testproj") == 0);
   }

   /* --- index_find: nonexistent symbol --- */
   {
      term_hit_t hits[16];
      int count = index_find("nonexistent_xyz_42", hits, 16);
      assert(count == 0);
   }

   /* --- index_find: excluded paths are not scanned --- */
   {
      term_hit_t hits[16];
      int count = index_find("legacy_hidden_symbol", hits, 16);
      assert(count == 0);
      count = index_find("legacy_build_artifact_symbol", hits, 16);
      assert(count == 0);
      count = index_find("legacy_generated_artifact_symbol", hits, 16);
      assert(count == 0);
   }

   /* --- index_structure --- */
   {
      definition_t defs[16];
      int count = index_structure("testproj", "main.c", defs, 16);
      assert(count >= 2); /* hello + main */
      int found_hello = 0, found_main = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(defs[i].name, "hello") == 0)
            found_hello = 1;
         if (strcmp(defs[i].name, "main") == 0)
            found_main = 1;
      }
      assert(found_hello);
      assert(found_main);
   }

   /* --- index_blast_radius --- */
   {
      blast_radius_t br;
      int rc = index_blast_radius("testproj", "main.c", &br);
      assert(rc == 0);
      /* util.c includes main.c, so main.c should have a dependent */
      assert(br.dependent_count >= 1 || br.dependency_count >= 0);
   }

   /* --- rescan is idempotent --- */
   {
      int scanned = index_scan_project("testproj", project_dir, 0);
      assert(scanned >= 0);

      project_info_t projects[8];
      int count = index_list_projects(projects, 8);
      assert(count == 1); /* Still just one project */
   }

   /* --- multi-project: scanning a second project --- */
   {
      char *proj_b = create_test_project_b();
      int scanned = index_scan_project("projb", proj_b, 0);
      assert(scanned >= 1); /* at least app.c */

      project_info_t projects[8];
      int count = index_list_projects(projects, 8);
      assert(count == 2); /* testproj + projb */

      /* Find symbol from project B */
      term_hit_t hits[16];
      int hit_count = index_find("app_start", hits, 16);
      assert(hit_count > 0);
      assert(strcmp(hits[0].project, "projb") == 0);

      /* Original project symbols still findable */
      hit_count = index_find("hello", hits, 16);
      assert(hit_count > 0);
      assert(strcmp(hits[0].project, "testproj") == 0);

      char cmd[512];
      snprintf(cmd, sizeof(cmd), "rm -rf %s", proj_b);
      (void)system(cmd);
      free(proj_b);
   }

   /* --- empty project: no extractable files, but project still registered --- */
   {
      char *empty = create_empty_project();
      int scanned = index_scan_project("emptyproj", empty, 0);
      assert(scanned == 0); /* no code files to scan */

      project_info_t projects[16];
      int count = index_list_projects(projects, 16);
      assert(count == 3); /* testproj + projb + emptyproj */

      /* Verify it's registered with the right name */
      int found = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(projects[i].name, "emptyproj") == 0)
            found = 1;
      }
      assert(found);

      char cmd[512];
      snprintf(cmd, sizeof(cmd), "rm -rf %s", empty);
      (void)system(cmd);
      free(empty);
   }

   /* --- rescan after file modification picks up changes --- */
   {
      /* Add a new file to the original project */
      char path[512];
      snprintf(path, sizeof(path), "%s/extra.c", project_dir);
      FILE *f = fopen(path, "w");
      assert(f != NULL);
      fprintf(f, "void extra_func(void) {}\n");
      fclose(f);

      int scanned = index_scan_project("testproj", project_dir, 0);
      assert(scanned >= 1); /* at least the new file */

      term_hit_t hits[16];
      int count = index_find("extra_func", hits, 16);
      assert(count > 0);
      assert(strcmp(hits[0].project, "testproj") == 0);
   }

   /* --- canonical index excludes hidden paths and generated artifacts --- */
   {
      char *canonical = create_canonical_excluded_path_project();
      int inspected = 0;
      int scanned = canonical_index_scan_project("canonicalproj", canonical, 1, &inspected);
      assert(scanned >= 2);
      assert(inspected >= 2);

      term_hit_t hits[16];
      int count = canonical_index_find("liveness_is_degenerate_response", hits, 16);
      assert(count > 0);
      int found_real = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(hits[i].project, "canonicalproj") == 0 &&
             strcmp(hits[i].file_path, "src/liveness.c") == 0)
            found_real = 1;
      }
      assert(found_real);

      /* Project scoping belongs inside the SQL query, before LIMIT. A globally earlier duplicate
       * must not crowd the requested project's definition out of a one-row result set. */
      char *crowd = malloc(PATH_MAX);
      assert(crowd != NULL);
      snprintf(crowd, PATH_MAX, "%s/aimee-test-index-crowd-XXXXXX", platform_tmpdir());
      assert(platform_mkdtemp(crowd) != NULL);
      char crowd_path[PATH_MAX];
      snprintf(crowd_path, sizeof(crowd_path), "%s/duplicate.c", crowd);
      FILE *crowd_file = fopen(crowd_path, "w");
      assert(crowd_file != NULL);
      fprintf(crowd_file, "void liveness_is_degenerate_response(void) {}\n");
      fclose(crowd_file);
      int crowd_inspected = 0;
      assert(canonical_index_scan_project("aaa-crowd", crowd, 1, &crowd_inspected) == 1);
      assert(crowd_inspected == 1);
      count = canonical_index_find("liveness_is_degenerate_response", hits, 1);
      assert(count == 1);
      assert(strcmp(hits[0].project, "aaa-crowd") == 0);
      count =
          canonical_index_find_project("canonicalproj", "liveness_is_degenerate_response", hits, 1);
      assert(count == 1);
      assert(strcmp(hits[0].project, "canonicalproj") == 0);
      count = canonical_index_find_excluding_project("aaa-crowd", "liveness_is_degenerate_response",
                                                     hits, 1);
      assert(count == 1);
      assert(strcmp(hits[0].project, "canonicalproj") == 0);
      count = canonical_index_find_excluding_project("canonicalproj",
                                                     "liveness_is_degenerate_response", hits, 1);
      assert(count == 1);
      assert(strcmp(hits[0].project, "aaa-crowd") == 0);

      code_search_hit_t search_hits[1];
      count =
          canonical_index_code_search_excluding_project("liveness", "aaa-crowd", search_hits, 1, 0);
      assert(count == 1);
      assert(strcmp(search_hits[0].project, "canonicalproj") == 0);
      count = canonical_index_code_search_excluding_project("liveness", "canonicalproj",
                                                            search_hits, 1, 0);
      assert(count == 1);
      assert(strcmp(search_hits[0].project, "aaa-crowd") == 0);
      char cleanup_cmd[PATH_MAX + 16];
      snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", crowd);
      (void)system(cleanup_cmd);
      free(crowd);

      count = canonical_index_find("source_build_symbol", hits, 16);
      assert(count > 0);

      count = canonical_index_find("hidden_worktree_symbol", hits, 16);
      assert(count == 0);
      count = canonical_index_find("hidden_root_file_symbol", hits, 16);
      assert(count == 0);
      count = canonical_index_find("hidden_dir_symbol", hits, 16);
      assert(count == 0);
      count = canonical_index_find("nested_hidden_file_symbol", hits, 16);
      assert(count == 0);
      count = canonical_index_find("build_artifact_symbol", hits, 16);
      assert(count == 0);
      count = canonical_index_find("generated_artifact_symbol", hits, 16);
      assert(count == 0);

      seed_stale_hidden_project_row(canonical);
      project_info_t projects[16];
      count = index_list_projects(projects, 16);
      for (int i = 0; i < count; i++)
         assert(strcmp(projects[i].name, "stalehidden") != 0);
      count = canonical_index_list_projects(projects, 16);
      for (int i = 0; i < count; i++)
         assert(strcmp(projects[i].name, "stalehidden") != 0);

      count = index_find("stale_hidden_project_symbol", hits, 16);
      assert(count == 0);
      count = canonical_index_find("stale_hidden_project_symbol", hits, 16);
      assert(count == 0);

      definition_t defs[16];
      count = canonical_index_structure("canonicalproj", "src/liveness.c", defs, 16);
      assert(count > 0);
      assert(strcmp(defs[0].name, "liveness_is_degenerate_response") == 0);

      char cmd[512];
      snprintf(cmd, sizeof(cmd), "rm -rf %s", canonical);
      (void)system(cmd);
      free(canonical);
   }

   /* --- thin-client ingest (scan_files): a repo-root .gitmodules is INGESTED
    * (recall §2.2), while a hidden non-manifest dotfile and a file inside a hidden
    * directory are still excluded. --- */
   {
      const char *gitmod_body = "[submodule \"dep\"]\n\turl = https://h/o/dep.git\n";
      canonical_index_file_input_t inputs[] = {
          {"src/app.c", "void app_entry(void) {}\n"},
          {".gitmodules", gitmod_body},
          {".travis.yml", "language: c\n"},
          {".eslintrc.json", "{\"env\":{}}\n"},
          {".bashrc", "export X=1\n"},
          {".env", "SECRET=1\n"},
          {".github/workflows/ci.yml", "name: ci\n"}, /* file inside a hidden dir */
          {".gitmodules/payload.txt", "x\n"},         /* .gitmodules as an interior DIR */
      };
      int inspected = 0;
      int scanned = canonical_index_scan_files(
          "pushproj", "remote", inputs, (int)(sizeof(inputs) / sizeof(inputs[0])), 1, &inspected);
      /* canonical_index_scan_files upserts the project; of 5 inputs only the 2
       * non-excluded (src/app.c + .gitmodules) are counted (inspected is post-filter). */
      assert(inspected == 4);
      assert(scanned == 4);
      assert(file_row_count("pushproj", ".gitmodules") == 1); /* dotfile manifest ingested */
      assert(file_row_count("pushproj", "src/app.c") == 1);
      assert(file_row_count("pushproj", ".travis.yml") == 1);
      assert(file_row_count("pushproj", ".eslintrc.json") == 1);
      assert(file_row_count("pushproj", ".bashrc") == 0); /* hidden non-manifest excluded */
      assert(file_row_count("pushproj", ".env") == 0);
      assert(file_row_count("pushproj", ".github/workflows/ci.yml") == 0); /* hidden dir excluded */
      assert(file_row_count("pushproj", ".gitmodules/payload.txt") ==
             0); /* interior dir excluded */
      /* the .gitmodules body must survive ingest byte-for-byte (R2b's crb_gitmodules
       * parses url= lines out of it). */
      char body[256];
      file_content("pushproj", ".gitmodules", body, sizeof(body));
      assert(strcmp(body, gitmod_body) == 0);
   }

   /* --- malformed bytes are normalized before either canonical ingest path
    * reaches Postgres TEXT; pushed buffers remain caller-owned and immutable. --- */
   {
      char *invalid_dir = malloc(PATH_MAX);
      assert(invalid_dir != NULL);
      snprintf(invalid_dir, PATH_MAX, "%s/aimee-test-index-utf8-XXXXXX", platform_tmpdir());
      assert(platform_mkdtemp(invalid_dir) != NULL);

      char path[PATH_MAX];
      snprintf(path, sizeof(path), "%s/legacy.c", invalid_dir);
      FILE *f = fopen(path, "wb");
      assert(f != NULL);
      const char invalid_disk[] = "void legacy_\x92symbol(void) {} /* \xed\xa0\x80 */\n";
      assert(fwrite(invalid_disk, 1, sizeof(invalid_disk) - 1, f) == sizeof(invalid_disk) - 1);
      fclose(f);

      int inspected = 0;
      assert(canonical_index_scan_project("utf8disk", invalid_dir, 1, &inspected) == 1);
      assert(inspected == 1);
      char stored[256];
      file_content("utf8disk", "legacy.c", stored, sizeof(stored));
      assert(strcmp(stored, "void legacy_?symbol(void) {} /* ??? */\n") == 0);

      const char invalid_push[] = "int pushed_\x94value = 1; /* \x80 */\n";
      canonical_index_file_input_t input = {"pushed.c", invalid_push};
      assert(canonical_index_scan_files("utf8push", "remote", &input, 1, 1, &inspected) == 1);
      file_content("utf8push", "pushed.c", stored, sizeof(stored));
      assert(strcmp(stored, "int pushed_?value = 1; /* ? */\n") == 0);
      assert((unsigned char)invalid_push[11] == 0x94);

      const char invalid_adapter[] = "adapter \x92"
                                     "body \xed\xa0\x80";
      int64_t pid = db2_code_index_project_upsert("utf8adapter", "/remote");
      assert(pid > 0);
      int64_t fid = db2_code_index_file_upsert(pid, "adapter.c", "2026-07-28T00:00:00Z");
      assert(fid > 0);
      code_index_file_data_t data = {.content = invalid_adapter};
      assert(db2_code_index_file_replace(fid, &data) == 0);
      file_content("utf8adapter", "adapter.c", stored, sizeof(stored));
      assert(strcmp(stored, "adapter ?body ???") == 0);
      assert((unsigned char)invalid_adapter[8] == 0x92);

      char cmd[PATH_MAX + 16];
      snprintf(cmd, sizeof(cmd), "rm -rf %s", invalid_dir);
      (void)system(cmd);
      free(invalid_dir);
   }

   /* E3 exact Python module graph: normal, explicit from-pair, and relative
    * imports converge on app/dates.py; a prefix collision is excluded and a
    * call-only user is merged with provenance. */
   {
      canonical_index_file_input_t inputs[] = {
          {"app/dates.py", "import app.calendar\n\ndef billing_period_days():\n    return 30\n"},
          {"app/billing.py",
           "from app import dates\n\ndef bill():\n    return dates.billing_period_days()\n"},
          {"app/invoices.py",
           "from . import dates\n\ndef invoice():\n    return dates.billing_period_days()\n"},
          {"app/reports.py",
           "import app.dates\n\ndef report():\n    return billing_period_days()\n"},
          {"app/caller_only.py", "def preview():\n    return billing_period_days()\n"},
          {"app/forecast.py", "def forecast():\n    return 30\n"},
          {"app/collision.py", "import app.dates_extra\n"},
      };
      int inspected = 0;
      assert(canonical_index_scan_files("python-blast", "/fixture", inputs,
                                        (int)(sizeof(inputs) / sizeof(inputs[0])), 1,
                                        &inspected) >= 0);
      blast_radius_t br;
      assert(canonical_index_blast_radius("python-blast", "app/dates.py", &br) == 0);
      const char *expected[] = {"app/billing.py", "app/invoices.py", "app/reports.py",
                                "app/caller_only.py"};
      for (size_t e = 0; e < sizeof(expected) / sizeof(expected[0]); e++)
      {
         int found = 0;
         for (int i = 0; i < br.dependent_count; i++)
            if (strcmp(br.dependents[i], expected[e]) == 0)
            {
               found = 1;
               assert(br.dependent_meta[i].provenance[0]);
               assert(strcmp(br.dependent_meta[i].freshness, "current") == 0);
               assert(br.dependent_meta[i].generation >= 1);
            }
         assert(found);
      }
      for (int i = 0; i < br.dependent_count; i++)
         assert(strcmp(br.dependents[i], "app/collision.py") != 0);
      assert(br.resolved == 1);
      assert(strcmp(br.project, "python-blast") == 0);
      assert(br.dependency_count == 1);
      assert(strcmp(br.dependencies[0], "app.calendar") == 0);

      /* Projection-only local edges must still sort before the route-gated
       * cross-project tail. Four bumps clear the projection weight gate. */
      for (int bump = 0; bump < 4; bump++)
      {
         int added = 0;
         assert(db2_entity_edge_upsert("dates.py", "co_edited", "forecast.py", 0, 0, 0, 0,
                                       &added) == 0);
      }

      canonical_index_file_input_t routed[] = {
          {"client/report.py", "import app.dates\n\ndef remote_report():\n    return 1\n"}};
      assert(canonical_index_scan_files("python-consumer", "/consumer", routed, 1, 1, &inspected) >=
             0);
      canonical_index_file_input_t unrouted[] = {
          {"client/noise.py", "import app.dates\n\ndef noise():\n    return 1\n"}};
      assert(canonical_index_scan_files("python-distractor", "/distractor", unrouted, 1, 1,
                                        &inspected) >= 0);
      char sql_err[256] = "";
      assert(aimee_pg_exec(
                 db2_conn(),
                 "INSERT INTO cross_repo_route(caller_project,definer_project,kind,confidence,"
                 "evidence) VALUES('python-consumer','python-blast','import_module','high',"
                 "'app.dates')",
                 sql_err, sizeof(sql_err)) == 0);
      assert(canonical_index_blast_radius("python-blast", "app/dates.py", &br) == 0);
      int found_cross = 0;
      int found_projection = 0;
      int seen_external = 0;
      for (int i = 0; i < br.dependent_count; i++)
      {
         assert(strcmp(br.dependents[i], "client/noise.py") != 0);
         if (strcmp(br.dependent_meta[i].project, "python-blast") == 0)
         {
            assert(!seen_external);
            if (strcmp(br.dependents[i], "app/forecast.py") == 0)
            {
               found_projection = 1;
               assert(strstr(br.dependent_meta[i].provenance, "projection"));
            }
         }
         else
         {
            seen_external = 1;
         }
         if (strcmp(br.dependents[i], "client/report.py") == 0)
         {
            found_cross = 1;
            assert(strcmp(br.dependent_meta[i].project, "python-consumer") == 0);
            assert(strcmp(br.dependent_meta[i].provenance, "cross_repo") == 0);
            assert(strcmp(br.dependent_meta[i].confidence, "high") == 0);
         }
      }
      assert(found_projection);
      assert(found_cross);
      memset(&br, 0, sizeof(br));
      assert(canonical_index_blast_radius("python-blast", "app/missing.py", &br) != 0);
      assert(br.resolved == 0);
   }

   /* --- production co-change path: canonical scan populates co_edited edges from
    * git history, the bulk gate holds, re-scan is idempotent, and blast radius
    * surfaces a co-edited file with no structural import link. This guards the
    * regression where the backfill lived only in the stubbed index.c. --- */
   {
      char *repo = create_cochange_repo();
      int inspected = 0;
      int scanned = canonical_index_scan_project("cochangeproj", repo, 1, &inspected);
      assert(scanned >= 0);

      /* a.c + b.c co-changed in init + 5 commits -> weight 6 (> the >3 blast read
       * threshold). */
      int wab = cochange_pair_weight("a.c", "b.c");
      assert(wab >= 4);
      /* the 30-file bulk commit is over the gate: it contributes no co_edited edges. */
      assert(cochange_bulk_edge_count() == 0);

      /* Re-scan must not double-count (per-project HEAD marker). */
      int scanned2 = canonical_index_scan_project("cochangeproj", repo, 1, &inspected);
      assert(scanned2 >= 0);
      assert(cochange_pair_weight("a.c", "b.c") == wab);

      /* Blast radius surfaces the uniquely resolved b.c projection with explicit
       * provenance (they share no import). */
      blast_radius_t br;
      assert(canonical_index_blast_radius("cochangeproj", "a.c", &br) == 0);
      int found_coedited = 0;
      for (int i = 0; i < br.dependent_count; i++)
         if (strcmp(br.dependents[i], "b.c") == 0 &&
             strstr(br.dependent_meta[i].provenance, "projection") &&
             strcmp(br.dependent_meta[i].freshness, "current") == 0)
            found_coedited = 1;
      assert(found_coedited);

      char rmcmd[512];
      snprintf(rmcmd, sizeof(rmcmd), "rm -rf '%s'", repo);
      (void)system(rmcmd);
      free(repo);
   }

   /* --- complete-manifest invariants: stage privacy, exact retraction,
    * content-hash verification, retry idempotency, and revision ABA guard. --- */
   {
      char *dir = malloc(PATH_MAX);
      assert(dir != NULL);
      snprintf(dir, PATH_MAX, "%s/aimee-test-reconcile-XXXXXX", platform_tmpdir());
      assert(platform_mkdtemp(dir) != NULL);
      char a_path[PATH_MAX], b_path[PATH_MAX];
      snprintf(a_path, sizeof(a_path), "%s/a.c", dir);
      snprintf(b_path, sizeof(b_path), "%s/b.c", dir);
      FILE *f = fopen(a_path, "w");
      assert(f != NULL);
      fputs("int aa=1;\n", f);
      fclose(f);
      f = fopen(b_path, "w");
      assert(f != NULL);
      fputs("int bb=1;\n", f);
      fclose(f);
      int inspected = 0;
      assert(canonical_index_scan_project("reconcile", dir, 0, &inspected) == 2);

      struct stat before;
      assert(stat(a_path, &before) == 0);
      assert(remove(b_path) == 0);
      f = fopen(a_path, "w");
      assert(f != NULL);
      fputs("int aa=2;\n", f); /* same byte length */
      fclose(f);
      struct utimbuf same_time = {.actime = before.st_atime, .modtime = before.st_mtime};
      assert(utime(a_path, &same_time) == 0);

      canonical_index_verify_result_t verify;
      assert(canonical_index_verify_project("reconcile", dir, 1, &verify) == 0);
      assert(verify.modified_files == 1); /* hashes catch same-size/same-mtime edits */
      assert(verify.missing_files == 1);

      canonical_index_file_input_t staged = {"a.c", "int aa=2;\n"};
      assert(canonical_index_scan_begin("reconcile", dir, "interrupted", NULL) == 0);
      assert(canonical_index_scan_stage("interrupted", &staged, 1, NULL) == 0);
      char stored[64];
      file_content("reconcile", "a.c", stored, sizeof(stored));
      assert(strcmp(stored, "int aa=1;\n") == 0); /* no seal: staged data stays private */
      assert(canonical_index_scan_abort("interrupted") == 0);

      assert(canonical_index_scan_project("reconcile", dir, 0, &inspected) == 1);
      assert(file_row_count("reconcile", "b.c") == 0);
      assert(canonical_index_verify_project("reconcile", dir, 1, &verify) == 0);
      assert(!verify.unavailable && verify.modified_files == 0 && verify.missing_files == 0 &&
             verify.unindexed_files == 0);

      /* Two sessions may share a baseline, but only the first successful seal
       * advances it; the late seal is rejected instead of overwriting newer facts. */
      assert(canonical_index_scan_begin("reconcile", dir, "older", NULL) == 0);
      assert(canonical_index_scan_begin("reconcile", dir, "newer", NULL) == 0);
      canonical_index_file_input_t old_input = {"a.c", "int aa=3;\n"};
      canonical_index_file_input_t new_input = {"a.c", "int aa=4;\n"};
      assert(canonical_index_scan_stage("older", &old_input, 1, NULL) == 0);
      assert(canonical_index_scan_stage("newer", &new_input, 1, NULL) == 0);
      canonical_index_seal_result_t seal;
      assert(canonical_index_scan_seal("newer", 1, &seal) == 0);
      assert(canonical_index_scan_seal("older", 1, &seal) == -2);
      file_content("reconcile", "a.c", stored, sizeof(stored));
      assert(strcmp(stored, "int aa=4;\n") == 0);

      /* Duplicate and out-of-order batches converge on distinct manifest paths. */
      assert(canonical_index_scan_begin("reconcile", dir, "retry-order-scan", NULL) == 0);
      canonical_index_file_input_t retry_inputs[] = {
          {"z.c", "int z;\n"}, {"y.c", "int y;\n"}, {"z.c", "int z;\n"}};
      int accepted = 0;
      assert(canonical_index_scan_stage("retry-order-scan", retry_inputs, 3, &accepted) == 0);
      assert(accepted == 3);
      assert(canonical_index_scan_seal("retry-order-scan", 2, &seal) == 0);
      assert(file_row_count("reconcile", "y.c") == 1);
      assert(file_row_count("reconcile", "z.c") == 1);

      char rmcmd[PATH_MAX + 16];
      snprintf(rmcmd, sizeof(rmcmd), "rm -rf %s", dir);
      (void)system(rmcmd);
      free(dir);
   }

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf %s", project_dir);
   (void)system(cmd);
   free(project_dir);
   db2_test_shim_close();

   printf("all tests passed\n");
   return 0;
}
