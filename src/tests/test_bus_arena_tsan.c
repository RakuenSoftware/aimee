/* test_bus_arena_tsan.c: the cross-thread arena lease-table race harness.
 *
 * bus_arena's lease table is host-private but NOT single-threaded: a co-located
 * producer (D7) allocates and fills leases from its own thread while the host's
 * pump thread publishes and releases them, and a consumer thread reads and
 * releases in place. This harness runs exactly that shape — three threads all
 * touching one lease table through the arena API — so ThreadSanitizer can prove
 * a->lock actually serialises every table transition. Built and run only in the
 * TSan lane (scripts/run-bus-arena-tsan.sh); a race aborts the process.
 *
 * It is also a functional stress on its own: a small arena forces leases to
 * recycle under contention, and at the end every byte must have been read intact
 * and the arena must have drained back to empty (no leak, no stranded lease).
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bus_client.h"
#include "bus_host.h"
#include "bus_ring.h"

#define KIND_A     700
#define N_MESSAGES 20000
#define MIN_LEN    200
#define MAX_LEN    3000

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      abort();
   }
}

/* ---- shared state ---- */

static bus_host_t g_host;
static bus_client_t g_producer;
static bus_client_t g_consumer;
static atomic_int g_produced;  /* messages the producer has emitted */
static atomic_int g_consumed;  /* messages the consumer has read + released */
static atomic_int g_stop_pump; /* set once producer+consumer are done */

/* ---- attach one client over a socketpair the host serves on a helper thread -- */

struct serve_arg
{
   int fd;
};
static void *serve_thread(void *p)
{
   bus_host_serve_attach(&g_host, ((struct serve_arg *)p)->fd);
   return NULL;
}
static void attach(bus_client_t *c)
{
   int sv[2];
   must(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair");
   struct serve_arg a = {.fd = sv[1]};
   pthread_t t;
   must(pthread_create(&t, NULL, serve_thread, &a) == 0, "spawn serve");
   must(bus_client_attach(sv[0], c) == BUS_CLIENT_OK, "attach");
   must(pthread_join(t, NULL) == 0, "join serve");
   close(sv[0]);
   close(sv[1]);
}

/* ---- the three racing threads ---- */

/* Producer: allocate a lease on the host arena (a table write from THIS thread),
 * fill the span (outside the lock), then emit the reference frame. Backpressure
 * (cap reached, arena full, outbound full) is transient — the pump and consumer
 * drain it — so retry with a yield rather than fail. */
static void *producer_main(void *arg)
{
   (void)arg;
   uint32_t seed = 0x1234;
   for (int i = 0; i < N_MESSAGES; i++)
   {
      seed = seed * 1664525u + 1013904223u;
      uint32_t len = MIN_LEN + (seed % (MAX_LEN - MIN_LEN));
      uint8_t fill = (uint8_t)(seed >> 24);

      uint32_t lease;
      while (bus_arena_alloc(&g_host.arena, g_producer.reply.handle_id, len, &lease) !=
             BUS_ARENA_OK)
         sched_yield(); /* cap or space: a consumer release will free room */

      uint8_t *p = NULL;
      must(bus_arena_fill_ptr(&g_host.arena, lease, &p) == BUS_ARENA_OK, "fill");
      memset(p, fill, len); /* payload byte carries its own expected value */

      bus_arena_ref_t ref;
      must(bus_arena_ref(&g_host.arena, lease, &ref) == BUS_ARENA_OK, "ref");

      while (bus_client_publish_arena(&g_producer, KIND_A, lease, ref.generation, len) !=
             BUS_CLIENT_OK)
         sched_yield(); /* outbound full: the pump will drain it */

      atomic_fetch_add_explicit(&g_produced, 1, memory_order_relaxed);
   }
   return NULL;
}

/* Pump: route on its own thread, publishing/releasing leases concurrently with
 * the producer's allocs and the consumer's reads. */
static void *pump_main(void *arg)
{
   (void)arg;
   while (!atomic_load_explicit(&g_stop_pump, memory_order_acquire))
   {
      bus_host_pump(&g_host);
      sched_yield();
   }
   bus_host_pump(&g_host); /* final drain after stop */
   return NULL;
}

/* Consumer: poll the reference frame, read the leased span in place (a table read
 * from THIS thread), verify the bytes are self-consistent, and release. */
static void *consumer_main(void *arg)
{
   (void)arg;
   while (atomic_load_explicit(&g_consumed, memory_order_relaxed) < N_MESSAGES)
   {
      bus_event_t ev;
      bus_client_result_t r = bus_client_poll(&g_consumer, &ev);
      if (r != BUS_CLIENT_OK)
      {
         sched_yield();
         continue;
      }
      if (!(ev.frame.hdr_flags & BUS_F_ARENA))
         continue; /* only arena frames in this harness */

      const uint8_t *p = NULL;
      must(bus_arena_read_ptr(&g_host.arena, (uint32_t)ev.frame.payload_ref, ev.frame.generation,
                              g_consumer.reply.handle_id, &p) == BUS_ARENA_OK,
           "read_ptr");
      /* Every byte of the span must equal the first: a torn write or a bled-in
       * neighbouring lease would break this. */
      uint8_t v = p[0];
      for (uint32_t j = 1; j < ev.frame.payload_len; j++)
         must(p[j] == v, "span self-consistent (no torn write, no cross-lease bleed)");

      must(bus_arena_release(&g_host.arena, (uint32_t)ev.frame.payload_ref, ev.frame.generation,
                             g_consumer.reply.handle_id) == BUS_ARENA_OK,
           "release");
      atomic_fetch_add_explicit(&g_consumed, 1, memory_order_relaxed);
   }
   return NULL;
}

int main(void)
{
   printf("test_bus_arena_tsan: %d messages across producer/pump/consumer threads\n", N_MESSAGES);

   bus_host_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.max_slots = 4;
   cfg.slot_size = 256;
   cfg.inline_budget = 192;
   cfg.queue_capacity = 64;
   cfg.arena_size = 64 * 1024; /* small: forces leases to recycle under contention */
   must(bus_host_create(&g_host, &cfg, NULL, NULL) == BUS_HOST_OK, "host");

   attach(&g_producer);
   attach(&g_consumer);
   must(bus_host_subscribe(&g_host, g_consumer.reply.handle_id, KIND_A) == BUS_HOST_OK,
        "subscribe");

   pthread_t prod, pump, cons;
   must(pthread_create(&pump, NULL, pump_main, NULL) == 0, "spawn pump");
   must(pthread_create(&prod, NULL, producer_main, NULL) == 0, "spawn producer");
   must(pthread_create(&cons, NULL, consumer_main, NULL) == 0, "spawn consumer");

   must(pthread_join(prod, NULL) == 0, "join producer");
   must(pthread_join(cons, NULL) == 0, "join consumer");
   atomic_store_explicit(&g_stop_pump, 1, memory_order_release);
   must(pthread_join(pump, NULL) == 0, "join pump");

   must(atomic_load(&g_produced) == N_MESSAGES, "every message produced");
   must(atomic_load(&g_consumed) == N_MESSAGES, "every message consumed and released");
   /* The arena drained back to empty: no leaked span, no stranded lease. */
   must(bus_arena_bytes_in_use(&g_host.arena) == 0, "arena fully drained (no leak)");
   must(bus_arena_live_leases(&g_host.arena, g_producer.reply.handle_id) == 0, "no live leases");

   bus_client_detach(&g_producer);
   bus_client_detach(&g_consumer);
   bus_host_destroy(&g_host);
   printf("test_bus_arena_tsan: OK (%d messages, arena drained, no race)\n", N_MESSAGES);
   return 0;
}
