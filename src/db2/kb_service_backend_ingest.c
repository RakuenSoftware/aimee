/* db2/kb_service_backend_ingest.c: kb_ingest_queue and kb_file_index
 * CRUD helpers — split from kb_service_backend.c to keep that file
 * under the 2000-line limit. */

#include "kb_service_backend.h"
#include "aimee.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>

#define KBS_ERRBUF 256

static const char *col_text_or_empty(aimee_pg_stmt_t *stmt, int col)
{
   const char *t = aimee_pg_column_text(stmt, col);
   return t ? t : "";
}

static void db2_kb_resolve_project(const char *project, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   if (project && project[0])
   {
      snprintf(out, out_len, "%s", project);
      return;
   }
   char path[MAX_PATH_LEN];
   if (!getcwd(path, sizeof(path)))
      path[0] = '\0';
   const char *base = strrchr(path, '/');
   snprintf(out, out_len, "%s", base ? base + 1 : path);
}

/* ── kb_ingest_queue ─────────────────────────────────────────────────────── */

int db2_kb_ingest_queue_reset_running(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *s =
       aimee_pg_prepare(conn,
                        "UPDATE kb_ingest_queue SET status = 'pending', started_at = NULL"
                        " WHERE status = 'running'",
                        err, sizeof(err));
   if (!s)
      return -1;
   (void)aimee_pg_step(s, err, sizeof(err));
   int reset = aimee_pg_stmt_changes(s);
   aimee_pg_finalize(s);
   return reset;
}

int db2_kb_ingest_queue_enqueue(const char *project, const char *root_path, const char *workspace,
                                int force)
{
   void *conn = db2_conn();
   if (!conn || !project || !root_path)
      return -1;

   char proj[256];
   db2_kb_resolve_project(project, proj, sizeof(proj));
   const char *ws = workspace ? workspace : "";

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *s =
       aimee_pg_prepare(conn,
                        "INSERT INTO kb_ingest_queue (project, root_path, workspace, force)"
                        " VALUES (?1, ?2, ?3, ?4)"
                        " ON CONFLICT (project) WHERE status IN ('pending', 'running')"
                        " DO UPDATE SET force = GREATEST(EXCLUDED.force, kb_ingest_queue.force)",
                        err, sizeof(err));
   if (!s)
      return -1;

   aimee_pg_bind_text(s, "?1", proj);
   aimee_pg_bind_text(s, "?2", root_path);
   aimee_pg_bind_text(s, "?3", ws);
   aimee_pg_bind_int(s, "?4", force);
   (void)aimee_pg_step(s, err, sizeof(err));
   int inserted = aimee_pg_stmt_changes(s);
   aimee_pg_finalize(s);
   return inserted; /* 0 = deduped, 1 = queued */
}

int db2_kb_ingest_queue_claim_next(db2_kb_ingest_job_t *out)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return -1;

   memset(out, 0, sizeof(*out));

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *s =
       aimee_pg_prepare(conn,
                        "UPDATE kb_ingest_queue SET status = 'running', started_at = pg_now_text()"
                        " WHERE id = ("
                        "   SELECT id FROM kb_ingest_queue WHERE status = 'pending'"
                        "   ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED"
                        " ) RETURNING id, project, root_path, workspace, force",
                        err, sizeof(err));
   if (!s)
      return -1;

   int found = 0;
   aimee_pg_step_t step = aimee_pg_step(s, err, sizeof(err));
   if (step == AIMEE_PG_ROW)
   {
      out->id = aimee_pg_column_int64(s, 0);
      snprintf(out->project, sizeof(out->project), "%s", col_text_or_empty(s, 1));
      snprintf(out->root_path, sizeof(out->root_path), "%s", col_text_or_empty(s, 2));
      snprintf(out->workspace, sizeof(out->workspace), "%s", col_text_or_empty(s, 3));
      out->force = aimee_pg_column_int(s, 4);
      found = 1;
   }
   else if (step != AIMEE_PG_DONE)
   {
      found = -1;
   }

   aimee_pg_finalize(s);
   return found;
}

int db2_kb_ingest_queue_complete(int64_t job_id, int files_indexed, int chunks_added,
                                 int embeddings_added)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *s =
       aimee_pg_prepare(conn,
                        "UPDATE kb_ingest_queue"
                        " SET status = 'done', completed_at = pg_now_text(),"
                        "     files_indexed = ?2, chunks_added = ?3, embeddings_added = ?4"
                        " WHERE id = ?1",
                        err, sizeof(err));
   if (!s)
      return -1;

   aimee_pg_bind_int64(s, "?1", job_id);
   aimee_pg_bind_int(s, "?2", files_indexed);
   aimee_pg_bind_int(s, "?3", chunks_added);
   aimee_pg_bind_int(s, "?4", embeddings_added);
   (void)aimee_pg_step(s, err, sizeof(err));
   aimee_pg_finalize(s);
   return 0;
}

int db2_kb_ingest_queue_fail(int64_t job_id, const char *error_message)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *s =
       aimee_pg_prepare(conn,
                        "UPDATE kb_ingest_queue"
                        " SET status = 'failed', completed_at = pg_now_text(), error_message = ?2"
                        " WHERE id = ?1",
                        err, sizeof(err));
   if (!s)
      return -1;

   aimee_pg_bind_int64(s, "?1", job_id);
   aimee_pg_bind_text(s, "?2", error_message ? error_message : "");
   (void)aimee_pg_step(s, err, sizeof(err));
   aimee_pg_finalize(s);
   return 0;
}

int db2_kb_ingest_queue_stats(db2_kb_ingest_queue_stats_t *out)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return -1;

   memset(out, 0, sizeof(*out));
   char err[KBS_ERRBUF] = "";

   aimee_pg_stmt_t *s = aimee_pg_prepare(
       conn, "SELECT status, COUNT(*) FROM kb_ingest_queue GROUP BY status", err, sizeof(err));
   if (!s)
      return -1;

   while (aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *st = col_text_or_empty(s, 0);
      int n = aimee_pg_column_int(s, 1);
      if (strcmp(st, "pending") == 0)
         out->pending = n;
      else if (strcmp(st, "running") == 0)
         out->running = n;
   }
   aimee_pg_finalize(s);

   s = aimee_pg_prepare(conn,
                        "SELECT COUNT(*) FROM kb_ingest_queue WHERE status = 'done'"
                        " AND completed_at >= pg_now_text('-1 day')",
                        err, sizeof(err));
   if (s && aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW)
      out->done_last_24h = aimee_pg_column_int(s, 0);
   aimee_pg_finalize(s);

   s = aimee_pg_prepare(conn,
                        "SELECT COUNT(*) FROM kb_ingest_queue WHERE status = 'failed'"
                        " AND completed_at >= pg_now_text('-1 day')",
                        err, sizeof(err));
   if (s && aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW)
      out->failed_last_24h = aimee_pg_column_int(s, 0);
   aimee_pg_finalize(s);

   return 0;
}

int db2_kb_ingest_queue_recent(db2_kb_ingest_recent_t *rows, int max_rows)
{
   void *conn = db2_conn();
   if (!conn || !rows || max_rows <= 0)
      return 0;

   char err[KBS_ERRBUF] = "";
   /* running/pending first (active work), then completed/failed by recency.
    * COALESCE(completed_at, started_at, queued_at) is always populated. */
   aimee_pg_stmt_t *s =
       aimee_pg_prepare(conn,
                        "SELECT project, status,"
                        "       COALESCE(completed_at, started_at, queued_at),"
                        "       files_indexed, chunks_added, error_message"
                        " FROM kb_ingest_queue"
                        " ORDER BY"
                        "   CASE status WHEN 'running' THEN 0 WHEN 'pending' THEN 1 ELSE 2 END,"
                        "   COALESCE(completed_at, started_at, queued_at) DESC NULLS LAST"
                        " LIMIT ?1",
                        err, sizeof(err));
   if (!s)
      return 0;

   aimee_pg_bind_int(s, "?1", max_rows);
   int n = 0;
   while (n < max_rows && aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      snprintf(rows[n].project, sizeof(rows[n].project), "%s", col_text_or_empty(s, 0));
      snprintf(rows[n].status, sizeof(rows[n].status), "%s", col_text_or_empty(s, 1));
      snprintf(rows[n].completed_at, sizeof(rows[n].completed_at), "%s", col_text_or_empty(s, 2));
      rows[n].files_indexed = aimee_pg_column_int(s, 3);
      rows[n].chunks_added = aimee_pg_column_int(s, 4);
      snprintf(rows[n].error_message, sizeof(rows[n].error_message), "%s", col_text_or_empty(s, 5));
      n++;
   }
   aimee_pg_finalize(s);
   return n;
}

/* ── kb_file_index ───────────────────────────────────────────────────────── */

/* Upsert a file's index row. |content| is the WHOLE original file text (served
 * back by kb_handle_file_get / GET /v1/kb/file). Pass NULL to update only the
 * hash/timestamp WITHOUT touching a previously stored body: the skip/dedup paths
 * call with NULL and must not erase content captured on a prior full index, so the
 * UPDATE coalesces NULL to the existing value rather than blanking it. */
int db2_kb_file_index_upsert(const char *project, const char *file_path, const char *file_hash,
                             const char *content)
{
   void *conn = db2_conn();
   if (!conn || !project || !file_path || !file_hash)
      return -1;

   char proj[256];
   db2_kb_resolve_project(project, proj, sizeof(proj));

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *s =
       aimee_pg_prepare(conn,
                        "INSERT INTO kb_file_index (project, file_path, file_hash, content)"
                        " VALUES (?1, ?2, ?3, ?4)"
                        " ON CONFLICT (project, file_path) DO UPDATE"
                        " SET file_hash = EXCLUDED.file_hash, ingested_at = pg_now_text(),"
                        "     content = COALESCE(EXCLUDED.content, kb_file_index.content)",
                        err, sizeof(err));
   if (!s)
      return -1;

   aimee_pg_bind_text(s, "?1", proj);
   aimee_pg_bind_text(s, "?2", file_path);
   aimee_pg_bind_text(s, "?3", file_hash);
   if (content)
      aimee_pg_bind_text(s, "?4", content);
   else
      aimee_pg_bind_null(s, "?4");
   (void)aimee_pg_step(s, err, sizeof(err));
   aimee_pg_finalize(s);
   return 0;
}

char *db2_kb_file_index_get_content(const char *project, const char *file_path)
{
   void *conn = db2_conn();
   if (!conn || !project || !file_path)
      return NULL;

   char proj[256];
   db2_kb_resolve_project(project, proj, sizeof(proj));

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(conn,
                                         "SELECT content FROM kb_file_index"
                                         " WHERE project = ?1 AND file_path = ?2",
                                         err, sizeof(err));
   if (!s)
      return NULL;

   aimee_pg_bind_text(s, "?1", proj);
   aimee_pg_bind_text(s, "?2", file_path);

   char *result = NULL;
   if (aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *val = col_text_or_empty(s, 0);
      if (val && val[0])
         result = strdup(val);
   }
   aimee_pg_finalize(s);
   return result;
}

int db2_kb_file_index_get(const char *project, const char *file_path, char *hash_out,
                          size_t hash_cap, char *ingested_at_out, size_t ingested_at_cap)
{
   void *conn = db2_conn();
   if (!conn || !project || !file_path)
      return 0;

   char proj[256];
   db2_kb_resolve_project(project, proj, sizeof(proj));

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(conn,
                                         "SELECT file_hash, ingested_at FROM kb_file_index"
                                         " WHERE project = ?1 AND file_path = ?2",
                                         err, sizeof(err));
   if (!s)
      return 0;

   aimee_pg_bind_text(s, "?1", proj);
   aimee_pg_bind_text(s, "?2", file_path);

   int found = 0;
   if (aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (hash_out && hash_cap)
         snprintf(hash_out, hash_cap, "%s", col_text_or_empty(s, 0));
      if (ingested_at_out && ingested_at_cap)
         snprintf(ingested_at_out, ingested_at_cap, "%s", col_text_or_empty(s, 1));
      found = 1;
   }
   aimee_pg_finalize(s);
   return found;
}

int db2_kb_file_index_delete_project(const char *project)
{
   void *conn = db2_conn();
   if (!conn || !project)
      return -1;

   char proj[256];
   db2_kb_resolve_project(project, proj, sizeof(proj));

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *s =
       aimee_pg_prepare(conn, "DELETE FROM kb_file_index WHERE project = ?1", err, sizeof(err));
   if (!s)
      return -1;

   aimee_pg_bind_text(s, "?1", proj);
   (void)aimee_pg_step(s, err, sizeof(err));
   int deleted = aimee_pg_stmt_changes(s);
   aimee_pg_finalize(s);
   return deleted;
}

cJSON *db2_kb_file_index_snapshot_json(const char *project)
{
   void *conn = db2_conn();
   if (!conn || !project)
      return cJSON_CreateArray();

   char proj[256];
   db2_kb_resolve_project(project, proj, sizeof(proj));

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       conn, "SELECT file_path, file_hash, ingested_at FROM kb_file_index WHERE project = ?1", err,
       sizeof(err));
   if (!s)
      return cJSON_CreateArray();

   aimee_pg_bind_text(s, "?1", proj);

   cJSON *arr = cJSON_CreateArray();
   while (aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      cJSON *entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "rel_path", col_text_or_empty(s, 0));
      cJSON_AddStringToObject(entry, "hash", col_text_or_empty(s, 1));
      cJSON_AddStringToObject(entry, "ingested_at", col_text_or_empty(s, 2));
      cJSON_AddItemToArray(arr, entry);
   }
   aimee_pg_finalize(s);
   return arr;
}
