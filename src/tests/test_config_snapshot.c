/* test_config_snapshot: the live config snapshot (double-buffer + seqlock) — init/get,
 * no-op vs changed reload, validate-or-keep, and a concurrent torn-read stress. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "config_sections.h"
#include "platform_path.h"
#include "platform_test_util.h"

/* P3 re-applier probe. */
static _Atomic int g_reapply_calls = 0;
static int g_last_agg = -1;
static void probe_reapplier(const config_t *o, const config_t *n)
{
   (void)o;
   g_last_agg = n->economizer_tier;
   atomic_fetch_add_explicit(&g_reapply_calls, 1, memory_order_relaxed);
}

/* Author a config file with a MARKER PAIR (aggressive, budget) via config_save so reload
 * observes a real change. The pair is what the torn-read check keys on. */
static void write_marker(int aggressive, int budget)
{
   static config_t c;
   memset(&c, 0, sizeof c);
   config_load(&c);
   /* keep the config VALID regardless of what a prior (deliberately-invalid) test left on
    * disk, so config_reload's validate-or-keep never rejects a marker write. */
   c.economizer_tier = aggressive ? ECON_TIER_AGGRESSIVE : ECON_TIER_OFF;
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
      int a = (c.economizer_tier == ECON_TIER_AGGRESSIVE && c.coord_closet_budget_bytes == 1111);
      int b = (c.economizer_tier == ECON_TIER_OFF && c.coord_closet_budget_bytes == 2222);
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
      assert(got.economizer_tier == ECON_TIER_AGGRESSIVE);
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
      assert(got.economizer_tier == ECON_TIER_OFF);
      assert(got.coord_closet_budget_bytes == 2222);
   }
   /* reloading the same file again is a no-op */
   assert(config_reload() == 0);

   /* --- validate-or-keep: an INVALID file -> reload -1, snapshot unchanged. In strict mode a
    * schema type error (int key given a string) is fatal, so config_load_file returns -1 and
    * config_reload keeps the last good snapshot. --- */
   {
      extern int g_config_strict;
      const char *path = config_default_path();
      FILE *fp = fopen(path, "w");
      assert(fp);
      fputs("db2_pool_size: \"not-a-number\"\n", fp); /* SCHEMA_INT given a string */
      fclose(fp);
      g_config_strict = 1;
      assert(config_reload() == -1); /* kept */
      g_config_strict = 0;
      config_t got;
      assert(config_snapshot_get(&got) == 0);
      assert(got.coord_closet_budget_bytes == 2222); /* still the last GOOD snapshot */
   }

   /* --- P3 re-applier registry: a hook fires after a changed reload, with the NEW config --- */
   {
      config_reload_register_reapplier(probe_reapplier);
      int before = atomic_load_explicit(&g_reapply_calls, memory_order_relaxed);
      write_marker(1, 1111); /* change back to aggressive=1 */
      assert(config_reload() == 1);
      assert(atomic_load_explicit(&g_reapply_calls, memory_order_relaxed) == before + 1);
      assert(g_last_agg == ECON_TIER_AGGRESSIVE); /* re-applier saw the NEW value */
      /* a no-op reload does NOT fire the re-applier */
      int mid = atomic_load_explicit(&g_reapply_calls, memory_order_relaxed);
      assert(config_reload() == 0);
      assert(atomic_load_explicit(&g_reapply_calls, memory_order_relaxed) == mid);
   }

   /* --- P4 config_reload_if_changed: an OUT-OF-BAND file write (as a CLI local config.set,
    * a manual edit, or the autonomous config_save produces) is picked up on the main-loop
    * tick WITHOUT an explicit config_reload()/SIGHUP; an unchanged file is a no-op. --- */
   {
      (void)config_reload_if_changed();        /* first call seeds the baseline + reconciles */
      assert(config_reload_if_changed() == 0); /* no on-disk change since -> no-op */
      write_marker(0, 3333);                   /* out-of-band write */
      assert(config_reload_if_changed() == 1); /* detected the change + published */
      {
         config_t got;
         assert(config_snapshot_get(&got) == 0);
         assert(got.coord_closet_budget_bytes == 3333); /* the new value is live now */
      }
      assert(config_reload_if_changed() == 0); /* stable again -> no-op */
   }

   /* --- autonomy-live: config_autonomy_lookup (operator env override > live snapshot,
    * non-config var -> fall back) --- */
   {
      long v;
      /* a non-config autonomy var -> 0 so the caller uses its own env/default */
      platform_unsetenv("AIMEE_AUTONOMY_MAX_TURNS");
      assert(config_autonomy_lookup("AIMEE_AUTONOMY_MAX_TURNS", &v) == 0);
      /* operator env override wins */
      platform_setenv("AIMEE_AUTONOMY_SKEPTICS", "7");
      assert(config_autonomy_lookup("AIMEE_AUTONOMY_SKEPTICS", &v) == 1 && v == 7);
      /* no env -> the LIVE snapshot value (write skeptics=9, reload, look up) */
      platform_unsetenv("AIMEE_AUTONOMY_SKEPTICS");
      static config_t sc;
      memset(&sc, 0, sizeof sc);
      config_load(&sc); /* snapshot base */
      sc.autonomy_skeptics = 9;
      assert(config_save(&sc) == 0);
      assert(config_reload() == 1);
      assert(config_autonomy_lookup("AIMEE_AUTONOMY_SKEPTICS", &v) == 1 && v == 9);
   }

   /* --- concurrent torn-read stress: readers spin while the writer toggles + reloads --- */
   {
      platform_unsetenv("AIMEE_NO_CACHE"); /* exercise the real cached read path under threads */
      write_marker(0, 2222);               /* a real change vs the prior block's {1,1111} */
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
