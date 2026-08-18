/* test_interaction_events.c: DB1 interaction event stream tests */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>
#include "db1.h"
#include "interaction_events.h"

static void tmp_db_path(char *buf, size_t buflen, const char *tag)
{
   snprintf(buf, buflen, "/tmp/aimee-ie-%s-%d.sqlite", tag, (int)getpid());
   unlink(buf);
}

static int scalar_int(const char *db_path, const char *sql)
{
   sqlite3 *db = NULL;
   sqlite3_stmt *st = NULL;
   int value = -1;
   assert(sqlite3_open(db_path, &db) == SQLITE_OK);
   assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
   if (sqlite3_step(st) == SQLITE_ROW)
      value = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   sqlite3_close(db);
   return value;
}

static void scalar_text(const char *db_path, const char *sql, char *out, size_t outlen)
{
   sqlite3 *db = NULL;
   sqlite3_stmt *st = NULL;
   out[0] = '\0';
   assert(sqlite3_open(db_path, &db) == SQLITE_OK);
   assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
   if (sqlite3_step(st) == SQLITE_ROW)
   {
      const unsigned char *txt = sqlite3_column_text(st, 0);
      snprintf(out, outlen, "%s", txt ? (const char *)txt : "");
   }
   sqlite3_finalize(st);
   sqlite3_close(db);
}

static void exec_sql(const char *db_path, const char *sql)
{
   sqlite3 *db = NULL;
   char *err = NULL;
   assert(sqlite3_open(db_path, &db) == SQLITE_OK);
   assert(sqlite3_exec(db, sql, NULL, NULL, &err) == SQLITE_OK);
   sqlite3_free(err);
   sqlite3_close(db);
}

static void test_event_type_names(void)
{
   assert(strcmp(ie_event_type_name(IE_DELEGATE_EXIT), "delegate_exit") == 0);
   assert(strcmp(ie_event_type_name(IE_FAILOVER_EVENT), "failover_event") == 0);
   assert(strcmp(ie_event_type_name((ie_event_type_t)999), "unknown") == 0);
}

static void test_record_without_db_fails(void)
{
   db1_shutdown();
   assert(db1_interaction_event_record("s", ie_event_type_name(IE_DELEGATE_EXIT), "system", "{}",
                                       "ok") == -1);
}

static void test_record_delegate_exit_defaults_and_truncates(void)
{
   char path[256];
   tmp_db_path(path, sizeof(path), "record");
   assert(db1_init(path) == 0);

   char payload[8192];
   memset(payload, 'x', sizeof(payload) - 1);
   payload[0] = '{';
   payload[1] = '"';
   payload[2] = 'x';
   payload[3] = '"';
   payload[4] = ':';
   payload[5] = '"';
   payload[sizeof(payload) - 2] = '"';
   payload[sizeof(payload) - 1] = '\0';

   assert(db1_interaction_event_record("sess-1", ie_event_type_name(IE_DELEGATE_EXIT), NULL,
                                       payload, NULL) == 0);
   db1_shutdown();

   assert(scalar_int(path, "SELECT COUNT(*) FROM interaction_events") == 1);
   char value[128];
   scalar_text(path, "SELECT event_type FROM interaction_events", value, sizeof(value));
   assert(strcmp(value, "delegate_exit") == 0);
   scalar_text(path, "SELECT actor FROM interaction_events", value, sizeof(value));
   assert(strcmp(value, "system") == 0);
   scalar_text(path, "SELECT outcome FROM interaction_events", value, sizeof(value));
   assert(strcmp(value, "ok") == 0);
   int len = scalar_int(path, "SELECT length(payload) FROM interaction_events");
   assert(len == 4095);
   unlink(path);
}

static void test_record_failover_event_payload(void)
{
   char path[256];
   tmp_db_path(path, sizeof(path), "failover");
   assert(db1_init(path) == 0);

   assert(db1_interaction_event_record(
              "sess-failover", ie_event_type_name(IE_FAILOVER_EVENT), NULL,
              "{\"provider\":\"openrouter\",\"model\":\"m\",\"http_status\":404,"
              "\"reason\":\"provider_policy\",\"action\":\"fallback_provider\","
              "\"attempt\":1,\"recovered\":false}",
              "error") == 0);
   db1_shutdown();

   assert(scalar_int(path, "SELECT COUNT(*) FROM interaction_events WHERE "
                           "event_type='failover_event' AND outcome='error'") == 1);
   char value[128];
   scalar_text(path, "SELECT actor FROM interaction_events WHERE event_type='failover_event'",
               value, sizeof(value));
   assert(strcmp(value, "system") == 0);
   scalar_text(path, "SELECT payload FROM interaction_events WHERE event_type='failover_event'",
               value, sizeof(value));
   assert(strstr(value, "provider_policy") != NULL);
   unlink(path);
}

static void test_eviction_prefers_reflected_promoted(void)
{
   char path[256];
   tmp_db_path(path, sizeof(path), "evict");
   assert(db1_init(path) == 0);
   assert(db1_interaction_event_record("sess", ie_event_type_name(IE_USER_TURN), NULL, "{\"n\":1}",
                                       "ok") == 0);
   assert(db1_interaction_event_record("sess", ie_event_type_name(IE_AGENT_TURN), NULL, "{\"n\":2}",
                                       "ok") == 0);
   assert(db1_interaction_event_record("sess", ie_event_type_name(IE_DELEGATE_EXIT), NULL,
                                       "{\"n\":3}", "error") == 0);
   exec_sql(path, "UPDATE interaction_events SET reflected_at='r', promoted_at='p' WHERE "
                  "event_type='user_turn'");
   assert(db1_interaction_event_evict_if_needed(2) == 0);
   db1_shutdown();

   assert(scalar_int(path, "SELECT COUNT(*) FROM interaction_events") == 2);
   assert(scalar_int(path,
                     "SELECT COUNT(*) FROM interaction_events WHERE event_type='user_turn'") == 0);
   assert(scalar_int(path,
                     "SELECT COUNT(*) FROM interaction_events WHERE event_type='agent_turn'") == 1);
   assert(scalar_int(path,
                     "SELECT COUNT(*) FROM interaction_events WHERE event_type='delegate_exit'") ==
          1);
   unlink(path);
}

static void test_reflection_and_promotion_feeds(void)
{
   char path[256];
   tmp_db_path(path, sizeof(path), "feed");
   assert(db1_init(path) == 0);
   assert(db1_interaction_event_record("sess-feed", ie_event_type_name(IE_TOOL_CALL), "agent",
                                       "{\"tool\":\"bash\"}", "ok") == 0);
   assert(db1_interaction_event_record("sess-feed", ie_event_type_name(IE_TOOL_OUTCOME), "system",
                                       "{\"ok\":true}", "ok") == 0);
   assert(db1_interaction_event_record("other", ie_event_type_name(IE_USER_TURN), "user",
                                       "{\"tokens\":3}", "ok") == 0);

   ie_event_row_t rows[4];
   int n = db1_interaction_event_list_unreflected("sess-feed", rows, 4);
   assert(n == 2);
   assert(strcmp(rows[0].session_id, "sess-feed") == 0);
   assert(strcmp(rows[0].event_type, "tool_call") == 0);
   assert(strstr(rows[0].payload, "bash") != NULL);

   int ids[2] = {rows[0].id, rows[1].id};
   assert(db1_interaction_event_mark_reflected(ids, 2) == 0);
   assert(db1_interaction_event_list_unreflected("sess-feed", rows, 4) == 0);

   n = db1_interaction_event_list_promotion_feed(rows, 4);
   assert(n == 2);
   assert(strcmp(rows[0].event_type, "tool_call") == 0);
   assert(db1_interaction_event_mark_promoted(ids, 2) == 0);
   assert(db1_interaction_event_list_promotion_feed(rows, 4) == 0);

   db1_shutdown();
   unlink(path);
}

int main(void)
{
   test_event_type_names();
   test_record_without_db_fails();
   test_record_delegate_exit_defaults_and_truncates();
   test_record_failover_event_payload();
   test_eviction_prefers_reflected_promoted();
   test_reflection_and_promotion_feeds();
   printf("interaction_events: all tests passed\n");
   return 0;
}
