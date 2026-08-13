/* test_payload_rewrite_state.c: unit tests for db1/payload_rewrite_state.c.
 *
 * Tests state get/set, deferred rewrite recording, forced rewrite recording,
 * and counter accumulation across multiple events. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db1.h"
#include "payload_rewrite_state.h"
#include "platform_test_util.h"

static void test_state_not_found(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   payload_rewrite_state_t s;
   int rc = db1_payload_rewrite_state_get("sess-x", &s);
   assert(rc != 0); /* not found */

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_state_set_get(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   payload_rewrite_state_t s;
   memset(&s, 0, sizeof(s));
   snprintf(s.session_id, sizeof(s.session_id), "sess-1");
   s.payload_epoch = 3;
   s.compaction_epoch = 5;
   snprintf(s.last_prefix_hash, sizeof(s.last_prefix_hash), "abc123");
   s.last_payload_tokens = 1200;
   s.deferred_rewrite_count = 2;
   s.bytes_saved_pending = 4096;
   snprintf(s.rewrite_reason, sizeof(s.rewrite_reason), "below_threshold");

   int rc = db1_payload_rewrite_state_set(&s);
   assert(rc == 0);

   payload_rewrite_state_t out;
   rc = db1_payload_rewrite_state_get("sess-1", &out);
   assert(rc == 0);
   assert(out.payload_epoch == 3);
   assert(out.compaction_epoch == 5);
   assert(strcmp(out.last_prefix_hash, "abc123") == 0);
   assert(out.last_payload_tokens == 1200);
   assert(out.deferred_rewrite_count == 2);
   assert(out.bytes_saved_pending == 4096);
   assert(strcmp(out.rewrite_reason, "below_threshold") == 0);

   /* Upsert */
   s.payload_epoch = 4;
   rc = db1_payload_rewrite_state_set(&s);
   assert(rc == 0);
   rc = db1_payload_rewrite_state_get("sess-1", &out);
   assert(rc == 0);
   assert(out.payload_epoch == 4);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_record_deferred(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   /* First deferred event creates the row */
   int rc = db1_payload_rewrite_record("sess-2", 1, 512, 800, "below_threshold", "hash1");
   assert(rc == 0);

   payload_rewrite_state_t out;
   rc = db1_payload_rewrite_state_get("sess-2", &out);
   assert(rc == 0);
   assert(out.deferred_rewrite_count == 1);
   assert(out.bytes_saved_pending == 512);
   assert(out.last_payload_tokens == 800);
   assert(strcmp(out.rewrite_reason, "below_threshold") == 0);
   assert(strcmp(out.last_prefix_hash, "hash1") == 0);

   /* Second deferred event accumulates */
   rc = db1_payload_rewrite_record("sess-2", 1, 256, 900, "below_threshold", "hash1");
   assert(rc == 0);
   rc = db1_payload_rewrite_state_get("sess-2", &out);
   assert(rc == 0);
   assert(out.deferred_rewrite_count == 2);
   assert(out.bytes_saved_pending == 768); /* 512 + 256 */

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_record_forced(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   /* Deferred first, then forced */
   db1_payload_rewrite_record("sess-3", 1, 1024, 500, "deferred", "h1");
   db1_payload_rewrite_record("sess-3", 0, 0, 1500, "context_pressure", "h2");

   payload_rewrite_state_t out;
   int rc = db1_payload_rewrite_state_get("sess-3", &out);
   assert(rc == 0);
   /* Forced rewrite: payload_epoch increments, bytes_saved_pending resets */
   assert(out.payload_epoch == 1);
   assert(out.bytes_saved_pending == 0);
   assert(out.last_payload_tokens == 1500);
   assert(strcmp(out.rewrite_reason, "context_pressure") == 0);
   assert(strcmp(out.last_prefix_hash, "h2") == 0);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

int main(void)
{
   printf("payload_rewrite_state: ");

   char path[256];
   snprintf(path, sizeof path, "%s/test_payload_rewrite_XXXXXX.db", platform_tmpdir());
   int fd = mkstemps(path, 3);
   if (fd >= 0)
      close(fd);

   test_state_not_found(path);
   test_state_set_get(path);
   test_record_deferred(path);
   test_record_forced(path);

   printf("ok\n");
   return 0;
}
