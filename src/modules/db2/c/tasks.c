/* db2/tasks.c: task graph — Postgres via libpq. */

#include "tasks.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "aimee.h" /* now_utc, MAX_QUERY_LEN */

#include <stdio.h>
#include <string.h>

#define TASK_ERRBUF 256

static void row_to_task(aimee_pg_stmt_t *st, aimee_task_t *t)
{
   memset(t, 0, sizeof(*t));
   t->id = aimee_pg_column_int64(st, 0);
   t->parent_id = aimee_pg_column_int64(st, 1);
   db2_copy_col_text(t->title, sizeof(t->title), st, 2);
   db2_copy_col_text(t->state, sizeof(t->state), st, 3);
   t->confidence = aimee_pg_column_double(st, 4);
   db2_copy_col_text(t->session_id, sizeof(t->session_id), st, 5);
   db2_copy_col_text(t->created_at, sizeof(t->created_at), st, 6);
   db2_copy_col_text(t->updated_at, sizeof(t->updated_at), st, 7);
}

/* --- Task CRUD --- */

int db2_task_create(const char *title, const char *session_id, int64_t parent_id, aimee_task_t *out)
{
   void *conn = db2_conn();
   if (!conn || !title)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[TASK_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO tasks (parent_id, title, state, confidence, session_id, "
                        "created_at, updated_at) VALUES (?1, ?2, 'todo', 1.0, ?3, ?4, ?5) "
                        "RETURNING id",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int64(st, "?1", parent_id);
   aimee_pg_bind_text(st, "?2", title);
   aimee_pg_bind_text(st, "?3", session_id ? session_id : "");
   aimee_pg_bind_text(st, "?4", ts);
   aimee_pg_bind_text(st, "?5", ts);

   int64_t new_id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      new_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   if (new_id < 0)
      return -1;

   if (out)
      db2_task_get(new_id, out);
   return 0;
}

int db2_task_get(int64_t id, aimee_task_t *out)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return -1;

   char err[TASK_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT id, parent_id, title, state, confidence, "
                                          "session_id, created_at, updated_at FROM tasks "
                                          "WHERE id = ?1",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);

   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      row_to_task(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_task_update_state(int64_t id, const char *state)
{
   void *conn = db2_conn();
   if (!conn || !state)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[TASK_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE tasks SET state = ?1, updated_at = ?2 WHERE id = ?3", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", state);
   aimee_pg_bind_text(st, "?2", ts);
   aimee_pg_bind_int64(st, "?3", id);
   aimee_pg_step_t step_rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   if (step_rc != AIMEE_PG_DONE)
      return -1;
   return changes > 0 ? 0 : -1;
}

int db2_task_list(const char *state, const char *session_id, int limit, aimee_task_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return 0;

   char query[MAX_QUERY_LEN];
   int pos = 0;
   int bind_idx = 0;
   int state_bind = 0, session_bind = 0;

   pos += snprintf(query + pos, sizeof(query) - pos,
                   "SELECT id, parent_id, title, state, confidence, session_id, "
                   "created_at, updated_at FROM tasks WHERE 1=1");

   if (state && state[0])
   {
      bind_idx++;
      state_bind = bind_idx;
      pos += snprintf(query + pos, sizeof(query) - pos, " AND state = ?%d", bind_idx);
   }
   if (session_id && session_id[0])
   {
      bind_idx++;
      session_bind = bind_idx;
      pos += snprintf(query + pos, sizeof(query) - pos, " AND session_id = ?%d", bind_idx);
   }

   pos += snprintf(query + pos, sizeof(query) - pos, " ORDER BY updated_at DESC");
   if (limit > 0)
      snprintf(query + pos, sizeof(query) - pos, " LIMIT %d", limit);

   char err[TASK_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, query, err, sizeof(err));
   if (!st)
      return 0;

   if (state_bind)
   {
      char name[8];
      snprintf(name, sizeof(name), "?%d", state_bind);
      aimee_pg_bind_text(st, name, state);
   }
   if (session_bind)
   {
      char name[8];
      snprintf(name, sizeof(name), "?%d", session_bind);
      aimee_pg_bind_text(st, name, session_id);
   }

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
      row_to_task(st, &out[count++]);
   aimee_pg_finalize(st);
   return count;
}

int db2_task_delete(int64_t id)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[TASK_ERRBUF] = "";
   /* Delete edges first (the schema's FK cascade covers this; explicit
    * for clarity). */
   aimee_pg_stmt_t *e = aimee_pg_prepare(
       conn, "DELETE FROM task_edges WHERE source_id = ?1 OR target_id = ?2", err, sizeof(err));
   if (e)
   {
      aimee_pg_bind_int64(e, "?1", id);
      aimee_pg_bind_int64(e, "?2", id);
      (void)aimee_pg_step(e, err, sizeof(err));
      aimee_pg_finalize(e);
   }

   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "DELETE FROM tasks WHERE id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_step_t step_rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   if (step_rc != AIMEE_PG_DONE)
      return -1;
   return changes > 0 ? 0 : -1;
}

int db2_task_add_edge(int64_t source, int64_t target, const char *relation)
{
   void *conn = db2_conn();
   if (!conn || !relation)
      return -1;

   char err[TASK_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "INSERT INTO task_edges (source_id, target_id, relation) VALUES (?1, ?2, ?3)", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", source);
   aimee_pg_bind_int64(st, "?2", target);
   aimee_pg_bind_text(st, "?3", relation);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_task_get_edges(int64_t task_id, task_edge_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return 0;

   char err[TASK_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, source_id, target_id, relation FROM task_edges "
                        "WHERE source_id = ?1 OR target_id = ?2 LIMIT ?3",
                        err, sizeof(err));
   if (!st)
      return 0;

   aimee_pg_bind_int64(st, "?1", task_id);
   aimee_pg_bind_int64(st, "?2", task_id);
   aimee_pg_bind_int(st, "?3", max);

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
   {
      out[count].id = aimee_pg_column_int64(st, 0);
      out[count].source_id = aimee_pg_column_int64(st, 1);
      out[count].target_id = aimee_pg_column_int64(st, 2);
      db2_copy_col_text(out[count].relation, sizeof(out[count].relation), st, 3);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_task_get_subtasks(int64_t parent_id, aimee_task_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return 0;

   char err[TASK_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT id, parent_id, title, state, confidence, "
                                          "session_id, created_at, updated_at FROM tasks "
                                          "WHERE parent_id = ?1 ORDER BY created_at ASC",
                                          err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", parent_id);

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
      row_to_task(st, &out[count++]);
   aimee_pg_finalize(st);
   return count;
}

int64_t db2_task_get_active(const char *session_id)
{
   void *conn = db2_conn();
   if (!conn || !session_id)
      return 0;

   char err[TASK_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id FROM tasks WHERE state = 'in_progress' AND session_id = ?1 "
                        "ORDER BY updated_at DESC LIMIT 1",
                        err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", session_id);

   int64_t result = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      result = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return result;
}
