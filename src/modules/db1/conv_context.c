/* db1/conv_context.c: DB1 storage for virtual context tool-chain events. */
#include "db1_internal.h"
#include "conv_context.h"
#include <stdio.h>
#include <string.h>

int64_t db1_conv_record_event(const char *session_id, const char *tool_name, const char *tool_input,
                              const char *tool_result, int result_bytes)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id || !tool_name)
      return -1;

   const char *sql = "INSERT INTO conv_tool_events"
                     " (session_id, tool_name, tool_input, tool_result, result_bytes)"
                     " VALUES (?,?,?,?,?)";
   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, tool_name, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, tool_input ? tool_input : "{}", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, tool_result ? tool_result : "", -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 5, result_bytes);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   return (int64_t)sqlite3_last_insert_rowid(db);
}

int db1_conv_set_chain_id(int64_t event_id_first, int64_t event_id_last, int64_t chain_id)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   const char *sql = "UPDATE conv_tool_events SET chain_id=? WHERE id>=? AND id<=? AND chain_id=0";
   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(stmt, 1, chain_id);
   sqlite3_bind_int64(stmt, 2, event_id_first);
   sqlite3_bind_int64(stmt, 3, event_id_last);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return 0;
}

int64_t db1_conv_insert_chain(const char *session_id, int64_t event_id_first, int64_t event_id_last,
                              const char *tools, const char *stub, int raw_bytes, int stub_bytes)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id)
      return -1;

   const char *sql =
       "INSERT INTO conv_tool_chains"
       " (session_id, event_id_first, event_id_last, tools, stub, raw_bytes, stub_bytes)"
       " VALUES (?,?,?,?,?,?,?)";
   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 2, event_id_first);
   sqlite3_bind_int64(stmt, 3, event_id_last);
   sqlite3_bind_text(stmt, 4, tools ? tools : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, stub ? stub : "", -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 6, raw_bytes);
   sqlite3_bind_int(stmt, 7, stub_bytes);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (int64_t)sqlite3_last_insert_rowid(db);
}

int db1_conv_pending_events(const char *session_id, conv_tool_event_t *out, int max)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id || !out || max <= 0)
      return 0;

   const char *sql =
       "SELECT id, session_id, tool_name, tool_input, tool_result, result_bytes, chain_id,"
       " created_at FROM conv_tool_events"
       " WHERE session_id=? AND chain_id=0 ORDER BY id LIMIT ?";
   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      conv_tool_event_t *e = &out[count];
      e->id = sqlite3_column_int64(stmt, 0);
      const char *s;
      s = (const char *)sqlite3_column_text(stmt, 1);
      if (s)
         snprintf(e->session_id, sizeof(e->session_id), "%s", s);
      s = (const char *)sqlite3_column_text(stmt, 2);
      if (s)
         snprintf(e->tool_name, sizeof(e->tool_name), "%s", s);
      s = (const char *)sqlite3_column_text(stmt, 3);
      if (s)
         snprintf(e->tool_input, sizeof(e->tool_input), "%s", s);
      s = (const char *)sqlite3_column_text(stmt, 4);
      if (s)
         snprintf(e->tool_result, sizeof(e->tool_result), "%s", s);
      e->result_bytes = sqlite3_column_int(stmt, 5);
      e->chain_id = sqlite3_column_int64(stmt, 6);
      s = (const char *)sqlite3_column_text(stmt, 7);
      if (s)
         snprintf(e->created_at, sizeof(e->created_at), "%s", s);
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}

int db1_conv_list_chains(const char *session_id, conv_tool_chain_t *out, int max)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id || !out || max <= 0)
      return 0;

   const char *sql = "SELECT id, session_id, event_id_first, event_id_last, tools, stub,"
                     " raw_bytes, stub_bytes, state, created_at"
                     " FROM conv_tool_chains WHERE session_id=? ORDER BY id DESC LIMIT ?";
   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      conv_tool_chain_t *c = &out[count];
      c->id = sqlite3_column_int64(stmt, 0);
      const char *s;
      s = (const char *)sqlite3_column_text(stmt, 1);
      if (s)
         snprintf(c->session_id, sizeof(c->session_id), "%s", s);
      c->event_id_first = sqlite3_column_int64(stmt, 2);
      c->event_id_last = sqlite3_column_int64(stmt, 3);
      s = (const char *)sqlite3_column_text(stmt, 4);
      if (s)
         snprintf(c->tools, sizeof(c->tools), "%s", s);
      s = (const char *)sqlite3_column_text(stmt, 5);
      if (s)
         snprintf(c->stub, sizeof(c->stub), "%s", s);
      c->raw_bytes = sqlite3_column_int(stmt, 6);
      c->stub_bytes = sqlite3_column_int(stmt, 7);
      s = (const char *)sqlite3_column_text(stmt, 8);
      if (s)
         snprintf(c->state, sizeof(c->state), "%s", s);
      s = (const char *)sqlite3_column_text(stmt, 9);
      if (s)
         snprintf(c->created_at, sizeof(c->created_at), "%s", s);
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}

int db1_conv_chain_events(int64_t chain_id, conv_tool_event_t *out, int max)
{
   sqlite3 *db = db1_conn();
   if (!db || !out || max <= 0)
      return 0;

   const char *sql =
       "SELECT id, session_id, tool_name, tool_input, tool_result, result_bytes, chain_id,"
       " created_at FROM conv_tool_events WHERE chain_id=? ORDER BY id LIMIT ?";
   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int64(stmt, 1, chain_id);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      conv_tool_event_t *e = &out[count];
      e->id = sqlite3_column_int64(stmt, 0);
      const char *s;
      s = (const char *)sqlite3_column_text(stmt, 1);
      if (s)
         snprintf(e->session_id, sizeof(e->session_id), "%s", s);
      s = (const char *)sqlite3_column_text(stmt, 2);
      if (s)
         snprintf(e->tool_name, sizeof(e->tool_name), "%s", s);
      s = (const char *)sqlite3_column_text(stmt, 3);
      if (s)
         snprintf(e->tool_input, sizeof(e->tool_input), "%s", s);
      s = (const char *)sqlite3_column_text(stmt, 4);
      if (s)
         snprintf(e->tool_result, sizeof(e->tool_result), "%s", s);
      e->result_bytes = sqlite3_column_int(stmt, 5);
      e->chain_id = sqlite3_column_int64(stmt, 6);
      s = (const char *)sqlite3_column_text(stmt, 7);
      if (s)
         snprintf(e->created_at, sizeof(e->created_at), "%s", s);
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}

int db1_conv_search_chains(const char *session_id, const char *query, conv_tool_chain_t *out,
                           int max)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id || !query || !out || max <= 0)
      return 0;

   /* Simple substring search over stub text */
   const char *sql =
       "SELECT id, session_id, event_id_first, event_id_last, tools, stub,"
       " raw_bytes, stub_bytes, state, created_at"
       " FROM conv_tool_chains WHERE session_id=? AND instr(lower(stub), lower(?)) > 0"
       " ORDER BY id DESC LIMIT ?";
   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, query, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 3, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      conv_tool_chain_t *c = &out[count];
      c->id = sqlite3_column_int64(stmt, 0);
      const char *s;
      s = (const char *)sqlite3_column_text(stmt, 1);
      if (s)
         snprintf(c->session_id, sizeof(c->session_id), "%s", s);
      c->event_id_first = sqlite3_column_int64(stmt, 2);
      c->event_id_last = sqlite3_column_int64(stmt, 3);
      s = (const char *)sqlite3_column_text(stmt, 4);
      if (s)
         snprintf(c->tools, sizeof(c->tools), "%s", s);
      s = (const char *)sqlite3_column_text(stmt, 5);
      if (s)
         snprintf(c->stub, sizeof(c->stub), "%s", s);
      c->raw_bytes = sqlite3_column_int(stmt, 6);
      c->stub_bytes = sqlite3_column_int(stmt, 7);
      s = (const char *)sqlite3_column_text(stmt, 8);
      if (s)
         snprintf(c->state, sizeof(c->state), "%s", s);
      s = (const char *)sqlite3_column_text(stmt, 9);
      if (s)
         snprintf(c->created_at, sizeof(c->created_at), "%s", s);
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}

int db1_conv_state_get(const char *session_id, int64_t *last_event_id_out, int *chain_count_out,
                       int *event_count_out)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id)
      return -1;

   const char *sql =
       "SELECT last_event_id, chain_count, event_count FROM conv_context_state WHERE session_id=?";
   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      if (last_event_id_out)
         *last_event_id_out = sqlite3_column_int64(stmt, 0);
      if (chain_count_out)
         *chain_count_out = sqlite3_column_int(stmt, 1);
      if (event_count_out)
         *event_count_out = sqlite3_column_int(stmt, 2);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_conv_state_update(const char *session_id, int64_t last_event_id, int chain_count,
                          int event_count)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id)
      return -1;

   const char *sql = "INSERT OR REPLACE INTO conv_context_state"
                     " (session_id, last_event_id, chain_count, event_count, updated_at)"
                     " VALUES (?,?,?,?,datetime('now'))";
   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 2, last_event_id);
   sqlite3_bind_int(stmt, 3, chain_count);
   sqlite3_bind_int(stmt, 4, event_count);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return 0;
}
