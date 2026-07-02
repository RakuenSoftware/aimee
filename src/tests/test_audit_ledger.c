/* test_audit_ledger.c: unit tests for the S3 governed-action ledger reader.
 * Verifies tool_action extraction, ts ordering, window filtering, rotated-file
 * chronology, and tolerance of legacy/malformed lines. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audit_ledger.h"
#include "cJSON.h"

static char g_home[512];

static void set_home(void)
{
   snprintf(g_home, sizeof g_home, "/tmp/aimee-ledger-test-%d", (int)getpid());
   mkdir(g_home, 0700);
   setenv("AIMEE_HOME", g_home, 1);
   /* Clear any audit.log* left by a prior test so cases stay isolated. */
   char p[600];
   snprintf(p, sizeof p, "%s/audit.log", g_home);
   unlink(p);
   for (int i = 0; i <= 8; i++)
   {
      snprintf(p, sizeof p, "%s/audit.log.%d", g_home, i);
      unlink(p);
   }
}

static void write_file(const char *name, const char *content)
{
   char p[600];
   snprintf(p, sizeof p, "%s/%s", g_home, name);
   FILE *f = fopen(p, "w");
   assert(f);
   fputs(content, f);
   fclose(f);
}

static const char *row_ts(cJSON *arr, int i)
{
   cJSON *o = cJSON_GetArrayItem(arr, i);
   cJSON *ts = cJSON_GetObjectItemCaseSensitive(o, "ts");
   return ts && ts->valuestring ? ts->valuestring : "";
}

static const char *row_tool(cJSON *arr, int i)
{
   cJSON *o = cJSON_GetArrayItem(arr, i);
   cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "tool");
   return t && t->valuestring ? t->valuestring : "";
}

static void test_extract_order_and_skip(void)
{
   set_home();
   /* Out-of-order tool_action rows + a legacy {event,detail} row + a malformed
    * line + a non-tool_action kind. Only tool_action rows survive, ts-ordered. */
   write_file("audit.log",
              "{\"ts\":\"2026-07-02T10:00:03Z\",\"kind\":\"tool_action\",\"tool\":\"C\"}\n"
              "{\"ts\":\"2026-07-02T10:00:01Z\",\"kind\":\"tool_action\",\"tool\":\"A\"}\n"
              "{\"ts\":\"2026-07-02T09:59:59Z\",\"event\":\"read_before_write\",\"detail\":\"x\"}\n"
              "this is not json at all\n"
              "{\"ts\":\"2026-07-02T10:00:02Z\",\"kind\":\"tool_action\",\"tool\":\"B\"}\n"
              "{\"ts\":\"2026-07-02T10:00:05Z\",\"kind\":\"other_kind\",\"tool\":\"Z\"}\n");
   cJSON *arr = audit_ledger_read(NULL, NULL);
   assert(arr && cJSON_IsArray(arr));
   assert(cJSON_GetArraySize(arr) == 3); /* A, B, C only */
   assert(strcmp(row_tool(arr, 0), "A") == 0);
   assert(strcmp(row_tool(arr, 1), "B") == 0);
   assert(strcmp(row_tool(arr, 2), "C") == 0);
   assert(strcmp(row_ts(arr, 0), "2026-07-02T10:00:01Z") == 0);
   cJSON_Delete(arr);
}

static void test_window_filter(void)
{
   set_home();
   write_file("audit.log",
              "{\"ts\":\"2026-07-02T10:00:01Z\",\"kind\":\"tool_action\",\"tool\":\"A\"}\n"
              "{\"ts\":\"2026-07-02T10:00:05Z\",\"kind\":\"tool_action\",\"tool\":\"B\"}\n"
              "{\"ts\":\"2026-07-02T10:00:09Z\",\"kind\":\"tool_action\",\"tool\":\"C\"}\n");
   cJSON *arr = audit_ledger_read("2026-07-02T10:00:03Z", "2026-07-02T10:00:07Z");
   assert(cJSON_GetArraySize(arr) == 1); /* only B is in [03,07] */
   assert(strcmp(row_tool(arr, 0), "B") == 0);
   cJSON_Delete(arr);
}

static void test_rotated_chronology(void)
{
   set_home();
   /* audit.log.0 is the most recent ROTATED file (older than current); audit.log
    * is newest. Same-second rows must order older-file-first. */
   write_file("audit.log.1",
              "{\"ts\":\"2026-07-02T10:00:00Z\",\"kind\":\"tool_action\",\"tool\":\"oldest\"}\n");
   write_file("audit.log.0",
              "{\"ts\":\"2026-07-02T10:00:00Z\",\"kind\":\"tool_action\",\"tool\":\"mid\"}\n");
   write_file("audit.log",
              "{\"ts\":\"2026-07-02T10:00:00Z\",\"kind\":\"tool_action\",\"tool\":\"newest\"}\n");
   cJSON *arr = audit_ledger_read(NULL, NULL);
   assert(cJSON_GetArraySize(arr) == 3);
   assert(strcmp(row_tool(arr, 0), "oldest") == 0);
   assert(strcmp(row_tool(arr, 1), "mid") == 0);
   assert(strcmp(row_tool(arr, 2), "newest") == 0);
   cJSON_Delete(arr);
}

static void test_missing_log_is_empty_not_null(void)
{
   snprintf(g_home, sizeof g_home, "/tmp/aimee-ledger-empty-%d", (int)getpid());
   mkdir(g_home, 0700);
   setenv("AIMEE_HOME", g_home, 1);
   cJSON *arr = audit_ledger_read(NULL, NULL);
   assert(arr && cJSON_IsArray(arr));
   assert(cJSON_GetArraySize(arr) == 0);
   cJSON_Delete(arr);
}

/* A line with a valid JSON prefix but trailing garbage must be rejected by the
 * strict parse, not accepted as a row. */
static void test_strict_parse_rejects_trailing_garbage(void)
{
   set_home();
   write_file(
       "audit.log",
       "{\"ts\":\"2026-07-02T10:00:01Z\",\"kind\":\"tool_action\",\"tool\":\"A\"} trailing junk\n"
       "{\"ts\":\"2026-07-02T10:00:02Z\",\"kind\":\"tool_action\",\"tool\":\"B\"}\n");
   cJSON *arr = audit_ledger_read(NULL, NULL);
   assert(cJSON_GetArraySize(arr) == 1); /* only the clean B row */
   assert(strcmp(row_tool(arr, 0), "B") == 0);
   cJSON_Delete(arr);
}

int main(void)
{
   test_extract_order_and_skip();
   test_window_filter();
   test_rotated_chronology();
   test_missing_log_is_empty_not_null();
   test_strict_parse_rejects_trailing_garbage();
   printf("test_audit_ledger: all passed\n");
   return 0;
}
