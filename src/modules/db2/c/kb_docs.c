/* db2/kb_docs.c: aimee-kb ingest API — docs table accessors (DB2).
 *
 * See docs/proposals/pending/aimee-kb-ingest-api-and-corpus-staging.md */

#include "kb_docs.h"
#include "corpus_jobs.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int64_t db2_kb_doc_write(const char *content_hash, const char *filename, const char *scope,
                         const char *converter, const char *converter_version,
                         const char *normalized_text, int *was_existing)
{
   void *conn = db2_conn();
   if (!conn || !content_hash || !converter || !converter_version || !scope)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO docs"
       " (content_hash, filename, scope, converter, converter_version, normalized_text)"
       " VALUES (?1,?2,?3,?4,?5,?6)"
       " ON CONFLICT (content_hash, converter, converter_version, scope) DO NOTHING",
       err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", content_hash);
   aimee_pg_bind_text(st, "?2", filename ? filename : "");
   aimee_pg_bind_text(st, "?3", scope);
   aimee_pg_bind_text(st, "?4", converter);
   aimee_pg_bind_text(st, "?5", converter_version);
   aimee_pg_bind_text(st, "?6", normalized_text ? normalized_text : "");

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changed = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);

   if (rc != AIMEE_PG_DONE && rc != AIMEE_PG_ROW)
      return -1;

   if (was_existing)
      *was_existing = (changed == 0) ? 1 : 0;

   st = aimee_pg_prepare(
       conn,
       "SELECT id FROM docs"
       " WHERE content_hash=?1 AND converter=?2 AND converter_version=?3 AND scope=?4",
       err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", content_hash);
   aimee_pg_bind_text(st, "?2", converter);
   aimee_pg_bind_text(st, "?3", converter_version);
   aimee_pg_bind_text(st, "?4", scope);

   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   if (id > 0)
   {
      if (db2_corpus_job_seed_doc(id, content_hash) != 0)
         return -1;
      if (db2_corpus_job_record_version(id, scope, filename, content_hash) != 0)
         return -1;
   }
   return id;
}

int db2_kb_doc_exists_by_hash_scope(const char *content_hash, const char *scope)
{
   void *conn = db2_conn();
   if (!conn || !content_hash || !content_hash[0] || !scope || !scope[0])
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT 1 FROM docs WHERE content_hash=?1 AND scope=?2 LIMIT 1", err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", content_hash);
   aimee_pg_bind_text(st, "?2", scope);

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc == AIMEE_PG_ROW)
      return 1;
   if (rc == AIMEE_PG_DONE)
      return 0;
   return -1;
}

int db2_kb_doc_read(int64_t id, db2_kb_doc_t *out)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, content_hash, filename, scope, converter, converter_version,"
                        "       state, review_needed, review_reason, created_at, updated_at"
                        " FROM docs WHERE id=?1",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int64(st, "?1", id);

   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return -1;
   }

   memset(out, 0, sizeof(*out));
   out->id = aimee_pg_column_int64(st, 0);
   const char *v;
   v = aimee_pg_column_text(st, 1);
   if (v)
      snprintf(out->content_hash, sizeof(out->content_hash), "%s", v);
   v = aimee_pg_column_text(st, 2);
   if (v)
      snprintf(out->filename, sizeof(out->filename), "%s", v);
   v = aimee_pg_column_text(st, 3);
   if (v)
      snprintf(out->scope, sizeof(out->scope), "%s", v);
   v = aimee_pg_column_text(st, 4);
   if (v)
      snprintf(out->converter, sizeof(out->converter), "%s", v);
   v = aimee_pg_column_text(st, 5);
   if (v)
      snprintf(out->converter_version, sizeof(out->converter_version), "%s", v);
   v = aimee_pg_column_text(st, 6);
   if (v)
      snprintf(out->state, sizeof(out->state), "%s", v);
   out->review_needed = aimee_pg_column_int(st, 7);
   v = aimee_pg_column_text(st, 8);
   if (v)
      snprintf(out->review_reason, sizeof(out->review_reason), "%s", v);
   v = aimee_pg_column_text(st, 9);
   if (v)
      snprintf(out->created_at, sizeof(out->created_at), "%s", v);
   v = aimee_pg_column_text(st, 10);
   if (v)
      snprintf(out->updated_at, sizeof(out->updated_at), "%s", v);

   aimee_pg_finalize(st);
   return 0;
}

int db2_kb_doc_set_state(int64_t id, const char *state, int clear_review_needed,
                         const char *review_reason)
{
   void *conn = db2_conn();
   if (!conn || !state)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[256] = "";
   aimee_pg_stmt_t *st;

   if (clear_review_needed && review_reason)
   {
      st = aimee_pg_prepare(
          conn,
          "UPDATE docs SET state=?1, review_needed=false, review_reason=?2, updated_at=?3"
          " WHERE id=?4",
          err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", state);
      aimee_pg_bind_text(st, "?2", review_reason);
      aimee_pg_bind_text(st, "?3", ts);
      aimee_pg_bind_int64(st, "?4", id);
   }
   else if (clear_review_needed)
   {
      st = aimee_pg_prepare(
          conn, "UPDATE docs SET state=?1, review_needed=false, updated_at=?2 WHERE id=?3", err,
          sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", state);
      aimee_pg_bind_text(st, "?2", ts);
      aimee_pg_bind_int64(st, "?3", id);
   }
   else
   {
      st = aimee_pg_prepare(conn, "UPDATE docs SET state=?1, updated_at=?2 WHERE id=?3", err,
                            sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", state);
      aimee_pg_bind_text(st, "?2", ts);
      aimee_pg_bind_int64(st, "?3", id);
   }

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_kb_doc_delete(int64_t id)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, "DELETE FROM docs WHERE id=?1", err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int64(st, "?1", id);

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changed = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE && changed > 0) ? 0 : -1;
}

int db2_kb_doc_list_review(int limit, int64_t cursor_id, db2_kb_doc_t *out, int max_out)
{
   void *conn = db2_conn();
   if (!conn || !out || max_out <= 0)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, content_hash, filename, scope, converter, converter_version,"
                        "       state, review_needed, review_reason, created_at, updated_at"
                        " FROM docs WHERE state='staged' AND review_needed=true AND id > ?1"
                        " ORDER BY id ASC LIMIT ?2",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int64(st, "?1", cursor_id);
   aimee_pg_bind_int(st, "?2", limit > 0 ? limit : 50);

   int n = 0;
   while (n < max_out && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_kb_doc_t *row = &out[n++];
      memset(row, 0, sizeof(*row));
      row->id = aimee_pg_column_int64(st, 0);
      const char *v;
      v = aimee_pg_column_text(st, 1);
      if (v)
         snprintf(row->content_hash, sizeof(row->content_hash), "%s", v);
      v = aimee_pg_column_text(st, 2);
      if (v)
         snprintf(row->filename, sizeof(row->filename), "%s", v);
      v = aimee_pg_column_text(st, 3);
      if (v)
         snprintf(row->scope, sizeof(row->scope), "%s", v);
      v = aimee_pg_column_text(st, 4);
      if (v)
         snprintf(row->converter, sizeof(row->converter), "%s", v);
      v = aimee_pg_column_text(st, 5);
      if (v)
         snprintf(row->converter_version, sizeof(row->converter_version), "%s", v);
      v = aimee_pg_column_text(st, 6);
      if (v)
         snprintf(row->state, sizeof(row->state), "%s", v);
      row->review_needed = aimee_pg_column_int(st, 7);
      v = aimee_pg_column_text(st, 8);
      if (v)
         snprintf(row->review_reason, sizeof(row->review_reason), "%s", v);
      v = aimee_pg_column_text(st, 9);
      if (v)
         snprintf(row->created_at, sizeof(row->created_at), "%s", v);
      v = aimee_pg_column_text(st, 10);
      if (v)
         snprintf(row->updated_at, sizeof(row->updated_at), "%s", v);
   }
   aimee_pg_finalize(st);
   return n;
}
