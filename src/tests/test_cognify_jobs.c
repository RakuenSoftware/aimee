#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include "db1.h"
#include "platform_test_util.h"

/* Private to src/db1/, but this test verifies db1_init's migration path
 * for the old placeholder table plus the typed queue API. */
extern sqlite3 *db1_conn(void);

static int table_exists(sqlite3 *db, const char *name)
{
   sqlite3_stmt *stmt = NULL;
   int found = 0;

   assert(sqlite3_prepare_v2(db,
                             "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
                             -1, &stmt, NULL) == SQLITE_OK);
   assert(sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT) == SQLITE_OK);
   found = (sqlite3_step(stmt) == SQLITE_ROW);
   sqlite3_finalize(stmt);
   return found;
}

static int column_exists(sqlite3 *db, const char *table, const char *column)
{
   sqlite3_stmt *stmt = NULL;
   char sql[128];
   int found = 0;

   snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
   assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *name = sqlite3_column_text(stmt, 1);
      if (name && strcmp((const char *)name, column) == 0)
      {
         found = 1;
         break;
      }
   }
   sqlite3_finalize(stmt);
   return found;
}

int main(void)
{
   char path[256];
   snprintf(path, sizeof path, "%s/test_cognify_jobs_XXXXXX.db", platform_tmpdir());
   int fd = mkstemps(path, 3);
   assert(fd >= 0);
   close(fd);
   platform_test_remove_sqlite(path);

   printf("cognify_jobs: ");
   assert(db1_init(path) == 0);

   sqlite3 *db = db1_conn();
   assert(db != NULL);
   assert(table_exists(db, "memory_cognify_jobs"));
   assert(column_exists(db, "memory_cognify_jobs", "kind"));
   assert(column_exists(db, "memory_cognify_jobs", "max_attempts"));
   assert(column_exists(db, "memory_cognify_jobs", "claimed_by"));
   assert(column_exists(db, "memory_cognify_jobs", "claimed_at"));

   assert(db1_cognify_job_enqueue(42) == 0);
   assert(db1_cognify_job_enqueue(42) == 0); /* duplicate ignored */

   db1_cognify_job_stats_t stats;
   assert(db1_cognify_job_status(&stats) == 0);
   assert(stats.pending == 1);
   assert(stats.running == 0);
   assert(stats.done == 0);
   assert(stats.failed == 0);
   assert(stats.total == 1);

   db1_cognify_job_t job;
   memset(&job, 0, sizeof(job));
   assert(db1_cognify_job_claim_next(&job) == 1);
   assert(job.id > 0);
   assert(job.memory_id == 42);
   assert(job.attempts == 1);
   assert(job.max_attempts == 3);
   assert(strcmp(job.kind, "cognify_unit") == 0);
   assert(strcmp(job.status, "running") == 0);

   assert(db1_cognify_job_status(&stats) == 0);
   assert(stats.pending == 0);
   assert(stats.running == 1);
   assert(stats.total == 1);

   assert(db1_cognify_job_mark(job.id, "pending", "retry later") == 0);
   assert(db1_cognify_job_status(&stats) == 0);
   assert(stats.pending == 1);
   assert(stats.running == 0);

   memset(&job, 0, sizeof(job));
   assert(db1_cognify_job_claim_next(&job) == 1);
   assert(job.attempts == 2);
   assert(db1_cognify_job_mark(job.id, "failed", "boom") == 0);

   assert(db1_cognify_job_status(&stats) == 0);
   assert(stats.pending == 0);
   assert(stats.running == 0);
   assert(stats.failed == 1);
   assert(stats.total == 1);

   assert(db1_cognify_job_claim_next(&job) == 0);

   db1_shutdown();
   platform_test_remove_sqlite(path);
   printf("PASS\n");
   return 0;
}
