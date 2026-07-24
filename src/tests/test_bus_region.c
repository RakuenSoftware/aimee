/* test_bus_region.c: slice 3 of the event-bus feature tree.
 *
 * The regions are the D1 isolation boundary, so the tests check the boundary as
 * a fact rather than a convention:
 *
 *   - A client maps the control region PROT_READ. Writing it must FAULT, not be
 *     tolerated. That is verified by forking a child that writes the mapping and
 *     asserting it dies on SIGSEGV/SIGBUS — the enforced half of D2, proven
 *     against the MMU rather than asserted in prose.
 *
 *   - Every header a client reads is treated as written by another process:
 *     corrupt magic, version, or geometry is refused, not propagated into a
 *     mapping size or a ring walk.
 *
 *   - The D4 parameters are read back from the control region, so a value that
 *     was never compiled in survives create -> map -> attach.
 *
 *   - A stale host_epoch is detected.
 *
 *   - A queue-pair region round-trips real traffic through both of its rings
 *     while mapped, which is the proof that the region layout actually places
 *     two working rings where the header claims.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus_region.h"

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      abort();
   }
}

static void must_result(bus_region_result_t got, bus_region_result_t want, const char *what)
{
   if (got != want)
   {
      fprintf(stderr, "FAIL: %s: expected %s, got %s\n", what, bus_region_result_name(want),
              bus_region_result_name(got));
      abort();
   }
}

/* ------------------------------------------------------------------ */
/* control region                                                      */

static void test_control_roundtrip(void)
{
   /* Deliberately non-default parameters, so "read back from the region" is a
    * real claim and not "matches a compiled constant". */
   const uint32_t slot = 320, inl = 200, cap = 32;
   const uint64_t arena = 512 * 1024;

   bus_region_t create;
   must_result(bus_region_create("ctl", bus_control_bytes(), &create), BUS_REGION_OK, "create");

   bus_region_t host;
   must_result(bus_region_map(create.fd, create.size, 1, &host), BUS_REGION_OK, "host map rw");
   must_result(bus_control_init(&host, slot, inl, cap, arena), BUS_REGION_OK, "control init");

   /* A client maps the same fd read-only and reads the parameters back. */
   bus_region_t client;
   must_result(bus_region_map(create.fd, create.size, 0, &client), BUS_REGION_OK,
               "client map ro");
   bus_control_t *c = NULL;
   must_result(bus_control_attach(&client, &c), BUS_REGION_OK, "control attach");
   must(atomic_load_explicit(&c->slot_size, memory_order_relaxed) == slot, "slot_size read back");
   must(atomic_load_explicit(&c->inline_budget, memory_order_relaxed) == inl,
        "inline_budget read back");
   must(atomic_load_explicit(&c->queue_capacity, memory_order_relaxed) == cap,
        "capacity read back");
   must(atomic_load_explicit(&c->arena_size, memory_order_relaxed) == arena, "arena read back");

   /* Epoch: cache it, bump it on the host mapping, detect the change. */
   uint64_t attached = bus_control_epoch(c);
   must(attached == 1, "fresh epoch is 1");
   must(!bus_control_epoch_changed(c, attached), "no change yet");
   bus_control_bump_epoch((bus_control_t *)host.base);
   must(bus_control_epoch_changed(c, attached), "client sees the epoch bump");

   bus_region_unmap(&client);
   bus_region_unmap(&host);
   close(create.fd);
   printf("  control: params read back from the region, epoch bump detected\n");
}

#define RO_FAULT_SENTINEL 42

static void ro_fault_handler(int sig)
{
   (void)sig;
   _exit(RO_FAULT_SENTINEL);
}

/* Fork a child that writes the read-only control mapping. The write must fault.
 * Doing it in a child keeps the fault from taking the test down, and turns "is
 * the mapping really read-only" into a checked outcome.
 *
 * How the fault surfaces depends on the build: on a plain build the child's own
 * SIGSEGV/SIGBUS handler runs and exits with a sentinel; under ASAN, ASAN's
 * deadly-signal handler runs first, reports the SEGV, and aborts the child with
 * SIGABRT. Both prove the write faulted. The one outcome that fails the test is
 * the child exiting 0 — that would mean the write to a PROT_READ page was
 * tolerated. */
static void test_control_readonly_faults(void)
{
   bus_region_t create;
   must_result(bus_region_create("ctl-ro", bus_control_bytes(), &create), BUS_REGION_OK,
               "create");
   bus_region_t host;
   must_result(bus_region_map(create.fd, create.size, 1, &host), BUS_REGION_OK, "host map");
   must_result(bus_control_init(&host, 256, 192, 16, 4096), BUS_REGION_OK, "init");

   bus_region_t ro;
   must_result(bus_region_map(create.fd, create.size, 0, &ro), BUS_REGION_OK, "map ro");

   fflush(stdout);
   pid_t pid = fork();
   must(pid >= 0, "fork");
   if (pid == 0)
   {
      signal(SIGSEGV, ro_fault_handler);
      signal(SIGBUS, ro_fault_handler);
      /* Child: this store is to a PROT_READ page. It must fault. */
      volatile uint32_t *p = (volatile uint32_t *)ro.base;
      *p = 0xdeadbeef;
      _exit(0); /* reached only if the write did NOT fault */
   }

   int status = 0;
   must(waitpid(pid, &status, 0) == pid, "waitpid");

   int faulted = 0;
   const char *how = "?";
   if (WIFEXITED(status) && WEXITSTATUS(status) == RO_FAULT_SENTINEL)
   {
      faulted = 1;
      how = "handler";
   }
   else if (WIFSIGNALED(status))
   {
      int sig = WTERMSIG(status);
      faulted = (sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT);
      how = (sig == SIGABRT) ? "sanitizer-abort" : "signal";
   }
   must(faulted, "write to the read-only mapping faulted (child did not exit 0)");

   bus_region_unmap(&ro);
   bus_region_unmap(&host);
   close(create.fd);
   printf("  control read-only: a client write faults (%s)\n", how);
}

static void test_control_corruption_refused(void)
{
   bus_region_t create;
   must_result(bus_region_create("ctl-bad", bus_control_bytes(), &create), BUS_REGION_OK,
               "create");
   bus_region_t host;
   must_result(bus_region_map(create.fd, create.size, 1, &host), BUS_REGION_OK, "map");
   must_result(bus_control_init(&host, 256, 192, 16, 4096), BUS_REGION_OK, "init");
   bus_control_t *c = (bus_control_t *)host.base;
   bus_control_t *dummy;

   uint32_t u32;
   uint64_t u64;

   u32 = atomic_load_explicit(&c->magic, memory_order_relaxed);
   atomic_store_explicit(&c->magic, 0, memory_order_relaxed);
   must_result(bus_control_attach(&host, &dummy), BUS_REGION_ERR_MAGIC, "bad magic refused");
   atomic_store_explicit(&c->magic, u32, memory_order_relaxed);

   u32 = atomic_load_explicit(&c->layout_version, memory_order_relaxed);
   atomic_store_explicit(&c->layout_version, u32 + 1, memory_order_relaxed);
   must_result(bus_control_attach(&host, &dummy), BUS_REGION_ERR_VERSION, "bad version refused");
   atomic_store_explicit(&c->layout_version, u32, memory_order_relaxed);

   u32 = atomic_load_explicit(&c->slot_size, memory_order_relaxed);
   atomic_store_explicit(&c->slot_size, 0, memory_order_relaxed);
   must_result(bus_control_attach(&host, &dummy), BUS_REGION_ERR_GEOMETRY,
               "zero slot_size refused");
   atomic_store_explicit(&c->slot_size, u32, memory_order_relaxed);

   /* inline_budget larger than the slot is nonsense a client must not size on. */
   atomic_store_explicit(&c->inline_budget, u32 + 1, memory_order_relaxed);
   must_result(bus_control_attach(&host, &dummy), BUS_REGION_ERR_GEOMETRY,
               "inline_budget past slot refused");
   atomic_store_explicit(&c->inline_budget, 192, memory_order_relaxed);

   u64 = atomic_load_explicit(&c->arena_size, memory_order_relaxed);
   atomic_store_explicit(&c->arena_size, 0, memory_order_relaxed);
   must_result(bus_control_attach(&host, &dummy), BUS_REGION_ERR_GEOMETRY,
               "zero arena_size refused");
   atomic_store_explicit(&c->arena_size, u64, memory_order_relaxed);

   u32 = atomic_load_explicit(&c->spec_version, memory_order_relaxed);
   atomic_store_explicit(&c->spec_version, u32 + 1, memory_order_relaxed);
   must_result(bus_control_attach(&host, &dummy), BUS_REGION_ERR_VERSION,
               "bad spec_version refused");
   atomic_store_explicit(&c->spec_version, u32, memory_order_relaxed);

   /* queue_capacity sizes the queue-pair rings; a non-power-of-two must be
    * refused here rather than sizing a ring on it later. */
   u32 = atomic_load_explicit(&c->queue_capacity, memory_order_relaxed);
   atomic_store_explicit(&c->queue_capacity, 15, memory_order_relaxed);
   must_result(bus_control_attach(&host, &dummy), BUS_REGION_ERR_GEOMETRY,
               "bad queue_capacity refused");
   atomic_store_explicit(&c->queue_capacity, 0, memory_order_relaxed);
   must_result(bus_control_attach(&host, &dummy), BUS_REGION_ERR_GEOMETRY,
               "zero queue_capacity refused");
   atomic_store_explicit(&c->queue_capacity, u32, memory_order_relaxed);

   /* And a valid header still attaches after all that poking. */
   must_result(bus_control_attach(&host, &dummy), BUS_REGION_OK, "restored header attaches");

   bus_region_unmap(&host);
   close(create.fd);
   printf("  control corruption: magic, version, and geometry all refused\n");
}

/* A short backing object must be refused before it can fault, not merely a
 * short size claim. Two real cases:
 *   1. Mapping MORE than the memfd holds — mmap would succeed and later touches
 *      would SIGBUS. bus_region_map must refuse it up front.
 *   2. A memfd large enough to map but too small for the claimed layout — attach
 *      must refuse rather than walk past the backing. */
static void test_short_backing(void)
{
   /* Case 1: a genuinely short memfd, asked to map beyond its length. */
   bus_region_t small;
   must_result(bus_region_create("short-fd", 64, &small), BUS_REGION_OK, "create small");
   /* small.size is one page (rounded). Ask to map two pages of a one-page fd. */
   bus_region_t over;
   must_result(bus_region_map(small.fd, small.size + small.size, 0, &over), BUS_REGION_ERR_SIZE,
               "map beyond the backing fd is refused before it can SIGBUS");
   close(small.fd);

   /* Case 2: a one-page region is fine to map, but a queue-pair layout that
    * needs more than a page must be refused at init/attach, not walked past. */
   bus_region_t page;
   must_result(bus_region_create("one-page", 64, &page), BUS_REGION_OK, "create page");
   bus_region_t mapped;
   must_result(bus_region_map(page.fd, page.size, 1, &mapped), BUS_REGION_OK, "map one page");
   /* 4096-byte slots x 64 needs far more than a page. */
   must_result(bus_qpair_init(&mapped, 4096, 64), BUS_REGION_ERR_SIZE,
               "qpair layout larger than the mapping is refused");
   bus_region_unmap(&mapped);
   close(page.fd);
   printf("  short backing: over-length map and over-large layout both refused\n");
}

/* ------------------------------------------------------------------ */
/* queue-pair region                                                   */

static void test_qpair_roundtrip(void)
{
   const uint32_t slot = 128, cap = 16;
   size_t need = bus_qpair_bytes(slot, cap);
   must(need > 0, "qpair geometry valid");

   bus_region_t create;
   must_result(bus_region_create("qp", need, &create), BUS_REGION_OK, "create");
   bus_region_t host;
   must_result(bus_region_map(create.fd, create.size, 1, &host), BUS_REGION_OK, "map");
   must_result(bus_qpair_init(&host, slot, cap), BUS_REGION_OK, "qpair init");

   /* Attach as the host would, and drive both rings — inbound host->client,
    * outbound client->host — through the mapped region. */
   bus_qpair_t qp;
   must_result(bus_qpair_attach(&host, &qp), BUS_REGION_OK, "qpair attach");
   must(bus_ring_capacity(&qp.inbound) == cap, "inbound capacity");
   must(bus_ring_capacity(&qp.outbound) == cap, "outbound capacity");
   must(atomic_load_explicit(&qp.hdr->control_credits, memory_order_relaxed) ==
           BUS_CONTROL_CREDITS_DEFAULT,
        "control-class reserve initialised");

   /* Both rings, driven independently with distinct payloads, so a defect in
    * the inbound ring's placement or init cannot hide behind the outbound one.
    * inbound is host->client (0x2000+), outbound is client->host (0x1000+). */
   bus_ring_t *rings[2] = {&qp.inbound, &qp.outbound};
   uint32_t bases[2] = {0x2000, 0x1000};
   for (int r = 0; r < 2; r++)
   {
      for (uint32_t i = 0; i < cap; i++)
      {
         void *s = bus_ring_produce_begin(rings[r]);
         must(s != NULL, "produce into ring");
         *(uint32_t *)s = bases[r] + i;
         bus_ring_produce_commit(rings[r]);
      }
      must(bus_ring_produce_begin(rings[r]) == NULL, "ring full at capacity");
      for (uint32_t i = 0; i < cap; i++)
      {
         const void *s = bus_ring_consume_begin(rings[r]);
         must(s != NULL, "consume ring");
         must(*(const uint32_t *)s == bases[r] + i, "ring FIFO through the region");
         bus_ring_consume_commit(rings[r]);
      }
      must(bus_ring_count(rings[r]) == 0, "ring drained");
   }

   /* Corruption of the qpair header surface: magic, geometry, and offsets. */
   bus_qpair_t dummy;
   struct
   {
      const char *what;
      _Atomic uint32_t *field;
      uint32_t bad;
      bus_region_result_t want;
   } lies[] = {
      {"bad magic", &qp.hdr->magic, 0, BUS_REGION_ERR_MAGIC},
      {"bad slot_size", &qp.hdr->slot_size, 0, BUS_REGION_ERR_GEOMETRY},
      {"bad capacity", &qp.hdr->capacity, 15, BUS_REGION_ERR_GEOMETRY},
      {"moved inbound offset", &qp.hdr->inbound_off, 8, BUS_REGION_ERR_GEOMETRY},
      {"moved outbound offset",
       &qp.hdr->outbound_off,
       atomic_load_explicit(&qp.hdr->outbound_off, memory_order_relaxed) + 64,
       BUS_REGION_ERR_GEOMETRY},
   };
   for (size_t i = 0; i < sizeof lies / sizeof lies[0]; i++)
   {
      uint32_t saved = atomic_load_explicit(lies[i].field, memory_order_relaxed);
      atomic_store_explicit(lies[i].field, lies[i].bad, memory_order_relaxed);
      must_result(bus_qpair_attach(&host, &dummy), lies[i].want, lies[i].what);
      atomic_store_explicit(lies[i].field, saved, memory_order_relaxed);
   }
   must_result(bus_qpair_attach(&host, &dummy), BUS_REGION_OK, "restored qpair attaches");

   bus_region_unmap(&host);
   close(create.fd);
   printf("  queue-pair: both rings round-trip independently, header lies refused\n");
}

/* ------------------------------------------------------------------ */
/* arena region                                                        */

/* Corrupt the arena magic in place and confirm attach refuses it. Separated
 * from the body below only for readability. */
static void arena_corruption_refused(bus_region_t *host)
{
   uint8_t *base;
   uint64_t size;
   uint32_t *magic = (uint32_t *)host->base; /* first 4 bytes are the arena magic */
   uint32_t saved = *magic;
   *magic = 0;
   must_result(bus_arena_region_attach(host, &base, &size), BUS_REGION_ERR_MAGIC,
               "bad arena magic refused");
   *magic = saved;

   /* A size the mapping cannot back must be refused, not walked past. The size
    * field is the u64 at offset 8 of the arena header. */
   uint64_t *szp = (uint64_t *)((uint8_t *)host->base + 8);
   uint64_t saved_sz = *szp;
   *szp = host->size + (1u << 20); /* claim far more than is mapped */
   must_result(bus_arena_region_attach(host, &base, &size), BUS_REGION_ERR_SIZE,
               "arena size larger than the mapping refused");
   *szp = 0;
   must_result(bus_arena_region_attach(host, &base, &size), BUS_REGION_ERR_GEOMETRY,
               "zero arena size refused");
   *szp = saved_sz;
   must_result(bus_arena_region_attach(host, &base, &size), BUS_REGION_OK,
               "restored arena attaches");
}

static void test_arena_region(void)
{
   const uint64_t arena = 256 * 1024;
   size_t need = bus_arena_region_bytes(arena);
   must(need > 0, "arena geometry valid");

   bus_region_t create;
   must_result(bus_region_create("arena", need, &create), BUS_REGION_OK, "create");
   bus_region_t host;
   must_result(bus_region_map(create.fd, create.size, 1, &host), BUS_REGION_OK, "map");
   must_result(bus_arena_region_init(&host, arena), BUS_REGION_OK, "arena init");

   uint8_t *base = NULL;
   uint64_t size = 0;
   must_result(bus_arena_region_attach(&host, &base, &size), BUS_REGION_OK, "arena attach");
   must(size == arena, "arena size read back");
   must(base > (uint8_t *)host.base, "arena base is past the header");
   /* The usable arena is inside the mapping. */
   must(base + size <= (uint8_t *)host.base + host.size, "arena fits the mapping");

   arena_corruption_refused(&host);

   bus_region_unmap(&host);
   close(create.fd);
   printf("  arena: base and size validated, header corruption refused\n");
}

int main(void)
{
   printf("test_bus_region:\n");
   test_control_roundtrip();
   test_control_readonly_faults();
   test_control_corruption_refused();
   test_short_backing();
   test_qpair_roundtrip();
   test_arena_region();
   printf("test_bus_region: OK\n");
   return 0;
}
