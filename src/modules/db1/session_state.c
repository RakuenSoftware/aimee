/* db1/session_state.c: per-session guardrail/hook scratch state.
 *
 * Normalized replacement for the legacy ~/.config/aimee/session-<sid>.state
 * JSON file. Save rewrites the six child tables within a single transaction
 * so readers never see a half-updated state. */

#include "session_state.h"
#include "db1_internal.h"

#include <inttypes.h>
#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int db1_session_state_exists(const char *sid)
{
   if (!sid || !sid[0])
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT 1 FROM session_state WHERE session_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
   int found = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0;
   sqlite3_finalize(stmt);
   return found;
}

int db1_session_state_load(const char *sid, session_state_t *out)
{
   if (!sid || !sid[0] || !out)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *scalar_sql =
       "SELECT session_mode, guardrail_mode, tdd_mode, active_task_id, hook_call_count,"
       " orch_direct_edits, orch_nudge_sent, skill_find_symbols_advisory_sent,"
       " skill_condition_waiting_advisory_sent, skill_tdd_advisory_sent"
       " FROM session_state WHERE session_id = ?";
   if (sqlite3_prepare_v2(db, scalar_sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) != SQLITE_ROW)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   db1_copy_col_text(out->session_mode, sizeof(out->session_mode), stmt, 0);
   db1_copy_col_text(out->guardrail_mode, sizeof(out->guardrail_mode), stmt, 1);
   db1_copy_col_text(out->tdd_mode, sizeof(out->tdd_mode), stmt, 2);
   out->active_task_id = sqlite3_column_int64(stmt, 3);
   out->hook_call_count = sqlite3_column_int(stmt, 4);
   out->orch_direct_edits = sqlite3_column_int(stmt, 5);
   out->orch_nudge_sent = sqlite3_column_int(stmt, 6);
   out->skill_find_symbols_advisory_sent = sqlite3_column_int(stmt, 7);
   out->skill_condition_waiting_advisory_sent = sqlite3_column_int(stmt, 8);
   out->skill_tdd_advisory_sent = sqlite3_column_int(stmt, 9);
   sqlite3_finalize(stmt);

   /* seen_paths (ordered by seq) */
   static const char *seen_sql =
       "SELECT path FROM session_state_seen_paths WHERE session_id = ? ORDER BY seq";
   if (sqlite3_prepare_v2(db, seen_sql, -1, &stmt, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
      out->seen_count = 0;
      while (sqlite3_step(stmt) == SQLITE_ROW && out->seen_count < MAX_SEEN_PATHS)
      {
         db1_copy_col_text(out->seen_paths[out->seen_count], MAX_SEEN_LEN, stmt, 0);
         out->seen_count++;
      }
      sqlite3_finalize(stmt);
   }

   /* read_paths (ordered by seq) */
   static const char *read_sql =
       "SELECT path FROM session_state_read_paths WHERE session_id = ? ORDER BY seq";
   if (sqlite3_prepare_v2(db, read_sql, -1, &stmt, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
      out->read_path_count = 0;
      while (sqlite3_step(stmt) == SQLITE_ROW && out->read_path_count < MAX_READ_PATHS)
      {
         db1_copy_col_text(out->read_paths[out->read_path_count], MAX_SEEN_LEN, stmt, 0);
         out->read_path_count++;
      }
      sqlite3_finalize(stmt);
   }

   /* worktrees (ordered by seq to preserve insertion order) */
   static const char *wt_sql = "SELECT git_root, worktree_path FROM session_state_worktrees"
                               " WHERE session_id = ? ORDER BY seq";
   if (sqlite3_prepare_v2(db, wt_sql, -1, &stmt, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
      out->worktree_count = 0;
      while (sqlite3_step(stmt) == SQLITE_ROW && out->worktree_count < MAX_WORKTREES)
      {
         worktree_mapping_t *m = &out->worktrees[out->worktree_count];
         db1_copy_col_text(m->git_root, sizeof(m->git_root), stmt, 0);
         db1_copy_col_text(m->worktree_path, sizeof(m->worktree_path), stmt, 1);
         out->worktree_count++;
      }
      sqlite3_finalize(stmt);
   }

   /* tdd_writes (ordered by seq) */
   static const char *tdd_sql =
       "SELECT stem, is_test FROM session_state_tdd_writes WHERE session_id = ? ORDER BY seq";
   if (sqlite3_prepare_v2(db, tdd_sql, -1, &stmt, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
      out->tdd_write_count = 0;
      while (sqlite3_step(stmt) == SQLITE_ROW && out->tdd_write_count < MAX_TDD_WRITES)
      {
         tdd_write_t *w = &out->tdd_writes[out->tdd_write_count];
         db1_copy_col_text(w->stem, sizeof(w->stem), stmt, 0);
         w->is_test = sqlite3_column_int(stmt, 1);
         out->tdd_write_count++;
      }
      sqlite3_finalize(stmt);
   }

   /* ap_hits */
   static const char *ap_sql =
       "SELECT pattern_id, hits FROM session_state_ap_hits WHERE session_id = ?";
   if (sqlite3_prepare_v2(db, ap_sql, -1, &stmt, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
      out->ap_hit_count = 0;
      while (sqlite3_step(stmt) == SQLITE_ROW && out->ap_hit_count < MAX_AP_SESSION_HITS)
      {
         ap_session_hit_t *a = &out->ap_hits[out->ap_hit_count];
         a->pattern_id = sqlite3_column_int64(stmt, 0);
         a->hits = sqlite3_column_int(stmt, 1);
         out->ap_hit_count++;
      }
      sqlite3_finalize(stmt);
   }

   /* file_hashes: content_hash stored as TEXT (decimal uint64) */
   static const char *fh_sql =
       "SELECT path, content_hash FROM session_state_file_hashes WHERE session_id = ?";
   if (sqlite3_prepare_v2(db, fh_sql, -1, &stmt, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
      out->file_hash_count = 0;
      while (sqlite3_step(stmt) == SQLITE_ROW && out->file_hash_count < MAX_FILE_HASHES)
      {
         file_read_hash_t *r = &out->file_hashes[out->file_hash_count];
         db1_copy_col_text(r->path, sizeof(r->path), stmt, 0);
         const unsigned char *hv = sqlite3_column_text(stmt, 1);
         r->content_hash = hv ? (uint64_t)strtoull((const char *)hv, NULL, 10) : 0;
         out->file_hash_count++;
      }
      sqlite3_finalize(stmt);
   }

   return 0;
}

int db1_session_state_save(const char *sid, const session_state_t *in)
{
   if (!sid || !sid[0] || !in)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   if (db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return -1;

   /* Upsert scalar row. */
   sqlite3_stmt *stmt = NULL;
   static const char *upsert_sql =
       "INSERT INTO session_state ("
       "  session_id, session_mode, guardrail_mode, tdd_mode,"
       "  active_task_id, hook_call_count, orch_direct_edits, orch_nudge_sent,"
       "  skill_find_symbols_advisory_sent, skill_condition_waiting_advisory_sent,"
       "  skill_tdd_advisory_sent,"
       "  created_at, updated_at)"
       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now'), datetime('now'))"
       " ON CONFLICT(session_id) DO UPDATE SET"
       "  session_mode = excluded.session_mode,"
       "  guardrail_mode = excluded.guardrail_mode,"
       "  tdd_mode = excluded.tdd_mode,"
       "  active_task_id = excluded.active_task_id,"
       "  hook_call_count = excluded.hook_call_count,"
       "  orch_direct_edits = excluded.orch_direct_edits,"
       "  orch_nudge_sent = excluded.orch_nudge_sent,"
       "  skill_find_symbols_advisory_sent = excluded.skill_find_symbols_advisory_sent,"
       "  skill_condition_waiting_advisory_sent ="
       " excluded.skill_condition_waiting_advisory_sent,"
       "  skill_tdd_advisory_sent = excluded.skill_tdd_advisory_sent,"
       "  updated_at = datetime('now')";
   if (sqlite3_prepare_v2(db, upsert_sql, -1, &stmt, NULL) != SQLITE_OK)
      goto rollback;
   sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, in->session_mode[0] ? in->session_mode : "implement", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, in->guardrail_mode[0] ? in->guardrail_mode : "approve", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, in->tdd_mode[0] ? in->tdd_mode : "off", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 5, (sqlite3_int64)in->active_task_id);
   sqlite3_bind_int(stmt, 6, in->hook_call_count);
   sqlite3_bind_int(stmt, 7, in->orch_direct_edits);
   sqlite3_bind_int(stmt, 8, in->orch_nudge_sent);
   sqlite3_bind_int(stmt, 9, in->skill_find_symbols_advisory_sent);
   sqlite3_bind_int(stmt, 10, in->skill_condition_waiting_advisory_sent);
   sqlite3_bind_int(stmt, 11, in->skill_tdd_advisory_sent);
   if (sqlite3_step(stmt) != SQLITE_DONE)
   {
      sqlite3_finalize(stmt);
      goto rollback;
   }
   sqlite3_finalize(stmt);

   /* Rewrite child tables: DELETE + bulk INSERT per table. */
   static const char *const delete_children[] = {
       "DELETE FROM session_state_seen_paths WHERE session_id = ?",
       "DELETE FROM session_state_read_paths WHERE session_id = ?",
       "DELETE FROM session_state_worktrees WHERE session_id = ?",
       "DELETE FROM session_state_tdd_writes WHERE session_id = ?",
       "DELETE FROM session_state_ap_hits WHERE session_id = ?",
       "DELETE FROM session_state_file_hashes WHERE session_id = ?",
   };
   for (size_t i = 0; i < sizeof(delete_children) / sizeof(delete_children[0]); i++)
   {
      if (sqlite3_prepare_v2(db, delete_children[i], -1, &stmt, NULL) != SQLITE_OK)
         goto rollback;
      sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt) != SQLITE_DONE)
      {
         sqlite3_finalize(stmt);
         goto rollback;
      }
      sqlite3_finalize(stmt);
   }

   /* seen_paths */
   if (in->seen_count > 0)
   {
      static const char *ins_sql =
          "INSERT INTO session_state_seen_paths(session_id, seq, path) VALUES (?, ?, ?)";
      if (sqlite3_prepare_v2(db, ins_sql, -1, &stmt, NULL) != SQLITE_OK)
         goto rollback;
      for (int i = 0; i < in->seen_count; i++)
      {
         sqlite3_reset(stmt);
         sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int(stmt, 2, i);
         sqlite3_bind_text(stmt, 3, in->seen_paths[i], -1, SQLITE_TRANSIENT);
         if (sqlite3_step(stmt) != SQLITE_DONE)
         {
            sqlite3_finalize(stmt);
            goto rollback;
         }
      }
      sqlite3_finalize(stmt);
   }

   /* read_paths */
   if (in->read_path_count > 0)
   {
      static const char *ins_sql =
          "INSERT INTO session_state_read_paths(session_id, seq, path) VALUES (?, ?, ?)";
      if (sqlite3_prepare_v2(db, ins_sql, -1, &stmt, NULL) != SQLITE_OK)
         goto rollback;
      for (int i = 0; i < in->read_path_count; i++)
      {
         sqlite3_reset(stmt);
         sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int(stmt, 2, i);
         sqlite3_bind_text(stmt, 3, in->read_paths[i], -1, SQLITE_TRANSIENT);
         if (sqlite3_step(stmt) != SQLITE_DONE)
         {
            sqlite3_finalize(stmt);
            goto rollback;
         }
      }
      sqlite3_finalize(stmt);
   }

   /* worktrees */
   if (in->worktree_count > 0)
   {
      static const char *ins_sql =
          "INSERT INTO session_state_worktrees(session_id, seq, git_root, worktree_path)"
          " VALUES (?, ?, ?, ?)";
      if (sqlite3_prepare_v2(db, ins_sql, -1, &stmt, NULL) != SQLITE_OK)
         goto rollback;
      for (int i = 0; i < in->worktree_count; i++)
      {
         sqlite3_reset(stmt);
         sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int(stmt, 2, i);
         sqlite3_bind_text(stmt, 3, in->worktrees[i].git_root, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 4, in->worktrees[i].worktree_path, -1, SQLITE_TRANSIENT);
         if (sqlite3_step(stmt) != SQLITE_DONE)
         {
            sqlite3_finalize(stmt);
            goto rollback;
         }
      }
      sqlite3_finalize(stmt);
   }

   /* tdd_writes */
   if (in->tdd_write_count > 0)
   {
      static const char *ins_sql =
          "INSERT INTO session_state_tdd_writes(session_id, seq, stem, is_test)"
          " VALUES (?, ?, ?, ?)";
      if (sqlite3_prepare_v2(db, ins_sql, -1, &stmt, NULL) != SQLITE_OK)
         goto rollback;
      for (int i = 0; i < in->tdd_write_count; i++)
      {
         sqlite3_reset(stmt);
         sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int(stmt, 2, i);
         sqlite3_bind_text(stmt, 3, in->tdd_writes[i].stem, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int(stmt, 4, in->tdd_writes[i].is_test);
         if (sqlite3_step(stmt) != SQLITE_DONE)
         {
            sqlite3_finalize(stmt);
            goto rollback;
         }
      }
      sqlite3_finalize(stmt);
   }

   /* ap_hits */
   if (in->ap_hit_count > 0)
   {
      static const char *ins_sql =
          "INSERT INTO session_state_ap_hits(session_id, pattern_id, hits) VALUES (?, ?, ?)";
      if (sqlite3_prepare_v2(db, ins_sql, -1, &stmt, NULL) != SQLITE_OK)
         goto rollback;
      for (int i = 0; i < in->ap_hit_count; i++)
      {
         sqlite3_reset(stmt);
         sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int64(stmt, 2, (sqlite3_int64)in->ap_hits[i].pattern_id);
         sqlite3_bind_int(stmt, 3, in->ap_hits[i].hits);
         if (sqlite3_step(stmt) != SQLITE_DONE)
         {
            sqlite3_finalize(stmt);
            goto rollback;
         }
      }
      sqlite3_finalize(stmt);
   }

   /* file_hashes */
   if (in->file_hash_count > 0)
   {
      static const char *ins_sql =
          "INSERT INTO session_state_file_hashes(session_id, path, content_hash)"
          " VALUES (?, ?, ?)";
      if (sqlite3_prepare_v2(db, ins_sql, -1, &stmt, NULL) != SQLITE_OK)
         goto rollback;
      for (int i = 0; i < in->file_hash_count; i++)
      {
         char hbuf[32];
         snprintf(hbuf, sizeof(hbuf), "%" PRIu64, in->file_hashes[i].content_hash);
         sqlite3_reset(stmt);
         sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 2, in->file_hashes[i].path, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 3, hbuf, -1, SQLITE_TRANSIENT);
         if (sqlite3_step(stmt) != SQLITE_DONE)
         {
            sqlite3_finalize(stmt);
            goto rollback;
         }
      }
      sqlite3_finalize(stmt);
   }

   if (db1_txn_end(db, "COMMIT") != 0)
      return -1; /* gate already released; a failed COMMIT auto-rolls-back */
   return 0;

rollback:
   db1_txn_end(db, "ROLLBACK");
   return -1;
}

int db1_session_state_delete(const char *sid)
{
   if (!sid || !sid[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   /* ON DELETE CASCADE handles the child tables. */
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM session_state WHERE session_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_session_state_list(db1_session_state_summary_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT session_id, updated_at, hook_call_count"
                            " FROM session_state ORDER BY updated_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].session_id, sizeof(out[n].session_id), stmt, 0);
      db1_copy_col_text(out[n].updated_at, sizeof(out[n].updated_at), stmt, 1);
      out[n].hook_call_count = sqlite3_column_int(stmt, 2);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_session_state_get_summary(const char *sid, db1_session_state_summary_t *out)
{
   if (!sid || !sid[0] || !out)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT session_id, updated_at, hook_call_count"
                            " FROM session_state WHERE session_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out->session_id, sizeof(out->session_id), stmt, 0);
      db1_copy_col_text(out->updated_at, sizeof(out->updated_at), stmt, 1);
      out->hook_call_count = sqlite3_column_int(stmt, 2);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_session_state_list_expired(int threshold_seconds, char (*out_ids)[DB1_SS_SID_LEN], int max)
{
   if (!out_ids || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;
   char sql[192];
   snprintf(sql, sizeof(sql),
            "SELECT session_id FROM session_state"
            " WHERE updated_at <= datetime('now', '-%d seconds') LIMIT ?",
            threshold_seconds);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *v = sqlite3_column_text(stmt, 0);
      snprintf(out_ids[n], DB1_SS_SID_LEN, "%s", v ? (const char *)v : "");
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}
