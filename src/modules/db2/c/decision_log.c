/* db2/decision_log.c: task-keyed decision log — Postgres via libpq. */

#include "decision_log.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void row_from_stmt(aimee_pg_stmt_t *st, db2_decision_log_row_t *row)
{
   if (!st || !row)
      return;
   memset(row, 0, sizeof(*row));
   row->id = aimee_pg_column_int64(st, 0);
   row->task_id = aimee_pg_column_int64(st, 1);
   const char *col;
   col = aimee_pg_column_text(st, 2);
   snprintf(row->options, sizeof(row->options), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 3);
   snprintf(row->chosen, sizeof(row->chosen), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 4);
   snprintf(row->rationale, sizeof(row->rationale), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 5);
   snprintf(row->assumptions, sizeof(row->assumptions), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 6);
   snprintf(row->outcome, sizeof(row->outcome), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 7);
   snprintf(row->created_at, sizeof(row->created_at), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 8);
   snprintf(row->status, sizeof(row->status), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 9);
   snprintf(row->revisit_when, sizeof(row->revisit_when), "%s", col ? col : "");
   row->supersedes_id = aimee_pg_column_int64(st, 10);
   col = aimee_pg_column_text(st, 11);
   snprintf(row->subject, sizeof(row->subject), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 12);
   snprintf(row->author, sizeof(row->author), "%s", col ? col : "");
   row->linked_policy_id = aimee_pg_column_int64(st, 13);
}

int db2_decision_log_insert(int64_t task_id, const char *options, const char *chosen,
                            const char *rationale, const char *assumptions, const char *created_at,
                            db2_decision_log_row_t *out)
{
   if (!options || !chosen)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   const char *sql = created_at ? "INSERT INTO decision_log (task_id, options, chosen, rationale, "
                                  "assumptions, created_at)"
                                  " VALUES (?1, ?2, ?3, ?4, ?5, ?6) RETURNING id"
                                : "INSERT INTO decision_log (task_id, options, chosen, rationale, "
                                  "assumptions, created_at)"
                                  " VALUES (?1, ?2, ?3, ?4, ?5, pg_now_text()) RETURNING id";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int64(st, "?1", task_id);
   aimee_pg_bind_text(st, "?2", options);
   aimee_pg_bind_text(st, "?3", chosen);
   aimee_pg_bind_text(st, "?4", rationale ? rationale : "");
   aimee_pg_bind_text(st, "?5", assumptions ? assumptions : "");
   if (created_at)
      aimee_pg_bind_text(st, "?6", created_at);

   int64_t new_id = 0;
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ROW)
      new_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return -1;

   if (out)
      return db2_decision_log_get(new_id, out);
   return 0;
}

int db2_decision_log_record(const char *subject, const char *options, const char *chosen,
                            const char *rationale, const char *author, int64_t linked_policy_id,
                            const char *revisit_when, int64_t supersedes_id,
                            db2_decision_log_row_t *out)
{
   if (!subject || !subject[0] || !options || !chosen)
      return -1; /* a real scope is required (empty subject would be one global slot) */
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   /* Flip the decision this one replaces to 'superseded' in the same txn, so the
    * new active decision does not collide with it on the scope invariant. The
    * UPDATE is constrained to an ACTIVE decision in the SAME scope, and must
    * affect exactly one row — otherwise supersedes_id is stale/wrong-scope and
    * the whole record is rejected (no silent supersede of nothing/unrelated). */
   if (supersedes_id > 0)
   {
      aimee_pg_stmt_t *up = aimee_pg_prepare(conn,
                                             "UPDATE decision_log SET status = 'superseded'"
                                             " WHERE id = ?1 AND status = 'active' AND subject = ?2"
                                             " AND linked_policy_id = ?3",
                                             err, sizeof(err));
      if (!up)
      {
         aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
         return -1;
      }
      aimee_pg_bind_int64(up, "?1", supersedes_id);
      aimee_pg_bind_text(up, "?2", subject);
      aimee_pg_bind_int64(up, "?3", linked_policy_id);
      aimee_pg_step_t urc = aimee_pg_step(up, err, sizeof(err));
      int changes = aimee_pg_stmt_changes(up);
      aimee_pg_finalize(up);
      if (urc != AIMEE_PG_DONE || changes != 1)
      {
         aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
         return -1;
      }
   }

   const char *sql =
       "INSERT INTO decision_log (task_id, options, chosen, rationale, assumptions, created_at,"
       " status, revisit_when, supersedes_id, subject, author, linked_policy_id)"
       " VALUES (0, ?1, ?2, ?3, '', pg_now_text(), 'active', ?4, ?5, ?6, ?7, ?8) RETURNING id";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   /* Bind by explicit placeholder (?1..?8 = the INSERT column order above), which
    * differs from the C parameter order — bind names, not positions. */
   aimee_pg_bind_text(st, "?1", options);
   aimee_pg_bind_text(st, "?2", chosen);
   aimee_pg_bind_text(st, "?3", rationale ? rationale : "");
   aimee_pg_bind_text(st, "?4", revisit_when ? revisit_when : "");
   aimee_pg_bind_int64(st, "?5", supersedes_id);
   aimee_pg_bind_text(st, "?6", subject);
   aimee_pg_bind_text(st, "?7", author ? author : "");
   aimee_pg_bind_int64(st, "?8", linked_policy_id);

   int64_t new_id = 0;
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ROW)
      new_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
   {
      /* Most likely idx_dl_active_scope rejected a second active decision for
       * this (subject, linked_policy_id). */
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }

   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }

   if (out)
      return db2_decision_log_get(new_id, out);
   return 0;
}

int db2_decision_log_mark_revisit_due(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   /* idx_dl_revisit backs this predicate. revisit_when and pg_now_text() are both
    * ISO-8601, so the lexicographic <= is a correct time comparison; a date-only
    * revisit_when is a prefix of the datetime (same-day sorts as due). */
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE decision_log SET status = 'revisit_due'"
                                          " WHERE status = 'active' AND revisit_when != ''"
                                          " AND revisit_when <= pg_now_text()",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? changes : -1;
}

int db2_decision_log_get(int64_t id, db2_decision_log_row_t *out)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT id, task_id, options, chosen, rationale, assumptions, outcome, created_at,"
       " status, revisit_when, supersedes_id, subject, author, linked_policy_id"
       " FROM decision_log WHERE id = ?1",
       err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int64(st, "?1", id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      row_from_stmt(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_decision_log_set_outcome(int64_t id, const char *outcome)
{
   void *conn = db2_conn();
   if (!conn || !outcome)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE decision_log SET outcome = ?1 WHERE id = ?2", err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", outcome);
   aimee_pg_bind_int64(st, "?2", id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE && changes > 0) ? 0 : -1;
}

/* All columns in row_from_stmt order — keep in sync with that function. */
#define DL_COLS                                                                                    \
   "id, task_id, options, chosen, rationale, assumptions, outcome, created_at, status, "           \
   "revisit_when, supersedes_id, subject, author, linked_policy_id"

int db2_decision_log_list_scoped(const char *subject, const char *status, int limit,
                                 db2_decision_log_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   if (limit <= 0 || limit > max)
      limit = max;
   char sql[512];
   int pos = snprintf(sql, sizeof(sql), "SELECT " DL_COLS " FROM decision_log WHERE 1=1");
   int has_subject = subject && subject[0];
   int has_status = status && status[0];
   if (has_subject)
      pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos, " AND subject = ?1");
   if (has_status)
      pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos, " AND status = ?2");
   snprintf(sql + pos, sizeof(sql) - (size_t)pos, " ORDER BY created_at DESC, id DESC LIMIT ?3");
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   if (has_subject)
      aimee_pg_bind_text(st, "?1", subject);
   if (has_status)
      aimee_pg_bind_text(st, "?2", status);
   aimee_pg_bind_int64(st, "?3", limit);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      row_from_stmt(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

static int dl_update_field(int64_t id, const char *sql, const char *val)
{
   void *conn = db2_conn();
   if (!conn || !val)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", val);
   aimee_pg_bind_int64(st, "?2", id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE && changes > 0) ? 0 : -1;
}

int64_t db2_decision_log_active_id(const char *subject, int64_t linked_policy_id)
{
   if (!subject || !subject[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[256] = "";
   /* Matches idx_dl_active_scope exactly: (subject, linked_policy_id) WHERE active. */
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id FROM decision_log WHERE subject = ?1 AND linked_policy_id = ?2"
                        " AND status = 'active' LIMIT 1",
                        err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", subject);
   aimee_pg_bind_int64(st, "?2", linked_policy_id);
   int64_t id = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_decision_log_set_status(int64_t id, const char *status)
{
   return dl_update_field(id, "UPDATE decision_log SET status = ?1 WHERE id = ?2", status);
}

int db2_decision_log_set_revisit(int64_t id, const char *revisit_when)
{
   return dl_update_field(id, "UPDATE decision_log SET revisit_when = ?1 WHERE id = ?2",
                          revisit_when);
}

int db2_decision_log_list(const char *outcome, int limit, db2_decision_log_row_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out || max <= 0)
      return 0;

   char sql[512];
   int pos =
       snprintf(sql, sizeof(sql),
                "SELECT id, task_id, options, chosen, rationale, assumptions, outcome, created_at,"
                " status, revisit_when, supersedes_id, subject, author, linked_policy_id"
                " FROM decision_log WHERE 1=1");
   int bind_outcome = 0;
   if (outcome && outcome[0])
   {
      pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos, " AND outcome = ?1");
      bind_outcome = 1;
   }
   pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos, " ORDER BY created_at DESC");
   if (limit > 0)
      snprintf(sql + pos, sizeof(sql) - (size_t)pos, " LIMIT %d", limit);

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   if (bind_outcome)
      aimee_pg_bind_text(st, "?1", outcome);

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      row_from_stmt(st, &out[count++]);

   aimee_pg_finalize(st);
   return count;
}
