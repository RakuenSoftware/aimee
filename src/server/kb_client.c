#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kb_client.h"
#include "kb_client_cache.h"
#include "kb_client_internal.h"
#include "kb_client_mtls.h"
#include "agent_exec.h"
#include "cli_client.h"
#include "kb_paths.h"
#include "platform_ipc.h"
#include "platform_path.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef AIMEE_POSIX
#include <poll.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

typedef char *(*kb_client_error_json_fn)(const char *message);

/* Distributed-mode mTLS transport (kb_client_mtls.c) is linked into aimee and
 * aimee-server, where these are overridden by the real implementation. These
 * WEAK defaults make the dependency optional: a binary/test that links only
 * kb_client.o (no mTLS transport) simply sees "not configured" and falls through
 * to the HTTP-URL / Unix-socket transports — no extra link deps. */
__attribute__((weak)) int kb_client_mtls_configured(void)
{
   return 0;
}
__attribute__((weak)) char *kb_client_mtls_request(const char *method, const char *path,
                                                   const char *body, int *status_out)
{
   (void)method;
   (void)path;
   (void)body;
   if (status_out)
      *status_out = -1;
   return NULL;
}

static char *kb_v1_action_request_timeout(const char *action, cJSON *req, int timeout_ms,
                                          kb_client_error_json_fn error_json);

char *kb_client_query_escape(const char *s)
{
   if (!s)
      s = "";
   size_t len = strlen(s);
   char *out = malloc(len * 3 + 1);
   if (!out)
      return NULL;
   size_t j = 0;
   for (size_t i = 0; i < len; i++)
   {
      unsigned char c = (unsigned char)s[i];
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      {
         out[j++] = (char)c;
      }
      else
      {
         out[j++] = '%';
         out[j++] = "0123456789ABCDEF"[(c >> 4) & 0x0F];
         out[j++] = "0123456789ABCDEF"[c & 0x0F];
      }
   }
   out[j] = '\0';
   return out;
}

static char *kb_status_unavailable_json(const char *message)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return strdup("{}");
   cJSON_AddStringToObject(obj, "status", "error");
   cJSON_AddStringToObject(obj, "summary_status", "unavailable");
   cJSON_AddStringToObject(obj, "owner", "knowledge-service");
   cJSON_AddBoolToObject(obj, "available", 0);
   cJSON_AddStringToObject(obj, "message", message ? message : "knowledge service unavailable");
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{}");
}

static char *kb_vector_unavailable_json(const char *message)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return strdup("{}");
   cJSON_AddStringToObject(obj, "status", "unavailable");
   cJSON_AddStringToObject(obj, "owner", "knowledge-service");
   cJSON_AddBoolToObject(obj, "available", 0);
   cJSON_AddStringToObject(obj, "message", message ? message : "knowledge service unavailable");
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{}");
}

int kb_client_is_live(void)
{
   /* A configured remote endpoint owns its own reachability/timeout, so treat
    * it as "live" and let the actual call apply its timeout rather than probing
    * twice here. */
   return kb_client_v1_base_url() != NULL;
}

char *kb_client_health_json(void)
{
   int http_status = -1;
   char *body = kb_client_v1_get_json("/v1/health", CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (!body)
      return NULL;
   if (http_status < 200 || http_status >= 300)
   {
      free(body);
      return NULL;
   }
   return body;
}

/* Cached read of the KB's advertised typed-facts capability (proposal §8).
 * aimee-server gates per-turn fact injection on THIS instead of owning
 * typed_facts_enabled itself — the KB is the single source of truth. Short TTL so
 * a facts-off deployment does not fetch /v1/health every turn; a console/config
 * change is picked up within the TTL. Benign cross-thread race on the cache (worst
 * case: a couple of extra fetches). On transport failure the last-known value (or
 * off) is kept, so a briefly-unreachable KB never spuriously injects. */
int kb_client_typed_facts_enabled(void)
{
   static int cached = -1; /* -1 = never fetched */
   static time_t fetched_at = 0;
   const int TTL_S = 15;
   time_t now = time(NULL);
   if (cached >= 0 && (now - fetched_at) < TTL_S)
      return cached;
   int v = cached >= 0 ? cached : 0;
   char *h = kb_client_health_json();
   if (h)
   {
      cJSON *j = cJSON_Parse(h);
      free(h);
      if (j)
      {
         v = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "typed_facts_enabled")) ? 1 : 0;
         cJSON_Delete(j);
      }
   }
   cached = v;
   fetched_at = now;
   return v;
}

/* §2c: POST /v1/reembed {confirm, force, dry_run} — the dim-change reset. Returns
 * the raw response JSON (caller frees), or NULL on transport failure; *status_out
 * (optional) gets the HTTP status. A long timeout covers the DROP + schema re-apply. */
char *kb_client_reembed(int confirm, int force, int dry_run, int target_dim, int clear_maintenance,
                        int *status_out)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return NULL;
   if (clear_maintenance)
      cJSON_AddBoolToObject(req, "clear_maintenance", 1);
   cJSON_AddBoolToObject(req, "confirm", confirm ? 1 : 0);
   cJSON_AddBoolToObject(req, "force", force ? 1 : 0);
   cJSON_AddBoolToObject(req, "dry_run", dry_run ? 1 : 0);
   if (target_dim > 0)
      cJSON_AddNumberToObject(req, "target_dim", target_dim);
   char *resp = kb_client_v1_post_json("/v1/reembed", req, 120000, status_out);
   cJSON_Delete(req);
   return resp;
}

/* The curator observability block from aimee-kb's /v1/health (§4), returned as a
 * standalone JSON object for the server's GET /v1/kb/curator surface. Never
 * returns NULL: an unavailable kb / missing block yields a status-error object. */
char *kb_client_curator_json(void)
{
   char *health = kb_client_health_json();
   if (!health)
      return kb_status_unavailable_json("knowledge service /v1/health did not respond");
   cJSON *root = cJSON_Parse(health);
   free(health);
   cJSON *cur = root ? cJSON_DetachItemFromObjectCaseSensitive(root, "curator") : NULL;
   cJSON_Delete(root);
   if (!cur)
      return kb_status_unavailable_json("curator status unavailable");
   char *out = cJSON_PrintUnformatted(cur);
   cJSON_Delete(cur);
   return out ? out : kb_status_unavailable_json("curator status serialization failed");
}

int kb_client_health(kb_health_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   out->freshness_days = -1;

   int http_status = -1;
   char *body = kb_client_v1_get_json("/v1/health", CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (!body)
      return -1;
   cJSON *resp = cJSON_Parse(body);
   free(body);
   if (http_status < 200 || http_status >= 300 || !resp)
   {
      cJSON_Delete(resp);
      return -1;
   }
   if (!resp)
      return -1;

   cJSON *s = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(s) || strcmp(s->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return -1;
   }
   out->process_ok = 1;

#define COPY_BOOL(field, key)                                                                      \
   do                                                                                              \
   {                                                                                               \
      cJSON *_j = cJSON_GetObjectItemCaseSensitive(resp, key);                                     \
      if (cJSON_IsBool(_j))                                                                        \
         out->field = cJSON_IsTrue(_j) ? 1 : 0;                                                    \
   } while (0)
#define COPY_INT(field, key)                                                                       \
   do                                                                                              \
   {                                                                                               \
      cJSON *_j = cJSON_GetObjectItemCaseSensitive(resp, key);                                     \
      if (cJSON_IsNumber(_j))                                                                      \
         out->field = (int)_j->valuedouble;                                                        \
   } while (0)
#define COPY_STR(field, key)                                                                       \
   do                                                                                              \
   {                                                                                               \
      cJSON *_j = cJSON_GetObjectItemCaseSensitive(resp, key);                                     \
      if (cJSON_IsString(_j))                                                                      \
         snprintf(out->field, sizeof(out->field), "%s", _j->valuestring);                          \
   } while (0)

   COPY_BOOL(db2_ok, "db2_ok");
   COPY_BOOL(db2_kb_tables_ok, "db2_kb_tables_ok");
   COPY_BOOL(pgvec_ok, "pgvec_ok");
   COPY_BOOL(pgvec_collection_ok, "pgvec_collection_ok");
   COPY_INT(pgvec_vectors, "pgvec_vectors");
   COPY_INT(pgvec_indexed, "pgvec_indexed_vectors");
   COPY_BOOL(embed_ok, "embed_ok");
   COPY_STR(embed_command, "embed_command");
   COPY_INT(freshness_days, "freshness_days");
   COPY_STR(last_ingest_at, "last_ingest_at");
   COPY_INT(chunk_count, "chunk_count");
   COPY_INT(embedding_count, "embedding_count");

   /* maintenance fields */
   cJSON *lma = cJSON_GetObjectItemCaseSensitive(resp, "last_maintenance_at");
   if (cJSON_IsString(lma))
      snprintf(out->last_maintenance_at, sizeof(out->last_maintenance_at), "%s", lma->valuestring);

   cJSON *mdr = cJSON_GetObjectItemCaseSensitive(resp, "last_maintenance_rows_decayed");
   if (cJSON_IsNumber(mdr))
      out->last_maintenance_rows_decayed = (int)mdr->valuedouble;

   cJSON *mop = cJSON_GetObjectItemCaseSensitive(resp, "last_maintenance_orphans_pruned");
   if (cJSON_IsNumber(mop))
      out->last_maintenance_orphans_pruned = (int)mop->valuedouble;

   cJSON *men = cJSON_GetObjectItemCaseSensitive(resp, "maintenance_enabled");
   if (cJSON_IsBool(men))
      out->maintenance_enabled = cJSON_IsTrue(men) ? 1 : 0;

#undef COPY_BOOL
#undef COPY_INT
#undef COPY_STR

   cJSON *warns = cJSON_GetObjectItemCaseSensitive(resp, "warnings");
   if (cJSON_IsArray(warns))
   {
      size_t pos = 0;
      cJSON *w;
      cJSON_ArrayForEach(w, warns)
      {
         if (!cJSON_IsString(w))
            continue;
         if (pos > 0 && pos < sizeof(out->warnings) - 1)
            out->warnings[pos++] = '\n';
         size_t rem = sizeof(out->warnings) - pos - 1;
         if (rem == 0)
            break;
         size_t wlen = strlen(w->valuestring);
         if (wlen > rem)
            wlen = rem;
         memcpy(out->warnings + pos, w->valuestring, wlen);
         pos += wlen;
         out->warnings[pos] = '\0';
      }
   }

   cJSON_Delete(resp);
   return 0;
}

static char *kb_client_status_request(const char *project)
{
   char path[512];
   if (project && project[0])
   {
      char *encoded_project = kb_client_query_escape(project);
      if (!encoded_project)
         return kb_status_unavailable_json(
             "out of memory building knowledge service status request");
      int n = snprintf(path, sizeof(path), "/v1/health?status=1&project=%s", encoded_project);
      free(encoded_project);
      if (n < 0 || (size_t)n >= sizeof(path))
         return kb_status_unavailable_json("knowledge service status project name is too long");
   }
   else
      snprintf(path, sizeof(path), "/v1/health?status=1");

   int http_status = -1;
   char *resp = kb_client_v1_get_json(path, CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/health status returned HTTP %d",
               http_status);
      return kb_status_unavailable_json(msg);
   }
   return kb_status_unavailable_json("knowledge service /v1/health status did not respond");
}

char *kb_client_status_json(void)
{
   return kb_client_status_request(NULL);
}

char *kb_client_project_status_json(const char *project)
{
   return kb_client_status_request(project);
}

/* Long enough for a full force-rebuild of a large repo (kb_build can take
 * minutes).  If the operator wants to bound this further they can interrupt
 * the CLI which closes the socket and the server-side repair will continue
 * to completion — same trade-off as before this RPC existed. */
#define KB_CLIENT_REPAIR_TIMEOUT_MS (10 * 60 * 1000)

static char *kb_error_json(const char *message)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return strdup("{\"status\":\"error\"}");
   cJSON_AddStringToObject(obj, "status", "error");
   cJSON_AddStringToObject(obj, "message", message ? message : "knowledge service unavailable");
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{\"status\":\"error\"}");
}

char *kb_client_repair_json(const char *path, const char *project, const char *embedding_command)
{
   if (!path || !path[0] || !project || !project[0])
      return kb_error_json("repair requires path and project");

   cJSON *req = cJSON_CreateObject();
   if (!req)
      return kb_error_json("out of memory");
   cJSON_AddStringToObject(req, "path", path);
   cJSON_AddStringToObject(req, "project", project);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/maintenance/repair", req, KB_CLIENT_REPAIR_TIMEOUT_MS,
                                       &http_status);
   cJSON_Delete(req);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/maintenance/repair returned HTTP %d",
               http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/maintenance/repair did not respond");
}

char *kb_client_clear_json(const char *project)
{
   if (!project || !project[0])
      return kb_error_json("clear requires project");

   cJSON *req = cJSON_CreateObject();
   if (!req)
      return kb_error_json("out of memory");
   cJSON_AddStringToObject(req, "project", project);
   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/maintenance/clear", req, CLIENT_DEFAULT_TIMEOUT_MS,
                                       &http_status);
   cJSON_Delete(req);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/maintenance/clear returned HTTP %d",
               http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/maintenance/clear did not respond");
}

/* Synchronous build/update now run entirely inside aimee-kb (which owns DB2
 * and the filesystem) via the /v1/code/{build,update} endpoints — no
 * server-side compute or chunk push. */
static char *kb_client_code_post_json(const char *endpoint, const char *path, const char *project,
                                      const char *embedding_command, int force)
{
   if (!path || !path[0] || !project || !project[0])
      return kb_error_json("build/update requires path and project");

   cJSON *req = cJSON_CreateObject();
   if (!req)
      return kb_error_json("out of memory");
   cJSON_AddStringToObject(req, "path", path);
   cJSON_AddStringToObject(req, "project", project);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   if (force)
      cJSON_AddTrueToObject(req, "force");

   int http_status = -1;
   char *resp = kb_client_v1_post_json(endpoint, req, KB_CLIENT_REPAIR_TIMEOUT_MS, &http_status);
   cJSON_Delete(req);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[160];
      snprintf(msg, sizeof(msg), "knowledge service %s returned HTTP %d", endpoint, http_status);
      return kb_error_json(msg);
   }
   char msg[160];
   snprintf(msg, sizeof(msg), "knowledge service %s did not respond", endpoint);
   return kb_error_json(msg);
}

char *kb_client_build_json(const char *path, const char *project, const char *embedding_command,
                           int force)
{
   return kb_client_code_post_json("/v1/code/build", path, project, embedding_command,
                                   force ? 1 : 0);
}

char *kb_client_update_json(const char *path, const char *project, const char *embedding_command)
{
   return kb_client_code_post_json("/v1/code/update", path, project, embedding_command, 0);
}

char *kb_client_ingest_json(const char *workspace, const char *embedding_command, int force)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return kb_error_json("out of memory");
   if (workspace && workspace[0])
      cJSON_AddStringToObject(req, "workspace", workspace);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   if (force)
      cJSON_AddTrueToObject(req, "force");
   int http_status = -1;
   char *resp =
       kb_client_v1_post_json("/v1/ingest", req, KB_CLIENT_REPAIR_TIMEOUT_MS, &http_status);
   cJSON_Delete(req);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/ingest returned HTTP %d", http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/ingest did not respond");
}

char *kb_client_ingest_status_json(void)
{
   int http_status = -1;
   char *resp = kb_client_v1_get_json("/v1/ingest/status", CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/ingest/status returned HTTP %d",
               http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/ingest/status did not respond");
}

char *kb_client_queue_status_json(void)
{
   int http_status = -1;
   char *resp =
       kb_client_v1_get_json("/v1/pipeline/status", CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/pipeline/status returned HTTP %d",
               http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/pipeline/status did not respond");
}

char *kb_client_corpus_pipeline_status_json(void)
{
   int http_status = -1;
   char *resp =
       kb_client_v1_get_json("/v1/corpus/pipeline/status", CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/corpus/pipeline/status returned HTTP %d",
               http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/corpus/pipeline/status did not respond");
}

char *kb_client_job_status_json(int64_t job_id)
{
   if (job_id <= 0)
      return kb_error_json("invalid job id");
   char path[64];
   snprintf(path, sizeof(path), "/v1/jobs/%lld", (long long)job_id);
   int http_status = -1;
   char *resp = kb_client_v1_get_json(path, CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service %s returned HTTP %d", path, http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/jobs did not respond");
}

char *kb_client_queue_drain_json(const char *embedding_command, int timeout_secs)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return kb_error_json("out of memory");
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   cJSON_AddNumberToObject(req, "timeout", timeout_secs);
   /* Drains block until the queue empties or timeout elapses; size the
    * RPC timeout a bit above the caller's timeout so the network wait
    * outlasts the drain.  Cap at 10 minutes so a wedged drain can't
    * hang the CLI indefinitely. */
   int req_timeout = (timeout_secs > 0 ? timeout_secs + 30 : 600) * 1000;
   if (req_timeout > 10 * 60 * 1000)
      req_timeout = 10 * 60 * 1000;
   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/drain", req, req_timeout, &http_status);
   cJSON_Delete(req);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/drain returned HTTP %d", http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/drain did not respond");
}

char *kb_client_corpus_pipeline_drain_json(int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return kb_error_json("out of memory");
   cJSON_AddNumberToObject(req, "limit", limit);
   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/corpus/pipeline/drain", req, CLIENT_DEFAULT_TIMEOUT_MS,
                                       &http_status);
   cJSON_Delete(req);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/corpus/pipeline/drain returned HTTP %d",
               http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/corpus/pipeline/drain did not respond");
}

/* Reconcile scans DB2 and issues pgvector deletes; size the timeout generously
 * but bounded. */
#define KB_CLIENT_RECONCILE_TIMEOUT_MS (2 * 60 * 1000)

char *kb_client_reconcile_json(int dry_run)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return kb_error_json("out of memory");
   cJSON_AddBoolToObject(req, "dry_run", dry_run ? 1 : 0);
   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/maintenance/reconcile", req,
                                       KB_CLIENT_RECONCILE_TIMEOUT_MS, &http_status);
   cJSON_Delete(req);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/maintenance/reconcile returned HTTP %d",
               http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/maintenance/reconcile did not respond");
}

/* memory.reindex scans the memory corpus and rebuilds derived tables; give
 * it a bounded but generous timeout. */
#define KB_CLIENT_MEMORY_REINDEX_TIMEOUT_MS (5 * 60 * 1000)

char *kb_client_memory_reindex_json(int limit)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request_timeout("memory.reindex", req, KB_CLIENT_MEMORY_REINDEX_TIMEOUT_MS,
                                       kb_error_json);
}

/* Memory rebuild re-upserts every memory point to pgvector; size the timeout
 * like the repair path. */
#define KB_CLIENT_MEMORY_REBUILD_TIMEOUT_MS (10 * 60 * 1000)

char *kb_client_memory_rebuild_json(const char *version)
{
   cJSON *req = cJSON_CreateObject();
   if (version && version[0])
      cJSON_AddStringToObject(req, "version", version);
   return kb_v1_action_request_timeout("memory.rebuild", req, KB_CLIENT_MEMORY_REBUILD_TIMEOUT_MS,
                                       kb_error_json);
}

#define KB_CLIENT_DIRECTIVE_TIMEOUT_MS (60 * 1000)

static char *kb_v1_action_request_timeout(const char *action, cJSON *req, int timeout_ms,
                                          kb_client_error_json_fn error_json)
{
   if (!action || !action[0])
   {
      cJSON_Delete(req);
      return error_json("missing knowledge service action");
   }
   if (!req)
      req = cJSON_CreateObject();
   if (!req)
      return error_json("failed to allocate knowledge service request");

   char *escaped = kb_client_query_escape(action);
   if (!escaped)
   {
      cJSON_Delete(req);
      return error_json("failed to encode knowledge service action");
   }

   char path[256];
   snprintf(path, sizeof(path), "/v1/actions/%s", escaped);
   free(escaped);

   int http_status = -1;
   char *json = kb_client_v1_post_json(path, req, timeout_ms, &http_status);
   cJSON_Delete(req);
   if (json)
      return json;

   if (http_status >= 100)
   {
      char msg[160];
      snprintf(msg, sizeof(msg), "knowledge service action %s returned HTTP %d", action,
               http_status);
      return error_json(msg);
   }
   return error_json("knowledge service action did not respond");
}

/* Shared with kb_client_memory.c — keep external linkage. */
char *kb_v1_action_request(const char *action, cJSON *req)
{
   return kb_v1_action_request_timeout(action, req, KB_CLIENT_DIRECTIVE_TIMEOUT_MS, kb_error_json);
}

char *kb_client_memory_directive_create_json(const char *question, const char *topic,
                                             const char *entity, const char *file,
                                             const char *cause, int priority, const char *session,
                                             const char *valid_until)
{
   if (!question || !question[0])
      return kb_error_json("memory.directive_create requires question");
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "question", question);
   if (topic && topic[0])
      cJSON_AddStringToObject(req, "topic", topic);
   if (entity && entity[0])
      cJSON_AddStringToObject(req, "entity", entity);
   if (file && file[0])
      cJSON_AddStringToObject(req, "file", file);
   if (cause && cause[0])
      cJSON_AddStringToObject(req, "cause", cause);
   cJSON_AddNumberToObject(req, "priority", priority);
   if (session && session[0])
      cJSON_AddStringToObject(req, "session", session);
   if (valid_until && valid_until[0])
      cJSON_AddStringToObject(req, "valid_until", valid_until);
   return kb_v1_action_request("memory.directive_create", req);
}

char *kb_client_memory_directive_resolve_json(int64_t id, int64_t with_memory, const char *note)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   if (with_memory > 0)
      cJSON_AddNumberToObject(req, "with_memory", (double)with_memory);
   if (note && note[0])
      cJSON_AddStringToObject(req, "note", note);
   return kb_v1_action_request("memory.directive_resolve", req);
}

char *kb_client_memory_directive_suppress_json(int64_t id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   return kb_v1_action_request("memory.directive_suppress", req);
}

char *kb_client_memory_directive_sweep_expired_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_action_request("memory.directive_sweep_expired", req);
}

char *kb_client_memory_directive_list_json(const char *state, const char *cause, int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (state && state[0])
      cJSON_AddStringToObject(req, "state", state);
   if (cause && cause[0])
      cJSON_AddStringToObject(req, "cause", cause);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request("memory.directive_list", req);
}

char *kb_client_curiosity_list_json(const char *state, int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (state && state[0])
      cJSON_AddStringToObject(req, "state", state);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request("curiosity.list", req);
}

char *kb_client_curiosity_create_json(const char *gap_type, const char *target_entity,
                                      const char *target_topic, const char *evidence,
                                      double importance, double novelty, const char *source_session)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "gap_type", gap_type ? gap_type : "");
   if (target_entity && target_entity[0])
      cJSON_AddStringToObject(req, "target_entity", target_entity);
   if (target_topic && target_topic[0])
      cJSON_AddStringToObject(req, "target_topic", target_topic);
   if (evidence && evidence[0])
      cJSON_AddStringToObject(req, "evidence", evidence);
   cJSON_AddNumberToObject(req, "importance", importance);
   cJSON_AddNumberToObject(req, "novelty", novelty);
   if (source_session && source_session[0])
      cJSON_AddStringToObject(req, "source_session", source_session);
   return kb_v1_action_request("curiosity.create", req);
}

char *kb_client_curiosity_sweep_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_action_request("curiosity.sweep", req);
}

char *kb_client_curiosity_rescore_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_action_request("curiosity.rescore", req);
}

char *kb_client_curiosity_get_json(int64_t id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   return kb_v1_action_request("curiosity.get", req);
}

char *kb_client_curiosity_update_state_json(int64_t id, const char *new_state)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   cJSON_AddStringToObject(req, "state", new_state ? new_state : "");
   return kb_v1_action_request("curiosity.update_state", req);
}

char *kb_client_curiosity_route_top_json(int limit, const char *source_session)
{
   cJSON *req = cJSON_CreateObject();
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   if (source_session && source_session[0])
      cJSON_AddStringToObject(req, "source_session", source_session);
   return kb_v1_action_request("curiosity.route_top", req);
}

char *kb_client_note_create_json(const char *title, const char *content, const char *tags,
                                 const char *author)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "title", title ? title : "");
   cJSON_AddStringToObject(req, "content", content ? content : "");
   if (tags && tags[0])
      cJSON_AddStringToObject(req, "tags", tags);
   if (author && author[0])
      cJSON_AddStringToObject(req, "author", author);
   return kb_v1_action_request("notes.create", req);
}

char *kb_client_note_list_json(const char *tag, int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (tag && tag[0])
      cJSON_AddStringToObject(req, "tag", tag);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request("notes.list", req);
}

char *kb_client_note_search_json(const char *query, int limit)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query ? query : "");
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request("notes.search", req);
}

/* Rules + agent telemetry + maintenance + decision_log + anti_pattern
 * + feedback wrappers live in kb_client_agent.c. */
/* The memory.* RPC wrappers (find_facts, list, get, insert, briefing,
 * context_block, ask, entity_profile, entity_edges, search_graph,
 * get_episode) live in kb_client_memory.c so this file stays under
 * the per-file line cap. */
/* moved to kb_client_memory.c */

#define KB_CLIENT_LEARNING_TIMEOUT_MS (60 * 1000)

static char *kb_v1_learning_action_request(const char *method, cJSON *req)
{
   return kb_v1_action_request_timeout(method, req, KB_CLIENT_LEARNING_TIMEOUT_MS, kb_error_json);
}

char *kb_client_learning_list_proposals_json(const char *state, const char *sink, int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (state && state[0])
      cJSON_AddStringToObject(req, "state", state);
   if (sink && sink[0])
      cJSON_AddStringToObject(req, "sink", sink);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_learning_action_request("learning.list_proposals", req);
}

static char *kb_v1_learning_mutate(const char *method, int id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", id);
   return kb_v1_learning_action_request(method, req);
}

char *kb_client_learning_get_proposal_json(int id)
{
   return kb_v1_learning_mutate("learning.get_proposal", id);
}

char *kb_client_learning_accept_proposal_json(int id)
{
   return kb_v1_learning_mutate("learning.accept_proposal", id);
}

char *kb_client_learning_reject_proposal_json(int id)
{
   return kb_v1_learning_mutate("learning.reject_proposal", id);
}

/* artifacts.list_proposed / artifacts.set_state wrappers              */

char *kb_client_artifacts_list_proposed_json(const char *target_surface, int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (target_surface && target_surface[0])
      cJSON_AddStringToObject(req, "target_surface", target_surface);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_learning_action_request("artifacts.list_proposed", req);
}

char *kb_client_artifact_set_state_json(const char *id, const char *new_state,
                                        const char *verdict_tag, const char *verdict_scope,
                                        const char *counter_example, const char *reason)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "id", id ? id : "");
   cJSON_AddStringToObject(req, "new_state", new_state ? new_state : "");
   if (verdict_tag && verdict_tag[0])
      cJSON_AddStringToObject(req, "verdict_tag", verdict_tag);
   if (verdict_scope && verdict_scope[0])
      cJSON_AddStringToObject(req, "verdict_scope", verdict_scope);
   if (counter_example && counter_example[0])
      cJSON_AddStringToObject(req, "counter_example", counter_example);
   if (reason && reason[0])
      cJSON_AddStringToObject(req, "reason", reason);
   return kb_v1_learning_action_request("artifacts.set_state", req);
}

/* Repair sweeps the memories table and re-upserts into pgvector; size the timeout
 * like the rebuild path (10 minutes). */
#define KB_CLIENT_MEMORY_REPAIR_TIMEOUT_MS (10 * 60 * 1000)

char *kb_client_memory_repair_json(int limit, int failed_only, int reset_stuck, int64_t memory_id,
                                   const char *embedding_command)
{
   cJSON *req = cJSON_CreateObject();
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   cJSON_AddBoolToObject(req, "failed_only", failed_only ? 1 : 0);
   cJSON_AddBoolToObject(req, "reset_stuck", reset_stuck ? 1 : 0);
   if (memory_id > 0)
      cJSON_AddNumberToObject(req, "memory_id", (double)memory_id);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   return kb_v1_action_request_timeout("memory.repair", req, KB_CLIENT_MEMORY_REPAIR_TIMEOUT_MS,
                                       kb_error_json);
}

/* Verify runs a pgvector snapshot + DB2 scan; 30s is plenty for the normal
 * path and still under any reasonable timings budget. */
#define KB_CLIENT_MEMORY_VERIFY_TIMEOUT_MS (30 * 1000)

char *kb_client_memory_verify_json(int detail, int timings, const char *embedding_command)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddBoolToObject(req, "detail", detail ? 1 : 0);
   cJSON_AddBoolToObject(req, "timings", timings ? 1 : 0);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   return kb_v1_action_request_timeout("memory.verify", req, KB_CLIENT_MEMORY_VERIFY_TIMEOUT_MS,
                                       kb_error_json);
}

/* Embed paths are batch-heavy (reembed_start walks every stale memory).
 * Share the 10-minute budget used by repair/rebuild. */
#define KB_CLIENT_MEMORY_EMBED_TIMEOUT_MS (10 * 60 * 1000)

static char *kb_v1_memory_embed_action_request(const char *method, cJSON *req)
{
   return kb_v1_action_request_timeout(method, req, KB_CLIENT_MEMORY_EMBED_TIMEOUT_MS,
                                       kb_error_json);
}

char *kb_client_memory_embed_json(int all, int64_t memory_id, const char *version,
                                  const char *embedding_command)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddBoolToObject(req, "all", all ? 1 : 0);
   if (memory_id > 0)
      cJSON_AddNumberToObject(req, "memory_id", (double)memory_id);
   if (version && version[0])
      cJSON_AddStringToObject(req, "version", version);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   return kb_v1_memory_embed_action_request("memory.embed", req);
}

char *kb_client_memory_reembed_start_json(const char *version, const char *embedding_command)
{
   cJSON *req = cJSON_CreateObject();
   if (version && version[0])
      cJSON_AddStringToObject(req, "version", version);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   return kb_v1_memory_embed_action_request("memory.reembed_start", req);
}

char *kb_client_memory_reembed_status_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_memory_embed_action_request("memory.reembed_status", req);
}

char *kb_client_memory_reembed_cutover_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_memory_embed_action_request("memory.reembed_cutover", req);
}

char *kb_client_memory_reembed_rollback_json(const char *version)
{
   cJSON *req = cJSON_CreateObject();
   if (version && version[0])
      cJSON_AddStringToObject(req, "version", version);
   return kb_v1_memory_embed_action_request("memory.reembed_rollback", req);
}

char *kb_client_memory_scene_list_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_memory_embed_action_request("memory.scene_list", req);
}

char *kb_client_memory_scene_show_json(int64_t scene_id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "scene_id", (double)scene_id);
   return kb_v1_memory_embed_action_request("memory.scene_show", req);
}

/* Search may embed a query and hit pgvector; be generous but bounded so
 * a hung sidecar doesn't wedge the CLI indefinitely. */
#define KB_CLIENT_SEARCH_TIMEOUT_MS (2 * 60 * 1000)

const char *kb_client_v1_base_url(void)
{
   const char *url = getenv("AIMEE_KB_API_URL");
   return (url && url[0]) ? url : NULL;
}

static char *kb_client_v1_url(const char *path)
{
   const char *base = kb_client_v1_base_url();
   if (!base || !path || !path[0])
      return NULL;

   size_t base_len = strlen(base);
   while (base_len > 0 && base[base_len - 1] == '/')
      base_len--;
   size_t path_len = strlen(path);
   int path_has_slash = path[0] == '/';
   char *url = malloc(base_len + (path_has_slash ? 0 : 1) + path_len + 1);
   if (!url)
      return NULL;
   memcpy(url, base, base_len);
   size_t pos = base_len;
   if (!path_has_slash)
      url[pos++] = '/';
   memcpy(url + pos, path, path_len);
   url[pos + path_len] = '\0';
   return url;
}

static const char *kb_client_v1_auth_header(char *buf, size_t buf_len)
{
   /* The HTTP API can run without auth; include a bearer header only when the
    * operator provides the matching client-side token. */
   const char *token = getenv("AIMEE_KB_API_BEARER_TOKEN");
   if (!token || !token[0] || !buf || buf_len == 0)
      return NULL;
   snprintf(buf, buf_len, "Authorization: Bearer %s", token);
   return buf;
}

char *kb_client_v1_post_json(const char *path, cJSON *body, int timeout_ms, int *status_out)
{
   if (status_out)
      *status_out = -1;
   char *body_json = body ? cJSON_PrintUnformatted(body) : strdup("{}");
   if (!body_json)
      return NULL;

   /* Distributed mode: route to the remote kb over mTLS (see get_json). */
   if (kb_client_mtls_configured())
   {
      char *r = kb_client_mtls_request("POST", path, body_json, status_out);
      free(body_json);
      return r;
   }

   char *url = kb_client_v1_url(path);
   if (url)
   {
      char auth[320];
      char *response = NULL;
      int status = agent_http_post(url, kb_client_v1_auth_header(auth, sizeof(auth)), body_json,
                                   &response, timeout_ms, NULL);
      if (status_out)
         *status_out = status;
      free(body_json);
      free(url);
      if (status < 200 || status >= 300 || !response)
      {
         free(response);
         return NULL;
      }
      return response;
   }

   free(body_json);
   return NULL;
}

char *kb_client_v1_post_body(const char *path, const char *body, int timeout_ms, int *status_out)
{
   return kb_client_v1_post_body_with_type(path, body, "Content-Type: application/json", timeout_ms,
                                           status_out);
}

char *kb_client_v1_post_body_with_type(const char *path, const char *body, const char *content_type,
                                       int timeout_ms, int *status_out)
{
   if (status_out)
      *status_out = -1;

   char *url = kb_client_v1_url(path);
   if (url)
   {
      char auth[320];
      char *response = NULL;
      int status =
          agent_http_post_content_type(url, kb_client_v1_auth_header(auth, sizeof(auth)),
                                       content_type, body ? body : "", &response, timeout_ms, NULL);
      if (status_out)
         *status_out = status;
      free(url);
      if (status < 200 || status >= 300 || !response)
      {
         free(response);
         return NULL;
      }
      return response;
   }

   return NULL;
}

char *kb_client_v1_get_json(const char *path, int timeout_ms, int *status_out)
{
   if (status_out)
      *status_out = -1;

   /* Distributed mode: a configured remote kb (AIMEE_KB_CONN) is reached over
    * mTLS with the enrollment-issued client cert, ahead of HTTP-URL / socket. */
   if (kb_client_mtls_configured())
      return kb_client_mtls_request("GET", path, NULL, status_out);

   char *url = kb_client_v1_url(path);
   if (url)
   {
      char auth[320];
      char *response = NULL;
      int status =
          agent_http_get(url, kb_client_v1_auth_header(auth, sizeof(auth)), &response, timeout_ms);
      if (status_out)
         *status_out = status;
      free(url);
      if (status < 200 || status >= 300 || !response)
      {
         free(response);
         return NULL;
      }
      return response;
   }

   return NULL;
}

char *kb_client_search_json(const char *project, const char *query, const char *embedding_command,
                            int max_results, const char *format)
{
   return kb_client_search_json_ex(project, query, embedding_command, max_results, format, NULL);
}

char *kb_client_search_json_ex(const char *project, const char *query,
                               const char *embedding_command, int max_results, const char *format,
                               const char *fusion_mode_override)
{
   if (!query || !query[0])
      return kb_error_json("kb.search requires query");

   /* Short-TTL result cache (kb_client_cache.c): a hit skips the kb round-trip;
    * the kb_client_ws subscriber flushes the cache on every /v1/events
    * invalidation so results never outlive a release/ingest change. No-op
    * unless AIMEE_KB_CACHE_TTL_S (or config) enables it. */
   char cache_key[480];
   int have_key =
       snprintf(cache_key, sizeof(cache_key), "search|%s|%d|%s|%s|%s", query, max_results,
                project ? project : "", format ? format : "",
                fusion_mode_override ? fusion_mode_override : "") < (int)sizeof(cache_key);
   if (have_key)
   {
      char *cached = kb_cache_get(cache_key);
      if (cached)
         return cached;
   }

   cJSON *body = cJSON_CreateObject();
   if (!body)
      return kb_error_json("out of memory");
   if (project && project[0])
      cJSON_AddStringToObject(body, "project", project);
   cJSON_AddStringToObject(body, "query", query);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(body, "embedding_command", embedding_command);
   cJSON_AddNumberToObject(body, "max_results", max_results);
   if (format && format[0])
      cJSON_AddStringToObject(body, "format", format);
   if (fusion_mode_override && fusion_mode_override[0])
      cJSON_AddStringToObject(body, "fusion_mode", fusion_mode_override);
   int http_status = -1;
   char *resp =
       kb_client_v1_post_json("/v1/search", body, KB_CLIENT_SEARCH_TIMEOUT_MS, &http_status);
   cJSON_Delete(body);
   if (resp)
   {
      if (have_key)
         kb_cache_put(cache_key, resp);
      return resp;
   }
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/search returned HTTP %d", http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/search did not respond");
}

char *kb_client_curator_implements_json(const char *topic)
{
   cJSON *b = cJSON_CreateObject();
   if (!b)
      return NULL;
   cJSON_AddStringToObject(b, "topic", topic ? topic : "");
   int http = -1;
   char *r = kb_client_v1_post_json("/v1/implements", b, CLIENT_DEFAULT_TIMEOUT_MS, &http);
   cJSON_Delete(b);
   return r;
}

char *kb_client_curator_synthesize_json(const char *topic)
{
   cJSON *b = cJSON_CreateObject();
   if (!b)
      return NULL;
   cJSON_AddStringToObject(b, "topic", topic ? topic : "");
   int http = -1;
   char *r = kb_client_v1_post_json("/v1/synthesize", b, CLIENT_DEFAULT_TIMEOUT_MS, &http);
   cJSON_Delete(b);
   return r;
}

char *kb_client_curator_contradictions_json(int limit)
{
   cJSON *b = cJSON_CreateObject();
   if (!b)
      return NULL;
   cJSON_AddNumberToObject(b, "limit", limit);
   int http = -1;
   char *r = kb_client_v1_post_json("/v1/contradictions", b, CLIENT_DEFAULT_TIMEOUT_MS, &http);
   cJSON_Delete(b);
   return r;
}

char *kb_client_vector_status_json(void)
{
   char *status_json = kb_client_status_json();
   if (!status_json)
      return kb_vector_unavailable_json("knowledge service unavailable");

   cJSON *root = cJSON_Parse(status_json);
   free(status_json);
   if (!root)
      return kb_vector_unavailable_json("invalid knowledge service response");

   cJSON *vector = cJSON_GetObjectItemCaseSensitive(root, "vector");
   char *json = NULL;
   if (vector)
      json = cJSON_PrintUnformatted(vector);
   cJSON_Delete(root);
   return json ? json : kb_vector_unavailable_json("knowledge service returned no vector status");
}

char *kb_client_workers_json(void)
{
   return kb_client_v1_get_json("/v1/workers", CLIENT_DEFAULT_TIMEOUT_MS, NULL);
}

int kb_client_canonical_index_scan(const char *project, const char *root_path, int force)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return -1;
   cJSON_AddStringToObject(req, "project", project ? project : "");
   cJSON_AddStringToObject(req, "root_path", root_path ? root_path : "");
   if (force)
      cJSON_AddTrueToObject(req, "force");
   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/code/scan", req, 60000, &http_status);
   cJSON_Delete(req);
   int ok = (resp != NULL);
   free(resp);
   return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Intelligence readiness queries                                       */
/* ------------------------------------------------------------------ */

char *kb_client_calibrate_readiness_json(void)
{
   char *json = kb_client_v1_get_json("/v1/intelligence/calibration/readiness",
                                      CLIENT_DEFAULT_TIMEOUT_MS, NULL);
   return json ? json : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

char *kb_client_demote_check_json(void)
{
   char *json =
       kb_client_v1_get_json("/v1/intelligence/demotion/check", CLIENT_DEFAULT_TIMEOUT_MS, NULL);
   return json ? json : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

char *kb_client_ranker_export_view_json(void)
{
   char *json = kb_client_v1_get_json("/v1/intelligence/ranker/export-view",
                                      CLIENT_DEFAULT_TIMEOUT_MS, NULL);
   return json ? json : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

char *kb_client_ranker_fit_json(void)
{
   /* Fitting materializes the view, spawns the sidecar, and runs the benchmark
    * gate — allow a long window (mirrors the repair-class maintenance timeout). */
   cJSON *req = cJSON_CreateObject();
   char *json =
       kb_client_v1_post_json("/v1/intelligence/ranker/fit", req, KB_CLIENT_REPAIR_TIMEOUT_MS, NULL);
   cJSON_Delete(req);
   return json ? json : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

char *kb_client_bandit_export_json(void)
{
   char *json =
       kb_client_v1_get_json("/v1/intelligence/bandit/export", CLIENT_DEFAULT_TIMEOUT_MS, NULL);
   return json ? json : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

char *kb_client_bandit_replay_record_json(const char *decision_point, const char *result_json)
{
   if (!decision_point || !decision_point[0] || !result_json || !result_json[0])
      return strdup("{\"status\":\"error\",\"message\":\"decision_point and result required\"}");

   cJSON *body = cJSON_CreateObject();
   if (!body)
      return strdup("{\"status\":\"error\",\"message\":\"out of memory\"}");
   cJSON_AddStringToObject(body, "decision_point", decision_point);

   cJSON *result = cJSON_Parse(result_json);
   if (!result || !cJSON_IsObject(result))
   {
      cJSON_Delete(result);
      cJSON_Delete(body);
      return strdup("{\"status\":\"error\",\"message\":\"result must be a JSON object\"}");
   }
   cJSON_AddItemToObject(body, "result", result);

   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/intelligence/bandit/replay-record", body,
                                       CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   cJSON_Delete(body);
   return resp ? resp : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

/* Sample an arm for a server-side decision point via the kb DB2 bandit. On
 * success (status "ok"), fills arm_out + decision_id_out and returns 0. Returns
 * -1 when sampling is disabled (no optimize command), on transport failure, or
 * on a malformed response — the caller falls back to its default behaviour. */
int kb_client_bandit_sample(const char *decision_point, const char *const *arms, int n_arms,
                            char *arm_out, size_t arm_out_len, char *decision_id_out,
                            size_t decision_id_out_len)
{
   if (arm_out && arm_out_len)
      arm_out[0] = '\0';
   if (decision_id_out && decision_id_out_len)
      decision_id_out[0] = '\0';
   if (!decision_point || !decision_point[0] || !arms || n_arms < 1)
      return -1;

   cJSON *body = cJSON_CreateObject();
   if (!body)
      return -1;
   cJSON_AddStringToObject(body, "decision_point", decision_point);
   cJSON *aj = cJSON_CreateArray();
   for (int i = 0; i < n_arms; i++)
      if (arms[i] && arms[i][0])
         cJSON_AddItemToArray(aj, cJSON_CreateString(arms[i]));
   cJSON_AddItemToObject(body, "arms", aj);

   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/intelligence/bandit/sample", body,
                                       CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   cJSON_Delete(body);
   if (!resp)
      return -1;
   cJSON *r = cJSON_Parse(resp);
   free(resp);
   int rc = -1;
   if (r)
   {
      const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(r, "status"));
      const char *arm = cJSON_GetStringValue(cJSON_GetObjectItem(r, "arm"));
      const char *did = cJSON_GetStringValue(cJSON_GetObjectItem(r, "decision_id"));
      if (status && strcmp(status, "ok") == 0 && arm && did)
      {
         if (arm_out && arm_out_len)
            snprintf(arm_out, arm_out_len, "%s", arm);
         if (decision_id_out && decision_id_out_len)
            snprintf(decision_id_out, decision_id_out_len, "%s", did);
         rc = 0;
      }
   }
   cJSON_Delete(r);
   return rc;
}

/* Persist the production-default arm for a decision point. Returns the raw kb
 * response JSON (caller frees), which carries {status, rollback_arm}. */
char *kb_client_bandit_promote_json(const char *decision_point, const char *arm)
{
   if (!decision_point || !decision_point[0] || !arm || !arm[0])
      return strdup("{\"status\":\"error\",\"message\":\"decision_point and arm required\"}");
   cJSON *body = cJSON_CreateObject();
   if (!body)
      return strdup("{\"status\":\"error\",\"message\":\"out of memory\"}");
   cJSON_AddStringToObject(body, "decision_point", decision_point);
   cJSON_AddStringToObject(body, "arm", arm);
   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/intelligence/bandit/promote", body,
                                       CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   cJSON_Delete(body);
   return resp ? resp : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

/* Close a sampled decision with its observed reward [0,1]. Best-effort: a failed
 * close just means a missed learning sample, never a caller error. Returns 0. */
int kb_client_bandit_close(const char *decision_point, const char *decision_id, const char *arm_id,
                           double reward)
{
   if (!decision_point || !decision_point[0] || !decision_id || !decision_id[0] || !arm_id ||
       !arm_id[0])
      return -1;
   cJSON *body = cJSON_CreateObject();
   if (!body)
      return -1;
   cJSON_AddStringToObject(body, "decision_point", decision_point);
   cJSON_AddStringToObject(body, "decision_id", decision_id);
   cJSON_AddStringToObject(body, "arm_id", arm_id);
   cJSON_AddNumberToObject(body, "reward", reward);

   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/intelligence/bandit/close", body,
                                       CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   cJSON_Delete(body);
   free(resp); /* best-effort */
   return 0;
}
