/* db1/local_operator.c: local machine credential-to-operator mapping. */

#include "local_operator.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static int begin_tx(sqlite3 *db)
{
   return db1_txn_begin(db, "BEGIN IMMEDIATE TRANSACTION");
}

static void rollback_tx(sqlite3 *db)
{
   db1_txn_end(db, "ROLLBACK");
}

static int commit_tx(sqlite3 *db)
{
   return db1_txn_end(db, "COMMIT");
}

static int clear_active(sqlite3 *db)
{
   return sqlite3_exec(db, "UPDATE local_operator SET active = 0 WHERE active <> 0", NULL, NULL,
                       NULL) == SQLITE_OK
              ? 0
              : -1;
}

int db1_local_operator_upsert(const char *secret_ref, const char *operator_uuid, int active,
                              const char *display_hint)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *stmt = NULL;
   int rc = -1;
   static const char *sql =
       "INSERT INTO local_operator (secret_ref, operator_uuid, active, display_hint)"
       " VALUES (?, ?, ?, ?)"
       " ON CONFLICT(secret_ref) DO UPDATE SET"
       " operator_uuid = excluded.operator_uuid,"
       " active = excluded.active,"
       " display_hint = excluded.display_hint";

   if (!db || !secret_ref || !secret_ref[0] || !operator_uuid || !operator_uuid[0])
      return -1;

   if (begin_tx(db) != 0)
      return -1;
   if (active && clear_active(db) != 0)
      goto cleanup;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      goto cleanup;

   sqlite3_bind_text(stmt, 1, secret_ref, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, operator_uuid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, active ? 1 : 0);
   sqlite3_bind_text(stmt, 4, display_hint ? display_hint : "", -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) != SQLITE_DONE)
      goto cleanup;
   sqlite3_finalize(stmt);
   stmt = NULL;

   if (commit_tx(db) != 0)
      goto cleanup;
   rc = 0;

cleanup:
   if (stmt)
      sqlite3_finalize(stmt);
   if (rc != 0)
      rollback_tx(db);
   return rc;
}

int db1_local_operator_get(const char *secret_ref, db1_local_operator_t *out)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT secret_ref, operator_uuid, active, display_hint, created_at"
                            " FROM local_operator WHERE secret_ref = ?";

   if (!db || !secret_ref || !secret_ref[0] || !out)
      return -1;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, secret_ref, -1, SQLITE_TRANSIENT);
   memset(out, 0, sizeof(*out));
   if (sqlite3_step(stmt) != SQLITE_ROW)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   db1_copy_col_text(out->secret_ref, sizeof(out->secret_ref), stmt, 0);
   db1_copy_col_text(out->operator_uuid, sizeof(out->operator_uuid), stmt, 1);
   out->active = sqlite3_column_int(stmt, 2);
   db1_copy_col_text(out->display_hint, sizeof(out->display_hint), stmt, 3);
   db1_copy_col_text(out->created_at, sizeof(out->created_at), stmt, 4);
   sqlite3_finalize(stmt);
   return 0;
}

int db1_local_operator_get_active(db1_local_operator_t *out)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT secret_ref, operator_uuid, active, display_hint, created_at"
                            " FROM local_operator WHERE active <> 0"
                            " ORDER BY created_at DESC, secret_ref ASC LIMIT 1";

   if (!db || !out)
      return -1;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   memset(out, 0, sizeof(*out));
   if (sqlite3_step(stmt) != SQLITE_ROW)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   db1_copy_col_text(out->secret_ref, sizeof(out->secret_ref), stmt, 0);
   db1_copy_col_text(out->operator_uuid, sizeof(out->operator_uuid), stmt, 1);
   out->active = sqlite3_column_int(stmt, 2);
   db1_copy_col_text(out->display_hint, sizeof(out->display_hint), stmt, 3);
   db1_copy_col_text(out->created_at, sizeof(out->created_at), stmt, 4);
   sqlite3_finalize(stmt);
   return 0;
}

int db1_local_operator_set_active(const char *secret_ref)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *stmt = NULL;
   int rc = -1;
   static const char *sql = "UPDATE local_operator SET active = 1 WHERE secret_ref = ?";

   if (!db || !secret_ref || !secret_ref[0])
      return -1;
   if (begin_tx(db) != 0)
      return -1;
   if (clear_active(db) != 0)
      goto cleanup;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      goto cleanup;

   sqlite3_bind_text(stmt, 1, secret_ref, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(db) <= 0)
      goto cleanup;
   sqlite3_finalize(stmt);
   stmt = NULL;

   if (commit_tx(db) != 0)
      goto cleanup;
   rc = 0;

cleanup:
   if (stmt)
      sqlite3_finalize(stmt);
   if (rc != 0)
      rollback_tx(db);
   return rc;
}

int db1_local_operator_delete(const char *secret_ref)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM local_operator WHERE secret_ref = ?";
   int changed = 0;

   if (!db || !secret_ref || !secret_ref[0])
      return -1;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, secret_ref, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) == SQLITE_DONE)
      changed = sqlite3_changes(db);
   sqlite3_finalize(stmt);
   return changed > 0 ? 0 : -1;
}

int db1_local_operator_list(db1_local_operator_t *out, int max)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *stmt = NULL;
   int n = 0;
   static const char *sql = "SELECT secret_ref, operator_uuid, active, display_hint, created_at"
                            " FROM local_operator"
                            " ORDER BY active DESC, created_at DESC, secret_ref ASC LIMIT ?";

   if (!out || max <= 0)
      return 0;
   if (!db)
      return -1;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, max);
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_local_operator_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      db1_copy_col_text(row->secret_ref, sizeof(row->secret_ref), stmt, 0);
      db1_copy_col_text(row->operator_uuid, sizeof(row->operator_uuid), stmt, 1);
      row->active = sqlite3_column_int(stmt, 2);
      db1_copy_col_text(row->display_hint, sizeof(row->display_hint), stmt, 3);
      db1_copy_col_text(row->created_at, sizeof(row->created_at), stmt, 4);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}
