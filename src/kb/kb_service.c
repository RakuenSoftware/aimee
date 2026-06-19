#include "aimee.h"
#include "config.h" /* config_load — reembed default embedder */
#include "kb_background.h"
#include "kb_service.h"
#include "kb_service_code_embed.h"
#include "kb_service_kb.h"
#include "kb.h"
#include "cJSON.h"
#include "json_fluent.h" /* jo_ok */
#include "db2/kb_service_backend.h"
#include "db2/db2_internal.h"
#include "db2/pgvec_kb_service.h"
#include "db2/vector_verify.h"
#include "learning.h"
#include "log.h"
#include "memory.h"
#include "lifecycle.h"
#include "kb_vectors.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef AIMEE_WINDOWS
#include <unistd.h>
#endif

extern kb_service_ctx_t *g_kb_ctx;

/* ------------------------------------------------------------------ */
/* Connection-worker slot tracking                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
   int active;
   char method[64];
   time_t started_at;
} kb_conn_slot_t;

static kb_conn_slot_t g_kb_conn_slots[KB_WORKER_MAX];
static pthread_mutex_t g_kb_conn_slots_mu = PTHREAD_MUTEX_INITIALIZER;

char *kb_service_conn_slots_json(int configured)
{
   cJSON *arr = cJSON_CreateArray();
   pthread_mutex_lock(&g_kb_conn_slots_mu);
   time_t now = time(NULL);
   for (int i = 0; i < configured && i < KB_WORKER_MAX; i++)
   {
      kb_conn_slot_t *s = &g_kb_conn_slots[i];
      cJSON *w = cJSON_CreateObject();
      cJSON_AddNumberToObject(w, "index", i);
      cJSON_AddBoolToObject(w, "active", s->active);
      if (s->active)
      {
         cJSON_AddStringToObject(w, "method", s->method);
         cJSON_AddNumberToObject(w, "elapsed_secs", (double)(now - s->started_at));
      }
      cJSON_AddItemToArray(arr, w);
   }
   pthread_mutex_unlock(&g_kb_conn_slots_mu);
   char *out = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return out;
}

/* ------------------------------------------------------------------ */

static long kb_ipc_write(int fd, const char *buf, size_t len)
{
#ifdef AIMEE_WINDOWS
   HANDLE h = (HANDLE)(intptr_t)fd;
   DWORD written = 0;
   DWORD chunk = len > (size_t)((DWORD)~0u) ? (DWORD)~0u : (DWORD)len;
   if (!WriteFile(h, buf, chunk, &written, NULL))
   {
      errno = GetLastError() == ERROR_BROKEN_PIPE ? EPIPE : EIO;
      return -1;
   }
   return (long)written;
#else
   return (long)write(fd, buf, len);
#endif
}

/* Shared with kb_service_memory.c — keep external linkage. */
int kb_send_response(int fd, cJSON *resp)
{
   char *json = cJSON_PrintUnformatted(resp);
   if (!json)
      return -1;
   size_t len = strlen(json);
   size_t off = 0;
   while (off < len)
   {
      long n = kb_ipc_write(fd, json + off, len - off);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         free(json);
         return -1;
      }
      off += (size_t)n;
   }
   free(json);
   return kb_ipc_write(fd, "\n", 1) == 1 ? 0 : -1;
}

/* Shared with kb_service_memory.c — keep external linkage. */
int kb_send_error(int fd, const char *message)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "error");
   cJSON_AddStringToObject(resp, "message", message);
   int rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return rc;
}

/* Response epilogue shared by every kb_handle_* wrapper: when `resp` is NULL
 * (the backend failed) send an error envelope with `err_msg`; otherwise send
 * `resp`, free it, and return the send rc. */
int kb_reply_or_error(int fd, cJSON *resp, const char *err_msg)
{
   if (!resp)
      return kb_send_error(fd, err_msg);
   int rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return rc;
}

static int kb_handle_memory_rebuild(int fd, cJSON *req)
{
   cJSON *version_j = cJSON_GetObjectItemCaseSensitive(req, "version");
   const char *requested_version =
       (cJSON_IsString(version_j) && version_j->valuestring[0]) ? version_j->valuestring : NULL;

   char active_ver[256] = "";
   const char *version = requested_version;
   if (!version)
   {
      (void)db2_kb_service_get_active_embedder_version(active_ver, sizeof(active_ver));
      if (!active_ver[0])
         return kb_send_error(fd,
                              "no active embedder version; pass a version or run memory reembed");
      version = active_ver;
   }

   int failed = 0;
   int rebuilt = memory_rebuild_vector_index_for_version(version, &failed);

   if (rebuilt < 0)
      return kb_send_error(
          fd, "memory rebuild failed (another rebuild may be running; check rebuild_lock_held)");

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "version", version);
   cJSON_AddNumberToObject(resp, "rebuilt", rebuilt);
   cJSON_AddNumberToObject(resp, "failed", failed);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

static int kb_handle_reindex(int fd, cJSON *req)
{
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 0;

   if (!db2_is_initialized())
      return kb_send_error(fd, "failed to open knowledge service store");
   int rebuilt = memory_rebuild_derived_indexes(limit);
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "rebuilt", rebuilt);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

static int kb_handle_memory_repair(int fd, cJSON *req)
{
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   cJSON *failed_only_j = cJSON_GetObjectItemCaseSensitive(req, "failed_only");
   cJSON *reset_stuck_j = cJSON_GetObjectItemCaseSensitive(req, "reset_stuck");
   cJSON *memory_id_j = cJSON_GetObjectItemCaseSensitive(req, "memory_id");
   cJSON *embed_j = cJSON_GetObjectItemCaseSensitive(req, "embedding_command");

   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 0;
   int failed_only = cJSON_IsTrue(failed_only_j) ? 1 : 0;
   int reset_stuck = cJSON_IsTrue(reset_stuck_j) ? 1 : 0;
   int64_t memory_id = cJSON_IsNumber(memory_id_j) ? (int64_t)memory_id_j->valuedouble : 0;
   const char *embed_cmd =
       (cJSON_IsString(embed_j) && embed_j->valuestring[0]) ? embed_j->valuestring : "builtin";

   if (reset_stuck)
   {
      int reset = db2_kb_service_reset_stuck_vector_ops(8);

      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "mode", "reset_stuck");
      cJSON_AddNumberToObject(resp, "reset_stuck", reset);
      int srv_rc = kb_send_response(fd, resp);
      cJSON_Delete(resp);
      return srv_rc;
   }

   if (!db2_is_initialized())
      return kb_send_error(fd, "failed to open knowledge service store");
   if (pgvec_kb_service_ensure_memory_collection(384) != 0)
   {
      return kb_send_error(fd, "failed to initialize the memory retrieval index");
   }

   int repaired = 0;
   int failed = 0;
   const char *mode = "all";

   if (memory_id > 0)
   {
      mode = "single";
      if (memory_repair_vector_index(memory_id, embed_cmd) == 0)
         repaired = 1;
      else
         failed = 1;
   }
   else if (failed_only)
   {
      mode = "failed_only";
      repaired = memory_repair_vector_index_failed_only(embed_cmd, limit, &failed);
      if (repaired < 0)
      {
         return kb_send_error(fd, "failed to enumerate failed index ops");
      }
   }
   else
   {
      int64_t ids[1024];
      int id_count = db2_kb_service_list_memory_ids_by_updated(limit, ids, 1024);
      if (id_count < 0)
      {
         return kb_send_error(fd, "failed to enumerate memories");
      }

      for (int i = 0; i < id_count; i++)
      {
         if (memory_repair_vector_index(ids[i], embed_cmd) == 0)
            repaired++;
         else
            failed++;
      }
   }
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "mode", mode);
   cJSON_AddNumberToObject(resp, "repaired", repaired);
   cJSON_AddNumberToObject(resp, "failed", failed);
   if (memory_id > 0)
      cJSON_AddNumberToObject(resp, "memory_id", (double)memory_id);
   if (limit > 0)
      cJSON_AddNumberToObject(resp, "limit", limit);
   if (failed_only)
      cJSON_AddBoolToObject(resp, "failed_only", 1);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

/* Verify collection + DB2 state for `aimee memory verify`. Fetches the pgvector
 * snapshot, DB2 row counts for memories/units/kb_documents, index_ops summary
 * (and optional failed detail), probes the embedder, and optionally runs a
 * synthetic search timing.  DB1 reads (schema version / rebuild lock) stay on
 * the CLI side — the caller computes overall_ok from the returned data plus
 * those local reads, and separately calls memory.repair / memory.rebuild if
 * --repair surfaced problems. */
static int kb_handle_memory_verify(int fd, cJSON *req)
{
   cJSON *detail_j = cJSON_GetObjectItemCaseSensitive(req, "detail");
   cJSON *timings_j = cJSON_GetObjectItemCaseSensitive(req, "timings");
   cJSON *embed_j = cJSON_GetObjectItemCaseSensitive(req, "embedding_command");
   int do_detail = cJSON_IsTrue(detail_j) ? 1 : 0;
   int do_timings = cJSON_IsTrue(timings_j) ? 1 : 0;
   const char *embed_cmd =
       (cJSON_IsString(embed_j) && embed_j->valuestring[0]) ? embed_j->valuestring : "builtin";

   pgvec_verify_snapshot_t snap;
   memset(&snap, 0, sizeof(snap));
   (void)pgvec_verify_snapshot(&snap);

   db2_kb_service_memory_verify_t db2_verify;
   memset(&db2_verify, 0, sizeof(db2_verify));
   (void)db2_kb_service_collect_memory_verify(do_detail, 8, &db2_verify);
   db2_kb_service_verify_snapshot_t db2_snapshot;
   memset(&db2_snapshot, 0, sizeof(db2_snapshot));
   (void)db2_kb_service_collect_verify_snapshot(&db2_snapshot);

   float probe_vec[EMBED_MAX_DIM];
   int embedder_dim = memory_embed_text("probe", embed_cmd, probe_vec, EMBED_MAX_DIM);

   int timings_trials = 0;
   int64_t timings_total_us = 0;
   int64_t timings_max_us = 0;
   if (do_timings && snap.memory_exists > 0)
   {
      const int trials = 10;
      for (int i = 0; i < trials; i++)
      {
         float qvec[EMBED_MAX_DIM];
         char probe[64];
         snprintf(probe, sizeof(probe), "probe query %d", i);
         int qdim = memory_embed_text(probe, "builtin", qvec, EMBED_MAX_DIM);
         if (qdim <= 0)
            continue;
         int64_t ids[8];
         double scores[8];
         struct timespec t_start, t_end;
         clock_gettime(CLOCK_MONOTONIC, &t_start);
         int hits = pgvec_kb_service_search_memory_points("memory", qvec, qdim, 5, ids, scores, 8);
         clock_gettime(CLOCK_MONOTONIC, &t_end);
         if (hits < 0)
            continue;
         int64_t us =
             (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_nsec - t_start.tv_nsec) / 1000;
         timings_total_us += us;
         if (us > timings_max_us)
            timings_max_us = us;
         timings_trials++;
      }
   }

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "server_version", snap.server_version);
   cJSON_AddStringToObject(resp, "active_embedder_version", db2_snapshot.active_ver);

   cJSON *sch = cJSON_AddObjectToObject(resp, "schema");
   cJSON_AddStringToObject(sch, "expected", pgvec_schema_version());
   cJSON_AddStringToObject(sch, "stored", db2_verify.stored_schema_ver);

   cJSON_AddBoolToObject(resp, "rebuild_lock_held", db2_verify.rebuild_lock_held);

   cJSON *emb = cJSON_AddObjectToObject(resp, "embedder");
   cJSON_AddNumberToObject(emb, "dim", embedder_dim);

   cJSON *mem = cJSON_AddObjectToObject(resp, "memory");
   cJSON_AddStringToObject(mem, "collection", snap.memory_collection);
   cJSON_AddBoolToObject(mem, "collection_exists", snap.memory_exists > 0);
   cJSON_AddNumberToObject(mem, "db2_memories", (double)db2_snapshot.mem_rows);
   cJSON_AddNumberToObject(mem, "db2_units", (double)db2_snapshot.unit_rows);
   cJSON_AddNumberToObject(mem, "vector_points", (double)snap.memory_points);
   if (snap.memory_indexed_fields)
      cJSON_AddStringToObject(mem, "indexed_fields", snap.memory_indexed_fields);

   cJSON *kb = cJSON_AddObjectToObject(resp, "kb");
   cJSON_AddStringToObject(kb, "collection", snap.kb_collection);
   cJSON_AddBoolToObject(kb, "collection_exists", snap.kb_exists > 0);
   cJSON_AddNumberToObject(kb, "db2_chunks", (double)db2_snapshot.kb_rows);
   cJSON_AddNumberToObject(kb, "vector_points", (double)snap.kb_points);
   if (snap.kb_indexed_fields)
      cJSON_AddStringToObject(kb, "indexed_fields", snap.kb_indexed_fields);

   cJSON *ops_j = cJSON_AddObjectToObject(resp, "index_ops");
   cJSON_AddNumberToObject(ops_j, "ok", (double)db2_verify.ops.ok_ops);
   cJSON_AddNumberToObject(ops_j, "pending", (double)db2_verify.ops.pending_ops);
   cJSON_AddNumberToObject(ops_j, "failed", (double)db2_verify.ops.failed_ops);
   cJSON_AddNumberToObject(ops_j, "stuck", (double)db2_verify.ops.stuck_ops);

   if (do_detail)
   {
      cJSON *fails = cJSON_AddArrayToObject(resp, "failed_ops");
      for (int i = 0; i < db2_verify.failed_detail_count; i++)
      {
         cJSON *row = cJSON_CreateObject();
         cJSON_AddNumberToObject(row, "point_id", (double)db2_verify.failed_detail[i].point_id);
         cJSON_AddStringToObject(row, "collection", db2_verify.failed_detail[i].collection);
         cJSON_AddNumberToObject(row, "memory_id", (double)db2_verify.failed_detail[i].memory_id);
         cJSON_AddNumberToObject(row, "attempts", db2_verify.failed_detail[i].attempts);
         cJSON_AddStringToObject(row, "last_error", db2_verify.failed_detail[i].last_error);
         cJSON_AddStringToObject(row, "updated_at", db2_verify.failed_detail[i].updated_at);
         cJSON_AddItemToArray(fails, row);
      }
   }

   if (do_timings)
   {
      cJSON *tim = cJSON_AddObjectToObject(resp, "timings");
      cJSON_AddNumberToObject(tim, "trials", timings_trials);
      cJSON_AddNumberToObject(tim, "total_us", (double)timings_total_us);
      cJSON_AddNumberToObject(tim, "max_us", (double)timings_max_us);
   }

   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   pgvec_verify_snapshot_cleanup(&snap);
   return srv_rc;
}

/* memory.embed: single (memory_id>0) or batch (--all) re-embed at the
 * active embedder version.  For batch mode the caller passes the active
 * version so server-side config state is out of the loop. */
/* KB-side handler for the `code_embeddings_refresh` action: embeds the
 * (changed or all) code definitions of a project into code_embeddings and
 * records code_index_ops, via the shared kb_code_embed_refresh. Previously the
 * client wrapper (kb_client_code_embeddings_refresh) had no server-side
 * dispatch, so the action was a no-op — this wires it. */
static int kb_handle_code_embeddings_refresh(int fd, cJSON *req)
{
   cJSON *proj_j = cJSON_GetObjectItemCaseSensitive(req, "project");
   const char *project =
       (cJSON_IsString(proj_j) && proj_j->valuestring[0]) ? proj_j->valuestring : "";
   if (!project[0])
      return kb_send_error(fd, "code_embeddings_refresh requires project");

   cJSON *scope_j = cJSON_GetObjectItemCaseSensitive(req, "scope");
   const char *scope = (cJSON_IsString(scope_j) && scope_j->valuestring[0]) ? scope_j->valuestring
                                                                            : "changed_files";
   cJSON *bs_j = cJSON_GetObjectItemCaseSensitive(req, "batch_size");
   cJSON *mp_j = cJSON_GetObjectItemCaseSensitive(req, "max_points");
   int batch_size = cJSON_IsNumber(bs_j) ? (int)bs_j->valuedouble : 0;
   int max_points = cJSON_IsNumber(mp_j) ? (int)mp_j->valuedouble : 0;
   int dry_run = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "dry_run")) ? 1 : 0;

   const char **paths = NULL;
   int path_count = 0;
   cJSON *paths_j = cJSON_GetObjectItemCaseSensitive(req, "paths");
   if (cJSON_IsArray(paths_j))
   {
      int n = cJSON_GetArraySize(paths_j);
      if (n > 0 && (paths = calloc((size_t)n, sizeof(*paths))) != NULL)
      {
         for (int i = 0; i < n; i++)
         {
            cJSON *p = cJSON_GetArrayItem(paths_j, i);
            if (cJSON_IsString(p) && p->valuestring[0])
               paths[path_count++] = p->valuestring;
         }
      }
   }

   kb_code_embed_result_t out;
   memset(&out, 0, sizeof(out));
   int rc = kb_code_embed_refresh(project, scope, paths, path_count, batch_size, max_points,
                                  dry_run, &out);
   free(paths);
   if (rc != 0)
      return kb_send_error(fd, "code embeddings refresh failed");

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "project", out.project);
   cJSON_AddStringToObject(resp, "scope", out.scope);
   cJSON_AddNumberToObject(resp, "embedded", (double)out.embedded);
   cJSON_AddNumberToObject(resp, "skipped_unchanged", (double)out.skipped_unchanged);
   cJSON_AddNumberToObject(resp, "estimated_points", (double)out.estimated_points);
   cJSON_AddBoolToObject(resp, "dry_run", out.dry_run ? 1 : 0);
   cJSON_AddStringToObject(resp, "writer", out.writer);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

static int kb_handle_memory_embed(int fd, cJSON *req)
{
   cJSON *mem_j = cJSON_GetObjectItemCaseSensitive(req, "memory_id");
   cJSON *all_j = cJSON_GetObjectItemCaseSensitive(req, "all");
   cJSON *ver_j = cJSON_GetObjectItemCaseSensitive(req, "version");
   cJSON *embed_j = cJSON_GetObjectItemCaseSensitive(req, "embedding_command");
   int64_t memory_id = cJSON_IsNumber(mem_j) ? (int64_t)mem_j->valuedouble : 0;
   int all = cJSON_IsTrue(all_j) ? 1 : 0;
   const char *version = (cJSON_IsString(ver_j) && ver_j->valuestring[0]) ? ver_j->valuestring : "";
   const char *embed_cmd =
       (cJSON_IsString(embed_j) && embed_j->valuestring[0]) ? embed_j->valuestring : "builtin";

   if (!all && memory_id <= 0)
      return kb_send_error(fd, "memory.embed requires memory_id>0 or all=true");

   if (memory_id > 0)
   {
      int rc = memory_embed(memory_id, embed_cmd);
      if (rc != 0)
         return kb_send_error(fd, "memory embed failed");
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "mode", "single");
      cJSON_AddNumberToObject(resp, "memory_id", (double)memory_id);
      int srv_rc = kb_send_response(fd, resp);
      cJSON_Delete(resp);
      return srv_rc;
   }

   if (!version[0])
      return kb_send_error(fd, "memory.embed all=true requires version");

   int64_t ids[1024];
   int id_count = db2_kb_service_list_unembedded_memory_ids(version, ids, 1024);
   if (id_count < 0)
      return kb_send_error(fd, "failed to query memories");

   int success = 0, fail = 0;
   for (int i = 0; i < id_count; i++)
   {
      if (memory_embed(ids[i], embed_cmd) == 0)
         success++;
      else
         fail++;
   }

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "mode", "all");
   cJSON_AddNumberToObject(resp, "embedded", success);
   cJSON_AddNumberToObject(resp, "failed", fail);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

static int kb_handle_memory_reembed_status(int fd, cJSON *req)
{
   (void)req;
   char active_ver[256] = "";
   (void)db2_kb_service_get_active_embedder_version(active_ver, sizeof(active_ver));

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "active_version", active_ver);

   db2_kb_service_reembed_status_t st;
   memset(&st, 0, sizeof(st));
   (void)db2_kb_service_collect_reembed_status(&st);
   if (st.have_job)
   {
      cJSON *job = cJSON_AddObjectToObject(resp, "job");
      cJSON_AddStringToObject(job, "target_version", st.target_version);
      cJSON_AddNumberToObject(job, "last_id", st.last_id);
      cJSON_AddNumberToObject(job, "total", st.total);
      cJSON_AddNumberToObject(job, "done", st.done);
      cJSON_AddStringToObject(job, "started_at", st.started_at);
      cJSON_AddStringToObject(job, "finished_at", st.finished_at);
   }
   cJSON_AddBoolToObject(resp, "has_job", st.have_job);

   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

static int kb_handle_memory_reembed_rollback(int fd, cJSON *req)
{
   cJSON *ver_j = cJSON_GetObjectItemCaseSensitive(req, "version");
   if (!cJSON_IsString(ver_j) || !ver_j->valuestring[0])
      return kb_send_error(fd, "missing version");
   const char *version = ver_j->valuestring;

   int count = db2_kb_service_count_embeddings_for_version(version);
   if (count < 0)
      return kb_send_error(fd, "rollback: prepare failed");
   if (count == 0)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "rollback: no embeddings found for version '%s'", version);
      return kb_send_error(fd, msg);
   }

   char ts[32];
   now_utc(ts, sizeof(ts));
   if (db2_kb_service_set_active_embedder_version(version, ts) != 0)
      return kb_send_error(fd, "rollback: update failed");

   int rebuild_failed = 0;
   int rebuilt = memory_rebuild_vector_index_for_version(version, &rebuild_failed);
   if (rebuilt < 0)
      return kb_send_error(fd,
                           "rollback: failed to rebuild the vector index for requested version");

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "version", version);
   cJSON_AddNumberToObject(resp, "embedding_count", count);
   cJSON_AddNumberToObject(resp, "rebuilt", rebuilt);
   cJSON_AddNumberToObject(resp, "rebuild_failed", rebuild_failed);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

static int kb_handle_memory_reembed_cutover(int fd, cJSON *req)
{
   (void)req;
   db2_kb_service_reembed_status_t st;
   memset(&st, 0, sizeof(st));
   (void)db2_kb_service_collect_reembed_status(&st);
   if (!st.target_version[0])
      return kb_send_error(fd, "cutover: no completed job found — run reembed_start first");
   if (st.done == 0)
      return kb_send_error(fd, "cutover: job has zero embeddings — re-embed may not have run");

   char ts[32];
   now_utc(ts, sizeof(ts));
   if (db2_kb_service_set_active_embedder_version(st.target_version, ts) != 0)
      return kb_send_error(fd, "cutover: update failed");

   (void)db2_kb_service_mark_reembed_finished(ts);

   int rebuild_failed = 0;
   int rebuilt = memory_rebuild_vector_index_for_version(st.target_version, &rebuild_failed);
   if (rebuilt < 0)
      return kb_send_error(fd, "cutover: failed to rebuild the retrieval index for target version");

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "version", st.target_version);
   cJSON_AddNumberToObject(resp, "done", st.done);
   cJSON_AddNumberToObject(resp, "rebuilt", rebuilt);
   cJSON_AddNumberToObject(resp, "rebuild_failed", rebuild_failed);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

static int kb_handle_memory_reembed_start(int fd, cJSON *req)
{
   cJSON *ver_j = cJSON_GetObjectItemCaseSensitive(req, "version");
   cJSON *embed_j = cJSON_GetObjectItemCaseSensitive(req, "embedding_command");
   if (!cJSON_IsString(ver_j) || !ver_j->valuestring[0])
      return kb_send_error(fd, "missing version");
   const char *version = ver_j->valuestring;
   /* Default to the server's CONFIGURED embedder, never "builtin": builtin emits
    * 384-dim vectors, which can never insert into a real halfvec(1024)/(2560)
    * column (Postgres rejects "expected N dimensions, not 384") and silently
    * leaves memory_embeddings empty. Only fall back to builtin if no embedder is
    * configured at all (e.g. a 384-dim shim/test setup). */
   config_t reembed_cfg;
   config_load(&reembed_cfg);
   const char *embed_cmd =
       (cJSON_IsString(embed_j) && embed_j->valuestring[0])
           ? embed_j->valuestring
           : (reembed_cfg.embedding_command[0] ? reembed_cfg.embedding_command : "builtin");

   char ts_now[32];
   now_utc(ts_now, sizeof(ts_now));
   db2_kb_service_reembed_start_t start;
   memset(&start, 0, sizeof(start));
   if (db2_kb_service_prepare_reembed_start(version, ts_now, &start) != 0)
      return kb_send_error(fd, "reembed_start: progress upsert prepare failed");

   int success = 0, fail = 0, last_id = start.resume_last_id;
   for (;;)
   {
      int64_t ids[1024];
      int id_count = db2_kb_service_list_pending_reembed_memory_ids(version, last_id, ids, 1024);
      if (id_count < 0)
         return kb_send_error(fd, "reembed_start: id query failed");
      if (id_count == 0)
         break;

      for (int i = 0; i < id_count; i++)
      {
         int64_t mid = ids[i];
         if (memory_embed(mid, embed_cmd) == 0)
            success++;
         else
            fail++;
         last_id = (int)mid;

         if ((success + fail) % 50 == 0)
            (void)db2_kb_service_update_reembed_progress(last_id, success);
      }
   }

   (void)db2_kb_service_update_reembed_progress(last_id, success);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "version", version);
   cJSON_AddNumberToObject(resp, "total", start.total_count);
   cJSON_AddNumberToObject(resp, "resume_last_id", start.resume_last_id);
   cJSON_AddNumberToObject(resp, "embedded", success);
   cJSON_AddNumberToObject(resp, "failed", fail);
   cJSON_AddNumberToObject(resp, "last_id", last_id);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

#define KB_SCENE_LIST_MAX    100
#define KB_SCENE_MEMBERS_MAX 512

static int kb_handle_memory_scene_list(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_scene_list_json(KB_SCENE_LIST_MAX);
   return kb_reply_or_error(fd, resp, "failed to query scenes");
}

static int kb_handle_memory_scene_show(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "scene_id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing scene_id");
   int64_t scene_id = (int64_t)id_j->valuedouble;

   cJSON *resp = db2_kb_service_scene_members_json(scene_id, KB_SCENE_MEMBERS_MAX);
   return kb_reply_or_error(fd, resp, "failed to query scene members");
}

static int kb_handle_curiosity_list(int fd, cJSON *req)
{
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "state");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *state =
       (cJSON_IsString(state_j) && state_j->valuestring[0]) ? state_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 20;
   if (limit < 1)
      limit = 1;
   if (limit > 128)
      limit = 128;

   cJSON *resp = db2_kb_service_curiosity_list_json(state, limit);
   return kb_reply_or_error(fd, resp, "failed to list curiosity items");
}

static int kb_handle_curiosity_create(int fd, cJSON *req)
{
   cJSON *gap_j = cJSON_GetObjectItemCaseSensitive(req, "gap_type");
   cJSON *entity_j = cJSON_GetObjectItemCaseSensitive(req, "target_entity");
   cJSON *topic_j = cJSON_GetObjectItemCaseSensitive(req, "target_topic");
   cJSON *evidence_j = cJSON_GetObjectItemCaseSensitive(req, "evidence");
   cJSON *imp_j = cJSON_GetObjectItemCaseSensitive(req, "importance");
   cJSON *nov_j = cJSON_GetObjectItemCaseSensitive(req, "novelty");
   cJSON *sess_j = cJSON_GetObjectItemCaseSensitive(req, "source_session");
   if (!cJSON_IsString(gap_j))
      return kb_send_error(fd, "curiosity.create requires gap_type");

   const char *entity = cJSON_IsString(entity_j) ? entity_j->valuestring : NULL;
   const char *topic = cJSON_IsString(topic_j) ? topic_j->valuestring : NULL;
   const char *evidence = cJSON_IsString(evidence_j) ? evidence_j->valuestring : NULL;
   const char *sess = cJSON_IsString(sess_j) ? sess_j->valuestring : NULL;
   double importance = cJSON_IsNumber(imp_j) ? imp_j->valuedouble : 0.0;
   double novelty = cJSON_IsNumber(nov_j) ? nov_j->valuedouble : 0.0;

   cJSON *resp = db2_kb_service_curiosity_create_json(gap_j->valuestring, entity, topic, evidence,
                                                      importance, novelty, sess);
   return kb_reply_or_error(fd, resp, "failed to create curiosity item");
}

static int kb_handle_curiosity_sweep(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_curiosity_sweep_json();
   return kb_reply_or_error(fd, resp, "failed to sweep curiosity");
}

static int kb_handle_curiosity_rescore(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_curiosity_rescore_json();
   return kb_reply_or_error(fd, resp, "failed to rescore curiosity");
}

static int kb_handle_curiosity_get(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "curiosity.get requires id");
   cJSON *resp = db2_kb_service_curiosity_get_json((int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to get curiosity item");
}

static int kb_handle_curiosity_update_state(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "state");
   if (!cJSON_IsNumber(id_j) || !cJSON_IsString(state_j))
      return kb_send_error(fd, "curiosity.update_state requires id and state");
   cJSON *resp =
       db2_kb_service_curiosity_update_state_json((int64_t)id_j->valuedouble, state_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to update curiosity state");
}

static int kb_handle_curiosity_route_top(int fd, cJSON *req)
{
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   cJSON *sess_j = cJSON_GetObjectItemCaseSensitive(req, "source_session");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 5;
   const char *sess = cJSON_IsString(sess_j) ? sess_j->valuestring : NULL;
   cJSON *resp = db2_kb_service_curiosity_route_top_json(limit, sess);
   return kb_reply_or_error(fd, resp, "failed to route curiosity items");
}

static int kb_handle_note_create(int fd, cJSON *req)
{
   cJSON *title_j = cJSON_GetObjectItemCaseSensitive(req, "title");
   cJSON *content_j = cJSON_GetObjectItemCaseSensitive(req, "content");
   cJSON *tags_j = cJSON_GetObjectItemCaseSensitive(req, "tags");
   cJSON *author_j = cJSON_GetObjectItemCaseSensitive(req, "author");
   if (!cJSON_IsString(title_j) || !cJSON_IsString(content_j))
      return kb_send_error(fd, "notes.create requires title and content");
   const char *tags = cJSON_IsString(tags_j) ? tags_j->valuestring : NULL;
   const char *author = cJSON_IsString(author_j) ? author_j->valuestring : NULL;

   cJSON *resp =
       db2_kb_service_note_create_json(title_j->valuestring, content_j->valuestring, tags, author);
   return kb_reply_or_error(fd, resp, "failed to create note");
}

static int kb_handle_note_list(int fd, cJSON *req)
{
   cJSON *tag_j = cJSON_GetObjectItemCaseSensitive(req, "tag");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *tag = (cJSON_IsString(tag_j) && tag_j->valuestring[0]) ? tag_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 20;
   if (limit < 1)
      limit = 1;
   if (limit > 100)
      limit = 100;

   cJSON *resp = db2_kb_service_note_list_json(tag, limit);
   return kb_reply_or_error(fd, resp, "failed to list notes");
}

static int kb_handle_note_search(int fd, cJSON *req)
{
   cJSON *query_j = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (!cJSON_IsString(query_j))
      return kb_send_error(fd, "notes.search requires query");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 10;
   if (limit < 1)
      limit = 1;
   if (limit > 100)
      limit = 100;

   cJSON *resp = db2_kb_service_note_search_json(query_j->valuestring, limit);
   return kb_reply_or_error(fd, resp, "failed to search notes");
}

/* Rules + collab_rules + agent + maintenance + decision_log + anti_pattern
 * dispatch handlers live in kb_service_agent.c */
#include "kb_service_agent.h"
#include "kb_service_graph.h"

/* Memory.* dispatch handlers live in kb_service_memory.c */
#include "kb_service_memory.h"

/* artifacts.* dispatch handlers live in kb_service_artifacts.c */
#include "kb_service_artifacts.h"

/* roadmap.* dispatch handlers live in kb_service_roadmap.c */
#include "kb_service_roadmap.h"

static int kb_handle_learning_list(int fd, cJSON *req)
{
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "state");
   cJSON *sink_j = cJSON_GetObjectItemCaseSensitive(req, "sink");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *state =
       (cJSON_IsString(state_j) && state_j->valuestring[0]) ? state_j->valuestring : NULL;
   const char *sink =
       (cJSON_IsString(sink_j) && sink_j->valuestring[0]) ? sink_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 50;
   if (limit < 1)
      limit = 1;
   if (limit > 64)
      limit = 64;

   cJSON *resp = db2_kb_service_learning_list_json(state, sink, limit);
   return kb_reply_or_error(fd, resp, "failed to list learning proposals");
}

static int kb_handle_learning_mutate(int fd, cJSON *req, const char *verb)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing id");
   int id = (int)id_j->valuedouble;

   cJSON *proposal = NULL;
   int rc = 0;
   if (strcmp(verb, "get") == 0)
      proposal = db2_kb_service_learning_get_json(id);
   else if (strcmp(verb, "accept") == 0)
   {
      if (!db2_is_initialized())
         return kb_send_error(fd, "failed to open knowledge service store");
      learning_proposal_t row;
      rc = learning_accept_proposal(id, &row);
      if (rc == 0)
         proposal = learning_proposal_to_json(&row);
   }
   else if (strcmp(verb, "reject") == 0)
      proposal = db2_kb_service_learning_reject_json(id);
   else
      rc = -1;

   if (rc != 0 || !proposal)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "learning proposal %s failed", verb);
      return kb_send_error(fd, msg);
   }

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "proposal", proposal);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

/* Method -> handler dispatch for the uniform `int (int fd, cJSON *req)` kb RPCs.
 * The handful that do not fit that shape stay inline in kb_handle_request
 * (server.info / server.health, which read ctx; the learning.* mutate verbs,
 * which take an extra arg).
 *
 * Deliberately NOT served by aimee-kb (they need DB1 context the kb process
 * lacks) and are ported through aimee-server with DB2 writes routed back via
 * typed kb RPCs:
 *   - maintenance.eval_feedback_loop  (DB1 eval tasks)
 *   - maintenance.run, maintenance.trace_mine  (DB1 context)
 *   - memory.cognify_drain  (spans DB1 cognify_jobs + DB2 memories) */
typedef int (*kb_rpc_fn)(int fd, cJSON *req);
static const struct
{
   const char *method;
   kb_rpc_fn fn;
} kb_rpc_table[] = {
    {"kb.file.get", kb_handle_file_get},
    {"kb.maintenance.run", kb_handle_maintenance_run},
    {"kb.export", kb_handle_kb_export},
    {"kb.import", kb_handle_kb_import},
    {"memory.reindex", kb_handle_reindex},
    {"memory.rebuild", kb_handle_memory_rebuild},
    {"memory.repair", kb_handle_memory_repair},
    {"memory.verify", kb_handle_memory_verify},
    {"memory.embed", kb_handle_memory_embed},
    {"code_embeddings_refresh", kb_handle_code_embeddings_refresh},
    {"memory.reembed_start", kb_handle_memory_reembed_start},
    {"memory.reembed_status", kb_handle_memory_reembed_status},
    {"memory.reembed_cutover", kb_handle_memory_reembed_cutover},
    {"memory.reembed_rollback", kb_handle_memory_reembed_rollback},
    {"memory.scene_list", kb_handle_memory_scene_list},
    {"memory.scene_show", kb_handle_memory_scene_show},
    {"memory.directive_create", kb_handle_directive_create},
    {"memory.directive_resolve", kb_handle_directive_resolve},
    {"memory.directive_suppress", kb_handle_directive_suppress},
    {"memory.directive_sweep_expired", kb_handle_directive_sweep_expired},
    {"memory.directive_list", kb_handle_directive_list},
    {"curiosity.list", kb_handle_curiosity_list},
    {"curiosity.create", kb_handle_curiosity_create},
    {"curiosity.sweep", kb_handle_curiosity_sweep},
    {"curiosity.rescore", kb_handle_curiosity_rescore},
    {"curiosity.get", kb_handle_curiosity_get},
    {"curiosity.update_state", kb_handle_curiosity_update_state},
    {"curiosity.route_top", kb_handle_curiosity_route_top},
    {"notes.create", kb_handle_note_create},
    {"notes.list", kb_handle_note_list},
    {"notes.search", kb_handle_note_search},
    {"rules.list", kb_handle_rules_list},
    {"rules.generate", kb_handle_rules_generate},
    {"collab_rules.propose", kb_handle_collab_rules_propose},
    {"collab_rules.list", kb_handle_collab_rules_list},
    {"collab_rules.list_active", kb_handle_collab_rules_list_active},
    {"collab_rules.approve", kb_handle_collab_rules_approve},
    {"collab_rules.reject", kb_handle_collab_rules_reject},
    {"collab_rules.retire", kb_handle_collab_rules_retire},
    {"collab_rules.inject", kb_handle_collab_rules_inject},
    {"learning.propose_signal", kb_handle_learning_propose_signal},
    {"agent.outcome_record", kb_handle_agent_outcome_record},
    {"agent.hint_consume", kb_handle_agent_hint_consume},
    {"maintenance.anti_pattern_extract_from_feedback",
     kb_handle_anti_pattern_extract_from_feedback},
    {"maintenance.anti_pattern_extract_from_failures",
     kb_handle_anti_pattern_extract_from_failures},
    {"maintenance.anti_pattern_escalate", kb_handle_anti_pattern_escalate},
    {"maintenance.rules_decay", kb_handle_rules_decay},
    {"maintenance.calibrate_promotions", kb_handle_maintenance_calibrate_promotions},
    {"maintenance.compute_demotions", kb_handle_maintenance_compute_demotions},
    {"memory.record_retrieval_outcome", kb_handle_memory_record_retrieval_outcome},
    {"maintenance.memory_learn_style", kb_handle_memory_learn_style},
    {"decision_log.insert", kb_handle_decision_log_insert},
    {"decision_log.list", kb_handle_decision_log_list},
    {"anti_pattern.list", kb_handle_anti_pattern_list},
    {"anti_pattern.insert", kb_handle_anti_pattern_insert},
    {"anti_pattern.delete", kb_handle_anti_pattern_delete},
    {"anti_pattern.check", kb_handle_anti_pattern_check},
    {"anti_pattern.bump", kb_handle_anti_pattern_bump},
    {"maintenance.fold_session", kb_handle_memory_fold_session},
    {"rules.delete", kb_handle_rules_delete},
    {"rules.update_directive_type", kb_handle_rules_update_directive_type},
    {"feedback.record", kb_handle_feedback_record},
    {"maintenance.expire_session_directives", kb_handle_directive_expire_session},
    {"maintenance.scan_conversations", kb_handle_memory_scan_conversations},
    {"dashboard.memory_stats", kb_handle_dashboard_memory_stats},
    {"dashboard.logs", kb_handle_dashboard_logs},
    {"dashboard.reminders", kb_handle_dashboard_reminders},
    {"dashboard.recall", kb_handle_dashboard_recall},
    {"dashboard.directives", kb_handle_dashboard_directives},
    {"memory.find_facts", kb_handle_memory_find_facts},
    {"memory.list", kb_handle_memory_list},
    {"memory.get", kb_handle_memory_get},
    {"memory.load_eval_corpus", kb_handle_memory_load_eval_corpus},
    {"memory.top_l2_facts", kb_handle_memory_top_l2_facts},
    {"session_briefing.commitments", kb_handle_session_briefing_commitments},
    {"session_briefing.directives", kb_handle_session_briefing_directives},
    {"memory.prospective_list", kb_handle_memory_prospective_list},
    {"memory.prospective_create", kb_handle_memory_prospective_create},
    {"memory.prospective_complete", kb_handle_memory_prospective_complete},
    {"memory.prospective_match", kb_handle_memory_prospective_match},
    {"memory.prospective_mark_triggered", kb_handle_memory_prospective_mark_triggered},
    {"memory.get_provenance", kb_handle_memory_get_provenance},
    {"memory.tag_workspace", kb_handle_memory_tag_workspace},
    {"memory.scope_visibility_rank", kb_handle_memory_scope_visibility_rank},
    {"memory.episode_card_generate", kb_handle_memory_episode_card_generate},
    {"memory.tag_scope", kb_handle_memory_tag_scope},
    {"memory.prospective_sweep_expired", kb_handle_memory_prospective_sweep_expired},
    {"memory.maintenance_run", kb_handle_memory_maintenance_run},
    {"memory.lint", kb_handle_memory_lint},
    {"memory.alerts", kb_handle_memory_alerts},
    {"memory.recall", kb_handle_memory_recall},
    {"memory.upsert_workflow", kb_handle_memory_upsert_workflow},
    {"memory.delete", kb_handle_memory_delete},
    {"memory.touch", kb_handle_memory_touch},
    {"memory.update", kb_handle_memory_update},
    {"memory.reject", kb_handle_memory_reject},
    {"memory.stats", kb_handle_memory_stats},
    {"memory.list_conflicts", kb_handle_memory_list_conflicts},
    {"memory.query_health", kb_handle_memory_query_health},
    {"memory.effectiveness_stats", kb_handle_memory_effectiveness_stats},
    {"memory.query_edges", kb_handle_memory_query_edges},
    {"memory.compact_windows", kb_handle_memory_compact_windows},
    {"memory.assemble_context", kb_handle_memory_assemble_context},
    {"memory.search", kb_handle_memory_search},
    {"memory.export_jsonl", kb_handle_memory_export_jsonl},
    {"memory.decisions_export_jsonl", kb_handle_memory_decisions_export_jsonl},
    {"memory.key_exists", kb_handle_memory_key_exists},
    {"rules.export_jsonl", kb_handle_rules_export_jsonl},
    {"rules.insert", kb_handle_rules_insert},
    {"tool_registry.snapshot", kb_handle_tool_registry_snapshot},
    {"tool_registry.lookup", kb_handle_tool_registry_lookup},
    {"relations.schema_list", kb_handle_relations_schema_list},
    {"memory.find_facts_visible", kb_handle_memory_find_facts_visible},
    {"memory.find_facts_scoped", kb_handle_memory_find_facts_scoped},
    {"memory.diagnose_scoped", kb_handle_memory_diagnose_scoped},
    {"memory.explain_match", kb_handle_memory_explain_match},
    {"graph.sync_code", kb_handle_graph_sync_code},
    {"graph.explain", kb_handle_graph_explain},
    {"code.audit", kb_handle_code_audit},
    {"memory.link_create", kb_handle_memory_link_create},
    {"memory.link_query", kb_handle_memory_link_query},
    {"memory.link_delete", kb_handle_memory_link_delete},
    {"memory.store", kb_handle_memory_store},
    {"memory.find_id_by_key_kind", kb_handle_memory_find_id_by_key_kind},
    {"memory.search_facts_patterns_by_keyword", kb_handle_memory_search_facts_patterns_by_keyword},
    {"memory.supersede", kb_handle_memory_supersede},
    {"memory.fact_history", kb_handle_memory_fact_history},
    {"memory.check_drift", kb_handle_memory_check_drift},
    {"memory.list_session_scope_priority", kb_handle_memory_list_session_scope_priority},
    {"memory.list_session_scope_priority_like", kb_handle_memory_list_session_scope_priority_like},
    {"memory.list_low_effectiveness", kb_handle_memory_list_low_effectiveness},
    {"memory.list_unused_l2", kb_handle_memory_list_unused_l2},
    {"memory.list_superseded_keys", kb_handle_memory_list_superseded_keys},
    {"memory.set_artifact", kb_handle_memory_set_artifact},
    {"task.list", kb_handle_task_list},
    {"task.create", kb_handle_task_create},
    {"task.update_state", kb_handle_task_update_state},
    {"task.delete", kb_handle_task_delete},
    {"task.add_edge", kb_handle_task_add_edge},
    {"task.get_edges", kb_handle_task_get_edges},
    {"memory.briefing", kb_handle_memory_briefing},
    {"memory.context_block", kb_handle_memory_context_block},
    {"evidence.emit_retrieval_event", kb_handle_evidence_emit_retrieval_event},
    {"evidence.merge_retrieval_event", kb_handle_evidence_merge_retrieval_event},
    {"evidence.trace_retrieval_event", kb_handle_evidence_trace_retrieval_event},
    {"evidence.provenance_retrieval_event", kb_handle_evidence_provenance},
    {"evidence.fidelity_retrieval_event", kb_handle_evidence_fidelity},
    {"css.signals", kb_handle_css_signals},
    {"memory.entity_profile", kb_handle_memory_entity_profile},
    {"memory.entity_edges", kb_handle_memory_entity_edges},
    {"memory.search_graph", kb_handle_memory_search_graph},
    {"memory.search_graph_as_of", kb_handle_memory_search_graph_as_of},
    {"memory.get_episode", kb_handle_memory_get_episode},
    {"memory.ask", kb_handle_memory_ask},
    {"artifacts.list_proposed", kb_handle_artifacts_list_proposed},
    {"artifacts.set_state", kb_handle_artifacts_set_state},
    {"roadmap.create_from_decomposition", kb_handle_roadmap_create_from_decomposition},
    {"roadmap.validate", kb_handle_roadmap_validate},
    {"roadmap.show", kb_handle_roadmap_show},
    {"roadmap.list", kb_handle_roadmap_list},
    {"roadmap.rebuild", kb_handle_roadmap_rebuild},
    {"roadmap.report", kb_handle_roadmap_report},
    {"learning.list_proposals", kb_handle_learning_list},
};

static int kb_handle_request(kb_service_ctx_t *ctx, int fd, cJSON *req)
{
   ctx->last_session_rpc_ts = (long)time(NULL);

   cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
   if (!cJSON_IsString(method))
      return kb_send_error(fd, "missing method");

   if (strcmp(method->valuestring, "server.info") == 0)
   {
      cJSON *resp = jo_ok();
      cJSON_AddNumberToObject(resp, "protocol_version", 1);
      cJSON_AddStringToObject(resp, "server_version", AIMEE_VERSION);
      cJSON_AddStringToObject(resp, "service", "knowledge-service");
      int rc = kb_send_response(fd, resp);
      cJSON_Delete(resp);
      return rc;
   }

   if (strcmp(method->valuestring, "server.health") == 0)
   {
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "service", "knowledge-service");
      cJSON_AddNumberToObject(resp, "uptime", (double)(time(NULL) - (time_t)ctx->start_time));
      int rc = kb_send_response(fd, resp);
      cJSON_Delete(resp);
      return rc;
   }

   /* Method names are unique, so table order is irrelevant: first exact match
    * wins, exactly as the prior strcmp ladder did. */
   for (size_t i = 0; i < sizeof(kb_rpc_table) / sizeof(kb_rpc_table[0]); i++)
      if (strcmp(method->valuestring, kb_rpc_table[i].method) == 0)
         return kb_rpc_table[i].fn(fd, req);

   if (strcmp(method->valuestring, "learning.get_proposal") == 0)
      return kb_handle_learning_mutate(fd, req, "get");
   if (strcmp(method->valuestring, "learning.accept_proposal") == 0)
      return kb_handle_learning_mutate(fd, req, "accept");
   if (strcmp(method->valuestring, "learning.reject_proposal") == 0)
      return kb_handle_learning_mutate(fd, req, "reject");

   return kb_send_error(fd, "unknown method");
}

int kb_dispatch_action_json(const char *action, const char *body, int body_len, char *out_buf,
                            int out_cap)
{
   if (!out_buf || out_cap <= 0)
      return 500;
   out_buf[0] = '\0';
   if (!action || !action[0])
   {
      snprintf(out_buf, (size_t)out_cap, "{\"status\":\"error\",\"message\":\"missing action\"}");
      return 400;
   }
   if (strcmp(action, "v1.http") == 0 || strncmp(action, "server.", 7) == 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"status\":\"error\",\"message\":\"invalid action\"}");
      return 404;
   }
   if (!g_kb_ctx)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"error\",\"message\":\"knowledge service unavailable\"}");
      return 503;
   }

   cJSON *req = NULL;
   if (body && body_len > 0)
      req = cJSON_ParseWithLength(body, (size_t)body_len);
   if (!req)
      req = cJSON_CreateObject();
   if (!cJSON_IsObject(req))
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"error\",\"message\":\"action body must be an object\"}");
      return 400;
   }
   cJSON_DeleteItemFromObjectCaseSensitive(req, "method");
   cJSON_AddStringToObject(req, "method", action);

   FILE *tmp = tmpfile();
   if (!tmp)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"error\",\"message\":\"failed to buffer action response\"}");
      return 500;
   }

   int fd = fileno(tmp);
   int rc = kb_handle_request(g_kb_ctx, fd, req);
   cJSON_Delete(req);
   fflush(tmp);
   if (fseek(tmp, 0, SEEK_SET) != 0)
   {
      fclose(tmp);
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"error\",\"message\":\"failed to read action response\"}");
      return 500;
   }

   size_t n = fread(out_buf, 1, (size_t)out_cap - 1, tmp);
   out_buf[n] = '\0';
   fclose(tmp);
   while (n > 0 && (out_buf[n - 1] == '\n' || out_buf[n - 1] == '\r'))
      out_buf[--n] = '\0';
   if (n == 0)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"error\",\"message\":\"empty action response\"}");
      return 500;
   }
   int status = rc == 0 ? 200 : 500;
   cJSON *parsed = cJSON_Parse(out_buf);
   cJSON *status_j = parsed ? cJSON_GetObjectItemCaseSensitive(parsed, "status") : NULL;
   cJSON *message_j = parsed ? cJSON_GetObjectItemCaseSensitive(parsed, "message") : NULL;
   if (cJSON_IsString(status_j) && strcmp(status_j->valuestring, "error") == 0 &&
       cJSON_IsString(message_j) && strcmp(message_j->valuestring, "unknown method") == 0)
      status = 404;
   cJSON_Delete(parsed);
   return status;
}
