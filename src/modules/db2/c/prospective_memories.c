/* db2/prospective_memories.c: storage primitives for prospective_memories
 * — Postgres via libpq. */

#include "../headers/aimee.h" /* memory_prospective_t and related sizes */
#include "prospective_memories.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define PM_ERRBUF 256

static const char *PM_SELECT_COLS =
    "id, trigger_text, action_text, anchor_entity, anchor_file, recurrence, state,"
    " valid_until, source_session, trigger_count, last_triggered_at, created_at, updated_at";

static void pm_row_from_stmt(aimee_pg_stmt_t *st, memory_prospective_t *out)
{
   memset(out, 0, sizeof(*out));
   out->id = aimee_pg_column_int64(st, 0);
   const char *col;
   col = aimee_pg_column_text(st, 1);
   snprintf(out->trigger_text, sizeof(out->trigger_text), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 2);
   snprintf(out->action_text, sizeof(out->action_text), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 3);
   snprintf(out->anchor_entity, sizeof(out->anchor_entity), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 4);
   snprintf(out->anchor_file, sizeof(out->anchor_file), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 5);
   snprintf(out->recurrence, sizeof(out->recurrence), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 6);
   snprintf(out->state, sizeof(out->state), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 7);
   snprintf(out->valid_until, sizeof(out->valid_until), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 8);
   snprintf(out->source_session, sizeof(out->source_session), "%s", col ? col : "");
   out->trigger_count = aimee_pg_column_int(st, 9);
   col = aimee_pg_column_text(st, 10);
   snprintf(out->last_triggered_at, sizeof(out->last_triggered_at), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 11);
   snprintf(out->created_at, sizeof(out->created_at), "%s", col ? col : "");
   col = aimee_pg_column_text(st, 12);
   snprintf(out->updated_at, sizeof(out->updated_at), "%s", col ? col : "");
}

int64_t db2_prospective_insert(const char *trigger_text, const char *action_text,
                               const char *anchor_entity, const char *anchor_file,
                               const char *recurrence, const char *valid_until,
                               const char *source_session)
{
   if (!trigger_text || !*trigger_text || !action_text || !*action_text || !recurrence)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[PM_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO prospective_memories"
                        " (trigger_text, action_text, anchor_entity, anchor_file, recurrence,"
                        "  state, valid_until, source_session)"
                        " VALUES (?1, ?2, ?3, ?4, ?5, 'armed', ?6, ?7) RETURNING id",
                        err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", trigger_text);
   aimee_pg_bind_text(st, "?2", action_text);
   aimee_pg_bind_text(st, "?3", anchor_entity ? anchor_entity : "");
   aimee_pg_bind_text(st, "?4", anchor_file ? anchor_file : "");
   aimee_pg_bind_text(st, "?5", recurrence);
   aimee_pg_bind_text(st, "?6", valid_until ? valid_until : "");
   aimee_pg_bind_text(st, "?7", source_session ? source_session : "");
   int64_t id = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_prospective_get(int64_t id, memory_prospective_t *out)
{
   if (!out || id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char sql[512];
   snprintf(sql, sizeof(sql), "SELECT %s FROM prospective_memories WHERE id = ?1", PM_SELECT_COLS);
   char err[PM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      pm_row_from_stmt(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_prospective_list(const char *state, memory_prospective_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char sql[1024];
   if (state && state[0])
      snprintf(sql, sizeof(sql),
               "SELECT %s FROM prospective_memories WHERE state = ?1 ORDER BY created_at DESC,"
               " id DESC LIMIT ?2",
               PM_SELECT_COLS);
   else
      snprintf(sql, sizeof(sql),
               "SELECT %s FROM prospective_memories ORDER BY created_at DESC, id DESC LIMIT ?1",
               PM_SELECT_COLS);

   char err[PM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   if (state && state[0])
   {
      aimee_pg_bind_text(st, "?1", state);
      aimee_pg_bind_int(st, "?2", max);
   }
   else
   {
      aimee_pg_bind_int(st, "?1", max);
   }
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      pm_row_from_stmt(st, &out[count++]);
   aimee_pg_finalize(st);
   return count;
}

int db2_prospective_set_state(int64_t id, const char *new_state)
{
   if (id <= 0 || !new_state || !*new_state)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[PM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE prospective_memories SET state = ?1,"
                                          " updated_at = pg_now_text() WHERE id = ?2",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", new_state);
   aimee_pg_bind_int64(st, "?2", id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE && changes > 0) ? 0 : -1;
}

int db2_prospective_sweep_expired(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[PM_ERRBUF] = "";
   /* The schema's datetime() shim returns the same canonical UTC text
    * format used in valid_until, so the < comparison is safe. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE prospective_memories SET state = 'expired', updated_at = pg_now_text()"
       " WHERE state = 'armed' AND valid_until != ''"
       "   AND valid_until < pg_now_text()",
       err, sizeof(err));
   if (!st)
      return 0;
   (void)aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return changes;
}

int db2_prospective_record_trigger(int64_t id, int terminal)
{
   if (id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   const char *sql = terminal
                         ? "UPDATE prospective_memories SET state = 'triggered', trigger_count ="
                           " trigger_count + 1, last_triggered_at = pg_now_text(),"
                           " updated_at = pg_now_text() WHERE id = ?1"
                         : "UPDATE prospective_memories SET trigger_count = trigger_count + 1,"
                           " last_triggered_at = pg_now_text(), updated_at = pg_now_text()"
                           " WHERE id = ?1";
   char err[PM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

static int run_select(const char *sql, const char *bind1, int bind_int_pos, int bind_int_value,
                      memory_prospective_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[PM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   if (bind1)
      aimee_pg_bind_text(st, "?1", bind1);
   if (bind_int_pos > 0)
   {
      char name[8];
      snprintf(name, sizeof(name), "?%d", bind_int_pos);
      aimee_pg_bind_int(st, name, bind_int_value);
   }
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      pm_row_from_stmt(st, &out[count++]);
   aimee_pg_finalize(st);
   return count;
}

int db2_prospective_list_by_entity(const char *entity_lc, memory_prospective_t *out, int max)
{
   if (!entity_lc || !*entity_lc)
      return 0;
   char sql[1024];
   snprintf(sql, sizeof(sql),
            "SELECT %s FROM prospective_memories"
            " WHERE state = 'armed' AND LOWER(anchor_entity) = ?1"
            " ORDER BY created_at DESC, id DESC LIMIT ?2",
            PM_SELECT_COLS);
   return run_select(sql, entity_lc, 2, max, out, max);
}

int db2_prospective_list_by_file(const char *file, memory_prospective_t *out, int max)
{
   if (!file || !*file)
      return 0;
   char sql[1024];
   snprintf(sql, sizeof(sql),
            "SELECT %s FROM prospective_memories"
            " WHERE state = 'armed' AND anchor_file = ?1"
            " ORDER BY created_at DESC, id DESC LIMIT ?2",
            PM_SELECT_COLS);
   return run_select(sql, file, 2, max, out, max);
}

static int pm_is_stop_token(const char *token)
{
   static const char *stops[] = {"the", "a",   "an",  "and", "or",   "of",   "to",
                                 "is",  "are", "was", "has", "have", "that", "this",
                                 "it",  "its", "be",  "for", "on",   "in",   NULL};
   if (!token || !*token)
      return 1;
   for (int i = 0; stops[i]; i++)
   {
      if (strcmp(token, stops[i]) == 0)
         return 1;
   }
   return 0;
}

/* Build the DB2 lexical trigger query from raw turn text. Short/common tokens
 * are ignored so broad chatter does not trigger every armed reminder. */
static int pm_build_trigger_tsquery(const char *turn_text, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';
   if (!turn_text)
      return 0;

   size_t o = 0;
   int emitted = 0;
   size_t tlen = strlen(turn_text);
   size_t i = 0;
   while (i < tlen)
   {
      while (i < tlen && !isalnum((unsigned char)turn_text[i]))
         i++;
      size_t start = i;
      while (i < tlen && (isalnum((unsigned char)turn_text[i]) || turn_text[i] == '_'))
         i++;
      size_t len = i - start;
      if (len < 3)
         continue;

      char token[64];
      size_t copy = len < sizeof(token) - 1 ? len : sizeof(token) - 1;
      for (size_t k = 0; k < copy; k++)
         token[k] = (char)tolower((unsigned char)turn_text[start + k]);
      token[copy] = '\0';
      if (pm_is_stop_token(token))
         continue;

      size_t need = (emitted > 0 ? 3 : 0) + copy + 2 /* :* */;
      if (o + need + 1 >= out_len)
         return 0;
      if (emitted > 0)
      {
         memcpy(out + o, " | ", 3);
         o += 3;
      }
      memcpy(out + o, token, copy);
      o += copy;
      out[o++] = ':';
      out[o++] = '*';
      out[o] = '\0';
      emitted++;
   }
   return emitted > 0;
}

int db2_prospective_list_by_trigger_terms(const char *turn_text, memory_prospective_t *out, int max)
{
   if (!turn_text || !*turn_text)
      return 0;
   char tsq[2048];
   if (!pm_build_trigger_tsquery(turn_text, tsq, sizeof(tsq)))
      return 0;
   char sql[2048];
   /* Use the 'english' config so prefix queries pick up morphology
    * (rotate:* matches "rotation", etc.). The schema-stored tsv column
    * is 'simple', so we recompute inline — fine for the small number of
    * prospective rows. */
   snprintf(sql, sizeof(sql),
            "SELECT %s FROM prospective_memories"
            " WHERE state = 'armed'"
            "   AND to_tsvector('english', trigger_text) @@ to_tsquery('english', ?1)"
            " ORDER BY created_at DESC, id DESC LIMIT ?2",
            PM_SELECT_COLS);
   return run_select(sql, tsq, 2, max, out, max);
}

int db2_prospective_list_armed(memory_prospective_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char sql[1024];
   snprintf(sql, sizeof(sql),
            "SELECT %s FROM prospective_memories WHERE state = 'armed'"
            " ORDER BY created_at DESC, id DESC",
            PM_SELECT_COLS);
   char err[PM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      pm_row_from_stmt(st, &out[count++]);
   aimee_pg_finalize(st);
   return count;
}

void db2_prospective_count_by_state(int *armed_out, int *triggered_out, int *completed_out,
                                    int *expired_out)
{
   if (armed_out)
      *armed_out = 0;
   if (triggered_out)
      *triggered_out = 0;
   if (completed_out)
      *completed_out = 0;
   if (expired_out)
      *expired_out = 0;

   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql = "SELECT state, COUNT(*) FROM prospective_memories GROUP BY state";
   char err[PM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *state = aimee_pg_column_text(st, 0);
      int n = aimee_pg_column_int(st, 1);
      if (!state)
         continue;
      if (armed_out && strcmp(state, MEMORY_PROSPECTIVE_STATE_ARMED) == 0)
         *armed_out = n;
      else if (triggered_out && strcmp(state, MEMORY_PROSPECTIVE_STATE_TRIGGERED) == 0)
         *triggered_out = n;
      else if (completed_out && strcmp(state, MEMORY_PROSPECTIVE_STATE_COMPLETED) == 0)
         *completed_out = n;
      else if (expired_out && strcmp(state, MEMORY_PROSPECTIVE_STATE_EXPIRED) == 0)
         *expired_out = n;
   }
   aimee_pg_finalize(st);
}
