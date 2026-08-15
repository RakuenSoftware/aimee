/* db1/roadmap_runtime.c: DB1 CRUD helpers for the spec-driven roadmap dispatch
 * loop (roadmap_dispatch, roadmap_unit_dispatch, roadmap_milestone_lease).
 *
 * Follows the exact pattern of coord_jobs.c: raw sqlite3 via db1_conn(),
 * typed helpers, no backend types in the header. */

#include "roadmap_runtime.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── roadmap_dispatch ────────────────────────────────────────────────────── */

int rdm_dispatch_upsert(const char *roadmap_id, const char *token_profile,
                        int require_slice_discussion, int budget_ceiling_tokens)
{
   sqlite3 *db = db1_conn();
   if (!db || !roadmap_id || !roadmap_id[0])
      return -1;
   const char *sql = "INSERT OR REPLACE INTO roadmap_dispatch"
                     " (roadmap_id, status, phase, token_profile,"
                     "  require_slice_discussion, budget_ceiling_tokens,"
                     "  exit_reason, created_at, updated_at)"
                     " VALUES (?, 'running', 'plan', ?,"
                     "         ?, ?,"
                     "         '', datetime('now'), datetime('now'))";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, roadmap_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, token_profile ? token_profile : "balanced", -1, SQLITE_STATIC);
   sqlite3_bind_int(st, 3, require_slice_discussion);
   sqlite3_bind_int(st, 4, budget_ceiling_tokens);
   int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
   sqlite3_finalize(st);
   return rc;
}

int rdm_dispatch_get(const char *roadmap_id, rdm_dispatch_t *out)
{
   sqlite3 *db = db1_conn();
   if (!db || !roadmap_id || !roadmap_id[0] || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   const char *sql = "SELECT id, roadmap_id, status, phase, token_profile,"
                     "       require_slice_discussion, budget_ceiling_tokens,"
                     "       exit_reason, created_at, updated_at"
                     " FROM roadmap_dispatch WHERE roadmap_id = ? LIMIT 1";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, roadmap_id, -1, SQLITE_STATIC);
   int rc = -1;
   if (sqlite3_step(st) == SQLITE_ROW)
   {
      out->id = sqlite3_column_int(st, 0);
      db1_copy_col_text(out->roadmap_id, sizeof(out->roadmap_id), st, 1);
      db1_copy_col_text(out->status, sizeof(out->status), st, 2);
      db1_copy_col_text(out->phase, sizeof(out->phase), st, 3);
      db1_copy_col_text(out->token_profile, sizeof(out->token_profile), st, 4);
      out->require_slice_discussion = sqlite3_column_int(st, 5);
      out->budget_ceiling_tokens = sqlite3_column_int(st, 6);
      db1_copy_col_text(out->exit_reason, sizeof(out->exit_reason), st, 7);
      db1_copy_col_text(out->created_at, sizeof(out->created_at), st, 8);
      db1_copy_col_text(out->updated_at, sizeof(out->updated_at), st, 9);
      rc = 0;
   }
   sqlite3_finalize(st);
   return rc;
}

int rdm_dispatch_set_status(const char *roadmap_id, const char *status, const char *exit_reason)
{
   sqlite3 *db = db1_conn();
   if (!db || !roadmap_id || !roadmap_id[0])
      return -1;
   const char *sql = "UPDATE roadmap_dispatch"
                     " SET status = ?,"
                     "     exit_reason = COALESCE(NULLIF(?, ''), exit_reason),"
                     "     updated_at = datetime('now')"
                     " WHERE roadmap_id = ?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, status ? status : "running", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, exit_reason ? exit_reason : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, roadmap_id, -1, SQLITE_STATIC);
   int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
   sqlite3_finalize(st);
   return rc;
}

int rdm_dispatch_set_phase(const char *roadmap_id, const char *phase)
{
   sqlite3 *db = db1_conn();
   if (!db || !roadmap_id || !roadmap_id[0])
      return -1;
   const char *sql = "UPDATE roadmap_dispatch SET phase = ?, updated_at = datetime('now')"
                     " WHERE roadmap_id = ?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, phase ? phase : "plan", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, roadmap_id, -1, SQLITE_STATIC);
   int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
   sqlite3_finalize(st);
   return rc;
}

/* ── roadmap_unit_dispatch ───────────────────────────────────────────────── */

int rdm_unit_ensure(const char *roadmap_id, const char *unit_id, const char *level,
                    const char *tool_policy_mode)
{
   sqlite3 *db = db1_conn();
   if (!db || !roadmap_id || !unit_id)
      return -1;
   const char *sql = "INSERT OR IGNORE INTO roadmap_unit_dispatch"
                     " (roadmap_id, unit_id, level, state, tool_policy_mode,"
                     "  created_at, updated_at)"
                     " VALUES (?, ?, ?, 'pending', ?,"
                     "         datetime('now'), datetime('now'))";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, roadmap_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, unit_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, level ? level : "task", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 4, tool_policy_mode ? tool_policy_mode : "execution", -1, SQLITE_STATIC);
   int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
   sqlite3_finalize(st);
   return rc;
}

int rdm_unit_get(const char *roadmap_id, const char *unit_id, rdm_unit_dispatch_t *out)
{
   sqlite3 *db = db1_conn();
   if (!db || !roadmap_id || !unit_id || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   const char *sql = "SELECT id, roadmap_id, unit_id, level, state, tool_policy_mode,"
                     "       claimed_by, claimed_at, heartbeat_at,"
                     "       verify_attempts, dispatch_attempts,"
                     "       worktree_path, coord_job_id, result, error,"
                     "       created_at, updated_at"
                     " FROM roadmap_unit_dispatch"
                     " WHERE roadmap_id = ? AND unit_id = ? LIMIT 1";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, roadmap_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, unit_id, -1, SQLITE_STATIC);
   int rc = -1;
   if (sqlite3_step(st) == SQLITE_ROW)
   {
      out->id = sqlite3_column_int(st, 0);
      db1_copy_col_text(out->roadmap_id, sizeof(out->roadmap_id), st, 1);
      db1_copy_col_text(out->unit_id, sizeof(out->unit_id), st, 2);
      db1_copy_col_text(out->level, sizeof(out->level), st, 3);
      db1_copy_col_text(out->state, sizeof(out->state), st, 4);
      db1_copy_col_text(out->tool_policy_mode, sizeof(out->tool_policy_mode), st, 5);
      db1_copy_col_text(out->claimed_by, sizeof(out->claimed_by), st, 6);
      db1_copy_col_text(out->claimed_at, sizeof(out->claimed_at), st, 7);
      db1_copy_col_text(out->heartbeat_at, sizeof(out->heartbeat_at), st, 8);
      out->verify_attempts = sqlite3_column_int(st, 9);
      out->dispatch_attempts = sqlite3_column_int(st, 10);
      db1_copy_col_text(out->worktree_path, sizeof(out->worktree_path), st, 11);
      out->coord_job_id = sqlite3_column_int(st, 12);
      db1_copy_col_text(out->result, sizeof(out->result), st, 13);
      db1_copy_col_text(out->error, sizeof(out->error), st, 14);
      db1_copy_col_text(out->created_at, sizeof(out->created_at), st, 15);
      db1_copy_col_text(out->updated_at, sizeof(out->updated_at), st, 16);
      rc = 0;
   }
   sqlite3_finalize(st);
   return rc;
}

int rdm_unit_set_state(const char *roadmap_id, const char *unit_id, const char *state)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   const char *sql = "UPDATE roadmap_unit_dispatch SET state = ?, updated_at = datetime('now')"
                     " WHERE roadmap_id = ? AND unit_id = ?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, state ? state : "pending", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, roadmap_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, unit_id, -1, SQLITE_STATIC);
   int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
   sqlite3_finalize(st);
   return rc;
}

int rdm_unit_claim(const char *roadmap_id, const char *unit_id, const char *owner,
                   const char *worktree_path)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   const char *sql = "UPDATE roadmap_unit_dispatch"
                     " SET claimed_by = ?,"
                     "     claimed_at = datetime('now'),"
                     "     heartbeat_at = datetime('now'),"
                     "     state = 'active',"
                     "     dispatch_attempts = dispatch_attempts + 1,"
                     "     worktree_path = ?,"
                     "     updated_at = datetime('now')"
                     " WHERE roadmap_id = ? AND unit_id = ?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, owner ? owner : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, worktree_path ? worktree_path : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, roadmap_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 4, unit_id, -1, SQLITE_STATIC);
   int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
   sqlite3_finalize(st);
   return rc;
}

int rdm_unit_heartbeat(const char *roadmap_id, const char *unit_id)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   const char *sql = "UPDATE roadmap_unit_dispatch"
                     " SET heartbeat_at = datetime('now'), updated_at = datetime('now')"
                     " WHERE roadmap_id = ? AND unit_id = ?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, roadmap_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, unit_id, -1, SQLITE_STATIC);
   int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
   sqlite3_finalize(st);
   return rc;
}

int rdm_unit_finish(const char *roadmap_id, const char *unit_id, const char *state,
                    const char *result, const char *error)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   const char *sql = "UPDATE roadmap_unit_dispatch"
                     " SET state = ?, result = ?, error = ?, updated_at = datetime('now')"
                     " WHERE roadmap_id = ? AND unit_id = ?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, state ? state : "done", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, result ? result : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, error ? error : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 4, roadmap_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 5, unit_id, -1, SQLITE_STATIC);
   int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
   sqlite3_finalize(st);
   return rc;
}

int rdm_unit_set_coord_job(const char *roadmap_id, const char *unit_id, int coord_job_id)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   const char *sql = "UPDATE roadmap_unit_dispatch"
                     " SET coord_job_id = ?, updated_at = datetime('now')"
                     " WHERE roadmap_id = ? AND unit_id = ?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(st, 1, coord_job_id);
   sqlite3_bind_text(st, 2, roadmap_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, unit_id, -1, SQLITE_STATIC);
   int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
   sqlite3_finalize(st);
   return rc;
}

int rdm_unit_increment_verify_attempts(const char *roadmap_id, const char *unit_id)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   const char *sql = "UPDATE roadmap_unit_dispatch"
                     " SET verify_attempts = verify_attempts + 1, updated_at = datetime('now')"
                     " WHERE roadmap_id = ? AND unit_id = ?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, roadmap_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, unit_id, -1, SQLITE_STATIC);
   int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
   sqlite3_finalize(st);
   return rc;
}

int rdm_unit_select_next(const char *roadmap_id, char *out_unit_id, size_t len)
{
   sqlite3 *db = db1_conn();
   if (!db || !roadmap_id || !out_unit_id)
      return -1;
   /* Single-track greedy: first pending task with no unclaimed dependencies.
    * A dependency is satisfied when its rdm_unit_dispatch row has state='done'
    * (or the row doesn't exist yet — treated as not-yet-known, so skipped).
    * For the Phase-2 single-track baseline we pick the lowest-id pending task
    * whose dispatch row is pending/not-started with no active or pending deps.
    * Full DAG dependency checking is done by the caller (roadmap_auto.c) once
    * the unit payload is loaded; here we do a fast SQL approximation. */
   const char *sql = "SELECT unit_id FROM roadmap_unit_dispatch"
                     " WHERE roadmap_id = ?"
                     "   AND level = 'task'"
                     "   AND state = 'pending'"
                     "   AND claimed_by = ''"
                     " ORDER BY id"
                     " LIMIT 1";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, roadmap_id, -1, SQLITE_STATIC);
   int rc = 1; /* default: no unit found */
   if (sqlite3_step(st) == SQLITE_ROW)
   {
      const unsigned char *v = sqlite3_column_text(st, 0);
      if (v && v[0])
      {
         snprintf(out_unit_id, len, "%s", (const char *)v);
         rc = 0;
      }
   }
   sqlite3_finalize(st);
   return rc;
}
