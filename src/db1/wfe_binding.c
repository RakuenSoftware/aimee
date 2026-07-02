/* db1/wfe_binding.c: session <-> work-item binding accessors. See wfe_binding.h. */
#include "wfe_binding.h"

#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int db1_wfe_bind(const char *session_id, const char *work_item_id, const char *enforce_stage)
{
   if (!session_id || !session_id[0] || !work_item_id || !work_item_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   if (!enforce_stage)
      enforce_stage = "off";

   /* IMMEDIATE so the single-writer check + upsert are one atomic write txn
    * (two concurrent binds on the same work-item can't both pass the check). */
   if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
      return -1;

   int rc = 0;
   /* single-writer: is this work-item already bound to a DIFFERENT session? */
   {
      sqlite3_stmt *q = NULL;
      static const char *sel =
          "SELECT aimee_session_id FROM workflow_binding WHERE work_item_id=?1";
      if (sqlite3_prepare_v2(db, sel, -1, &q, NULL) == SQLITE_OK)
      {
         sqlite3_bind_text(q, 1, work_item_id, -1, SQLITE_TRANSIENT);
         while (sqlite3_step(q) == SQLITE_ROW)
         {
            const char *owner = (const char *)sqlite3_column_text(q, 0);
            if (owner && strcmp(owner, session_id) != 0)
            {
               rc = -2;
               break;
            }
         }
      }
      else
         rc = -1;
      sqlite3_finalize(q);
   }
   if (rc != 0)
   {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return rc;
   }

   /* idempotent upsert keyed on the session; a re-bind updates work_item_id +
    * updated_at but leaves enforce_stage untouched (stamped once -> monotonic). */
   {
      sqlite3_stmt *stmt = NULL;
      static const char *sql =
          "INSERT INTO workflow_binding (aimee_session_id, work_item_id, enforce_stage) "
          "VALUES (?1,?2,?3) ON CONFLICT(aimee_session_id) DO UPDATE SET "
          "work_item_id=excluded.work_item_id, updated_at=datetime('now')";
      if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK)
      {
         sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 2, work_item_id, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 3, enforce_stage, -1, SQLITE_TRANSIENT);
         rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
      }
      else
         rc = -1;
      sqlite3_finalize(stmt);
   }
   sqlite3_exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
   return rc;
}

int db1_wfe_binding_get(const char *session_id, char *wi_out, size_t wi_n, char *stage_out,
                        size_t stage_n)
{
   if (wi_out && wi_n)
      wi_out[0] = '\0';
   if (stage_out && stage_n)
      stage_out[0] = '\0';
   if (!session_id || !session_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT work_item_id, enforce_stage FROM workflow_binding WHERE aimee_session_id=?1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   int found = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      found = 1;
      const char *wi = (const char *)sqlite3_column_text(stmt, 0);
      const char *st = (const char *)sqlite3_column_text(stmt, 1);
      if (wi_out && wi_n)
         snprintf(wi_out, wi_n, "%s", wi ? wi : "");
      if (stage_out && stage_n)
         snprintf(stage_out, stage_n, "%s", st ? st : "");
   }
   sqlite3_finalize(stmt);
   return found;
}

int db1_wfe_lease_renew(const char *session_id, int ttl_secs)
{
   if (!session_id || !session_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   /* ttl==0 -> clear the lease (store '', never stale); else lease_expiry :=
    * now + ttl_secs. A negative ttl sets a PAST expiry (immediately stale) -- used
    * to force expiry (tests / an explicit orphan). Modifier built in C so the value
    * is a bound param, not string-concatenated SQL. */
   char mod[32];
   snprintf(mod, sizeof mod, "%+d seconds", ttl_secs);
   static const char *sql = "UPDATE workflow_binding SET lease_expiry="
                            "CASE WHEN ?2 = 0 THEN '' ELSE datetime('now', ?3) END "
                            "WHERE aimee_session_id=?1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, ttl_secs);
   sqlite3_bind_text(stmt, 3, mod, -1, SQLITE_TRANSIENT);
   int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
   sqlite3_finalize(stmt);
   return rc;
}

int db1_wfe_lease_expiry_get(const char *session_id, char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   if (!session_id || !session_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT lease_expiry FROM workflow_binding WHERE aimee_session_id=?1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   int found = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      found = 1;
      const char *e = (const char *)sqlite3_column_text(stmt, 0);
      if (out && n)
         snprintf(out, n, "%s", e ? e : "");
   }
   sqlite3_finalize(stmt);
   return found;
}

int db1_wfe_lease_stale_work_items(char (*out)[80], int max)
{
   if (!out || max <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT work_item_id FROM workflow_binding "
       "WHERE lease_expiry != '' AND lease_expiry < datetime('now') ORDER BY lease_expiry ASC";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *wi = (const char *)sqlite3_column_text(stmt, 0);
      snprintf(out[n], 80, "%s", wi ? wi : "");
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_wfe_unbind(const char *session_id)
{
   if (!session_id || !session_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM workflow_binding WHERE aimee_session_id=?1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
   sqlite3_finalize(stmt);
   return rc;
}
