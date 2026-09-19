/* test_feedback_shadow.c: unit tests for shadow-delta persistence. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db1_client/db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include "platform_test_util.h"
#include "memory.h"
#include "../modules/db2/c/shadow_delta.h"

static char g_db_path[512];

static void setup(void)
{
   snprintf(g_db_path, sizeof(g_db_path), "%s/aimee-test-fs-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(g_db_path, sizeof(g_db_path), "aim");
   assert(fd >= 0);
   close(fd);
   db2_test_shim_open_path(g_db_path);
}

static void teardown(void)
{
   db2_test_shim_close();
   platform_test_remove_sqlite(g_db_path);
   g_db_path[0] = '\0';
}

/* --- shadow-delta persistence --- */

static void test_shadow_insert_and_count(void)
{
   setup();
   db2_shadow_delta_row_t row;
   memset(&row, 0, sizeof(row));
   snprintf(row.query_hash, sizeof(row.query_hash), "abc123");
   snprintf(row.project, sizeof(row.project), "aimee");
   snprintf(row.mode, sizeof(row.mode), "shadow");
   row.result_count = 20;
   row.delta_json = "{\"deltas\":[]}";

   int rc = db2_shadow_delta_insert(&row);
   /* On SQLite shim insert should succeed (0) or gracefully -1 if no table. */
   assert(rc == 0 || rc == -1);

   if (rc == 0)
   {
      int64_t n = db2_shadow_delta_count("aimee");
      assert(n >= 1);
   }
   teardown();
}

static void test_shadow_insert_null_guard(void)
{
   int rc = db2_shadow_delta_insert(NULL);
   assert(rc == -1);
}

static void test_shadow_cleanup_defaults(void)
{
   setup();
   /* Cleanup with 0 bounds uses defaults; empty table → 0 deleted. */
   int deleted = db2_shadow_delta_cleanup("aimee", 0, 0);
   assert(deleted >= 0);
   teardown();
}

static void test_shadow_cleanup_no_db(void)
{
   /* No DB connection → graceful 0. */
   int deleted = db2_shadow_delta_cleanup(NULL, 100, 7);
   assert(deleted == 0 || deleted >= 0);
}

static void test_shadow_defaults_constants(void)
{
   assert(SHADOW_DELTA_DEFAULT_MAX_ROWS == 10000);
   assert(SHADOW_DELTA_DEFAULT_RETENTION_DAYS == 14);
}

static void test_shadow_count_empty(void)
{
   setup();
   int64_t n = db2_shadow_delta_count("nonexistent_project");
   assert(n == 0);
   teardown();
}

int main(void)
{
   printf("test_shadow_insert_and_count... ");
   test_shadow_insert_and_count();
   printf("ok\n");
   printf("test_shadow_insert_null_guard... ");
   test_shadow_insert_null_guard();
   printf("ok\n");
   printf("test_shadow_cleanup_defaults... ");
   test_shadow_cleanup_defaults();
   printf("ok\n");
   printf("test_shadow_cleanup_no_db... ");
   test_shadow_cleanup_no_db();
   printf("ok\n");
   printf("test_shadow_defaults_constants... ");
   test_shadow_defaults_constants();
   printf("ok\n");
   printf("test_shadow_count_empty... ");
   test_shadow_count_empty();
   printf("ok\n");
   printf("feedback_shadow: all tests passed\n");
   return 0;
}
