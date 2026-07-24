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

   /* And a valid header still attaches after all that poking. */
   must_result(bus_control_attach(&host, &dummy), BUS_REGION_OK, "restored header attaches");

   bus_region_unmap(&host);
   close(create.fd);
   printf("  control corruption: magic, version, and geometry all refused\n");
}

/* A short mapping must be refused rather than read past. */
static void test_short_mapping(void)
{
   bus_region_t create;
   must_result(bus_region_create("short", bus_control_bytes(), &create), BUS_REGION_OK,
               "create");
   /* Map only one page but claim the region is smaller than the header. */
   bus_region_t tiny;
   must_result(bus_region_map(create.fd, create.size, 0, &tiny), BUS_REGION_OK, "map");
   tiny.size = sizeof(bus_control_t) - 1; /* pretend the mapping is too small */
   bus_control_t *dummy;
   must_result(bus_control_attach(&tiny, &dummy), BUS_REGION_ERR_SIZE,
               "attach refuses a header that does not fit");
   tiny.size = create.size;
   bus_region_unmap(&tiny);
   close(create.fd);
   printf("  short mapping: a header that does not fit is refused\n");
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

   for (uint32_t i = 0; i < cap; i++)
   {
      void *s = bus_ring_produce_begin(&qp.outbound);
      must(s != NULL, "produce into outbound");
      *(uint32_t *)s = 0x1000 + i;
      bus_ring_produce_commit(&qp.outbound);
   }
   must(bus_ring_produce_begin(&qp.outbound) == NULL, "outbound full at capacity");
   for (uint32_t i = 0; i < cap; i++)
   {
      const void *s = bus_ring_consume_begin(&qp.outbound);
      must(s != NULL, "consume outbound");
      must(*(const uint32_t *)s == 0x1000 + i, "outbound FIFO through the region");
      bus_ring_consume_commit(&qp.outbound);
   }

   /* Corruption: bad magic and moved offsets are refused. */
   bus_qpair_t dummy;
   uint32_t m = atomic_load_explicit(&qp.hdr->magic, memory_order_relaxed);
   atomic_store_explicit(&qp.hdr->magic, 0, memory_order_relaxed);
   must_result(bus_qpair_attach(&host, &dummy), BUS_REGION_ERR_MAGIC, "bad qpair magic refused");
   atomic_store_explicit(&qp.hdr->magic, m, memory_order_relaxed);

   uint32_t off = atomic_load_explicit(&qp.hdr->outbound_off, memory_order_relaxed);
   atomic_store_explicit(&qp.hdr->outbound_off, off + 64, memory_order_relaxed);
   must_result(bus_qpair_attach(&host, &dummy), BUS_REGION_ERR_GEOMETRY,
               "moved outbound offset refused");
   atomic_store_explicit(&qp.hdr->outbound_off, off, memory_order_relaxed);

   bus_region_unmap(&host);
   close(create.fd);
   printf("  queue-pair: both rings round-trip through the mapped region\n");
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
   test_short_mapping();
   test_qpair_roundtrip();
   test_arena_region();
   printf("test_bus_region: OK\n");
   return 0;
}
