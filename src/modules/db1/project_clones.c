/* db1/project_clones.c: local checkout path to shared project identity mapping. */

#include "project_clones.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static int load_rows(sqlite3_stmt *stmt, db1_project_clone_t *out, int max)
{
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_project_clone_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      db1_copy_col_text(row->clone_path, sizeof(row->clone_path), stmt, 0);
      db1_copy_col_text(row->project_uuid, sizeof(row->project_uuid), stmt, 1);
      db1_copy_col_text(row->canonical_url, sizeof(row->canonical_url), stmt, 2);
      db1_copy_col_text(row->origin_url, sizeof(row->origin_url), stmt, 3);
      db1_copy_col_text(row->upstream_url, sizeof(row->upstream_url), stmt, 4);
      db1_copy_col_text(row->last_seen_at, sizeof(row->last_seen_at), stmt, 5);
      n++;
   }
   return n;
}

int db1_project_clone_upsert(const char *clone_path, const char *project_uuid,
                             const char *canonical_url, const char *origin_url,
                             const char *upstream_url)
{
   if (!clone_path || !clone_path[0] || !project_uuid || !project_uuid[0])
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO project_clones (clone_path, project_uuid, canonical_url, origin_url,"
       " upstream_url, last_seen_at)"
       " VALUES (?, ?, ?, ?, ?, datetime('now'))"
       " ON CONFLICT(clone_path) DO UPDATE SET"
       " project_uuid = excluded.project_uuid,"
       " canonical_url = excluded.canonical_url,"
       " origin_url = excluded.origin_url,"
       " upstream_url = excluded.upstream_url,"
       " last_seen_at = datetime('now')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, clone_path, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, project_uuid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, canonical_url ? canonical_url : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, origin_url ? origin_url : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, upstream_url ? upstream_url : "", -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_project_clone_get(const char *clone_path, db1_project_clone_t *out)
{
   if (!clone_path || !clone_path[0] || !out)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT clone_path, project_uuid, canonical_url, origin_url, upstream_url, last_seen_at"
       " FROM project_clones WHERE clone_path = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, clone_path, -1, SQLITE_TRANSIENT);
   memset(out, 0, sizeof(*out));
   int rc = load_rows(stmt, out, 1) == 1 ? 0 : -1;
   sqlite3_finalize(stmt);
   return rc;
}

int db1_project_clone_delete(const char *clone_path)
{
   if (!clone_path || !clone_path[0])
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM project_clones WHERE clone_path = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, clone_path, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed > 0 ? 0 : -1;
}

int db1_project_clone_list(db1_project_clone_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT clone_path, project_uuid, canonical_url, origin_url, upstream_url, last_seen_at"
       " FROM project_clones ORDER BY last_seen_at DESC, clone_path ASC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, max);
   int n = load_rows(stmt, out, max);
   sqlite3_finalize(stmt);
   return n;
}

int db1_project_clone_list_by_project(const char *project_uuid, db1_project_clone_t *out, int max)
{
   if (!project_uuid || !project_uuid[0] || !out || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT clone_path, project_uuid, canonical_url, origin_url, upstream_url, last_seen_at"
       " FROM project_clones WHERE project_uuid = ?"
       " ORDER BY last_seen_at DESC, clone_path ASC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, project_uuid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, max);
   int n = load_rows(stmt, out, max);
   sqlite3_finalize(stmt);
   return n;
}
