/* db2/kb_service_backend.c: kb-service backend SQL primitives — Postgres via libpq. */

#include "kb_service_backend.h"

#include "aimee.h"
#include "config.h"
#include "curiosity.h"
#include "epistemic_directives.h"
#include "notes.h"
#include "db2_internal.h"
#include "kb_payload.h"
#include "kb_runtime_state.h"
#include "pgvec_transport.h"
#include "vector_index_ops.h"
#include "code_index_ops.h"
#include "db_postgres.h"
#include "memory_scenes.h"
#include "platform_process.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define KBS_ERRBUF 256

static const char *DB2_KB_DIRECTIVE_SELECT_COLS =
    "id, question, topic, anchor_entity, anchor_file, cause, priority, state,"
    " memory_a_id, memory_b_id, resolution_memory_id, evidence, source_session,"
    " surfaced_count, last_surfaced_at, resolved_at, valid_until, created_at, updated_at";
static const char *DB2_KB_LEARNING_SELECT_COLS =
    "id, signal_id, sink, state, target_key, target_memory_id, action_json,"
    " evidence_refs, corroboration_count, expires_at, committed_at, archive_reason,"
    " created_at, updated_at";

static int db2_kb_directive_cause_valid(const char *cause)
{
   if (!cause)
      return 0;
   return strcmp(cause, "contradiction") == 0 || strcmp(cause, "retrieval_failure") == 0 ||
          strcmp(cause, "missing_config") == 0 || strcmp(cause, "user_follow_up") == 0;
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

static const char *col_text_or_empty(aimee_pg_stmt_t *stmt, int col)
{
   const char *t = aimee_pg_column_text(stmt, col);
   return t ? t : "";
}

static cJSON *db2_kb_directive_json_from_stmt(aimee_pg_stmt_t *stmt)
{
   cJSON *j = cJSON_CreateObject();
   if (!j)
      return NULL;

   cJSON_AddNumberToObject(j, "id", (double)aimee_pg_column_int64(stmt, 0));
   cJSON_AddStringToObject(j, "question", col_text_or_empty(stmt, 1));
   cJSON_AddStringToObject(j, "topic", col_text_or_empty(stmt, 2));
   cJSON_AddStringToObject(j, "anchor_entity", col_text_or_empty(stmt, 3));
   cJSON_AddStringToObject(j, "anchor_file", col_text_or_empty(stmt, 4));
   cJSON_AddStringToObject(j, "cause", col_text_or_empty(stmt, 5));
   cJSON_AddNumberToObject(j, "priority", aimee_pg_column_int(stmt, 6));
   cJSON_AddStringToObject(j, "state", col_text_or_empty(stmt, 7));
   cJSON_AddNumberToObject(j, "memory_a_id", (double)aimee_pg_column_int64(stmt, 8));
   cJSON_AddNumberToObject(j, "memory_b_id", (double)aimee_pg_column_int64(stmt, 9));
   cJSON_AddNumberToObject(j, "resolution_memory_id", (double)aimee_pg_column_int64(stmt, 10));
   cJSON_AddStringToObject(j, "evidence", col_text_or_empty(stmt, 11));
   cJSON_AddNumberToObject(j, "surfaced_count", aimee_pg_column_int(stmt, 13));
   cJSON_AddStringToObject(j, "last_surfaced_at", col_text_or_empty(stmt, 14));
   cJSON_AddStringToObject(j, "resolved_at", col_text_or_empty(stmt, 15));
   cJSON_AddStringToObject(j, "valid_until", col_text_or_empty(stmt, 16));
   cJSON_AddStringToObject(j, "created_at", col_text_or_empty(stmt, 17));
   return j;
}

static cJSON *db2_kb_service_directive_get_json(int64_t id)
{
   void *conn = db2_conn();
   if (!conn)
      return NULL;

   char sql[512];
   snprintf(sql, sizeof(sql), "SELECT %s FROM epistemic_directives WHERE id = ?1",
            DB2_KB_DIRECTIVE_SELECT_COLS);
   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!stmt)
      return NULL;

   aimee_pg_bind_int64(stmt, "?1", id);
   cJSON *row = NULL;
   if (aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
      row = db2_kb_directive_json_from_stmt(stmt);
   aimee_pg_finalize(stmt);
   return row;
}

static int db2_kb_service_directive_state_is_open(int64_t id)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       conn, "SELECT state FROM epistemic_directives WHERE id = ?1", err, sizeof(err));
   if (!stmt)
      return 0;

   aimee_pg_bind_int64(stmt, "?1", id);
   int is_open = 0;
   if (aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *s = aimee_pg_column_text(stmt, 0);
      is_open = (s && strcmp(s, "open") == 0);
   }
   aimee_pg_finalize(stmt);
   return is_open;
}

static cJSON *db2_kb_learning_json_from_stmt(aimee_pg_stmt_t *stmt)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;

   cJSON_AddNumberToObject(obj, "id", aimee_pg_column_int(stmt, 0));
   cJSON_AddNumberToObject(obj, "signal_id", aimee_pg_column_int(stmt, 1));
   cJSON_AddStringToObject(obj, "sink", col_text_or_empty(stmt, 2));
   cJSON_AddStringToObject(obj, "state", col_text_or_empty(stmt, 3));
   cJSON_AddStringToObject(obj, "target_key", col_text_or_empty(stmt, 4));
   cJSON_AddNumberToObject(obj, "target_memory_id", (double)aimee_pg_column_int64(stmt, 5));
   cJSON_AddStringToObject(obj, "action_json", col_text_or_empty(stmt, 6));
   cJSON_AddStringToObject(obj, "evidence_refs", col_text_or_empty(stmt, 7));
   cJSON_AddNumberToObject(obj, "corroboration_count", aimee_pg_column_int(stmt, 8));
   cJSON_AddStringToObject(obj, "expires_at", col_text_or_empty(stmt, 9));
   cJSON_AddStringToObject(obj, "committed_at", col_text_or_empty(stmt, 10));
   cJSON_AddStringToObject(obj, "archive_reason", col_text_or_empty(stmt, 11));
   cJSON_AddStringToObject(obj, "created_at", col_text_or_empty(stmt, 12));
   cJSON_AddStringToObject(obj, "updated_at", col_text_or_empty(stmt, 13));
   return obj;
}

static void db2_kb_learning_archive_expired(void)
{
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[KBS_ERRBUF] = "";
   /* pg_now_text() returns the DB2 canonical UTC text format. */
   (void)aimee_pg_exec(
       conn,
       "UPDATE learning_proposals"
       " SET state = 'archived', archive_reason = 'expired', updated_at = pg_now_text()"
       " WHERE state = 'pending' AND expires_at != '' AND expires_at < pg_now_text()",
       err, sizeof(err));
}

int db2_kb_service_reset_stuck_vector_ops(int max_attempts)
{
   /* Reset both the memory/evidence vector ops and the code-chunk ops so a
    * single `memory repair --reset-stuck` retries orphaned code embeds too. */
   return db2_vector_index_ops_reset_stuck(max_attempts) +
          db2_code_index_ops_reset_stuck(max_attempts);
}

int db2_kb_service_collect_memory_verify(int include_failed_detail, int max_attempts,
                                         db2_kb_service_memory_verify_t *out)
{
   if (!out)
      return -1;

   memset(out, 0, sizeof(*out));

   if (db2_vector_index_ops_summary(max_attempts, &out->ops) != 0)
      return -1;

   if (include_failed_detail)
      out->failed_detail_count = db2_vector_index_ops_list_failed(
          out->failed_detail, (int)(sizeof(out->failed_detail) / sizeof(out->failed_detail[0])));

   (void)db2_kb_runtime_state_get("vector_schema_version", out->stored_schema_ver,
                                  sizeof(out->stored_schema_ver));
   out->rebuild_lock_held = db2_kb_runtime_state_vector_rebuild_lock_held();

   return 0;
}

int db2_kb_service_async_queue_status(db2_kb_service_async_queue_stats_t *out)
{
   if (!out)
      return -1;

   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       conn, "SELECT status, COUNT(*) FROM kb_async_jobs GROUP BY status", err, sizeof(err));
   if (!stmt)
      return -1;

   while (aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *status = col_text_or_empty(stmt, 0);
      int count = aimee_pg_column_int(stmt, 1);
      if (strcmp(status, "pending") == 0)
         out->pending = count;
      else if (strcmp(status, "running") == 0)
         out->running = count;
      else if (strcmp(status, "done") == 0)
         out->done = count;
      else if (strcmp(status, "failed") == 0)
         out->failed = count;
      out->total += count;
   }

   aimee_pg_finalize(stmt);
   return 0;
}

int db2_kb_service_async_job_get(int64_t job_id, db2_kb_service_async_job_t *out)
{
   if (job_id <= 0 || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt =
       aimee_pg_prepare(conn,
                        "SELECT id, kind, document_id, project, status, attempts, last_error,"
                        " claimed_by, claimed_at, created_at, updated_at"
                        " FROM kb_async_jobs WHERE id = ?1",
                        err, sizeof(err));
   if (!stmt)
      return -1;

   aimee_pg_bind_int64(stmt, "?1", job_id);
   aimee_pg_step_t step = aimee_pg_step(stmt, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(stmt);
      return step == AIMEE_PG_DONE ? 0 : -1;
   }

   out->id = aimee_pg_column_int64(stmt, 0);
   db2_copy_text(out->kind, sizeof(out->kind), aimee_pg_column_text(stmt, 1));
   out->document_id = aimee_pg_column_int64(stmt, 2);
   db2_copy_text(out->project, sizeof(out->project), aimee_pg_column_text(stmt, 3));
   db2_copy_text(out->status, sizeof(out->status), aimee_pg_column_text(stmt, 4));
   out->attempts = aimee_pg_column_int(stmt, 5);
   db2_copy_text(out->last_error, sizeof(out->last_error), aimee_pg_column_text(stmt, 6));
   db2_copy_text(out->claimed_by, sizeof(out->claimed_by), aimee_pg_column_text(stmt, 7));
   db2_copy_text(out->claimed_at, sizeof(out->claimed_at), aimee_pg_column_text(stmt, 8));
   db2_copy_text(out->created_at, sizeof(out->created_at), aimee_pg_column_text(stmt, 9));
   db2_copy_text(out->updated_at, sizeof(out->updated_at), aimee_pg_column_text(stmt, 10));
   aimee_pg_finalize(stmt);
   return 1;
}

int db2_kb_service_async_queue_spawn_worker(void)
{
   char exe[MAX_PATH_LEN];
   if (platform_get_exe_path(exe, sizeof(exe)) != 0)
      return -1;

   const char *argv[] = {exe, "memory", "drain", NULL};
   return platform_spawn_daemon(argv) > 0 ? 0 : -1;
}

static int db2_kb_service_async_queue_claim_next(int64_t *job_id, int64_t *document_id, char *kind,
                                                 size_t kind_len)
{
   void *conn = db2_conn();
   if (!conn || !job_id || !document_id || !kind || kind_len == 0)
      return -1;

   char err[KBS_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   /* Single-claim correctness comes from the follow-up UPDATE's WHERE
    * id=?2 AND status='pending' guard plus the changes!=1 check below: if
    * two workers race on the same row, only one UPDATE will see status =
    * 'pending' and report changes==1; the loser ROLLBACKs and retries. */
   aimee_pg_stmt_t *sel = aimee_pg_prepare(conn,
                                           "SELECT id, document_id, kind"
                                           " FROM kb_async_jobs"
                                           " WHERE status = 'pending'"
                                           " ORDER BY id ASC LIMIT 1",
                                           err, sizeof(err));
   if (!sel)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }

   aimee_pg_step_t step = aimee_pg_step(sel, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(sel);
      (void)aimee_pg_exec(conn, "COMMIT", err, sizeof(err));
      return 0;
   }

   /* Materialize the SELECT row into locals before issuing the UPDATE on the
    * same conn (libpq one-active-result-per-conn). */
   int64_t id = aimee_pg_column_int64(sel, 0);
   int64_t doc_id = aimee_pg_column_int64(sel, 1);
   db2_copy_text(kind, kind_len, aimee_pg_column_text(sel, 2));
   aimee_pg_finalize(sel);

   aimee_pg_stmt_t *upd = aimee_pg_prepare(conn,
                                           "UPDATE kb_async_jobs"
                                           " SET status = 'running', claimed_by = ?1,"
                                           "     claimed_at = pg_now_text(),"
                                           "     attempts = attempts + 1,"
                                           "     updated_at = pg_now_text()"
                                           " WHERE id = ?2 AND status = 'pending'",
                                           err, sizeof(err));
   if (!upd)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }

   aimee_pg_bind_text(upd, "?1", session_id());
   aimee_pg_bind_int64(upd, "?2", id);
   aimee_pg_step_t urc = aimee_pg_step(upd, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(upd);
   aimee_pg_finalize(upd);
   if (urc != AIMEE_PG_DONE || changes != 1)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return 0;
   }

   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      return -1;

   *job_id = id;
   *document_id = doc_id;
   return 1;
}

static int db2_kb_service_async_queue_mark_job(int64_t job_id, const char *status,
                                               const char *error)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt =
       aimee_pg_prepare(conn,
                        "UPDATE kb_async_jobs"
                        " SET status = ?1, last_error = ?2, updated_at = pg_now_text()"
                        " WHERE id = ?3",
                        err, sizeof(err));
   if (!stmt)
      return -1;

   aimee_pg_bind_text(stmt, "?1", status);
   aimee_pg_bind_text(stmt, "?2", error ? error : "");
   aimee_pg_bind_int64(stmt, "?3", job_id);
   aimee_pg_step_t rc = aimee_pg_step(stmt, err, sizeof(err));
   aimee_pg_finalize(stmt);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

static int db2_kb_service_async_process_embed_raw(int64_t document_id, const char *embedding_cmd,
                                                  const char *vector_collection,
                                                  db2_kb_service_vector_upsert_fn vector_upsert,
                                                  void *vector_upsert_ctx, char *errbuf,
                                                  size_t errbuf_size)
{
   if (!vector_collection || !vector_collection[0] || !vector_upsert)
   {
      snprintf(errbuf, errbuf_size, "vector upsert callback missing");
      return -1;
   }

   void *conn = db2_conn();
   if (!conn)
   {
      snprintf(errbuf, errbuf_size, "no db2 conn");
      return -1;
   }

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       conn, "SELECT heading_path, content FROM kb_documents WHERE id = ?1", err, sizeof(err));
   if (!stmt)
   {
      snprintf(errbuf, errbuf_size, "prepare failed");
      return -1;
   }

   aimee_pg_bind_int64(stmt, "?1", document_id);
   if (aimee_pg_step(stmt, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(stmt);
      snprintf(errbuf, errbuf_size, "document %lld missing", (long long)document_id);
      return -1;
   }

   /* Materialize heading + content into local buffers before issuing the
    * follow-up pgvector write. */
   char heading_buf[1024];
   char content_buf[3072];
   db2_copy_text(heading_buf, sizeof(heading_buf), aimee_pg_column_text(stmt, 0));
   db2_copy_text(content_buf, sizeof(content_buf), aimee_pg_column_text(stmt, 1));
   aimee_pg_finalize(stmt);

   char embed_text[4096];
   if (heading_buf[0])
      snprintf(embed_text, sizeof(embed_text), "%s\n%s", heading_buf, content_buf);
   else
      snprintf(embed_text, sizeof(embed_text), "%s", content_buf);

   float vec[EMBED_MAX_DIM];
   int dim = memory_embed_text(embed_text, embedding_cmd, vec, EMBED_MAX_DIM);
   if (dim <= 0)
   {
      snprintf(errbuf, errbuf_size, "embedding generation failed");
      return -1;
   }
   char *payload_json = db2_kb_build_document_payload(document_id);
   if (!payload_json)
   {
      db2_vector_index_op_record(document_id, vector_collection, 0, 0, "payload build failed");
      snprintf(errbuf, errbuf_size, "payload build failed");
      return -1;
   }
   int upsert_rc = vector_upsert(document_id, vec, dim, payload_json, vector_upsert_ctx);
   free(payload_json);
   db2_vector_index_op_record(document_id, vector_collection, 0, upsert_rc == 0,
                              upsert_rc ? "upsert failed" : NULL);
   if (upsert_rc != 0)
   {
      snprintf(errbuf, errbuf_size, "vector upsert failed");
      return -1;
   }
   return 0;
}

/* embed_pdf: structured-PDF Phase A1. Embeds a PDF chunk into the DEDICATED
 * kb_pdf_embeddings relation (pgvec_kbpdf_upsert), never kb_embeddings — so PDF
 * vectors stay structurally unreachable from general vector search. Two
 * defense-in-depth guards before any vector is written: the row must still be a
 * PDF chunk (doc_kind='pdf') and must NOT be quarantined-pending. A pending row is
 * skipped (marked done, no vector); when it is later confirmed, the confirm path
 * re-enqueues an embed_pdf job. Returns 0 on success or benign skip, -1 on error
 * (which fails the job so kb_async_jobs retries the embed). */
static int db2_kb_service_async_process_embed_pdf(int64_t document_id, const char *embedding_cmd,
                                                  char *errbuf, size_t errbuf_size)
{
   void *conn = db2_conn();
   if (!conn)
   {
      snprintf(errbuf, errbuf_size, "no db2 conn");
      return -1;
   }

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       conn,
       "SELECT heading_path, content, doc_kind, quarantine_state FROM kb_documents WHERE id = ?1",
       err, sizeof(err));
   if (!stmt)
   {
      snprintf(errbuf, errbuf_size, "prepare failed");
      return -1;
   }
   aimee_pg_bind_int64(stmt, "?1", document_id);
   if (aimee_pg_step(stmt, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(stmt);
      /* The chunk was deleted/re-ingested before this job ran — nothing to embed.
       * Treat as a benign skip so the job does not spin on a missing row. */
      return 0;
   }
   char heading_buf[1024];
   char content_buf[3072];
   char doc_kind[32];
   char quarantine[32];
   db2_copy_text(heading_buf, sizeof(heading_buf), aimee_pg_column_text(stmt, 0));
   db2_copy_text(content_buf, sizeof(content_buf), aimee_pg_column_text(stmt, 1));
   db2_copy_text(doc_kind, sizeof(doc_kind), aimee_pg_column_text(stmt, 2));
   db2_copy_text(quarantine, sizeof(quarantine), aimee_pg_column_text(stmt, 3));
   aimee_pg_finalize(stmt);

   /* Defense-in-depth: never write a PDF vector for a non-PDF row or for ANY quarantined
    * document (quarantine_state non-empty), not just 'pending'. Withholding strictly more
    * can never leak; if a future state (e.g. legal_hold) is added, the embedder stays safe
    * by default. The structural isolation (separate relation) is the primary control; this
    * predicate is the second layer. */
   if (strcmp(doc_kind, "pdf") != 0 || quarantine[0] != '\0')
      return 0;

   char embed_text[4096];
   if (heading_buf[0])
      snprintf(embed_text, sizeof(embed_text), "%s\n%s", heading_buf, content_buf);
   else
      snprintf(embed_text, sizeof(embed_text), "%s", content_buf);

   float vec[EMBED_MAX_DIM];
   int dim = memory_embed_text(embed_text, embedding_cmd, vec, EMBED_MAX_DIM);
   if (dim <= 0)
   {
      snprintf(errbuf, errbuf_size, "embedding generation failed");
      return -1;
   }
   char *payload_json = db2_kb_build_document_payload(document_id);
   if (!payload_json)
   {
      db2_vector_index_op_record(document_id, PGVEC_KBPDF_TABLE, 0, 0, "payload build failed");
      snprintf(errbuf, errbuf_size, "payload build failed");
      return -1;
   }
   int upsert_rc = pgvec_kbpdf_upsert(document_id, vec, dim, payload_json);
   free(payload_json);
   db2_vector_index_op_record(document_id, PGVEC_KBPDF_TABLE, 0, upsert_rc == 0,
                              upsert_rc ? "pdf upsert failed" : NULL);
   if (upsert_rc != 0)
   {
      snprintf(errbuf, errbuf_size, "pdf vector upsert failed");
      return -1;
   }
   return 0;
}

int db2_kb_service_async_queue_drain(const char *embedding_cmd, int timeout_secs,
                                     const char *vector_collection,
                                     db2_kb_service_vector_upsert_fn vector_upsert,
                                     void *vector_upsert_ctx,
                                     db2_kb_service_async_queue_stats_t *out)
{
   const char *effective_cmd = config_embedding_command(NULL, embedding_cmd);
   int processed = 0;

   time_t started = time(NULL);
   db2_kb_service_async_queue_stats_t stats;
   memset(&stats, 0, sizeof(stats));

   for (;;)
   {
      int64_t job_id = 0;
      int64_t document_id = 0;
      char kind[32];
      kind[0] = '\0';
      int claim = db2_kb_service_async_queue_claim_next(&job_id, &document_id, kind, sizeof(kind));
      if (claim < 0)
         return -1;
      if (claim == 0)
      {
         if (db2_kb_service_async_queue_status(&stats) != 0)
            return -1;
         if (stats.running == 0)
            break;
         if (timeout_secs > 0 && (int)(time(NULL) - started) >= timeout_secs)
            break;
         usleep(100000);
         continue;
      }

      char errbuf[256] = "";
      int rc = -1;
      if (strcmp(kind, "embed_raw") == 0)
         rc = db2_kb_service_async_process_embed_raw(document_id, effective_cmd, vector_collection,
                                                     vector_upsert, vector_upsert_ctx, errbuf,
                                                     sizeof(errbuf));
      else if (strcmp(kind, "embed_pdf") == 0)
         rc = db2_kb_service_async_process_embed_pdf(document_id, effective_cmd, errbuf,
                                                     sizeof(errbuf));
      else
         snprintf(errbuf, sizeof(errbuf), "unknown job kind: %s", kind);

      if (rc == 0)
         (void)db2_kb_service_async_queue_mark_job(job_id, "done", "");
      else
         (void)db2_kb_service_async_queue_mark_job(job_id, "failed", errbuf);
      processed++;

      if (timeout_secs > 0 && (int)(time(NULL) - started) >= timeout_secs)
         break;
   }

   if (db2_kb_service_async_queue_status(&stats) != 0)
      return -1;
   stats.processed = processed;
   if (out)
      *out = stats;
   return 0;
}

int db2_kb_service_collect_project_status(const char *project, db2_kb_service_project_status_t *out)
{
   if (!out)
      return -1;

   memset(out, 0, sizeof(*out));

   char proj[256];
   db2_kb_resolve_project(project, proj, sizeof(proj));
   snprintf(out->project, sizeof(out->project), "%s", proj);

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(conn,
                                            "SELECT COUNT(*), COALESCE(SUM(token_count), 0),"
                                            " COUNT(DISTINCT file_path)"
                                            " FROM kb_documents WHERE project = ?1",
                                            err, sizeof(err));
   if (!stmt)
      return -1;

   aimee_pg_bind_text(stmt, "?1", proj);
   if (aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->chunks = aimee_pg_column_int(stmt, 0);
      out->tokens = aimee_pg_column_int(stmt, 1);
      out->files = aimee_pg_column_int(stmt, 2);
   }
   aimee_pg_finalize(stmt);

   stmt = aimee_pg_prepare(conn,
                           "SELECT COUNT(*) FROM vector_index_ops q"
                           " JOIN kb_documents d ON d.id = q.point_id"
                           " WHERE d.project = ?1 AND q.collection = 'kb_chunks'"
                           "   AND q.status = 'ok'",
                           err, sizeof(err));
   if (!stmt)
      return -1;

   aimee_pg_bind_text(stmt, "?1", proj);
   if (aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
      out->embeddings = aimee_pg_column_int(stmt, 0);
   aimee_pg_finalize(stmt);

   return db2_kb_service_async_queue_status(&out->queue);
}

int db2_kb_service_clear_project(const char *project)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char proj[256];
   db2_kb_resolve_project(project, proj, sizeof(proj));

   char err[KBS_ERRBUF] = "";

   /* Clean up vector_index_ops for this project's docs before the rows
    * disappear (no FK cascade from kb_documents to vector_index_ops). */
   aimee_pg_stmt_t *ops_stmt =
       aimee_pg_prepare(conn,
                        "DELETE FROM vector_index_ops WHERE point_id IN"
                        "  (SELECT id FROM kb_documents WHERE project = ?1)",
                        err, sizeof(err));
   if (ops_stmt)
   {
      aimee_pg_bind_text(ops_stmt, "?1", proj);
      (void)aimee_pg_step(ops_stmt, err, sizeof(err));
      aimee_pg_finalize(ops_stmt);
   }

   aimee_pg_stmt_t *stmt =
       aimee_pg_prepare(conn, "DELETE FROM kb_documents WHERE project = ?1", err, sizeof(err));
   if (!stmt)
      return -1;

   aimee_pg_bind_text(stmt, "?1", proj);
   aimee_pg_step_t rc = aimee_pg_step(stmt, err, sizeof(err));
   int deleted = aimee_pg_stmt_changes(stmt);
   aimee_pg_finalize(stmt);
   if (rc != AIMEE_PG_DONE)
      return -1;

   return deleted;
}

int db2_kb_service_collect_verify_snapshot(db2_kb_service_verify_snapshot_t *out)
{
   if (!out)
      return -1;

   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(conn, "SELECT COUNT(*) FROM memories", err, sizeof(err));
   if (s && aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW)
      out->mem_rows = aimee_pg_column_int64(s, 0);
   aimee_pg_finalize(s);

   s = aimee_pg_prepare(conn, "SELECT COUNT(*) FROM memory_units", err, sizeof(err));
   if (s && aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW)
      out->unit_rows = aimee_pg_column_int64(s, 0);
   aimee_pg_finalize(s);

   s = aimee_pg_prepare(conn, "SELECT COUNT(*) FROM kb_documents", err, sizeof(err));
   if (s && aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW)
      out->kb_rows = aimee_pg_column_int64(s, 0);
   aimee_pg_finalize(s);

   aimee_pg_stmt_t *avs = aimee_pg_prepare(
       conn, "SELECT version FROM memory_active_embedder WHERE id = 1", err, sizeof(err));
   if (avs && aimee_pg_step(avs, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(avs, 0);
      if (v)
         snprintf(out->active_ver, sizeof(out->active_ver), "%s", v);
   }
   aimee_pg_finalize(avs);

   return 0;
}

int db2_kb_service_get_active_embedder_version(char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;

   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *avs = aimee_pg_prepare(
       conn, "SELECT version FROM memory_active_embedder WHERE id = 1", err, sizeof(err));
   if (!avs)
      return -1;

   int found = 0;
   if (aimee_pg_step(avs, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(avs, 0);
      if (v)
      {
         snprintf(out, out_len, "%s", v);
         found = 1;
      }
   }
   aimee_pg_finalize(avs);
   return found ? 0 : -1;
}

int db2_kb_service_set_active_embedder_version(const char *version, const char *updated_at)
{
   if (!version || !version[0] || !updated_at || !updated_at[0])
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *us = aimee_pg_prepare(
       conn,
       "INSERT INTO memory_active_embedder(id, version, updated_at) VALUES (1, ?1, ?2)"
       " ON CONFLICT (id) DO UPDATE SET"
       "   version = EXCLUDED.version,"
       "   updated_at = EXCLUDED.updated_at",
       err, sizeof(err));
   if (!us)
      return -1;

   aimee_pg_bind_text(us, "?1", version);
   aimee_pg_bind_text(us, "?2", updated_at);
   aimee_pg_step_t rc = aimee_pg_step(us, err, sizeof(err));
   aimee_pg_finalize(us);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_kb_service_collect_reembed_status(db2_kb_service_reembed_status_t *out)
{
   if (!out)
      return -1;

   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *ps =
       aimee_pg_prepare(conn,
                        "SELECT target_version, last_id, total, done, started_at, finished_at"
                        " FROM memory_reembed_progress WHERE id = 1",
                        err, sizeof(err));
   if (!ps)
      return -1;

   if (aimee_pg_step(ps, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *target = aimee_pg_column_text(ps, 0);
      const char *started = aimee_pg_column_text(ps, 4);
      const char *finished = aimee_pg_column_text(ps, 5);
      if (target)
         snprintf(out->target_version, sizeof(out->target_version), "%s", target);
      if (started)
         snprintf(out->started_at, sizeof(out->started_at), "%s", started);
      if (finished)
         snprintf(out->finished_at, sizeof(out->finished_at), "%s", finished);
      out->last_id = aimee_pg_column_int(ps, 1);
      out->total = aimee_pg_column_int(ps, 2);
      out->done = aimee_pg_column_int(ps, 3);
      out->have_job = 1;
   }

   aimee_pg_finalize(ps);
   return 0;
}

int db2_kb_service_mark_reembed_finished(const char *finished_at)
{
   if (!finished_at || !finished_at[0])
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *fs = aimee_pg_prepare(conn,
                                          "UPDATE memory_reembed_progress SET finished_at = ?1"
                                          " WHERE id = 1 AND finished_at IS NULL",
                                          err, sizeof(err));
   if (!fs)
      return -1;

   aimee_pg_bind_text(fs, "?1", finished_at);
   aimee_pg_step_t rc = aimee_pg_step(fs, err, sizeof(err));
   aimee_pg_finalize(fs);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_kb_service_prepare_reembed_start(const char *version, const char *started_at,
                                         db2_kb_service_reembed_start_t *out)
{
   if (!version || !version[0] || !started_at || !started_at[0] || !out)
      return -1;

   memset(out, 0, sizeof(*out));

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *ts_stmt = aimee_pg_prepare(
       conn, "SELECT COUNT(*) FROM memories WHERE tier IN ('L1', 'L2')", err, sizeof(err));
   if (ts_stmt)
   {
      if (aimee_pg_step(ts_stmt, err, sizeof(err)) == AIMEE_PG_ROW)
         out->total_count = aimee_pg_column_int(ts_stmt, 0);
      aimee_pg_finalize(ts_stmt);
   }

   aimee_pg_stmt_t *rp = aimee_pg_prepare(conn,
                                          "SELECT last_id FROM memory_reembed_progress"
                                          " WHERE id = 1 AND target_version = ?1"
                                          "   AND finished_at IS NULL",
                                          err, sizeof(err));
   if (rp)
   {
      aimee_pg_bind_text(rp, "?1", version);
      if (aimee_pg_step(rp, err, sizeof(err)) == AIMEE_PG_ROW)
         out->resume_last_id = aimee_pg_column_int(rp, 0);
      aimee_pg_finalize(rp);
   }

   aimee_pg_stmt_t *up = aimee_pg_prepare(
       conn,
       "INSERT INTO memory_reembed_progress(id, target_version, last_id, total, done, started_at)"
       " VALUES (1, ?1, ?2, ?3, 0, ?4)"
       " ON CONFLICT (id) DO UPDATE SET"
       "   target_version = EXCLUDED.target_version,"
       "   total = EXCLUDED.total,"
       "   last_id = CASE WHEN memory_reembed_progress.target_version = EXCLUDED.target_version"
       "                  THEN memory_reembed_progress.last_id ELSE 0 END,"
       "   done = CASE WHEN memory_reembed_progress.target_version = EXCLUDED.target_version"
       "               THEN memory_reembed_progress.done ELSE 0 END,"
       "   started_at = EXCLUDED.started_at,"
       "   finished_at = NULL",
       err, sizeof(err));
   if (!up)
      return -1;

   aimee_pg_bind_text(up, "?1", version);
   aimee_pg_bind_int(up, "?2", out->resume_last_id);
   aimee_pg_bind_int(up, "?3", out->total_count);
   aimee_pg_bind_text(up, "?4", started_at);
   aimee_pg_step_t rc = aimee_pg_step(up, err, sizeof(err));
   aimee_pg_finalize(up);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_kb_service_update_reembed_progress(int last_id, int done)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *u = aimee_pg_prepare(
       conn, "UPDATE memory_reembed_progress SET last_id = ?1, done = ?2 WHERE id = 1", err,
       sizeof(err));
   if (!u)
      return -1;

   aimee_pg_bind_int(u, "?1", last_id);
   aimee_pg_bind_int(u, "?2", done);
   aimee_pg_step_t rc = aimee_pg_step(u, err, sizeof(err));
   aimee_pg_finalize(u);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_kb_service_list_unembedded_memory_ids(const char *version, int64_t *ids, int max_ids)
{
   if (!version || !version[0] || !ids || max_ids < 1)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT m.id FROM memories m"
                            " WHERE m.tier IN ('L1', 'L2')";
   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!stmt)
      return -1;

   int count = 0;
   while (count < max_ids && aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
      ids[count++] = aimee_pg_column_int64(stmt, 0);
   aimee_pg_finalize(stmt);
   return count;
}

int db2_kb_service_list_pending_reembed_memory_ids(const char *version, int resume_last_id,
                                                   int64_t *ids, int max_ids)
{
   if (!version || !version[0] || !ids || max_ids < 1)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(conn,
                                            "SELECT m.id FROM memories m"
                                            " WHERE m.tier IN ('L1', 'L2')"
                                            "   AND m.id > ?1"
                                            " ORDER BY m.id",
                                            err, sizeof(err));
   if (!stmt)
      return -1;

   aimee_pg_bind_int(stmt, "?1", resume_last_id);
   int count = 0;
   while (count < max_ids && aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
      ids[count++] = aimee_pg_column_int64(stmt, 0);
   aimee_pg_finalize(stmt);
   return count;
}

int db2_kb_service_count_embeddings_for_version(const char *version)
{
   if (!version || !version[0])
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *cs = aimee_pg_prepare(conn,
                                          "SELECT COUNT(*) FROM vector_index_ops"
                                          " WHERE collection = 'memory_units'"
                                          "   AND status = 'ok'"
                                          "   AND memory_id IS NOT NULL",
                                          err, sizeof(err));
   if (!cs)
      return -1;

   int count = -1;
   if (aimee_pg_step(cs, err, sizeof(err)) == AIMEE_PG_ROW)
      count = aimee_pg_column_int(cs, 0);
   aimee_pg_finalize(cs);
   return count;
}

int db2_kb_service_list_memory_ids_by_updated(int limit, int64_t *ids, int max_ids)
{
   if (!ids || max_ids < 1)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT id FROM memories ORDER BY updated_at DESC%s",
            (limit > 0) ? " LIMIT ?1" : "");

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!stmt)
      return -1;

   if (limit > 0)
      aimee_pg_bind_int(stmt, "?1", limit);

   int count = 0;
   while (count < max_ids && aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
      ids[count++] = aimee_pg_column_int64(stmt, 0);
   aimee_pg_finalize(stmt);
   return count;
}

int db2_kb_service_memory_record_exists(int64_t record_id)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt =
       aimee_pg_prepare(conn,
                        "SELECT EXISTS(SELECT 1 FROM memories WHERE id = ?1)"
                        "    OR EXISTS(SELECT 1 FROM memory_units WHERE id = ?1)",
                        err, sizeof(err));
   if (!stmt)
      return -1;

   aimee_pg_bind_int64(stmt, "?1", record_id);
   int exists = -1;
   if (aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
      exists = aimee_pg_column_int(stmt, 0);
   aimee_pg_finalize(stmt);
   return exists;
}

int db2_kb_service_kb_document_exists(int64_t document_id)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       conn, "SELECT EXISTS(SELECT 1 FROM kb_documents WHERE id = ?1)", err, sizeof(err));
   if (!stmt)
      return -1;

   aimee_pg_bind_int64(stmt, "?1", document_id);
   int exists = -1;
   if (aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
      exists = aimee_pg_column_int(stmt, 0);
   aimee_pg_finalize(stmt);
   return exists;
}

int db2_kb_service_directive_create(const char *question, const char *topic,
                                    const char *anchor_entity, const char *anchor_file,
                                    const char *cause, int priority, int64_t memory_a_id,
                                    int64_t memory_b_id, const char *evidence,
                                    const char *source_session, const char *valid_until,
                                    int *dedup_out, cJSON **directive_out)
{
   if (!question || !question[0] || !db2_kb_directive_cause_valid(cause))
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   if (priority < 0)
      priority = 0;
   if (priority > 100)
      priority = 100;
   if (dedup_out)
      *dedup_out = 0;
   if (directive_out)
      *directive_out = NULL;

   char err[KBS_ERRBUF] = "";
   /* epistemic_directives has UNIQUE partial indexes (idx_directives_dedup_*).
    * ON CONFLICT DO NOTHING plus RETURNING id tells us whether the row was
    * inserted or deduped. */
   aimee_pg_stmt_t *stmt =
       aimee_pg_prepare(conn,
                        "INSERT INTO epistemic_directives"
                        " (question, topic, anchor_entity, anchor_file, cause, priority, state,"
                        "  memory_a_id, memory_b_id, evidence, source_session, valid_until)"
                        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'open', ?7, ?8, ?9, ?10, ?11)"
                        " ON CONFLICT DO NOTHING"
                        " RETURNING id",
                        err, sizeof(err));
   if (!stmt)
      return -1;

   aimee_pg_bind_text(stmt, "?1", question);
   aimee_pg_bind_text(stmt, "?2", topic ? topic : "");
   aimee_pg_bind_text(stmt, "?3", anchor_entity ? anchor_entity : "");
   aimee_pg_bind_text(stmt, "?4", anchor_file ? anchor_file : "");
   aimee_pg_bind_text(stmt, "?5", cause);
   aimee_pg_bind_int(stmt, "?6", priority);
   aimee_pg_bind_int64(stmt, "?7", memory_a_id);
   aimee_pg_bind_int64(stmt, "?8", memory_b_id);
   aimee_pg_bind_text(stmt, "?9", evidence ? evidence : "");
   aimee_pg_bind_text(stmt, "?10", source_session ? source_session : "");
   aimee_pg_bind_text(stmt, "?11", valid_until ? valid_until : "");

   int64_t new_id = 0;
   int inserted = 0;
   aimee_pg_step_t rc = aimee_pg_step(stmt, err, sizeof(err));
   if (rc == AIMEE_PG_ROW)
   {
      new_id = aimee_pg_column_int64(stmt, 0);
      inserted = 1;
   }
   else if (rc != AIMEE_PG_DONE)
   {
      aimee_pg_finalize(stmt);
      return -1;
   }
   aimee_pg_finalize(stmt);

   if (!inserted)
   {
      if (dedup_out)
         *dedup_out = 1;
      return 0;
   }

   if (directive_out)
      *directive_out = db2_kb_service_directive_get_json(new_id);
   return directive_out && !*directive_out ? -1 : 0;
}

int db2_kb_service_directive_resolve(int64_t id, int64_t resolution_memory_id, const char *note)
{
   (void)note;
   if (id <= 0 || !db2_kb_service_directive_state_is_open(id))
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt =
       aimee_pg_prepare(conn,
                        "UPDATE epistemic_directives SET state = 'resolved',"
                        " resolution_memory_id = ?1, resolved_at = pg_now_text(),"
                        " updated_at = pg_now_text() WHERE id = ?2",
                        err, sizeof(err));
   if (!stmt)
      return -1;

   aimee_pg_bind_int64(stmt, "?1", resolution_memory_id);
   aimee_pg_bind_int64(stmt, "?2", id);
   aimee_pg_step_t rc = aimee_pg_step(stmt, err, sizeof(err));
   aimee_pg_finalize(stmt);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_kb_service_directive_suppress(int64_t id)
{
   if (id <= 0 || !db2_kb_service_directive_state_is_open(id))
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(conn,
                                            "UPDATE epistemic_directives SET state = 'suppressed',"
                                            " updated_at = pg_now_text() WHERE id = ?1",
                                            err, sizeof(err));
   if (!stmt)
      return -1;

   aimee_pg_bind_int64(stmt, "?1", id);
   aimee_pg_step_t rc = aimee_pg_step(stmt, err, sizeof(err));
   aimee_pg_finalize(stmt);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_kb_service_directive_sweep_expired(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[KBS_ERRBUF] = "";
   /* pg_now_text() matches the DB2 canonical UTC text format used by
    * valid_until. */
   int affected = 0;
   if (aimee_pg_exec_with_changes(
           conn,
           "UPDATE epistemic_directives SET state = 'expired', updated_at = pg_now_text()"
           " WHERE state = 'open' AND valid_until != ''"
           "   AND valid_until < pg_now_text()",
           err, sizeof(err), &affected) != 0)
      return -1;
   return affected;
}

cJSON *db2_kb_service_directive_list_json(const char *state, const char *cause, int max_rows)
{
   if (max_rows < 1)
      return NULL;

   void *conn = db2_conn();
   if (!conn)
      return NULL;

   char sql[1024];
   int have_state = state && state[0];
   int have_cause = cause && cause[0];
   if (have_state && have_cause)
      snprintf(sql, sizeof(sql),
               "SELECT %s FROM epistemic_directives WHERE state = ?1 AND cause = ?2"
               " ORDER BY priority DESC, created_at DESC, id DESC LIMIT ?3",
               DB2_KB_DIRECTIVE_SELECT_COLS);
   else if (have_state)
      snprintf(sql, sizeof(sql),
               "SELECT %s FROM epistemic_directives WHERE state = ?1"
               " ORDER BY priority DESC, created_at DESC, id DESC LIMIT ?2",
               DB2_KB_DIRECTIVE_SELECT_COLS);
   else if (have_cause)
      snprintf(sql, sizeof(sql),
               "SELECT %s FROM epistemic_directives WHERE cause = ?1"
               " ORDER BY priority DESC, created_at DESC, id DESC LIMIT ?2",
               DB2_KB_DIRECTIVE_SELECT_COLS);
   else
      snprintf(sql, sizeof(sql),
               "SELECT %s FROM epistemic_directives"
               " ORDER BY priority DESC, created_at DESC, id DESC LIMIT ?1",
               DB2_KB_DIRECTIVE_SELECT_COLS);

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!stmt)
      return NULL;

   if (have_state && have_cause)
   {
      aimee_pg_bind_text(stmt, "?1", state);
      aimee_pg_bind_text(stmt, "?2", cause);
      aimee_pg_bind_int(stmt, "?3", max_rows);
   }
   else if (have_state)
   {
      aimee_pg_bind_text(stmt, "?1", state);
      aimee_pg_bind_int(stmt, "?2", max_rows);
   }
   else if (have_cause)
   {
      aimee_pg_bind_text(stmt, "?1", cause);
      aimee_pg_bind_int(stmt, "?2", max_rows);
   }
   else
   {
      aimee_pg_bind_int(stmt, "?1", max_rows);
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "directives") : NULL;
   if (!resp || !arr)
   {
      aimee_pg_finalize(stmt);
      cJSON_Delete(resp);
      return NULL;
   }

   cJSON_AddStringToObject(resp, "status", "ok");
   while (aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      cJSON *row = db2_kb_directive_json_from_stmt(stmt);
      if (!row)
      {
         aimee_pg_finalize(stmt);
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, row);
   }

   aimee_pg_finalize(stmt);
   return resp;
}

cJSON *db2_kb_service_curiosity_list_json(const char *state, int max_rows)
{
   if (max_rows < 1)
      return NULL;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "items") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   curiosity_item_t *rows = calloc((size_t)max_rows, sizeof(*rows));
   if (!rows)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   int n = db2_curiosity_list((state && state[0]) ? state : NULL, rows, max_rows);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
      {
         free(rows);
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(obj, "id", (double)rows[i].id);
      cJSON_AddStringToObject(obj, "gap_type", rows[i].gap_type);
      cJSON_AddStringToObject(obj, "target_entity", rows[i].target_entity);
      cJSON_AddStringToObject(obj, "target_topic", rows[i].target_topic);
      cJSON_AddStringToObject(obj, "evidence", rows[i].evidence);
      cJSON_AddNumberToObject(obj, "importance", rows[i].importance);
      cJSON_AddNumberToObject(obj, "novelty", rows[i].novelty);
      cJSON_AddNumberToObject(obj, "progress", rows[i].progress);
      cJSON_AddNumberToObject(obj, "routing_score", rows[i].routing_score);
      cJSON_AddStringToObject(obj, "state", rows[i].state);
      cJSON_AddStringToObject(obj, "created_at", rows[i].created_at);
      cJSON_AddItemToArray(arr, obj);
   }
   free(rows);
   return resp;
}

static cJSON *curiosity_to_json(const curiosity_item_t *it)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "id", (double)it->id);
   cJSON_AddStringToObject(obj, "gap_type", it->gap_type);
   cJSON_AddStringToObject(obj, "target_entity", it->target_entity);
   cJSON_AddStringToObject(obj, "target_topic", it->target_topic);
   cJSON_AddStringToObject(obj, "evidence", it->evidence);
   cJSON_AddNumberToObject(obj, "importance", it->importance);
   cJSON_AddNumberToObject(obj, "novelty", it->novelty);
   cJSON_AddNumberToObject(obj, "progress", it->progress);
   cJSON_AddNumberToObject(obj, "routing_score", it->routing_score);
   cJSON_AddStringToObject(obj, "state", it->state);
   cJSON_AddStringToObject(obj, "source_session", it->source_session);
   cJSON_AddStringToObject(obj, "created_at", it->created_at);
   cJSON_AddStringToObject(obj, "updated_at", it->updated_at);
   return obj;
}

cJSON *db2_kb_service_curiosity_create_json(const char *gap_type, const char *target_entity,
                                            const char *target_topic, const char *evidence,
                                            double importance, double novelty,
                                            const char *source_session)
{
   curiosity_item_t created;
   int rc = db2_curiosity_create(gap_type, target_entity, target_topic, evidence, importance,
                                 novelty, source_session, &created);
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "curiosity create failed");
      return resp;
   }
   cJSON *obj = curiosity_to_json(&created);
   if (!obj)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "item", obj);
   return resp;
}

cJSON *db2_kb_service_curiosity_sweep_json(void)
{
   int created = db2_curiosity_sweep_failed_queries();
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "created", created);
   return resp;
}

cJSON *db2_kb_service_curiosity_rescore_json(void)
{
   int rescored = db2_curiosity_rescore_all();
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "rescored", rescored);
   return resp;
}

cJSON *db2_kb_service_curiosity_get_json(int64_t id)
{
   curiosity_item_t item;
   int found = db2_curiosity_get(id, &item);
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (found <= 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message",
                              found == 0 ? "curiosity item not found" : "curiosity get failed");
      return resp;
   }
   cJSON *obj = curiosity_to_json(&item);
   if (!obj)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "item", obj);
   return resp;
}

cJSON *db2_kb_service_curiosity_update_state_json(int64_t id, const char *new_state)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (!new_state || !db2_curiosity_state_is_valid(new_state))
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "invalid state");
      return resp;
   }
   if (db2_curiosity_update_state(id, new_state) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "curiosity update_state failed");
      return resp;
   }
   /* Return the updated row so callers can render without a follow-up RPC. */
   curiosity_item_t after;
   if (db2_curiosity_get(id, &after) > 0)
   {
      cJSON *obj = curiosity_to_json(&after);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToObject(resp, "item", obj);
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

static const char *route_curiosity_gap_to_cause(const char *gap_type)
{
   if (!gap_type)
      return "missing_config";
   if (strcmp(gap_type, CURIOSITY_GAP_CONTRADICTION) == 0)
      return "contradiction";
   if (strcmp(gap_type, CURIOSITY_GAP_MISSING_FACT) == 0)
      return "retrieval_failure";
   return "missing_config";
}

static void route_curiosity_build_question(const curiosity_item_t *it, char *out, size_t cap)
{
   const char *target = it->target_topic[0] ? it->target_topic : it->target_entity;
   const char *evidence = it->evidence[0] ? it->evidence : "";
   if (strcmp(it->gap_type, CURIOSITY_GAP_CONTRADICTION) == 0)
      snprintf(out, cap, "Resolve contradiction about %s%s%s", target, evidence[0] ? ": " : "",
               evidence);
   else if (strcmp(it->gap_type, CURIOSITY_GAP_MISSING_FACT) == 0)
      snprintf(out, cap, "Find a fact to answer: %s", target);
   else if (strcmp(it->gap_type, CURIOSITY_GAP_STALE_FACT) == 0)
      snprintf(out, cap, "Re-verify a stale fact about %s", target);
   else if (strcmp(it->gap_type, CURIOSITY_GAP_WEAK_COVERAGE) == 0)
      snprintf(out, cap, "Expand coverage of %s", target);
   else if (strcmp(it->gap_type, CURIOSITY_GAP_UNVERIFIED_ASSUMPTION) == 0)
      snprintf(out, cap, "Verify assumption about %s%s%s", target, evidence[0] ? ": " : "",
               evidence);
   else
      snprintf(out, cap, "Investigate %s", target);
}

cJSON *db2_kb_service_curiosity_route_top_json(int limit, const char *source_session)
{
   if (limit <= 0)
      limit = 5;
   if (limit > 32)
      limit = 32;

   curiosity_item_t batch[32];
   int n = db2_curiosity_list_top_open_by_score(batch, limit);

   int routed = 0;
   for (int i = 0; i < n; i++)
   {
      const curiosity_item_t *it = &batch[i];
      const char *cause = route_curiosity_gap_to_cause(it->gap_type);
      char question[512];
      route_curiosity_build_question(it, question, sizeof(question));
      int priority = (int)(it->routing_score * 100.0);
      if (priority < 0)
         priority = 0;
      if (priority > 100)
         priority = 100;

      int64_t new_id = 0;
      int existed = 0;
      int rc = db2_directive_insert_ignore(
          question, it->target_topic, it->target_entity, "", cause, priority, 0, 0, it->evidence,
          source_session ? source_session : "", "", &new_id, &existed);
      /* rc == 0 means inserted; existed != 0 means deduped. Both
       * count as successfully routed for the purposes of moving
       * the curiosity item along. */
      if (rc == 0)
      {
         db2_curiosity_update_state(it->id, CURIOSITY_STATE_IN_PROGRESS);
         routed++;
      }
   }

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "routed", routed);
   return resp;
}

static cJSON *note_to_json(const note_t *note)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "id", (double)note->id);
   cJSON_AddStringToObject(obj, "title", note->title);
   cJSON_AddStringToObject(obj, "slug", note->slug);
   cJSON_AddStringToObject(obj, "content", note->content);
   cJSON_AddStringToObject(obj, "tags", note->tags);
   cJSON_AddStringToObject(obj, "author", note->author);
   cJSON_AddStringToObject(obj, "created_at", note->created_at);
   cJSON_AddStringToObject(obj, "updated_at", note->updated_at);
   return obj;
}

cJSON *db2_kb_service_note_create_json(const char *title, const char *content, const char *tags,
                                       const char *author)
{
   note_t note;
   if (db2_note_create(title, content, tags, author, &note) != 0)
   {
      cJSON *resp = cJSON_CreateObject();
      if (!resp)
         return NULL;
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to create note");
      return resp;
   }
   cJSON *resp = cJSON_CreateObject();
   cJSON *obj = note_to_json(&note);
   if (!resp || !obj)
   {
      cJSON_Delete(resp);
      cJSON_Delete(obj);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "note", obj);
   return resp;
}

cJSON *db2_kb_service_note_list_json(const char *tag, int max_rows)
{
   if (max_rows < 1)
      return NULL;
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "notes") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   note_t *rows = calloc((size_t)max_rows, sizeof(*rows));
   if (!rows)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   int n = db2_note_list((tag && tag[0]) ? tag : NULL, max_rows, rows, max_rows);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = note_to_json(&rows[i]);
      if (!obj)
      {
         free(rows);
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   free(rows);
   return resp;
}

cJSON *db2_kb_service_note_search_json(const char *query, int max_rows)
{
   if (max_rows < 1)
      return NULL;
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "notes") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   note_t *rows = calloc((size_t)max_rows, sizeof(*rows));
   if (!rows)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   int n = db2_note_search(query ? query : "", rows, max_rows);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = note_to_json(&rows[i]);
      if (!obj)
      {
         free(rows);
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   free(rows);
   return resp;
}

/* Rules + collab_rules + agent_* + learning.propose_signal RPC backends
 * live in kb_service_backend_agent.c */

/* Memory-domain RPC backends (find_facts, list, get, insert, briefing,
 * context_block, entity_profile, entity_edges) live in
 * db2/kb_service_backend_memory.c so this file stays under the per-file
 * line cap. */

cJSON *db2_kb_service_learning_list_json(const char *state, const char *sink, int max_rows)
{
   if (max_rows < 1)
      return NULL;

   db2_kb_learning_archive_expired();

   void *conn = db2_conn();
   if (!conn)
      return NULL;

   char err[KBS_ERRBUF] = "";
   char sql[1024];
   snprintf(sql, sizeof(sql),
            "SELECT %s FROM learning_proposals"
            " WHERE (?1 = '' OR state = ?2) AND (?3 = '' OR sink = ?4)"
            " ORDER BY id DESC LIMIT ?5",
            DB2_KB_LEARNING_SELECT_COLS);
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!stmt)
      return NULL;

   aimee_pg_bind_text(stmt, "?1", state ? state : "");
   aimee_pg_bind_text(stmt, "?2", state ? state : "");
   aimee_pg_bind_text(stmt, "?3", sink ? sink : "");
   aimee_pg_bind_text(stmt, "?4", sink ? sink : "");
   aimee_pg_bind_int(stmt, "?5", max_rows);

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "proposals") : NULL;
   if (!resp || !arr)
   {
      aimee_pg_finalize(stmt);
      cJSON_Delete(resp);
      return NULL;
   }

   cJSON_AddStringToObject(resp, "status", "ok");
   while (aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      cJSON *row = db2_kb_learning_json_from_stmt(stmt);
      if (!row)
      {
         aimee_pg_finalize(stmt);
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, row);
   }

   aimee_pg_finalize(stmt);
   return resp;
}

cJSON *db2_kb_service_learning_get_json(int id)
{
   if (id <= 0)
      return NULL;

   db2_kb_learning_archive_expired();

   void *conn = db2_conn();
   if (!conn)
      return NULL;

   char sql[512];
   snprintf(sql, sizeof(sql), "SELECT %s FROM learning_proposals WHERE id = ?1",
            DB2_KB_LEARNING_SELECT_COLS);
   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!stmt)
      return NULL;

   aimee_pg_bind_int(stmt, "?1", id);
   cJSON *row = NULL;
   if (aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
      row = db2_kb_learning_json_from_stmt(stmt);
   aimee_pg_finalize(stmt);
   return row;
}

cJSON *db2_kb_service_learning_reject_json(int id)
{
   if (id <= 0)
      return NULL;

   db2_kb_learning_archive_expired();

   void *conn = db2_conn();
   if (!conn)
      return NULL;

   char err[KBS_ERRBUF] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       conn, "SELECT state FROM learning_proposals WHERE id = ?1", err, sizeof(err));
   if (!stmt)
      return NULL;

   aimee_pg_bind_int(stmt, "?1", id);
   char state[32] = "";
   int found = 0;
   if (aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_copy_text(state, sizeof(state), aimee_pg_column_text(stmt, 0));
      found = 1;
   }
   aimee_pg_finalize(stmt);
   if (!found)
      return NULL;

   if (strcmp(state, "committed") != 0 && strcmp(state, "archived") != 0)
   {
      aimee_pg_stmt_t *upd =
          aimee_pg_prepare(conn,
                           "UPDATE learning_proposals"
                           " SET state = 'archived', archive_reason = 'rejected',"
                           " updated_at = pg_now_text()"
                           " WHERE id = ?1 AND state = 'pending'",
                           err, sizeof(err));
      if (!upd)
         return NULL;
      aimee_pg_bind_int(upd, "?1", id);
      if (aimee_pg_step(upd, err, sizeof(err)) != AIMEE_PG_DONE)
      {
         aimee_pg_finalize(upd);
         return NULL;
      }
      aimee_pg_finalize(upd);
   }

   return db2_kb_service_learning_get_json(id);
}

cJSON *db2_kb_service_scene_list_json(int max_rows)
{
   if (max_rows < 1)
      return NULL;

   db2_memory_scene_row_t rows[100];
   if (max_rows > (int)(sizeof(rows) / sizeof(rows[0])))
      max_rows = (int)(sizeof(rows) / sizeof(rows[0]));

   int n = db2_memory_scenes_list_recent(rows, max_rows);
   if (n < 0)
      return NULL;

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *arr = cJSON_AddArrayToObject(resp, "scenes");
   if (!arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }

   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(obj, "id", (double)rows[i].id);
      cJSON_AddStringToObject(obj, "workspace_id", rows[i].workspace_id);
      cJSON_AddNumberToObject(obj, "turn_count", rows[i].turn_count);
      cJSON_AddStringToObject(obj, "created_at", rows[i].created_at);
      cJSON_AddItemToArray(arr, obj);
   }

   return resp;
}

cJSON *db2_kb_service_scene_members_json(int64_t scene_id, int max_rows)
{
   if (max_rows < 1)
      return NULL;

   db2_memory_scene_member_t rows[512];
   if (max_rows > (int)(sizeof(rows) / sizeof(rows[0])))
      max_rows = (int)(sizeof(rows) / sizeof(rows[0]));

   int n = db2_memory_scene_members(scene_id, rows, max_rows);
   if (n < 0)
      return NULL;

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "scene_id", (double)scene_id);
   cJSON *arr = cJSON_AddArrayToObject(resp, "members");
   if (!arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }

   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(obj, "memory_id", (double)rows[i].memory_id);
      cJSON_AddStringToObject(obj, "key", rows[i].key);
      cJSON_AddNumberToObject(obj, "membership_strength", rows[i].membership_strength);
      cJSON_AddItemToArray(arr, obj);
   }

   return resp;
}
