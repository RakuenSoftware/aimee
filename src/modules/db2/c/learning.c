/* db2/learning.c: learning_signals + learning_proposals primitives —
 * Postgres via libpq. */

#include "db2_learning.h"
#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "log.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define LRN_ERRBUF 256

static void lrn_copy_text(char *dst, size_t dstsz, const char *src, const char *fallback)
{
   const char *s = src ? src : (fallback ? fallback : "");
   snprintf(dst, dstsz, "%s", s);
}

static void lrn_load_proposal_row(aimee_pg_stmt_t *st, learning_proposal_t *out)
{
   memset(out, 0, sizeof(*out));
   out->id = aimee_pg_column_int(st, 0);
   out->signal_id = aimee_pg_column_int(st, 1);
   lrn_copy_text(out->sink, sizeof(out->sink), aimee_pg_column_text(st, 2), "");
   lrn_copy_text(out->state, sizeof(out->state), aimee_pg_column_text(st, 3), "");
   lrn_copy_text(out->target_key, sizeof(out->target_key), aimee_pg_column_text(st, 4), "");
   out->target_memory_id = aimee_pg_column_int64(st, 5);
   lrn_copy_text(out->action_json, sizeof(out->action_json), aimee_pg_column_text(st, 6), "{}");
   lrn_copy_text(out->evidence_refs, sizeof(out->evidence_refs), aimee_pg_column_text(st, 7), "[]");
   out->corroboration_count = aimee_pg_column_int(st, 8);
   lrn_copy_text(out->expires_at, sizeof(out->expires_at), aimee_pg_column_text(st, 9), "");
   lrn_copy_text(out->committed_at, sizeof(out->committed_at), aimee_pg_column_text(st, 10), "");
   lrn_copy_text(out->archive_reason, sizeof(out->archive_reason), aimee_pg_column_text(st, 11),
                 "");
   lrn_copy_text(out->created_at, sizeof(out->created_at), aimee_pg_column_text(st, 12), "");
   lrn_copy_text(out->updated_at, sizeof(out->updated_at), aimee_pg_column_text(st, 13), "");
}

void db2_learning_proposals_archive_expired(void)
{
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql =
       "UPDATE learning_proposals"
       " SET state = 'archived', archive_reason = 'expired', updated_at = pg_now_text()"
       " WHERE state = 'pending' AND expires_at != '' AND expires_at < pg_now_text()";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
      LOG_WARN("db2.learning", "archive_expired: %s", err);
   aimee_pg_finalize(st);
}

int db2_learning_proposal_archive(int id, const char *reason)
{
   if (id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "UPDATE learning_proposals"
       " SET state = 'archived', archive_reason = ?2, updated_at = pg_now_text()"
       " WHERE id = ?1";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", id);
   aimee_pg_bind_text(st, "?2", reason ? reason : "");
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_proposal_bump_corroboration(int id)
{
   if (id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "UPDATE learning_proposals"
       " SET corroboration_count = corroboration_count + 1, updated_at = pg_now_text()"
       " WHERE id = ?1 AND state = 'pending'";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", id);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_commits_in_last_7_days(const char *sink)
{
   if (!sink || !*sink)
      return 0;
   return db2_scalar_int_text("SELECT COUNT(*) FROM learning_proposals"
                              " WHERE sink = ?1 AND state = 'committed'"
                              "   AND committed_at >= pg_now_text('-7 days')",
                              sink, 0);
}

int db2_learning_signal_insert(const learning_signal_input_t *input, const char *source_session)
{
   if (!input)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "INSERT INTO learning_signals ("
       " signal_type, source, polarity, title, description, target_key, target_memory_id,"
       " correction_text, workflow_project, workflow_signal_type, evidence_refs, source_session,"
       " created_at)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, pg_now_text())"
       " RETURNING id";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", input->signal_type);
   aimee_pg_bind_text(st, "?2", input->source[0] ? input->source : "explicit");
   aimee_pg_bind_text(st, "?3", input->polarity);
   aimee_pg_bind_text(st, "?4", input->title);
   aimee_pg_bind_text(st, "?5", input->description);
   aimee_pg_bind_text(st, "?6", input->target_key);
   aimee_pg_bind_int64(st, "?7", input->target_memory_id);
   aimee_pg_bind_text(st, "?8", input->correction_text);
   aimee_pg_bind_text(st, "?9", input->workflow_project);
   aimee_pg_bind_text(st, "?10", input->workflow_signal_type);
   aimee_pg_bind_text(st, "?11", input->evidence_refs_json ? input->evidence_refs_json : "[]");
   aimee_pg_bind_text(st, "?12", source_session ? source_session : "");

   int id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_learning_proposal_find_pending(const char *sink, const char *target_key,
                                       int64_t target_memory_id)
{
   if (!sink || !*sink)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT id FROM learning_proposals"
       " WHERE sink = ?1 AND state = 'pending' AND target_key = ?2 AND target_memory_id = ?3"
       " ORDER BY id DESC LIMIT 1";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", sink);
   aimee_pg_bind_text(st, "?2", target_key ? target_key : "");
   aimee_pg_bind_int64(st, "?3", target_memory_id);
   int id = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_learning_proposal_insert(int signal_id, const char *sink, const char *target_key,
                                 int64_t target_memory_id, const char *action_json,
                                 const char *evidence_refs, const char *expires_at)
{
   if (!sink || !*sink || !action_json)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "INSERT INTO learning_proposals ("
       " signal_id, sink, state, target_key, target_memory_id, action_json, evidence_refs,"
       " corroboration_count, expires_at, created_at, updated_at)"
       " VALUES (?1, ?2, 'pending', ?3, ?4, ?5, ?6, 1, ?7, pg_now_text(), pg_now_text())"
       " RETURNING id";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", signal_id);
   aimee_pg_bind_text(st, "?2", sink);
   aimee_pg_bind_text(st, "?3", target_key ? target_key : "");
   aimee_pg_bind_int64(st, "?4", target_memory_id);
   aimee_pg_bind_text(st, "?5", action_json);
   aimee_pg_bind_text(st, "?6", evidence_refs ? evidence_refs : "[]");
   aimee_pg_bind_text(st, "?7", expires_at ? expires_at : "");

   int id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_learning_proposal_mark_committed(int id)
{
   if (id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "UPDATE learning_proposals"
       " SET state = 'committed', committed_at = pg_now_text(), updated_at = pg_now_text()"
       " WHERE id = ?1";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", id);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   if (rc != 0)
      LOG_ERROR("db2.learning", "mark_committed: %s", err);
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_proposal_get(int id, learning_proposal_t *out)
{
   if (!out || id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "SELECT id, signal_id, sink, state, target_key, target_memory_id, action_json,"
       " evidence_refs, corroboration_count, expires_at, committed_at, archive_reason,"
       " created_at, updated_at"
       " FROM learning_proposals WHERE id = ?1";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      lrn_load_proposal_row(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_proposals_settled_counts(int window_days, int64_t *committed, int64_t *terminal)
{
   if (window_days <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char window_expr[32];
   snprintf(window_expr, sizeof(window_expr), "-%d days", window_days);

   static const char *sql =
       "SELECT"
       " SUM(CASE WHEN state = 'committed' THEN 1 ELSE 0 END),"
       " SUM(CASE WHEN state IN ('committed', 'archived') THEN 1 ELSE 0 END)"
       " FROM learning_proposals"
       " WHERE COALESCE(committed_at, updated_at, created_at) >= pg_now_text(?1)";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", window_expr);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (committed)
         *committed = aimee_pg_column_int64(st, 0);
      if (terminal)
         *terminal = aimee_pg_column_int64(st, 1);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_learning_proposal_list(const char *state, const char *sink, int limit,
                               learning_proposal_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   if (limit <= 0 || limit > max)
      limit = max;

   static const char *sql =
       "SELECT id, signal_id, sink, state, target_key, target_memory_id, action_json,"
       " evidence_refs, corroboration_count, expires_at, committed_at, archive_reason,"
       " created_at, updated_at"
       " FROM learning_proposals"
       " WHERE (?1 = '' OR state = ?2) AND (?3 = '' OR sink = ?4)"
       " ORDER BY id DESC LIMIT ?5";
   char err[LRN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", state ? state : "");
   aimee_pg_bind_text(st, "?2", state ? state : "");
   aimee_pg_bind_text(st, "?3", sink ? sink : "");
   aimee_pg_bind_text(st, "?4", sink ? sink : "");
   aimee_pg_bind_int(st, "?5", limit);

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      lrn_load_proposal_row(st, &out[count]);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}
