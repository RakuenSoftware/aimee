/* db1/payload_rewrite_state.c: DB1 storage for prompt-cache rewrite metadata. */
#include "db1_internal.h"
#include "payload_rewrite_state.h"
#include <stdio.h>
#include <string.h>

int db1_payload_rewrite_state_get(const char *session_id, payload_rewrite_state_t *out)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id || !out)
      return -1;

   const char *sql = "SELECT session_id, payload_epoch, compaction_epoch, last_prefix_hash,"
                     " last_payload_tokens, last_rewrite_at, deferred_rewrite_count,"
                     " consecutive_deferred_count, bytes_saved_pending, rewrite_reason, updated_at"
                     " FROM payload_rewrite_state WHERE session_id=?";
   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *s;
      memset(out, 0, sizeof(*out));
      s = (const char *)sqlite3_column_text(stmt, 0);
      if (s)
         snprintf(out->session_id, sizeof(out->session_id), "%s", s);
      out->payload_epoch = sqlite3_column_int64(stmt, 1);
      out->compaction_epoch = sqlite3_column_int64(stmt, 2);
      s = (const char *)sqlite3_column_text(stmt, 3);
      if (s)
         snprintf(out->last_prefix_hash, sizeof(out->last_prefix_hash), "%s", s);
      out->last_payload_tokens = sqlite3_column_int(stmt, 4);
      s = (const char *)sqlite3_column_text(stmt, 5);
      if (s)
         snprintf(out->last_rewrite_at, sizeof(out->last_rewrite_at), "%s", s);
      out->deferred_rewrite_count = sqlite3_column_int(stmt, 6);
      out->consecutive_deferred_count = sqlite3_column_int(stmt, 7);
      out->bytes_saved_pending = sqlite3_column_int(stmt, 8);
      s = (const char *)sqlite3_column_text(stmt, 9);
      if (s)
         snprintf(out->rewrite_reason, sizeof(out->rewrite_reason), "%s", s);
      s = (const char *)sqlite3_column_text(stmt, 10);
      if (s)
         snprintf(out->updated_at, sizeof(out->updated_at), "%s", s);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_payload_rewrite_state_set(const payload_rewrite_state_t *s)
{
   sqlite3 *db = db1_conn();
   if (!db || !s || !s->session_id[0])
      return -1;

   const char *sql =
       "INSERT OR REPLACE INTO payload_rewrite_state"
       " (session_id, payload_epoch, compaction_epoch, last_prefix_hash,"
       "  last_payload_tokens, last_rewrite_at, deferred_rewrite_count,"
       "  consecutive_deferred_count, bytes_saved_pending, rewrite_reason, updated_at)"
       " VALUES (?,?,?,?,?,?,?,?,?,?,datetime('now'))";
   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, s->session_id, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 2, s->payload_epoch);
   sqlite3_bind_int64(stmt, 3, s->compaction_epoch);
   sqlite3_bind_text(stmt, 4, s->last_prefix_hash, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 5, s->last_payload_tokens);
   sqlite3_bind_text(stmt, 6, s->last_rewrite_at, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 7, s->deferred_rewrite_count);
   sqlite3_bind_int(stmt, 8, s->consecutive_deferred_count);
   sqlite3_bind_int(stmt, 9, s->bytes_saved_pending);
   sqlite3_bind_text(stmt, 10, s->rewrite_reason, -1, SQLITE_STATIC);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return 0;
}

int db1_payload_rewrite_record(const char *session_id, int deferred, int bytes_saved,
                               int new_payload_tokens, const char *reason,
                               const char *new_prefix_hash)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id)
      return -1;

   const char *sql;
   if (deferred)
      sql = "INSERT INTO payload_rewrite_state"
            " (session_id, deferred_rewrite_count, consecutive_deferred_count,"
            "  bytes_saved_pending, last_payload_tokens, last_rewrite_at,"
            "  rewrite_reason, last_prefix_hash, updated_at)"
            " VALUES (?,1,1,?,?,datetime('now'),?,?,datetime('now'))"
            " ON CONFLICT(session_id) DO UPDATE SET"
            "  deferred_rewrite_count = deferred_rewrite_count + 1,"
            "  consecutive_deferred_count = consecutive_deferred_count + 1,"
            "  bytes_saved_pending = bytes_saved_pending + excluded.bytes_saved_pending,"
            "  last_payload_tokens = excluded.last_payload_tokens,"
            "  last_rewrite_at = excluded.last_rewrite_at,"
            "  rewrite_reason = excluded.rewrite_reason,"
            "  last_prefix_hash = excluded.last_prefix_hash,"
            "  updated_at = excluded.updated_at";
   else
      sql = "INSERT INTO payload_rewrite_state"
            " (session_id, payload_epoch, consecutive_deferred_count, bytes_saved_pending,"
            "  last_payload_tokens, last_rewrite_at, rewrite_reason, last_prefix_hash,"
            "  updated_at)"
            " VALUES (?,1,0,0,?,datetime('now'),?,?,datetime('now'))"
            " ON CONFLICT(session_id) DO UPDATE SET"
            "  payload_epoch = payload_epoch + 1,"
            "  consecutive_deferred_count = 0,"
            "  bytes_saved_pending = 0,"
            "  last_payload_tokens = excluded.last_payload_tokens,"
            "  last_rewrite_at = excluded.last_rewrite_at,"
            "  rewrite_reason = excluded.rewrite_reason,"
            "  last_prefix_hash = excluded.last_prefix_hash,"
            "  updated_at = excluded.updated_at";

   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   if (deferred)
   {
      /* params: session_id, bytes_saved, last_payload_tokens, reason, prefix_hash */
      sqlite3_bind_int(stmt, 2, bytes_saved);
      sqlite3_bind_int(stmt, 3, new_payload_tokens);
      sqlite3_bind_text(stmt, 4, reason ? reason : "", -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 5, new_prefix_hash ? new_prefix_hash : "", -1, SQLITE_STATIC);
   }
   else
   {
      /* params: session_id, last_payload_tokens, reason, prefix_hash */
      sqlite3_bind_int(stmt, 2, new_payload_tokens);
      sqlite3_bind_text(stmt, 3, reason ? reason : "", -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 4, new_prefix_hash ? new_prefix_hash : "", -1, SQLITE_STATIC);
   }
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return 0;
}
