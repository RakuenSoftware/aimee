#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "config_snapshot_test.h"

typedef struct
{
   pthread_mutex_t mutex;
   pthread_cond_t changed;
   int validated;
   int contended;
   int release_reader;
} critical_gate_t;

typedef struct
{
   config_t image;
   int result;
} critical_call_t;

static _Atomic int stop_readers;
static _Atomic int writer_running;
static _Atomic int readers_ready;
static _Atomic long accepted_reads;
static _Atomic long overlap_reads;
static _Atomic long mixed_reads;

static void critical_hook(config_snapshot_test_event_t event, unsigned slot, void *opaque)
{
   (void)slot;
   critical_gate_t *gate = opaque;
   pthread_mutex_lock(&gate->mutex);
   if (event == CONFIG_SNAPSHOT_TEST_PIN_VALIDATED && !gate->validated)
   {
      gate->validated = 1;
      pthread_cond_broadcast(&gate->changed);
      while (!gate->release_reader)
         pthread_cond_wait(&gate->changed, &gate->mutex);
   }
   else if (event == CONFIG_SNAPSHOT_TEST_RESERVE_CONTENDED)
   {
      gate->contended = 1;
      pthread_cond_broadcast(&gate->changed);
   }
   pthread_mutex_unlock(&gate->mutex);
}

static void critical_wait(critical_gate_t *gate, int *condition)
{
   pthread_mutex_lock(&gate->mutex);
   while (!*condition)
      pthread_cond_wait(&gate->changed, &gate->mutex);
   pthread_mutex_unlock(&gate->mutex);
}

static void *critical_read(void *opaque)
{
   critical_call_t *call = opaque;
   call->result = config_snapshot_get(&call->image);
   return NULL;
}

static void *critical_publish(void *opaque)
{
   critical_call_t *call = opaque;
   config_snapshot_init(&call->image);
   return NULL;
}

static void fill_image(config_t *image, int marker)
{
   /* Give every byte, including padding and otherwise-unused members, a
    * publication-specific value so the oracle detects any torn full copy. */
   memset(image, (unsigned char)(marker * 131u), sizeof(*image));
   image->economizer_mode = marker & 1 ? 3 : 0;
   image->coord_closet_budget_bytes = marker * 1009;
   image->autonomy_max_turns = marker * 17;
   image->server_api_rate_limit_per_min = marker * 31;
   image->require_aimee_git = marker & 1;
   image->memory_window_radius = marker * 43;
   snprintf(image->server_api_client_transport, sizeof(image->server_api_client_transport), "m%06d",
            marker);
}

static int complete_image(const config_t *image)
{
   int marker = image->coord_closet_budget_bytes / 1009;
   if (marker < 1 || marker > 10000)
      return 0;
   config_t expected;
   fill_image(&expected, marker);
   return memcmp(image, &expected, sizeof(expected)) == 0;
}

static void *reader(void *unused)
{
   (void)unused;
   atomic_fetch_add_explicit(&readers_ready, 1, memory_order_release);
   while (!atomic_load_explicit(&writer_running, memory_order_acquire))
      sched_yield();
   while (!atomic_load_explicit(&stop_readers, memory_order_acquire))
   {
      config_t image;
      if (config_snapshot_get(&image) != 0)
         continue;
      atomic_fetch_add_explicit(&accepted_reads, 1, memory_order_relaxed);
      if (atomic_load_explicit(&writer_running, memory_order_acquire))
         atomic_fetch_add_explicit(&overlap_reads, 1, memory_order_relaxed);
      if (!complete_image(&image))
         atomic_fetch_add_explicit(&mixed_reads, 1, memory_order_relaxed);
   }
   return NULL;
}

int main(void)
{
   config_t image, untouched;
   memset(&image, 0xa5, sizeof(image));
   untouched = image;
   assert(config_snapshot_get(&image) == -1);
   assert(memcmp(&image, &untouched, sizeof(image)) == 0);

   fill_image(&image, 1);
   config_snapshot_init(&image);

   /* Force the original failure schedule under TSan: a reader holds a
    * validated pin on the old active slot, one publication flips away from
    * it, and a consecutive publisher reaches reservation contention before
    * the reader is released to copy and unpin. */
   critical_gate_t gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};
   critical_call_t critical_reader = {0}, critical_writer = {0};
   pthread_t critical_reader_thread, critical_writer_thread;
   config_snapshot_test_set_hook(critical_hook, &gate);
   assert(pthread_create(&critical_reader_thread, NULL, critical_read, &critical_reader) == 0);
   critical_wait(&gate, &gate.validated);
   fill_image(&image, 2);
   config_snapshot_init(&image);
   fill_image(&critical_writer.image, 3);
   assert(pthread_create(&critical_writer_thread, NULL, critical_publish, &critical_writer) == 0);
   critical_wait(&gate, &gate.contended);
   pthread_mutex_lock(&gate.mutex);
   gate.release_reader = 1;
   pthread_cond_broadcast(&gate.changed);
   pthread_mutex_unlock(&gate.mutex);
   pthread_join(critical_reader_thread, NULL);
   pthread_join(critical_writer_thread, NULL);
   config_snapshot_test_set_hook(NULL, NULL);
   assert(critical_reader.result == 0 && complete_image(&critical_reader.image));
   pthread_cond_destroy(&gate.changed);
   pthread_mutex_destroy(&gate.mutex);

   pthread_t readers[4];
   for (size_t i = 0; i < sizeof(readers) / sizeof(readers[0]); ++i)
      assert(pthread_create(&readers[i], NULL, reader, NULL) == 0);
   while (atomic_load_explicit(&readers_ready, memory_order_acquire) != 4)
      sched_yield();
   atomic_store_explicit(&writer_running, 1, memory_order_release);
   for (int marker = 4; marker <= 10000; ++marker)
   {
      fill_image(&image, marker);
      config_snapshot_init(&image);
   }
   atomic_store_explicit(&writer_running, 0, memory_order_release);
   atomic_store_explicit(&stop_readers, 1, memory_order_release);
   for (size_t i = 0; i < sizeof(readers) / sizeof(readers[0]); ++i)
      pthread_join(readers[i], NULL);
   assert(atomic_load_explicit(&accepted_reads, memory_order_relaxed) > 0);
   assert(atomic_load_explicit(&overlap_reads, memory_order_relaxed) > 0);
   assert(atomic_load_explicit(&mixed_reads, memory_order_relaxed) == 0);
   puts("config snapshot race stress: ok");
   return 0;
}
