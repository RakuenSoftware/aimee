/* bus_bench.c: the event-bus dispatch-overhead benchmark (slice 12).
 *
 * It measures the one number the performance budget commits to: per-event bus
 * dispatch overhead — host enqueue (a producer writing its outbound ring)
 * through to client dequeue (a subscriber reading its inbound ring), excluding
 * any module work on the payload. That is the cost the bus adds to move one
 * event, and the number a later memory-migration slice is gated against.
 *
 * It prints one line the perf gate parses:
 *   dispatch_overhead_ns=<median per-event nanoseconds>
 *
 * This is a test binary, never linked into a shipping target (D7).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bus_client.h"
#include "bus_host.h"
#include "bus_ring.h"

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

struct serve_arg
{
   bus_host_t *h;
   int fd;
};
static void *serve_thread(void *p)
{
   struct serve_arg *a = p;
   bus_host_serve_attach(a->h, a->fd);
   return NULL;
}
static void attach_client(bus_host_t *h, bus_client_t *c)
{
   int sv[2];
   if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0)
      abort();
   struct serve_arg a = {.h = h, .fd = sv[1]};
   pthread_t t;
   pthread_create(&t, NULL, serve_thread, &a);
   if (bus_client_attach(sv[0], c) != BUS_CLIENT_OK)
      abort();
   pthread_join(t, NULL);
   close(sv[0]);
   close(sv[1]);
}

static int cmp_u64(const void *a, const void *b)
{
   uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
   return (x > y) - (x < y);
}

#define KIND 900

int main(int argc, char **argv)
{
   uint64_t total = (argc > 1) ? strtoull(argv[1], NULL, 10) : 2000000;

   bus_host_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.max_slots = 4;
   cfg.slot_size = 256;
   cfg.inline_budget = 192;
   cfg.queue_capacity = 64;
   cfg.arena_size = 256 * 1024;

   bus_host_t h;
   if (bus_host_create(&h, &cfg, NULL, NULL) != BUS_HOST_OK)
      abort();
   bus_client_t pub, sub;
   attach_client(&h, &pub);
   attach_client(&h, &sub);
   bus_host_subscribe(&h, sub.reply.handle_id, KIND);

   /* One event's dispatch = publish (producer enqueue) + host pump (route) +
    * poll (consumer dequeue). Batched to the ring's data capacity so timer
    * granularity does not dominate; each batch's time is divided by its events.
    * We record per-batch per-event samples and report the median, which is
    * robust to scheduler noise. */
   uint32_t batch = cfg.queue_capacity - 4; /* leave headroom below capacity */
   uint64_t batches = total / batch;
   if (batches < 100)
      batches = 100;

   uint64_t *samples = malloc(batches * sizeof(uint64_t));
   if (!samples)
      abort();

   const char *payload = "benchmark-payload-16b";
   uint32_t plen = 16;

   /* Warm up. */
   for (int w = 0; w < 1000; w++)
   {
      for (uint32_t i = 0; i < batch; i++)
         bus_client_publish(&pub, KIND, payload, plen);
      bus_host_pump(&h);
      bus_event_t ev;
      while (bus_client_poll(&sub, &ev) == BUS_CLIENT_OK)
         ;
   }

   for (uint64_t b = 0; b < batches; b++)
   {
      uint64_t t0 = now_ns();
      for (uint32_t i = 0; i < batch; i++)
         bus_client_publish(&pub, KIND, payload, plen);
      bus_host_pump(&h);
      bus_event_t ev;
      uint32_t got = 0;
      while (bus_client_poll(&sub, &ev) == BUS_CLIENT_OK)
         got++;
      uint64_t t1 = now_ns();
      if (got != batch)
      {
         fprintf(stderr, "bench: expected %u events, got %u\n", batch, got);
         return 1;
      }
      samples[b] = (t1 - t0) / batch;
   }

   qsort(samples, batches, sizeof(uint64_t), cmp_u64);
   uint64_t median = samples[batches / 2];
   uint64_t p99 = samples[(batches * 99) / 100];

   printf("dispatch_overhead_ns=%llu\n", (unsigned long long)median);
   printf("dispatch_overhead_p99_ns=%llu\n", (unsigned long long)p99);
   fprintf(stderr, "bench: %llu batches of %u events (median %llu ns/event, p99 %llu ns)\n",
           (unsigned long long)batches, batch, (unsigned long long)median, (unsigned long long)p99);

   free(samples);
   bus_client_detach(&pub);
   bus_client_detach(&sub);
   bus_host_destroy(&h);
   return 0;
}
