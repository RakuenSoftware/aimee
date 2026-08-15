/* db1/delegation_checkpoint.c: durable delegation checkpoints. */

#include "delegation_checkpoint.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>

int db1_delegation_checkpoint_save(const char *delegation_id, const char *job_id, int attempt,
                                   const char *steps_json, const char *last_output,
                                   const char *error)
{
   if (!delegation_id)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT OR REPLACE INTO delegation_checkpoint"
       " (delegation_id, job_id, steps_completed, last_output, error, attempt,"
       "  failed_at, created_at)"
       " VALUES (?, ?, ?, ?, ?, ?, strftime('%s','now'), strftime('%s','now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, job_id ? job_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, steps_json ? steps_json : "[]", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, last_output ? last_output : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, error ? error : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 6, attempt);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_delegation_checkpoint_load(const char *delegation_id, char *steps_out, size_t steps_cap,
                                   char *error_out, size_t error_cap, char *output_out,
                                   size_t output_cap)
{
   if (!delegation_id)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT steps_completed, error, last_output FROM delegation_checkpoint"
                            " WHERE delegation_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *steps = sqlite3_column_text(stmt, 0);
      const unsigned char *err = sqlite3_column_text(stmt, 1);
      const unsigned char *out = sqlite3_column_text(stmt, 2);
      if (steps_out && steps_cap > 0)
         snprintf(steps_out, steps_cap, "%s", steps ? (const char *)steps : "");
      if (error_out && error_cap > 0)
         snprintf(error_out, error_cap, "%s", err ? (const char *)err : "");
      if (output_out && output_cap > 0)
         snprintf(output_out, output_cap, "%s", out ? (const char *)out : "");
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}
