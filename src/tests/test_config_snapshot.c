/* test_config_snapshot: the live config snapshot (double-buffer + seqlock) — init/get,
 * no-op vs changed reload, validate-or-keep, and a concurrent torn-read stress. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "platform_path.h"
#include "platform_test_util.h"

/* Author a config file with a MARKER PAIR (aggressive, budget) via config_save so reload
 * observes a real change. The pair is what the torn-read check keys on. */
static void write_marker(int aggressive, int budget)
{
   static config_t c;
   memset(&c, 0, sizeof c);
   config_load(&c);
   /* keep the config VALID regardless of what a prior (deliberately-invalid) test left on
    * disk, so config_reload's validate-or-keep never rejects a marker write. */
   c.reduce_gateway_session_disable_ttl_ms = 3600000;
   c.economizer_aggressive = aggressive;
   c.coord_closet_budget_bytes = budget;
   assert(config_save(&c) == 0);
}

static _Atomic int g_stop = 0;
static _Atomic long g_reads = 0, g_torn = 0;

/* The two configs written by the writer are {agg=1,budget=1111} and {agg=0,budget=2222};
 * a reader must never observe a mismatched pair (that would be a torn cross-slot read). */
static void *reader_thread(void *arg)
{
   (void)arg;
   while (!atomic_load_explicit(&g_stop, memory_order_relaxed))
   {
      config_t c;
      if (config_snapshot_get(&c) != 0)
         continue;
      atomic_fetch_add_explicit(&g_reads, 1, memory_order_relaxed);
      int a = (c.economizer_aggressive == 1 && c.coord_closet_budget_bytes == 1111);
      int b = (c.economizer_aggressive == 0 && c.coord_closet_budget_bytes == 2222);
      if (!a && !b)
         atomic_fetch_add_explicit(&g_torn, 1, memory_order_relaxed);
   }
   return NULL;
}

int main(void)
{
   printf("config_snapshot: ");
   char tmpdir[512];
   snprintf(tmpdir, sizeof tmpdir, "%s/aimee-test-snap-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   platform_setenv("HOME", tmpdir);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1"); /* deterministic file reads for the functional part */

   /* --- get before init -> -1 --- */
   {
      config_t c;
      assert(config_snapshot_get(&c) == -1);
   }

   /* --- init + get roundtrip --- */
   write_marker(1, 1111);
   {
      static config_t c0;
      memset(&c0, 0, sizeof c0);
      config_load(&c0);
      config_snapshot_init(&c0);
      config_t got;
      assert(config_snapshot_get(&got) == 0);
      assert(got.economizer_aggressive == 1);
      assert(got.coord_closet_budget_bytes == 1111);
   }

   /* --- reload with NO change -> no-op (0) --- */
   assert(config_reload() == 0);

   /* --- reload after a real change -> published (1), snapshot reflects it --- */
   write_marker(0, 2222);
   assert(config_reload() == 1);
   {
      config_t got;
      assert(config_snapshot_get(&got) == 0);
      assert(got.economizer_aggressive == 0);
      assert(got.coord_closet_budget_bytes == 2222);
   }
   /* reloading the same file again is a no-op */
   assert(config_reload() == 0);

   /* --- validate-or-keep: an INVALID file -> reload -1, snapshot unchanged --- */
   {
      const char *path = config_default_path();
      FILE *fp = fopen(path, "w");
      assert(fp);
      /* ttl <= 0 is startup-fatal per config_reduce_validate */
      fputs("reduce:\n  gateway_session_disable_ttl_ms: 0\n", fp);
      fclose(fp);
      assert(config_reload() == -1); /* kept */
      config_t got;
      assert(config_snapshot_get(&got) == 0);
      assert(got.coord_closet_budget_bytes == 2222); /* still the last GOOD snapshot */
   }

   /* --- concurrent torn-read stress: readers spin while the writer toggles + reloads --- */
   {
      platform_unsetenv("AIMEE_NO_CACHE"); /* exercise the real cached read path under threads */
      write_marker(1, 1111);
      assert(config_reload() == 1);
      pthread_t th[4];
      for (int i = 0; i < 4; i++)
         assert(pthread_create(&th[i], NULL, reader_thread, NULL) == 0);
      for (int i = 0; i < 400; i++)
      {
         if (i & 1)
            write_marker(0, 2222);
         else
            write_marker(1, 1111);
         config_reload();
      }
      atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
      for (int i = 0; i < 4; i++)
         pthread_join(th[i], NULL);
      assert(atomic_load_explicit(&g_reads, memory_order_relaxed) > 0); /* readers ran */
      assert(atomic_load_explicit(&g_torn, memory_order_relaxed) == 0); /* no torn reads */
   }

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   printf("ok\n");
   return 0;
}
