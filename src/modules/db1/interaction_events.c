/* interaction_events.c: DB1 canonical interaction event stream. */
#include "db1_internal.h"
#include "interaction_events.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define IE_TABLE_CAP       50000
#define IE_PAYLOAD_MAX_LEN 4096

const char *ie_event_type_name(ie_event_type_t type)
{
   switch (type)
   {
   case IE_USER_TURN:
      return "user_turn";
   case IE_AGENT_TURN:
      return "agent_turn";
   case IE_TOOL_CALL:
      return "tool_call";
   case IE_TOOL_OUTCOME:
      return "tool_outcome";
   case IE_DELEGATE_EXIT:
      return "delegate_exit";
   case IE_GUARDRAIL_DECISION:
      return "guardrail_decision";
   case IE_SKILL_ACTIVATION:
      return "skill_activation";
   case IE_USER_CORRECTION:
      return "user_correction";
   case IE_FAILOVER_EVENT:
      return "failover_event";
   case IE_MCP_PACKAGE_CHECK:
      return "mcp_package_check";
   default:
      return "unknown";
   }
}

static const char *ie_default_actor(ie_event_type_t type)
{
   switch (type)
   {
   case IE_USER_TURN:
   case IE_USER_CORRECTION:
      return "user";
   case IE_DELEGATE_EXIT:
   case IE_TOOL_OUTCOME:
   case IE_GUARDRAIL_DECISION:
   case IE_FAILOVER_EVENT:
   case IE_MCP_PACKAGE_CHECK:
      return "system";
   default:
      return "agent";
   }
}

static void ie_copy_payload(const char *payload_json, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   if (!payload_json || !payload_json[0])
      payload_json = "{}";
   snprintf(out, out_len, "%.*s", IE_PAYLOAD_MAX_LEN - 1, payload_json);
}

static void ie_copy_column(sqlite3_stmt *st, int col, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   const unsigned char *txt = sqlite3_column_text(st, col);
   snprintf(out, out_len, "%s", txt ? (const char *)txt : "");
}

static void ie_row_from_stmt(sqlite3_stmt *st, ie_event_row_t *row)
{
   memset(row, 0, sizeof(*row));
   row->id = sqlite3_column_int(st, 0);
   ie_copy_column(st, 1, row->session_id, sizeof(row->session_id));
   ie_copy_column(st, 2, row->event_type, sizeof(row->event_type));
   ie_copy_column(st, 3, row->actor, sizeof(row->actor));
   ie_copy_column(st, 4, row->payload, sizeof(row->payload));
   ie_copy_column(st, 5, row->outcome, sizeof(row->outcome));
   ie_copy_column(st, 6, row->created_at, sizeof(row->created_at));
}

int interaction_events_evict_if_needed(int cap)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   if (cap <= 0)
      cap = IE_TABLE_CAP;

   sqlite3_stmt *st = NULL;
   int count = 0;
   if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM interaction_events", -1, &st, NULL) !=
       SQLITE_OK)
      return -1;
   if (sqlite3_step(st) == SQLITE_ROW)
      count = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);

   int excess = count - cap;
   if (excess <= 0)
      return 0;

   const char *sql = "DELETE FROM interaction_events WHERE id IN ("
                     "  SELECT id FROM interaction_events"
                     "  ORDER BY CASE WHEN reflected_at IS NOT NULL AND promoted_at IS NOT NULL "
                     "THEN 0 ELSE 1 END,"
                     "           created_at ASC, id ASC"
                     "  LIMIT ?1"
                     ")";
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(st, 1, excess);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int ie_record(const char *session_id, ie_event_type_t type, const char *actor,
              const char *payload_json, const char *outcome)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   if (!actor || !actor[0])
      actor = ie_default_actor(type);
   if (!outcome || !outcome[0])
      outcome = "ok";

   char payload[IE_PAYLOAD_MAX_LEN];
   ie_copy_payload(payload_json, payload, sizeof(payload));

   static const char *sql =
       "INSERT INTO interaction_events(session_id, event_type, actor, payload, outcome)"
       " VALUES(?, ?, ?, ?, ?)";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, ie_event_type_name(type), -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, actor, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, payload, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 5, outcome, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
      return -1;
   return interaction_events_evict_if_needed(IE_TABLE_CAP);
}

int ie_list_unreflected_for_session(const char *session_id, ie_event_row_t *out, int max)
{
   sqlite3 *db = db1_conn();
   if (!db || !out || max <= 0)
      return -1;

   static const char *sql = "SELECT id, session_id, event_type, actor, payload, outcome, created_at"
                            " FROM interaction_events"
                            " WHERE session_id = ?1 AND reflected_at IS NULL"
                            " ORDER BY created_at ASC, id ASC"
                            " LIMIT ?2";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 2, max);

   int n = 0;
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW)
      ie_row_from_stmt(st, &out[n++]);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? n : -1;
}

int ie_list_for_session(const char *session_id, ie_event_row_t *out, int max)
{
   sqlite3 *db = db1_conn();
   if (!db || !out || max <= 0 || !session_id || !session_id[0])
      return -1;

   static const char *sql = "SELECT id, session_id, event_type, actor, payload, outcome, created_at"
                            " FROM interaction_events"
                            " WHERE session_id = ?1"
                            " ORDER BY created_at ASC, id ASC"
                            " LIMIT ?2";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 2, max);

   int n = 0;
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW)
      ie_row_from_stmt(st, &out[n++]);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? n : -1;
}

static int ie_mark_timestamp(const char *column, const int *ids, int count)
{
   sqlite3 *db = db1_conn();
   if (!db || !column || !ids || count <= 0)
      return -1;

   char sql[128];
   snprintf(sql, sizeof(sql), "UPDATE interaction_events SET %s = datetime('now') WHERE id = ?1",
            column);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;

   int rc = 0;
   for (int i = 0; i < count; i++)
   {
      sqlite3_bind_int(st, 1, ids[i]);
      if (sqlite3_step(st) != SQLITE_DONE)
      {
         rc = -1;
         break;
      }
      sqlite3_reset(st);
      sqlite3_clear_bindings(st);
   }
   sqlite3_finalize(st);
   return rc;
}

int ie_mark_reflected(const int *ids, int count)
{
   return ie_mark_timestamp("reflected_at", ids, count);
}

int ie_list_promotion_feed(ie_event_row_t *out, int max)
{
   sqlite3 *db = db1_conn();
   if (!db || !out || max <= 0)
      return -1;

   static const char *sql = "SELECT id, session_id, event_type, actor, payload, outcome, created_at"
                            " FROM interaction_events"
                            " WHERE promoted_at IS NULL AND reflected_at IS NOT NULL"
                            " ORDER BY created_at ASC, id ASC"
                            " LIMIT ?1";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(st, 1, max);

   int n = 0;
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW)
      ie_row_from_stmt(st, &out[n++]);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? n : -1;
}

int ie_mark_promoted(const int *ids, int count)
{
   return ie_mark_timestamp("promoted_at", ids, count);
}
