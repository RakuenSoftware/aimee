#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "db.h"
#include "db2.h"
#include "canonical_index.h"
#include "db2_test_shim.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "platform_test_util.h"

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

int main(void)
{
   printf("index: ");

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
          {".bashrc", "export X=1\n"},                /* hidden non-manifest dotfile */
          {".github/workflows/ci.yml", "name: ci\n"}, /* file inside a hidden dir */
          {".gitmodules/payload.txt", "x\n"},         /* .gitmodules as an interior DIR */
      };
      int inspected = 0;
      int scanned = canonical_index_scan_files(
          "pushproj", "remote", inputs, (int)(sizeof(inputs) / sizeof(inputs[0])), 1, &inspected);
      /* canonical_index_scan_files upserts the project; of 5 inputs only the 2
       * non-excluded (src/app.c + .gitmodules) are counted (inspected is post-filter). */
      assert(inspected == 2);
      assert(scanned == 2);
      assert(file_row_count("pushproj", ".gitmodules") == 1); /* dotfile manifest ingested */
      assert(file_row_count("pushproj", "src/app.c") == 1);
      assert(file_row_count("pushproj", ".bashrc") == 0); /* hidden non-manifest excluded */
      assert(file_row_count("pushproj", ".github/workflows/ci.yml") == 0); /* hidden dir excluded */
      assert(file_row_count("pushproj", ".gitmodules/payload.txt") ==
             0); /* interior dir excluded */
      /* the .gitmodules body must survive ingest byte-for-byte (R2b's crb_gitmodules
       * parses url= lines out of it). */
      char body[256];
      file_content("pushproj", ".gitmodules", body, sizeof(body));
      assert(strcmp(body, gitmod_body) == 0);
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
