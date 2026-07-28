#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* test_kb_client_index_remote.c: verify that kb_client_index_scan pushes file
 * contents when AIMEE_KB_API_URL is set (remote / containerized kb mode). */

#include "kb_client.h"
#include "cJSON.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Stubs                                                               */
/* ------------------------------------------------------------------ */

static int g_remote_mode = 0; /* controls kb_client_v1_base_url return */
static char *g_last_path = NULL;
static char *g_last_get_path = NULL;
static char *g_last_body = NULL;
static int g_post_calls = 0;
static int g_get_calls = 0;
static int g_get_stale_once = 0;
static int g_files_pushed_total = 0; /* rel_path entries summed across calls */
static size_t g_max_body_bytes = 0;  /* largest single request body seen */
static int g_generation_mode = 0;
static int g_begin_calls = 0, g_append_calls = 0, g_finish_calls = 0;
static char g_generation_ref[1024], g_generation_commit[65], g_generation_tree[65];
static char g_generation_file_path[4096], g_generation_file_content[4096];
static const char *g_next_response =
    "{\"status\":\"ok\",\"skipped\":false,\"projects\":1,\"files\":0}";

const char *kb_client_v1_base_url(void)
{
   return g_remote_mode ? "http://127.0.0.1:18741" : NULL;
}

char *kb_client_v1_post_json(const char *path, cJSON *body, int timeout_ms, int *status_out)
{
   (void)timeout_ms;
   free(g_last_path);
   free(g_last_body);
   g_last_path = strdup(path ? path : "");
   g_last_body = body ? cJSON_PrintUnformatted(body) : strdup("{}");
   g_post_calls++;
   if (g_last_body && strlen(g_last_body) > g_max_body_bytes)
      g_max_body_bytes = strlen(g_last_body);
   cJSON *files = body ? cJSON_GetObjectItemCaseSensitive(body, "files") : NULL;
   if (cJSON_IsArray(files))
      g_files_pushed_total += cJSON_GetArraySize(files);
   if (status_out)
      *status_out = 200;
   if (g_generation_mode && body)
   {
      cJSON *action = cJSON_GetObjectItemCaseSensitive(body, "action");
      if (cJSON_IsString(action) && strcmp(action->valuestring, "begin_generation") == 0)
      {
         g_begin_calls++;
         cJSON *ref = cJSON_GetObjectItemCaseSensitive(body, "source_ref");
         cJSON *commit = cJSON_GetObjectItemCaseSensitive(body, "commit_sha");
         cJSON *tree = cJSON_GetObjectItemCaseSensitive(body, "tree_sha");
         snprintf(g_generation_ref, sizeof(g_generation_ref), "%s",
                  cJSON_IsString(ref) ? ref->valuestring : "");
         snprintf(g_generation_commit, sizeof(g_generation_commit), "%s",
                  cJSON_IsString(commit) ? commit->valuestring : "");
         snprintf(g_generation_tree, sizeof(g_generation_tree), "%s",
                  cJSON_IsString(tree) ? tree->valuestring : "");
         char response[2048];
         snprintf(response, sizeof(response),
                  "{\"status\":\"ok\",\"action\":\"begin_generation\","
                  "\"generation_id\":77,\"project\":\"generation:1:77\","
                  "\"repository_key\":\"repo-key\",\"source_ref\":\"%s\","
                  "\"commit_sha\":\"%s\",\"tree_sha\":\"%s\","
                  "\"generation_state\":\"staging\",\"already_current\":false}",
                  g_generation_ref, g_generation_commit, g_generation_tree);
         return strdup(response);
      }
      if (cJSON_IsString(action) && strcmp(action->valuestring, "append_generation") == 0)
      {
         g_append_calls++;
         cJSON *batch = cJSON_GetObjectItemCaseSensitive(body, "files");
         if (cJSON_IsArray(batch) && cJSON_GetArraySize(batch) > 0)
         {
            cJSON *entry = cJSON_GetArrayItem(batch, 0);
            cJSON *rel = cJSON_GetObjectItemCaseSensitive(entry, "rel_path");
            cJSON *content = cJSON_GetObjectItemCaseSensitive(entry, "content");
            snprintf(g_generation_file_path, sizeof(g_generation_file_path), "%s",
                     cJSON_IsString(rel) ? rel->valuestring : "");
            snprintf(g_generation_file_content, sizeof(g_generation_file_content), "%s",
                     cJSON_IsString(content) ? content->valuestring : "");
         }
         char response[256];
         snprintf(response, sizeof(response),
                  "{\"status\":\"ok\",\"action\":\"append_generation\","
                  "\"generation_id\":77,\"files\":%d,\"inspected\":%d}",
                  cJSON_IsArray(batch) ? cJSON_GetArraySize(batch) : 0,
                  cJSON_IsArray(batch) ? cJSON_GetArraySize(batch) : 0);
         return strdup(response);
      }
      if (cJSON_IsString(action) && strcmp(action->valuestring, "finish_generation") == 0)
      {
         g_finish_calls++;
         cJSON *count = cJSON_GetObjectItemCaseSensitive(body, "expected_file_count");
         char response[2048];
         snprintf(response, sizeof(response),
                  "{\"status\":\"ok\",\"action\":\"finish_generation\","
                  "\"generation_id\":77,\"project\":\"generation:1:77\","
                  "\"repository_key\":\"repo-key\",\"source_ref\":\"%s\","
                  "\"commit_sha\":\"%s\",\"tree_sha\":\"%s\","
                  "\"generation_state\":\"published\",\"files\":%d,"
                  "\"model_subjects\":%d,\"skipped\":false}",
                  g_generation_ref, g_generation_commit, g_generation_tree,
                  cJSON_IsNumber(count) ? count->valueint : 0,
                  cJSON_IsNumber(count) ? count->valueint : 0);
         return strdup(response);
      }
   }
   return strdup(g_next_response);
}

char *kb_client_v1_get_json(const char *path, int timeout_ms, int *status_out)
{
   (void)timeout_ms;
   free(g_last_get_path);
   g_last_get_path = strdup(path ? path : "");
   g_get_calls++;
   if (g_get_stale_once && g_get_calls == 1)
   {
      if (status_out)
         *status_out = 409;
      return NULL;
   }
   if (status_out)
      *status_out = 200;
   return strdup("{\"status\":\"ok\"}");
}

char *kb_client_query_escape(const char *s)
{
   return s ? strdup(s) : strdup("");
}

static void reset_stub(void)
{
   free(g_last_path);
   free(g_last_get_path);
   free(g_last_body);
   g_last_path = NULL;
   g_last_get_path = NULL;
   g_last_body = NULL;
   g_post_calls = 0;
   g_get_calls = 0;
   g_get_stale_once = 0;
   g_files_pushed_total = 0;
   g_max_body_bytes = 0;
   g_remote_mode = 0;
   g_generation_mode = 0;
   g_begin_calls = g_append_calls = g_finish_calls = 0;
   g_generation_ref[0] = g_generation_commit[0] = g_generation_tree[0] = '\0';
   g_generation_file_path[0] = g_generation_file_content[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Temp directory helpers                                              */
/* ------------------------------------------------------------------ */

static void write_file(const char *path, const char *content, size_t len)
{
   FILE *fp = fopen(path, "wb");
   assert(fp != NULL);
   if (len > 0)
      assert(fwrite(content, 1, len, fp) == len);
   fclose(fp);
}

static void mkdir_p(const char *path)
{
   mkdir(path, 0755);
}

/* Recursively remove a directory tree (best-effort). */
static void rmdir_r(const char *path)
{
   DIR *dir = opendir(path);
   if (!dir)
   {
      remove(path);
      return;
   }
   struct dirent *ent;
   while ((ent = readdir(dir)) != NULL)
   {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
         continue;
      char full[4096];
      snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
      struct stat st;
      if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
         rmdir_r(full);
      else
         remove(full);
   }
   closedir(dir);
   rmdir(path);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* Local mode (no base URL): request must NOT include a "files" array. */
static void test_local_mode_no_files_array(void)
{
   reset_stub();
   g_remote_mode = 0;

   char tmpdir[] = "/tmp/aimee_idx_test_XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char p[4096];
   snprintf(p, sizeof(p), "%s/foo.c", tmpdir);
   write_file(p, "int x;\n", 7);

   kb_client_index_scan_result_t res;
   kb_client_index_scan("proj", tmpdir, 0, &res);

   assert(g_post_calls == 1);
   assert(g_last_body != NULL);

   cJSON *body = cJSON_Parse(g_last_body);
   assert(body != NULL);
   /* files array must be absent in local mode */
   assert(cJSON_GetObjectItemCaseSensitive(body, "files") == NULL);
   cJSON_Delete(body);

   rmdir_r(tmpdir);
   reset_stub();
}

/* Remote mode: request must include a "files" array with the indexed files. */
static void test_remote_mode_pushes_files(void)
{
   reset_stub();
   g_remote_mode = 1;

   char tmpdir[] = "/tmp/aimee_idx_test_XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   /* Source files that should be collected. */
   char p[4096];
   snprintf(p, sizeof(p), "%s/main.c", tmpdir);
   write_file(p, "int main(){}\n", 13);

   snprintf(p, sizeof(p), "%s/header.h", tmpdir);
   write_file(p, "/* header */\n", 13);

   /* Subdirectory with a Markdown file. */
   char sub[4096];
   snprintf(sub, sizeof(sub), "%s/docs", tmpdir);
   mkdir_p(sub);
   snprintf(p, sizeof(p), "%s/README.md", sub);
   write_file(p, "# readme\n", 9);

   /* Binary file (null byte) — must be skipped. */
   snprintf(p, sizeof(p), "%s/blob.bin", tmpdir);
   const char bin[] = {'a', '\0', 'b'};
   write_file(p, bin, 3);

   /* Unsupported extension — must be skipped. */
   snprintf(p, sizeof(p), "%s/build.o", tmpdir);
   write_file(p, "not source\n", 11);

   /* node_modules dir — must be skipped. */
   char nm[4096];
   snprintf(nm, sizeof(nm), "%s/node_modules", tmpdir);
   mkdir_p(nm);
   snprintf(p, sizeof(p), "%s/dep.js", nm);
   write_file(p, "module.exports={}\n", 18);

   kb_client_index_scan_result_t res;
   kb_client_index_scan("proj", tmpdir, 0, &res);

   assert(g_post_calls == 1);
   assert(g_last_body != NULL);

   cJSON *body = cJSON_Parse(g_last_body);
   assert(body != NULL);

   /* files array must be present. */
   cJSON *files = cJSON_GetObjectItemCaseSensitive(body, "files");
   assert(cJSON_IsArray(files));

   /* Collect rel_paths from the pushed payload. */
   int n = cJSON_GetArraySize(files);
   assert(n == 3); /* main.c, header.h, docs/README.md */

   int found_main = 0, found_header = 0, found_readme = 0, found_blob = 0, found_obj = 0,
       found_nm = 0;
   for (int i = 0; i < n; i++)
   {
      cJSON *entry = cJSON_GetArrayItem(files, i);
      cJSON *rp = cJSON_GetObjectItemCaseSensitive(entry, "rel_path");
      cJSON *ct = cJSON_GetObjectItemCaseSensitive(entry, "content");
      assert(cJSON_IsString(rp));
      assert(cJSON_IsString(ct));
      const char *rel = rp->valuestring;
      if (strcmp(rel, "main.c") == 0)
      {
         found_main = 1;
         assert(strstr(ct->valuestring, "int main"));
      }
      if (strcmp(rel, "header.h") == 0)
         found_header = 1;
      if (strcmp(rel, "docs/README.md") == 0)
         found_readme = 1;
      if (strstr(rel, "blob.bin"))
         found_blob = 1;
      if (strstr(rel, ".o"))
         found_obj = 1;
      if (strstr(rel, "node_modules"))
         found_nm = 1;
   }
   assert(found_main);
   assert(found_header);
   assert(found_readme);
   assert(!found_blob);
   assert(!found_obj);
   assert(!found_nm);

   cJSON_Delete(body);
   rmdir_r(tmpdir);
   reset_stub();
}

/* Remote mode: empty project root should still be handled safely
 * (the collect helper returns 0 files, not a crash). */
static void test_remote_mode_empty_dir(void)
{
   reset_stub();
   g_remote_mode = 1;

   char tmpdir[] = "/tmp/aimee_idx_test_XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   kb_client_index_scan_result_t res;
   kb_client_index_scan("proj", tmpdir, 0, &res);

   assert(g_post_calls == 1);
   cJSON *body = cJSON_Parse(g_last_body);
   assert(body != NULL);
   cJSON *files = cJSON_GetObjectItemCaseSensitive(body, "files");
   assert(cJSON_IsArray(files));
   assert(cJSON_GetArraySize(files) == 0);
   cJSON_Delete(body);

   rmdir_r(tmpdir);
   reset_stub();
}

/* Remote mode: a tree whose content exceeds the per-request batch budget must
 * be pushed as MULTIPLE /v1/code/scan requests, each under aimee-kb's 1 MB
 * request-body cap, with every file present across the batches. A single
 * whole-tree push would be truncated at the kb and 400 as invalid JSON. */
static void test_remote_mode_batches_large_tree(void)
{
   reset_stub();
   g_remote_mode = 1;

   char tmpdir[] = "/tmp/aimee_idx_test_XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   /* 5 x 200 KB source files = ~1 MB of content against the 600 KB batch
    * budget (each file stays under CODE_COLLECT_MAX_FILE_BYTES). */
   enum
   {
      NFILES = 5,
      FBYTES = 200 * 1024
   };
   char *content = malloc(FBYTES + 1);
   assert(content != NULL);
   memset(content, 'x', FBYTES);
   content[FBYTES] = '\0';
   for (int i = 0; i < NFILES; i++)
   {
      char p[4096];
      snprintf(p, sizeof(p), "%s/big%d.c", tmpdir, i);
      write_file(p, content, FBYTES);
   }
   free(content);

   kb_client_index_scan_result_t res;
   int rc = kb_client_index_scan("proj", tmpdir, 0, &res);

   assert(rc == 0);
   assert(res.skipped == 0);
   assert(g_post_calls >= 2);              /* batched, not one giant push */
   assert(g_files_pushed_total == NFILES); /* nothing dropped */
   assert(g_max_body_bytes < 1048576);     /* every body under the kb cap */

   rmdir_r(tmpdir);
   reset_stub();
}

/* Remote mode: missing/invalid project or root → error path, no crash. */
static void test_remote_mode_missing_args(void)
{
   reset_stub();
   g_remote_mode = 1;

   kb_client_index_scan_result_t res;
   int rc = kb_client_index_scan("proj", "", 0, &res);
   assert(rc == -1);
   assert(res.skipped == 1);

   rc = kb_client_index_scan("", "/some/path", 0, &res);
   assert(rc == -1);
   assert(res.skipped == 1);

   assert(g_post_calls == 0); /* no request fired */
   reset_stub();
}

static void test_explicit_ref_uses_atomic_generation_protocol(void)
{
   reset_stub();
   g_remote_mode = 1;
   g_generation_mode = 1;
   char tmpdir[] = "/tmp/aimee_idx_git_XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[8192], path[4096];
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' init -q -b main && git -C '%s' config user.email t@t && "
            "git -C '%s' config user.name t",
            tmpdir, tmpdir, tmpdir);
   assert(system(cmd) == 0);
   snprintf(path, sizeof(path), "%s/value.c", tmpdir);
   write_file(path, "int value(void){return 1;}\n", 27);
   snprintf(cmd, sizeof(cmd), "git -C '%s' add -A && git -C '%s' commit -qm main", tmpdir,
            tmpdir);
   assert(system(cmd) == 0);
   snprintf(cmd, sizeof(cmd), "git -C '%s' checkout -q -b feature", tmpdir);
   assert(system(cmd) == 0);
   write_file(path, "int value(void){return 2;}\n", 27);
   snprintf(cmd, sizeof(cmd), "git -C '%s' add -A && git -C '%s' commit -qm feature", tmpdir,
            tmpdir);
   assert(system(cmd) == 0);
   write_file(path, "UNCOMMITTED WIP MUST NOT INDEX\n", 30);

   kb_client_index_scan_result_t res;
   int rc = kb_client_index_scan_ref("repo-key", tmpdir, "feature", 0, &res);
   assert(rc == 0);
   assert(g_begin_calls == 1 && g_append_calls == 1 && g_finish_calls == 1);
   assert(g_post_calls == 3);
   assert(strcmp(g_generation_ref, "feature") == 0);
   assert(strlen(g_generation_commit) == 40 && strlen(g_generation_tree) == 40);
   assert(strcmp(g_generation_file_path, "value.c") == 0);
   assert(strstr(g_generation_file_content, "return 2") != NULL);
   assert(strstr(g_generation_file_content, "UNCOMMITTED") == NULL);
   assert(res.generation_id == 77);
   assert(strcmp(res.generation_state, "published") == 0);
   assert(strcmp(res.physical_project, "generation:1:77") == 0);
   assert(res.files == 1 && res.model_subjects == 1);

   rmdir_r(tmpdir);
   reset_stub();
}

static void test_current_hybrid_repairs_stale_generation_once(void)
{
   reset_stub();
   g_remote_mode = 1;
   g_generation_mode = 1;
   g_get_stale_once = 1;
   char tmpdir[] = "/tmp/aimee_hybrid_git_XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[8192], path[4096];
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' init -q -b main && git -C '%s' config user.email t@t && "
            "git -C '%s' config user.name t",
            tmpdir, tmpdir, tmpdir);
   assert(system(cmd) == 0);
   snprintf(path, sizeof(path), "%s/value.c", tmpdir);
   write_file(path, "int value(void){return 1;}\n", 27);
   snprintf(cmd, sizeof(cmd), "git -C '%s' add -A && git -C '%s' commit -qm main && "
                              "git -C '%s' checkout -qb feature",
            tmpdir, tmpdir, tmpdir);
   assert(system(cmd) == 0);

   int status = -1;
   char *json = kb_client_code_hybrid_current("needle", "value", "repo-key", tmpdir, 8,
                                               &status);
   assert(json != NULL);
   assert(status == 200);
   assert(g_get_calls == 2);
   assert(g_begin_calls == 1 && g_append_calls == 1 && g_finish_calls == 1);
   assert(g_last_get_path != NULL);
   assert(strstr(g_last_get_path, "/v1/code/hybrid?query=needle&max_results=8") != NULL);
   assert(strstr(g_last_get_path, "repository_key=repo-key") != NULL);
   assert(strstr(g_last_get_path, "source_ref=feature") != NULL);
   assert(strstr(g_last_get_path, "source_commit=") != NULL);
   assert(strstr(g_last_get_path, "symbol=value") != NULL);
   free(json);

   rmdir_r(tmpdir);
   reset_stub();
}

int main(void)
{
   test_local_mode_no_files_array();
   test_remote_mode_pushes_files();
   test_remote_mode_empty_dir();
   test_remote_mode_batches_large_tree();
   test_remote_mode_missing_args();
   test_explicit_ref_uses_atomic_generation_protocol();
   test_current_hybrid_repairs_stale_generation_once();
   printf("kb_client_index_remote: all tests passed\n");
   return 0;
}
