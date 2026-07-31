/* test_config_snapshot: the live config snapshot (double-buffer + seqlock) — init/get,
 * no-op vs changed reload, validate-or-keep, and a concurrent torn-read stress. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "config_snapshot_test.h"
#include "config_sections.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "runtime_secret.h"

/* One staging buffer shared by the cases that must hand a whole config to
 * config_snapshot_init. Reused rather than redeclared: config_t is ~750 KB and
 * every mention of the type is tracked debt (check-config-encapsulation.py). */
static config_t g_stage;

/* P3 re-applier probe.
 *
 * The hook takes no arguments now and reads config the way a real re-applier
 * does. That makes this a STRONGER check than the old one: it used to be handed
 * the new config_t, which proved only that config_reload passed the right
 * pointer. Reading the accessor here proves the property the contract actually
 * rests on -- that the snapshot is already published when a re-applier runs, so
 * an accessor called inside one returns the NEW value. */
static _Atomic int g_reapply_calls = 0;
static int g_last_mode = -1;
static void probe_reapplier(void)
{
   g_last_mode = econ_mode_current();
   atomic_fetch_add_explicit(&g_reapply_calls, 1, memory_order_relaxed);
}

/* Author a config file with a MARKER PAIR (safe, budget) via config_save so reload
 * observes a real change. The pair is what the torn-read check keys on. */
static void write_marker(int safe, int budget)
{
   static config_t c;
   memset(&c, 0, sizeof c);
   config_load(&c);
   c.economizer_mode = safe ? ECON_MODE_SAFE : ECON_MODE_OFF;
   c.coord_closet_budget_bytes = budget;
   assert(config_save(&c) == 0);
}

static _Atomic int g_stop = 0;
static _Atomic long g_reads = 0, g_torn = 0;

typedef struct
{
   pthread_mutex_t mutex;
   pthread_cond_t changed;
   int enabled[7][2];
   int released[7][2];
   unsigned hits[7][2];
} snapshot_gate_t;

typedef struct
{
   config_t value;
   _Atomic int started;
   _Atomic int done;
} publish_once_t;

typedef struct
{
   config_t value;
   int result;
   _Atomic int done;
} read_once_t;

static void snapshot_gate_hook(config_snapshot_test_event_t event, unsigned slot, void *opaque)
{
   snapshot_gate_t *gate = opaque;
   assert(event > 0 && event < 7 && slot < 2);
   pthread_mutex_lock(&gate->mutex);
   gate->hits[event][slot]++;
   pthread_cond_broadcast(&gate->changed);
   while (gate->enabled[event][slot] && !gate->released[event][slot])
      pthread_cond_wait(&gate->changed, &gate->mutex);
   pthread_mutex_unlock(&gate->mutex);
}

static void snapshot_gate_init(snapshot_gate_t *gate)
{
   memset(gate, 0, sizeof(*gate));
   assert(pthread_mutex_init(&gate->mutex, NULL) == 0);
   assert(pthread_cond_init(&gate->changed, NULL) == 0);
}

static void snapshot_gate_destroy(snapshot_gate_t *gate)
{
   pthread_cond_destroy(&gate->changed);
   pthread_mutex_destroy(&gate->mutex);
}

static void snapshot_gate_enable(snapshot_gate_t *gate, config_snapshot_test_event_t event,
                                 unsigned slot)
{
   pthread_mutex_lock(&gate->mutex);
   gate->enabled[event][slot] = 1;
   gate->released[event][slot] = 0;
   pthread_mutex_unlock(&gate->mutex);
}

static void snapshot_gate_wait(snapshot_gate_t *gate, config_snapshot_test_event_t event,
                               unsigned slot, unsigned count)
{
   pthread_mutex_lock(&gate->mutex);
   while (gate->hits[event][slot] < count)
      pthread_cond_wait(&gate->changed, &gate->mutex);
   pthread_mutex_unlock(&gate->mutex);
}

static unsigned snapshot_gate_hits(snapshot_gate_t *gate, config_snapshot_test_event_t event,
                                   unsigned slot)
{
   pthread_mutex_lock(&gate->mutex);
   unsigned hits = gate->hits[event][slot];
   pthread_mutex_unlock(&gate->mutex);
   return hits;
}

static void snapshot_gate_release(snapshot_gate_t *gate, config_snapshot_test_event_t event,
                                  unsigned slot)
{
   pthread_mutex_lock(&gate->mutex);
   gate->released[event][slot] = 1;
   pthread_cond_broadcast(&gate->changed);
   pthread_mutex_unlock(&gate->mutex);
}

static void *read_once_thread(void *opaque)
{
   read_once_t *once = opaque;
   once->result = config_snapshot_get(&once->value);
   atomic_store_explicit(&once->done, 1, memory_order_release);
   return NULL;
}

static void *publish_once_thread(void *opaque)
{
   publish_once_t *once = opaque;
   atomic_store_explicit(&once->started, 1, memory_order_release);
   config_snapshot_init(&once->value);
   atomic_store_explicit(&once->done, 1, memory_order_release);
   return NULL;
}

static config_t snapshot_image(int marker)
{
   config_t image;
   assert(config_snapshot_get(&image) == 0);
   image.economizer_mode = marker & 1 ? ECON_MODE_SAFE : ECON_MODE_OFF;
   image.coord_closet_budget_bytes = marker * 1009;
   image.autonomy_max_turns = marker * 17;
   image.server_api_rate_limit_per_min = marker * 31;
   image.require_aimee_git = marker & 1;
   return image;
}

/* The two configs are {safe,budget=1111} and {off,budget=2222};
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
      int a = (c.economizer_mode == ECON_MODE_SAFE && c.coord_closet_budget_bytes == 1111);
      int b = (c.economizer_mode == ECON_MODE_OFF && c.coord_closet_budget_bytes == 2222);
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
      memset(&g_stage, 0, sizeof g_stage);
      config_load(&g_stage);
      config_snapshot_init(&g_stage);
      config_t got;
      assert(config_snapshot_get(&got) == 0);
      assert(got.economizer_mode == ECON_MODE_SAFE);
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
      assert(got.economizer_mode == ECON_MODE_OFF);
      assert(got.coord_closet_budget_bytes == 2222);
   }
   /* reloading the same file again is a no-op */
   assert(config_reload() == 0);

   /* --- a reload must not blank Vault-only secrets in the published snapshot ---
    *
    * These live only in the runtime secret store, never in aimee.yaml, so they are
    * reapplied by config_load. config_reload published config_load_file's result
    * directly, and config_load_file may serve the process cache and skip the parse
    * that rehydrates them — so a reload republished a snapshot with the secret
    * blank. On the appliance that emptied the minted API primary bearer one second
    * after boot and failed first-user bootstrap on every clean install.
    *
    * The store is seeded AFTER the snapshot is initialised on purpose: that is the
    * real ordering (server_main mints the primary after its first config_load). */
   {
      /* The file-read cache must be live: it is the thing that skips the parse.
       * The rest of this test disables it for determinism. */
      platform_unsetenv("AIMEE_NO_CACHE");
      write_marker(1, 4444); /* file and cache now agree, with no bearer in either */

      /* Mint exactly as server_main does AFTER that load: into the runtime store
       * and into the config it seeds the snapshot with, but never into aimee.yaml. */
      assert(runtime_secret_store("AIMEE_API_BEARER_TOKEN", "vault-only-primary") == 0);
      assert(config_snapshot_get(&g_stage) == 0);
      snprintf(g_stage.server_api_bearer_token, sizeof(g_stage.server_api_bearer_token), "%s",
               "vault-only-primary");
      config_snapshot_init(&g_stage);
      assert(strcmp(config_server_api_bearer_token(), "vault-only-primary") == 0);

      /* The 1s tick reloads with the file UNCHANGED, so config_load_file serves the
       * cached pre-mint copy and never re-parses. Publishing that blanked the bearer. */
      config_reload();
      assert(config_snapshot_get(&g_stage) == 0);
      assert(g_stage.coord_closet_budget_bytes == 4444); /* still the reloaded config */
      assert(strcmp(g_stage.server_api_bearer_token, "vault-only-primary") == 0);
      /* first-user bootstrap gates on the accessor, so assert that path too. */
      assert(strcmp(config_server_api_bearer_token(), "vault-only-primary") == 0);

      runtime_secret_remove("AIMEE_API_BEARER_TOKEN");
      platform_setenv("AIMEE_NO_CACHE", "1");
   }

   /* --- P3 re-applier registry: a hook fires after a changed reload, with the NEW config --- */
   {
      config_reload_register_reapplier(probe_reapplier);
      int before = atomic_load_explicit(&g_reapply_calls, memory_order_relaxed);
      write_marker(1, 1111); /* change back to safe */
      assert(config_reload() == 1);
      assert(atomic_load_explicit(&g_reapply_calls, memory_order_relaxed) == before + 1);
      /* The accessor, called from inside the re-applier, returned the NEW value. */
      assert(g_last_mode == ECON_MODE_SAFE);
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
      platform_unsetenv("AIMEE_AUTONOMY_USD_PER_SEC");
      assert(config_autonomy_lookup("AIMEE_AUTONOMY_USD_PER_SEC", &v) == 0);
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

      /* Run-safety caps are now config-backed + live too (GUI-tunable). Env override
       * wins; otherwise the live snapshot; a config.set applies without a restart. */
      platform_setenv("AIMEE_AUTONOMY_MAX_WALL_SECS", "5400");
      assert(config_autonomy_lookup("AIMEE_AUTONOMY_MAX_WALL_SECS", &v) == 1 && v == 5400);
      platform_unsetenv("AIMEE_AUTONOMY_MAX_WALL_SECS");
      platform_unsetenv("AIMEE_AUTONOMY_MAX_TURNS");
      memset(&sc, 0, sizeof sc);
      config_load(&sc);
      sc.autonomy_max_turns = 1234;
      sc.autonomy_max_wall_secs = 7200;
      sc.autonomy_auto_resume_cap_parks = 0; /* flip the default (ON) so the read is meaningful */
      assert(config_save(&sc) == 0);
      assert(config_reload() == 1);
      assert(config_autonomy_lookup("AIMEE_AUTONOMY_MAX_TURNS", &v) == 1 && v == 1234);
      assert(config_autonomy_lookup("AIMEE_AUTONOMY_MAX_WALL_SECS", &v) == 1 && v == 7200);
      assert(config_autonomy_lookup("AIMEE_AUTONOMY_AUTO_RESUME_CAP_PARKS", &v) == 1 && v == 0);
   }

   /* --- bounded saturation + WRITER_RESERVED is retryable contention --- */
   {
      unsigned active = config_snapshot_test_active_slot();
      config_t out, before;
      memset(&out, 0xa5, sizeof(out));
      before = out;
      assert(config_snapshot_test_set_slot_state(active, config_snapshot_test_reader_max()) == 0);
      assert(config_snapshot_get(&out) == -1);
      assert(memcmp(&out, &before, sizeof(out)) == 0);
      assert(config_snapshot_test_get_slot_state(active) == config_snapshot_test_reader_max());
      assert(config_snapshot_test_set_slot_state(active, 0) == 0);

      snapshot_gate_t gate;
      snapshot_gate_init(&gate);
      config_snapshot_test_set_hook(snapshot_gate_hook, &gate);
      assert(config_snapshot_test_set_slot_state(active, config_snapshot_test_writer_reserved()) ==
             0);
      read_once_t reader = {0};
      pthread_t thread;
      assert(pthread_create(&thread, NULL, read_once_thread, &reader) == 0);
      snapshot_gate_wait(&gate, CONFIG_SNAPSHOT_TEST_AFTER_OBSERVE, active, 3);
      assert(!atomic_load_explicit(&reader.done, memory_order_acquire));
      assert(config_snapshot_test_set_slot_state(active, 0) == 0);
      pthread_join(thread, NULL);
      assert(reader.result == 0);
      config_snapshot_test_set_hook(NULL, NULL);
      snapshot_gate_destroy(&gate);
   }

   /* --- delayed reader: writer reservation wins, so the old payload is unreachable --- */
   {
      config_t base = snapshot_image(10), next = snapshot_image(11), final = snapshot_image(12);
      config_snapshot_init(&base);
      unsigned old = config_snapshot_test_active_slot();
      snapshot_gate_t gate;
      snapshot_gate_init(&gate);
      snapshot_gate_enable(&gate, CONFIG_SNAPSHOT_TEST_AFTER_OBSERVE, old);
      snapshot_gate_enable(&gate, CONFIG_SNAPSHOT_TEST_BEFORE_WRITE, old);
      config_snapshot_test_set_hook(snapshot_gate_hook, &gate);

      read_once_t reader = {0};
      publish_once_t writer = {.value = final};
      pthread_t read_thread, write_thread;
      assert(pthread_create(&read_thread, NULL, read_once_thread, &reader) == 0);
      snapshot_gate_wait(&gate, CONFIG_SNAPSHOT_TEST_AFTER_OBSERVE, old, 1);
      config_snapshot_init(&next); /* first publication makes old the inactive target */
      assert(pthread_create(&write_thread, NULL, publish_once_thread, &writer) == 0);
      snapshot_gate_wait(&gate, CONFIG_SNAPSHOT_TEST_BEFORE_WRITE, old, 1);
      snapshot_gate_release(&gate, CONFIG_SNAPSHOT_TEST_AFTER_OBSERVE, old);
      pthread_join(read_thread, NULL);
      assert(reader.result == 0);
      assert(snapshot_gate_hits(&gate, CONFIG_SNAPSHOT_TEST_PIN_ACQUIRED, old) == 0);
      assert(!atomic_load_explicit(&writer.done, memory_order_acquire));
      snapshot_gate_release(&gate, CONFIG_SNAPSHOT_TEST_BEFORE_WRITE, old);
      pthread_join(write_thread, NULL);
      config_snapshot_test_set_hook(NULL, NULL);
      snapshot_gate_destroy(&gate);
   }

   /* --- delayed reader: reader pin wins, so the writer cannot reserve early --- */
   {
      config_t base = snapshot_image(20), next = snapshot_image(21), final = snapshot_image(22);
      config_snapshot_init(&base);
      unsigned old = config_snapshot_test_active_slot();
      snapshot_gate_t gate;
      snapshot_gate_init(&gate);
      snapshot_gate_enable(&gate, CONFIG_SNAPSHOT_TEST_AFTER_OBSERVE, old);
      snapshot_gate_enable(&gate, CONFIG_SNAPSHOT_TEST_PIN_ACQUIRED, old);
      snapshot_gate_enable(&gate, CONFIG_SNAPSHOT_TEST_BEFORE_RESERVE, old);
      config_snapshot_test_set_hook(snapshot_gate_hook, &gate);

      read_once_t reader = {0};
      publish_once_t writer = {.value = final};
      pthread_t read_thread, write_thread;
      assert(pthread_create(&read_thread, NULL, read_once_thread, &reader) == 0);
      snapshot_gate_wait(&gate, CONFIG_SNAPSHOT_TEST_AFTER_OBSERVE, old, 1);
      config_snapshot_init(&next);
      assert(pthread_create(&write_thread, NULL, publish_once_thread, &writer) == 0);
      snapshot_gate_wait(&gate, CONFIG_SNAPSHOT_TEST_BEFORE_RESERVE, old, 1);
      snapshot_gate_release(&gate, CONFIG_SNAPSHOT_TEST_AFTER_OBSERVE, old);
      snapshot_gate_wait(&gate, CONFIG_SNAPSHOT_TEST_PIN_ACQUIRED, old, 1);
      snapshot_gate_release(&gate, CONFIG_SNAPSHOT_TEST_BEFORE_RESERVE, old);
      snapshot_gate_wait(&gate, CONFIG_SNAPSHOT_TEST_RESERVE_CONTENDED, old, 1);
      assert(!atomic_load_explicit(&writer.done, memory_order_acquire));
      snapshot_gate_release(&gate, CONFIG_SNAPSHOT_TEST_PIN_ACQUIRED, old);
      pthread_join(read_thread, NULL);
      pthread_join(write_thread, NULL);
      assert(reader.result == 0);
      config_snapshot_test_set_hook(NULL, NULL);
      snapshot_gate_destroy(&gate);
   }

   /* --- validated old-slot pin blocks the consecutive publisher that would reuse it --- */
   {
      config_t base = snapshot_image(30), next = snapshot_image(31), final = snapshot_image(32);
      config_snapshot_init(&base);
      unsigned old = config_snapshot_test_active_slot();
      snapshot_gate_t gate;
      snapshot_gate_init(&gate);
      snapshot_gate_enable(&gate, CONFIG_SNAPSHOT_TEST_PIN_VALIDATED, old);
      config_snapshot_test_set_hook(snapshot_gate_hook, &gate);

      read_once_t reader = {0};
      publish_once_t writer = {.value = final};
      pthread_t read_thread, write_thread;
      assert(pthread_create(&read_thread, NULL, read_once_thread, &reader) == 0);
      snapshot_gate_wait(&gate, CONFIG_SNAPSHOT_TEST_PIN_VALIDATED, old, 1);
      config_snapshot_init(&next);
      assert(pthread_create(&write_thread, NULL, publish_once_thread, &writer) == 0);
      snapshot_gate_wait(&gate, CONFIG_SNAPSHOT_TEST_RESERVE_CONTENDED, old, 1);
      assert(!atomic_load_explicit(&writer.done, memory_order_acquire));
      snapshot_gate_release(&gate, CONFIG_SNAPSHOT_TEST_PIN_VALIDATED, old);
      pthread_join(read_thread, NULL);
      pthread_join(write_thread, NULL);
      assert(reader.result == 0);
      config_snapshot_test_set_hook(NULL, NULL);
      snapshot_gate_destroy(&gate);
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
