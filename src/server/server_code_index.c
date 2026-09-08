/* Private code indexing: reuse the shipping collector and language extractors;
 * persist through the existing memory and PostgreSQL modules. */
#include "server_code_index.h"
#include "module_stage_adapters.h"
#include "aimee.h"
#include "index.h"
#include "kb_client.h"
#include "code_collect.h"
#include "json_fluent.h"
#include "modules/workspace/workspace_scope.h"
#include "ws_registry.h"
#include "log.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static pthread_t scan_thread;
static int scan_started;
static atomic_int scan_stop;
static pthread_mutex_t scan_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scan_wake = PTHREAD_COND_INITIALIZER;

static cJSON *extract_file(const cJSON *file)
{
   const cJSON *path_j = cJSON_GetObjectItemCaseSensitive(file, "rel_path");
   const cJSON *content_j = cJSON_GetObjectItemCaseSensitive(file, "content");
   const char *path = cJSON_IsString(path_j) ? path_j->valuestring : "";
   const char *content = cJSON_IsString(content_j) ? content_j->valuestring : "";
   const char *ext = strrchr(path, '.');
   if (!ext)
      ext = "";
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "rel_path", path);
   cJSON_AddStringToObject(out, "content", content);
   char identity[MAX_PATH_LEN] = "";
   (void)code_path_import_identity(path, identity, sizeof(identity));
   cJSON_AddStringToObject(out, "module_identity", identity);
   definition_t defs[256];
   int n = extract_definitions(ext, content, defs, 256);
   cJSON *array = cJSON_AddArrayToObject(out, "definitions");
   for (int i = 0; i < n; ++i)
   {
      cJSON *d = cJSON_CreateObject();
      cJSON_AddStringToObject(d, "name", defs[i].name);
      cJSON_AddStringToObject(d, "kind", defs[i].kind);
      cJSON_AddNumberToObject(d, "line", defs[i].line);
      cJSON_AddNumberToObject(d, "line_end", defs[i].line_end);
      cJSON_AddItemToArray(array, d);
   }
   call_ref_t calls[512];
   n = extract_calls(ext, content, calls, 512);
   array = cJSON_AddArrayToObject(out, "calls");
   for (int i = 0; i < n; ++i)
   {
      cJSON *d = cJSON_CreateObject();
      cJSON_AddStringToObject(d, "caller", calls[i].caller);
      cJSON_AddStringToObject(d, "callee", calls[i].callee);
      cJSON_AddNumberToObject(d, "line", calls[i].line);
      cJSON_AddItemToArray(array, d);
   }
   char *imports[128];
   int system_import[128] = {0};
   n = extract_imports_sys(ext, content, imports, system_import, 128);
   array = cJSON_AddArrayToObject(out, "imports");
   for (int i = 0; i < n; ++i)
   {
      if (!system_import[i] &&
          code_import_identity(path, imports[i], identity, sizeof(identity)) == 0)
         cJSON_AddItemToArray(array, cJSON_CreateString(identity));
      free(imports[i]);
   }
   return out;
}

static cJSON *call_code(const char *route, const cJSON *body, const cJSON *file)
{
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "code-index");
   cJSON *index = cJSON_AddObjectToObject(request, "code_index");
   cJSON_AddStringToObject(index, "route", route);
   const char *fields[] = {"project", "root_path", "phase", "scan_id"};
   for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
   {
      const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, fields[i]);
      if (cJSON_IsString(v))
         cJSON_AddStringToObject(index, fields[i], v->valuestring);
   }
   const cJSON *count = cJSON_GetObjectItemCaseSensitive(body, "expected_files");
   if (cJSON_IsNumber(count))
      cJSON_AddNumberToObject(index, "expected_files", count->valuedouble);
   if (file)
   {
      cJSON *files = cJSON_AddArrayToObject(index, "files");
      cJSON_AddItemToArray(files, extract_file(file));
   }
   cJSON *reply = server_module_memory_data(request);
   cJSON_Delete(request);
   cJSON *payload = cJSON_DetachItemFromObjectCaseSensitive(reply, "payload");
   cJSON_Delete(reply);
   return payload;
}

static char *local_code(const char *route, const void *body_v, int *status)
{
   if (atomic_load(&scan_stop))
   {
      *status = 503;
      return strdup("{\"status\":\"error\",\"error\":\"server is stopping\"}");
   }
   const cJSON *body = body_v;
   const cJSON *files = cJSON_GetObjectItemCaseSensitive(body, "files");
   cJSON *reply = NULL;
   int n = cJSON_IsArray(files) ? cJSON_GetArraySize(files) : 0;
   if (n)
   {
      /* One file per frame keeps extraction metadata within the bus limit. */
      for (int i = 0; i < n; ++i)
      {
         cJSON_Delete(reply);
         reply = call_code(route, body, cJSON_GetArrayItem(files, i));
         if (!reply)
            break;
      }
      if (reply)
      {
         cJSON_ReplaceItemInObjectCaseSensitive(reply, "files", cJSON_CreateNumber(n));
         cJSON_ReplaceItemInObjectCaseSensitive(reply, "inspected", cJSON_CreateNumber(n));
      }
   }
   else
      reply = call_code(route, body, NULL);
   *status = reply ? 200 : 503;
   if (!reply)
   {
      reply = cJSON_CreateObject();
      cJSON_AddStringToObject(reply, "status", "error");
      cJSON_AddStringToObject(reply, "error", "local code index unavailable or request invalid");
   }
   char *encoded = cJSON_PrintUnformatted(reply);
   cJSON_Delete(reply);
   return encoded;
}

/* The clone lifecycle calls this before removing files, under its existing lock. */
int gp_local_index_delete(const char *ref)
{
   if (!kb_client_local_code_enabled())
      return 0;
   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "project", ref);
   cJSON *reply = call_code("/v1/code/project/delete", body, NULL);
   cJSON_Delete(body);
   int rc = reply ? 0 : -1;
   cJSON_Delete(reply);
   return rc;
}

typedef struct
{
   char root[4096];
   char sha[64];
   ino_t inode;
   time_t created;
} scanned_repo_t;
static scanned_repo_t scanned_repos[512];
static int scanned_count;

static void scan_repo(const char *root, void *base_v)
{
   if (atomic_load(&scan_stop) || !kb_client_local_code_enabled())
      return;
   struct stat root_stat;
   if (lstat(root, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode))
      return;
   char sha[64] = "";
   int stable =
       !code_index_source_is_worktree() && git_resolve_default_sha(root, sha, sizeof(sha)) == 0;
   int slot = -1;
   for (int i = 0; i < scanned_count; ++i)
      if (strcmp(scanned_repos[i].root, root) == 0)
      {
         slot = i;
         if (stable && scanned_repos[i].inode == root_stat.st_ino &&
             scanned_repos[i].created == root_stat.st_ctime &&
             strcmp(scanned_repos[i].sha, sha) == 0)
            return;
         break;
      }
   const char *base = base_v;
   size_t length = strlen(base);
   if (strncmp(root, base, length) != 0 || root[length] != '/')
      return;
   const char *ref = root + length + 1;
   if (!ws_scope_project_ref_valid(ref, strlen(ref)))
      return;
   int lock = ws_reg_lock(ref);
   if (lock < 0)
      return;
   char remote[4096];
   struct stat st;
   if (ws_reg_lookup(ref, remote, sizeof(remote)) == 1 && lstat(root, &st) == 0 &&
       S_ISDIR(st.st_mode))
   {
      kb_client_index_scan_result_t result;
      if (kb_client_index_scan(ref, root, 0, &result) != 0)
         aimee_log(LOG_WARN, "code-index", "local repository scan pending; will retry");
      else if (stable)
      {
         if (slot < 0 && scanned_count < 512)
            slot = scanned_count++;
         if (slot >= 0)
         {
            snprintf(scanned_repos[slot].root, sizeof(scanned_repos[slot].root), "%s", root);
            snprintf(scanned_repos[slot].sha, sizeof(scanned_repos[slot].sha), "%s", sha);
            scanned_repos[slot].inode = root_stat.st_ino;
            scanned_repos[slot].created = root_stat.st_ctime;
         }
      }
   }
   close(lock);
}

static void *scan_main(void *unused)
{
   (void)unused;
   while (!atomic_load(&scan_stop))
   {
      char base[4096];
      if (kb_client_local_code_enabled() && ws_reg_ready() &&
          ws_scope_environment_root(base, sizeof(base)) == 0)
         code_collect_discover_repos(base, scan_repo, base);
      pthread_mutex_lock(&scan_mutex);
      if (!atomic_load(&scan_stop))
      {
         struct timespec next;
         clock_gettime(CLOCK_REALTIME, &next);
         next.tv_sec += 60;
         pthread_cond_timedwait(&scan_wake, &scan_mutex, &next);
      }
      pthread_mutex_unlock(&scan_mutex);
   }
   return NULL;
}

void server_code_index_start(void)
{
   kb_client_set_local_code_provider(local_code);
   atomic_store(&scan_stop, 0);
   scan_started = pthread_create(&scan_thread, NULL, scan_main, NULL) == 0;
}

void server_code_index_stop(void)
{
   atomic_store(&scan_stop, 1);
   pthread_mutex_lock(&scan_mutex);
   pthread_cond_signal(&scan_wake);
   pthread_mutex_unlock(&scan_mutex);
   if (scan_started)
      pthread_join(scan_thread, NULL);
   scan_started = 0;
}
