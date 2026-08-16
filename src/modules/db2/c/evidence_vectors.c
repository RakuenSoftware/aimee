#include "evidence_vectors.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

#define EV_ERRBUF 256

int db2_evidence_enqueue(const char *artifact_id, const char *collection)
{
   if (!artifact_id || !artifact_id[0])
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "INSERT INTO evidence_index_ops (artifact_id, collection)"
                            " VALUES (?1, ?2) ON CONFLICT DO NOTHING";
   char err[EV_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", artifact_id);
   aimee_pg_bind_text(st, "?2", collection ? collection : "evidence");
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_evidence_store_vector(const char *artifact_id, const char *collection,
                              const char *embedding_text)
{
   if (!artifact_id || !artifact_id[0])
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Idempotent per artifact: drop any prior vector first so re-embedding (on a
    * model_version bump) overwrites rather than duplicating. artifact_id is
    * indexed but not unique, so this is a delete-then-insert. */
   char err[EV_ERRBUF] = "";
   static const char *sql_del = "DELETE FROM evidence_vectors WHERE artifact_id = ?1";
   aimee_pg_stmt_t *st_del = aimee_pg_prepare(conn, sql_del, err, sizeof(err));
   if (st_del)
   {
      aimee_pg_bind_text(st_del, "?1", artifact_id);
      (void)aimee_pg_step(st_del, err, sizeof(err));
      aimee_pg_finalize(st_del);
   }

   static const char *sql_insert =
       "INSERT INTO evidence_vectors (artifact_id, collection, embedding)"
       " VALUES (?1, ?2, ?3)";
   aimee_pg_stmt_t *st_ins = aimee_pg_prepare(conn, sql_insert, err, sizeof(err));
   if (!st_ins)
      return -1;

   aimee_pg_bind_text(st_ins, "?1", artifact_id);
   aimee_pg_bind_text(st_ins, "?2", collection ? collection : "evidence");
   aimee_pg_bind_text(st_ins, "?3", embedding_text ? embedding_text : "[]");
   int rc_ins = aimee_pg_step(st_ins, err, sizeof(err));
   aimee_pg_finalize(st_ins);
   if (rc_ins != AIMEE_PG_DONE && rc_ins != AIMEE_PG_ROW)
      return -1;

   static const char *sql_update =
       "UPDATE evidence_index_ops SET status = 'ok',"
       " updated_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')"
       " WHERE artifact_id = ?1";
   aimee_pg_stmt_t *st_upd = aimee_pg_prepare(conn, sql_update, err, sizeof(err));
   if (!st_upd)
      return -1;

   aimee_pg_bind_text(st_upd, "?1", artifact_id);
   int rc_upd = aimee_pg_step(st_upd, err, sizeof(err));
   aimee_pg_finalize(st_upd);
   return (rc_upd == AIMEE_PG_DONE || rc_upd == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_evidence_mark_failed(const char *artifact_id, const char *error)
{
   if (!artifact_id || !artifact_id[0])
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "UPDATE evidence_index_ops SET status = 'failed',"
                            " attempts = attempts + 1,"
                            " last_error = ?2,"
                            " updated_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')"
                            " WHERE artifact_id = ?1";
   char err[EV_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", artifact_id);
   aimee_pg_bind_text(st, "?2", error ? error : "");
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_evidence_list_pending(db2_evidence_pending_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT artifact_id, collection FROM evidence_index_ops"
                            " WHERE status = 'pending' ORDER BY artifact_id LIMIT ?1";
   char err[EV_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int(st, "?1", max);

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *aid = aimee_pg_column_text(st, 0);
      const char *col = aimee_pg_column_text(st, 1);
      if (aid)
         snprintf(out[count].artifact_id, sizeof(out[count].artifact_id), "%s", aid);
      if (col)
         snprintf(out[count].collection, sizeof(out[count].collection), "%s", col);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_evidence_reset_stuck(int max_attempts)
{
   if (max_attempts <= 0)
      return 0;

   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "UPDATE evidence_index_ops SET status = 'pending', attempts = 0"
                            " WHERE status = 'failed' AND attempts >= ?1";
   char err[EV_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   aimee_pg_bind_int(st, "?1", max_attempts);
   (void)aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return changes;
}

int db2_evidence_reembed_all(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "UPDATE evidence_index_ops SET status = 'pending', attempts = 0,"
                            " last_error = ''";
   char err[EV_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   (void)aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return changes;
}

int db2_evidence_ops_count(const char *status)
{
   if (status)
      return db2_scalar_int_text("SELECT COUNT(*) FROM evidence_index_ops WHERE status = ?1",
                                 status, -1);
   return db2_scalar_int("SELECT COUNT(*) FROM evidence_index_ops", -1);
}

int db2_evidence_vectors_list(db2_evidence_vector_row_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT ev.artifact_id, a.kind, ev.collection, ev.embedding"
                            " FROM evidence_vectors ev JOIN artifacts a ON a.id = ev.artifact_id"
                            " ORDER BY ev.artifact_id LIMIT ?1";
   char err[EV_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int(st, "?1", max);

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *aid = aimee_pg_column_text(st, 0);
      const char *kind = aimee_pg_column_text(st, 1);
      const char *col = aimee_pg_column_text(st, 2);
      const char *emb = aimee_pg_column_text(st, 3);
      snprintf(out[count].artifact_id, sizeof(out[count].artifact_id), "%s", aid ? aid : "");
      snprintf(out[count].kind, sizeof(out[count].kind), "%s", kind ? kind : "");
      snprintf(out[count].collection, sizeof(out[count].collection), "%s", col ? col : "");
      snprintf(out[count].embedding, sizeof(out[count].embedding), "%s", emb ? emb : "[]");
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}