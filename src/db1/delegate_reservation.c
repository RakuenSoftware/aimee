/* db1/delegate_reservation.c: execution-key -> delegate job reservations. */

#include "delegate_reservation.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* The Go control plane's migrations create this table. A server running without
 * that plane still launches delegates; it just cannot replay them. Reporting a
 * miss keeps that degradation silent and safe instead of failing every launch. */
static int reservation_table_present(sqlite3 *db)
{
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT 1 FROM sqlite_master WHERE type='table' AND name='lifecycle_delegate_job'";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   int present = sqlite3_step(stmt) == SQLITE_ROW;
   sqlite3_finalize(stmt);
   return present;
}

static int reservation_key_ok(const char *execution_key)
{
   return execution_key && execution_key[0] &&
          strlen(execution_key) < DB1_DELEGATE_RESERVATION_KEY_MAX;
}

int db1_delegate_reservation_get(const char *execution_key, int *out_job_id, char *participant,
                                 size_t participant_cap)
{
   if (participant && participant_cap)
      participant[0] = '\0';
   if (!reservation_key_ok(execution_key) || !out_job_id)
      return -1;
   *out_job_id = 0;
   sqlite3 *db = db1_conn();
   if (!db || !reservation_table_present(db))
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT job_id, participant_token FROM lifecycle_delegate_job WHERE execution_key = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, execution_key, -1, SQLITE_STATIC);
   int found = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      int job_id = sqlite3_column_int(stmt, 0);
      /* A row whose job id is unusable is worse than no row: it would replay a
       * launch that can never be polled. Treat it as a miss so the caller
       * launches and overwrites it. */
      if (job_id > 0)
      {
         *out_job_id = job_id;
         const unsigned char *token = sqlite3_column_text(stmt, 1);
         if (participant && participant_cap && token)
            snprintf(participant, participant_cap, "%s", (const char *)token);
         found = 0;
      }
   }
   sqlite3_finalize(stmt);
   return found;
}

int db1_delegate_reservation_adopt_sole_legacy(const char *execution_key, const char *work_item_id,
                                               int *out_job_id, char *participant,
                                               size_t participant_cap)
{
   if (participant && participant_cap)
      participant[0] = '\0';
   if (!reservation_key_ok(execution_key) || !work_item_id || !work_item_id[0] || !out_job_id)
      return -1;
   *out_job_id = 0;
   const char *hash_separator = strrchr(execution_key, ':');
   if (!hash_separator || hash_separator == execution_key || !hash_separator[1])
      return -1;
   size_t prefix_len = (size_t)(hash_separator - execution_key) + 1;
   if (prefix_len >= DB1_DELEGATE_RESERVATION_KEY_MAX)
      return -1;
   char prefix[DB1_DELEGATE_RESERVATION_KEY_MAX];
   memcpy(prefix, execution_key, prefix_len);
   prefix[prefix_len] = '\0';

   sqlite3 *db = db1_conn();
   if (!db || !reservation_table_present(db))
      return -1;
   sqlite3_stmt *stmt = NULL;
   /* One UPDATE owns both the cardinality check and the key move. There is no
    * read-then-write window in which a grouped seat can appear and be guessed. */
   static const char *sql =
       "UPDATE lifecycle_delegate_job SET execution_key = ?, updated_at = datetime('now') "
       "WHERE execution_key = ("
       " SELECT execution_key FROM lifecycle_delegate_job"
       " WHERE work_item_id = ? AND substr(execution_key, 1, ?) = ? AND job_id > 0 LIMIT 1"
       ") AND 1 = ("
       " SELECT COUNT(*) FROM lifecycle_delegate_job"
       " WHERE work_item_id = ? AND substr(execution_key, 1, ?) = ? AND job_id > 0"
       ") AND NOT EXISTS (SELECT 1 FROM lifecycle_delegate_job WHERE execution_key = ?) "
       "RETURNING job_id, participant_token";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, execution_key, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, work_item_id, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 3, (int)prefix_len);
   sqlite3_bind_text(stmt, 4, prefix, (int)prefix_len, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, work_item_id, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 6, (int)prefix_len);
   sqlite3_bind_text(stmt, 7, prefix, (int)prefix_len, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 8, execution_key, -1, SQLITE_STATIC);
   int adopted = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      int job_id = sqlite3_column_int(stmt, 0);
      if (job_id > 0)
      {
         *out_job_id = job_id;
         const unsigned char *token = sqlite3_column_text(stmt, 1);
         if (participant && participant_cap && token)
            snprintf(participant, participant_cap, "%s", (const char *)token);
         adopted = 0;
      }
   }
   sqlite3_finalize(stmt);
   return adopted;
}

int db1_delegate_reservation_save(const char *execution_key, const char *work_item_id, int job_id,
                                  const char *participant)
{
   if (!reservation_key_ok(execution_key) || job_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || !reservation_table_present(db))
      return -1;

   sqlite3_stmt *stmt = NULL;
   /* cancel_attempts resets with the job: the retry counter belongs to the
    * reservation's current job, not to the key across launches. */
   static const char *sql =
       "INSERT INTO lifecycle_delegate_job"
       " (execution_key, job_id, work_item_id, participant_token, cancel_attempts, updated_at)"
       " VALUES (?, ?, ?, ?, 0, datetime('now'))"
       " ON CONFLICT(execution_key) DO UPDATE SET job_id = excluded.job_id,"
       " work_item_id = excluded.work_item_id, participant_token = excluded.participant_token,"
       " cancel_attempts = 0, updated_at = datetime('now')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, execution_key, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, job_id);
   sqlite3_bind_text(stmt, 3, work_item_id ? work_item_id : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, participant ? participant : "", -1, SQLITE_STATIC);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_delegate_reservation_forget(const char *execution_key)
{
   if (!reservation_key_ok(execution_key))
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   /* No table means no reservation, which is the state the caller asked for. */
   if (!reservation_table_present(db))
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM lifecycle_delegate_job WHERE execution_key = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, execution_key, -1, SQLITE_STATIC);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_delegate_reservation_forget_if_matches(const char *execution_key, int job_id)
{
   if (!reservation_key_ok(execution_key) || job_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   if (!reservation_table_present(db))
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "DELETE FROM lifecycle_delegate_job WHERE execution_key = ? AND job_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, execution_key, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, job_id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc != SQLITE_DONE ? -1 : (sqlite3_changes(db) == 1 ? 1 : 0);
}
