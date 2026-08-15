/* db1/delegations.c: user-local delegation spawns + messages. */

#include "delegations.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int db1_delegation_message_record(const char *delegation_id, const char *direction,
                                  const char *content)
{
   if (!delegation_id || !direction || !content)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO delegation_messages (delegation_id, direction, content) VALUES (?, ?, ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, direction, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, content, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_delegation_spawn_record(const char *delegation_id, const char *parent_delegation_id,
                                const char *session_id, int depth, const char *role)
{
   if (!delegation_id)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO delegation_spawns (delegation_id, parent_delegation_id, session_id,"
       " depth, role, status) VALUES (?, ?, ?, ?, ?, 'running')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, parent_delegation_id ? parent_delegation_id : "", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 4, depth);
   sqlite3_bind_text(stmt, 5, role ? role : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_delegation_spawn_complete(const char *delegation_id)
{
   if (!delegation_id)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE delegation_spawns SET status = 'done', completed_at = datetime('now')"
       " WHERE delegation_id = ? AND status IN ('active', 'running')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_delegation_spawn_stop_reason(const char *delegation_id, char *out, size_t out_sz)
{
   if (!delegation_id || !delegation_id[0] || (!out && out_sz > 0))
      return -1;
   if (out && out_sz > 0)
      out[0] = '\0';

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT status FROM delegation_spawns"
                            " WHERE delegation_id = ? AND status IN ('cancelled', 'preempted')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   if (rc != SQLITE_ROW)
   {
      sqlite3_finalize(stmt);
      return 0;
   }
   const unsigned char *status = sqlite3_column_text(stmt, 0);
   if (out && out_sz > 0)
      snprintf(out, out_sz, "%s", status ? (const char *)status : "");
   sqlite3_finalize(stmt);
   return 1;
}

int db1_delegation_spawn_is_stopped(const char *delegation_id)
{
   return db1_delegation_spawn_stop_reason(delegation_id, NULL, 0) == 1;
}

int db1_delegation_spawn_preempt(const char *delegation_id)
{
   if (!delegation_id || !delegation_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE delegation_spawns SET status = 'preempted', completed_at = datetime('now')"
       " WHERE delegation_id = ? AND status IN ('active', 'running')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(stmt);
   return changed;
}

int db1_delegation_spawn_status(const char *delegation_id, char *out, size_t out_sz)
{
   if (!delegation_id || !delegation_id[0] || !out || out_sz == 0)
      return -1;
   out[0] = '\0';
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT status FROM delegation_spawns WHERE delegation_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   if (rc != SQLITE_ROW)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   const unsigned char *status = sqlite3_column_text(stmt, 0);
   snprintf(out, out_sz, "%s", status ? (const char *)status : "");
   sqlite3_finalize(stmt);
   return 0;
}

int db1_delegation_spawn_is_cancelled(const char *delegation_id)
{
   if (!delegation_id || !delegation_id[0])
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT 1 FROM delegation_spawns WHERE delegation_id = ? AND status = 'cancelled'";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   int cancelled = (sqlite3_step(stmt) == SQLITE_ROW);
   sqlite3_finalize(stmt);
   return cancelled;
}

int db1_delegation_spawn_is_active(const char *delegation_id)
{
   if (!delegation_id || !delegation_id[0])
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT 1 FROM delegation_spawns"
                            " WHERE delegation_id = ? AND status IN ('active', 'running')"
                            "   AND created_at >= datetime('now', '-24 hours')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   int active = (sqlite3_step(stmt) == SQLITE_ROW);
   sqlite3_finalize(stmt);
   return active;
}

int db1_delegation_spawn_count_total(const char *session_id)
{
   if (!session_id)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT COUNT(*) FROM delegation_spawns WHERE session_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

int db1_delegation_spawn_find_root(const char *delegation_id, char *out, size_t out_sz)
{
   if (!delegation_id || !delegation_id[0] || !out || out_sz == 0)
      return -1;
   out[0] = '\0';
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "WITH RECURSIVE ancestors(delegation_id, parent_delegation_id, depth) AS ("
       "  SELECT delegation_id, parent_delegation_id, 0"
       "    FROM delegation_spawns WHERE delegation_id = ?"
       "  UNION ALL"
       "  SELECT s.delegation_id, s.parent_delegation_id, ancestors.depth + 1"
       "    FROM delegation_spawns s"
       "    JOIN ancestors ON s.delegation_id = ancestors.parent_delegation_id"
       "   WHERE ancestors.parent_delegation_id <> ''"
       ")"
       "SELECT delegation_id FROM ancestors"
       " ORDER BY CASE WHEN parent_delegation_id = '' THEN 1 ELSE 0 END DESC,"
       "          depth DESC"
       " LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   if (rc != SQLITE_ROW)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   const unsigned char *root = sqlite3_column_text(stmt, 0);
   if (!root || !root[0])
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   snprintf(out, out_sz, "%s", (const char *)root);
   sqlite3_finalize(stmt);
   return 0;
}

int db1_delegation_spawn_count_descendants(const char *root_delegation_id)
{
   if (!root_delegation_id || !root_delegation_id[0])
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "WITH RECURSIVE descendants(delegation_id) AS ("
       "  SELECT delegation_id FROM delegation_spawns WHERE parent_delegation_id = ?"
       "  UNION ALL"
       "  SELECT s.delegation_id FROM delegation_spawns s"
       "    JOIN descendants d ON s.parent_delegation_id = d.delegation_id"
       ")"
       "SELECT COUNT(*) FROM descendants";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(stmt, 1, root_delegation_id, -1, SQLITE_TRANSIENT);
   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

int db1_delegation_spawn_list_active(int *out_ids, int max)
{
   if (!out_ids || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT id FROM delegation_spawns WHERE status IN ('active', 'running') LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
      out_ids[n++] = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

int db1_delegation_spawn_cancel_by_id(int spawn_id)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE delegation_spawns SET status = 'cancelled', completed_at = datetime('now')"
       " WHERE id = ? AND status IN ('active', 'running')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, spawn_id);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(stmt);
   return changed;
}

int db1_delegation_spawn_cancel_recursive(int spawn_id)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   /* Single UPDATE with a recursive CTE walks the parent_delegation_id
    * chain. The CTE seeds with the requested spawn row, then unions in
    * any spawn whose parent_delegation_id matches a previously-collected
    * delegation_id. The outer UPDATE flips every collected row that is
    * still active/running. One round-trip; no app-side recursion. */
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "WITH RECURSIVE descendants(id, delegation_id) AS ("
                            "  SELECT id, delegation_id FROM delegation_spawns WHERE id = ?"
                            "  UNION ALL"
                            "  SELECT s.id, s.delegation_id FROM delegation_spawns s"
                            "    JOIN descendants d ON s.parent_delegation_id = d.delegation_id"
                            ") "
                            "UPDATE delegation_spawns"
                            " SET status = 'cancelled', completed_at = datetime('now')"
                            " WHERE id IN (SELECT id FROM descendants)"
                            "   AND status IN ('active', 'running')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, spawn_id);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(stmt);
   return changed;
}

int db1_delegation_spawn_cancel_stale(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE delegation_spawns SET status = 'cancelled', completed_at = datetime('now')"
       " WHERE status IN ('active', 'running')"
       "   AND created_at < datetime('now', '-24 hours')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed;
}
