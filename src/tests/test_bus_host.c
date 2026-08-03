/* test_bus_host.c: slice 5 of the event-bus feature tree.
 *
 * The host is the admission boundary, so the tests check the boundary as an
 * outcome:
 *
 *   - An admitted client receives exactly three descriptors — control (which it
 *     maps read-only), arena, and its own queue pair — and can exchange traffic
 *     with the host through those rings.
 *   - Two admitted clients are isolated: each sees only its own inbound and its
 *     own outbound. A client holds no descriptor for another's queue pair and
 *     the directory that maps slot to identity is host-private, so it cannot
 *     reach or enumerate its peer.
 *   - A denied attach (policy or version) receives a typed reason and NO
 *     descriptors — an unadmitted process gets nothing to map.
 *   - A client whose heartbeat stalls is reaped: its slot is freed and its arena
 *     leases released; a client that keeps beating is left alone.
 *   - A host restart (epoch bump) is visible to a client through the control
 *     region.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <aimee/core/event_bus/bus_host.h>

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      abort();
   }
}

/* A client's view after a successful attach. */
typedef struct
{
   bus_attach_reply_t reply;
   bus_region_t control;
   bus_region_t arena;
   bus_region_t qpair;
   bus_control_t *ctl;
   bus_qpair_t qp;
} client_t;

/* Drive one attach over a connected socket: the client sends its request, the
 * host serves it (must be pumped in between), the client receives the reply and
 * any descriptors and maps them as a client would — control read-only. */
static bus_attach_status_t client_send_request(int sock, uint16_t vmin, uint16_t vmax,
                                               uint32_t principal)
{
   bus_attach_request_t req;
   memset(&req, 0, sizeof req);
   req.magic = BUS_ATTACH_REQ_MAGIC;
   req.wire_version_min = vmin;
   req.wire_version_max = vmax;
   req.principal_class = 1;
   req.principal_ref = principal;
   must(bus_fd_send(sock, &req, sizeof req, NULL, 0) == 0, "client sent request");
   return BUS_ATTACH_OK;
}

static bus_attach_status_t client_recv(int sock, client_t *c)
{
   int fds[3];
   int nfd = 0;
   memset(c, 0, sizeof *c);
   long n = bus_fd_recv(sock, &c->reply, sizeof c->reply, fds, 3, &nfd);
   must(n == (long)sizeof c->reply, "client received a full reply");
   must(c->reply.magic == BUS_ATTACH_REPLY_MAGIC, "reply magic");

   if (c->reply.status != BUS_ATTACH_OK)
   {
      must(nfd == 0, "a denied attach carries no descriptors");
      return (bus_attach_status_t)c->reply.status;
   }

   must(nfd == 3, "an admitted client receives exactly three descriptors");
   /* fds[0] control (read-only), fds[1] arena (rw), fds[2] own queue pair (rw). */

   /* The control descriptor must be genuinely read-only: mapping it writable has
    * to FAIL, or the "control is read-only to clients" invariant is only the
    * client library's good manners. */
   bus_region_t rw_attempt;
   must(bus_region_map(fds[0], bus_control_bytes(), 1, &rw_attempt) != BUS_REGION_OK,
        "control descriptor refuses a writable mapping");

   must(bus_region_map(fds[0], bus_control_bytes(), 0, &c->control) == BUS_REGION_OK,
        "map control read-only");
   size_t arena_bytes = bus_arena_region_bytes(c->reply.arena_size);
   must(bus_region_map(fds[1], arena_bytes, 1, &c->arena) == BUS_REGION_OK, "map arena rw");
   size_t qbytes = bus_qpair_bytes(c->reply.slot_size, c->reply.queue_capacity);
   must(bus_region_map(fds[2], qbytes, 1, &c->qpair) == BUS_REGION_OK, "map own queue pair rw");

   must(bus_control_attach(&c->control, &c->ctl) == BUS_REGION_OK, "attach control");
   must(bus_qpair_attach(&c->qpair, &c->qp) == BUS_REGION_OK, "attach queue pair");

   /* The mappings hold their own references; the descriptors can be closed now,
    * so a run of attaches does not leak fds. */
   for (int i = 0; i < 3; i++)
      close(fds[i]);
   return BUS_ATTACH_OK;
}

static void client_unmap(client_t *c)
{
   bus_region_unmap(&c->control);
   bus_region_unmap(&c->arena);
   bus_region_unmap(&c->qpair);
}

/* Deny a specific principal, to exercise the admission seam. */
static bus_attach_status_t admit_deny_666(void *ctx, int attach_fd, const bus_attach_request_t *req)
{
   (void)ctx;
   if (attach_fd < 0)
      return BUS_ATTACH_PROTOCOL;
   return req->principal_ref == 666 ? BUS_ATTACH_DENIED_POLICY : BUS_ATTACH_OK;
}

/* Attach a client over a fresh socketpair; returns the admit status. On OK the
 * client view is filled. The host end stays owned by the host for the slot's
 * lifetime is not needed here — the handshake is one shot. */
static bus_attach_status_t attach(bus_host_t *h, uint16_t vmin, uint16_t vmax, uint32_t principal,
                                  client_t *out)
{
   int sv[2];
   must(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair");
   client_send_request(sv[0], vmin, vmax, principal);
   bus_host_result_t r = bus_host_serve_attach(h, sv[1]);
   bus_attach_status_t st = client_recv(sv[0], out);
   if (st == BUS_ATTACH_OK)
      must(r == BUS_HOST_OK, "host reports admit on OK");
   else
      must(r == BUS_HOST_ERR_REFUSED, "host reports refusal on deny");
   close(sv[0]);
   close(sv[1]);
   return st;
}

static bus_host_config_t default_cfg(void)
{
   bus_host_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.max_slots = 4;
   cfg.slot_size = 256;
   cfg.inline_budget = 192;
   cfg.queue_capacity = 16;
   cfg.arena_size = 256 * 1024;
   return cfg;
}

/* ------------------------------------------------------------------ */

static void test_admit_and_traffic(void)
{
   bus_host_config_t cfg = default_cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cfg, admit_deny_666, NULL) == BUS_HOST_OK, "host create");

   client_t c;
   must(attach(&h, BUS_WIRE_VERSION, BUS_WIRE_VERSION, 1, &c) == BUS_ATTACH_OK, "client admitted");
   must(bus_host_admitted(&h) == 1, "one admitted");
   must(c.reply.slot_size == cfg.slot_size, "reply carries slot_size");
   must(c.reply.host_epoch == bus_control_epoch(h.control), "reply carries the epoch");

   /* Host -> client: the host writes into the client's inbound; the client reads
    * it through its own mapping. */
   bus_slot_t *slot = &h.slots[c.reply.handle_id];
   void *w = bus_ring_produce_begin(&slot->qpair.inbound);
   must(w != NULL, "host produces into the client's inbound");
   *(uint32_t *)w = 0xA1B2C3D4;
   bus_ring_produce_commit(&slot->qpair.inbound);

   const void *rd = bus_ring_consume_begin(&c.qp.inbound);
   must(rd != NULL, "client sees its inbound");
   must(*(const uint32_t *)rd == 0xA1B2C3D4, "client reads the host's bytes");
   bus_ring_consume_commit(&c.qp.inbound);

   /* Client -> host through the outbound ring. */
   void *cw = bus_ring_produce_begin(&c.qp.outbound);
   must(cw != NULL, "client produces into its outbound");
   *(uint32_t *)cw = 0x0BADF00D;
   bus_ring_produce_commit(&c.qp.outbound);

   const void *hr = bus_ring_consume_begin(&slot->qpair.outbound);
   must(hr != NULL, "host sees the client's outbound");
   must(*(const uint32_t *)hr == 0x0BADF00D, "host reads the client's bytes");
   bus_ring_consume_commit(&slot->qpair.outbound);

   client_unmap(&c);
   bus_host_destroy(&h);
   printf("  admit + traffic: three fds granted, both rings carry traffic\n");
}

static void test_two_clients_isolated(void)
{
   bus_host_config_t cfg = default_cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cfg, NULL, NULL) == BUS_HOST_OK, "host create");

   client_t a, b;
   must(attach(&h, BUS_WIRE_VERSION, BUS_WIRE_VERSION, 10, &a) == BUS_ATTACH_OK, "A admitted");
   must(attach(&h, BUS_WIRE_VERSION, BUS_WIRE_VERSION, 20, &b) == BUS_ATTACH_OK, "B admitted");
   must(a.reply.handle_id != b.reply.handle_id, "distinct handles");
   must(bus_host_admitted(&h) == 2, "two admitted");

   /* The host addresses each slot independently: a byte written to A's inbound
    * must reach A and not B. */
   bus_slot_t *sa = &h.slots[a.reply.handle_id];
   bus_slot_t *sb = &h.slots[b.reply.handle_id];
   void *wa = bus_ring_produce_begin(&sa->qpair.inbound);
   *(uint32_t *)wa = 0xAAAAAAAA;
   bus_ring_produce_commit(&sa->qpair.inbound);

   /* A sees its message; B's inbound is empty — A and B do not share a ring. */
   const void *ra = bus_ring_consume_begin(&a.qp.inbound);
   must(ra && *(const uint32_t *)ra == 0xAAAAAAAA, "A reads its own inbound");
   bus_ring_consume_commit(&a.qp.inbound);
   must(bus_ring_consume_begin(&b.qp.inbound) == NULL, "B's inbound is untouched");

   /* Symmetrically for outbound: B produces, only B's slot shows it. */
   void *wb = bus_ring_produce_begin(&b.qp.outbound);
   *(uint32_t *)wb = 0xBBBBBBBB;
   bus_ring_produce_commit(&b.qp.outbound);
   must(bus_ring_consume_begin(&sa->qpair.outbound) == NULL, "A's slot shows no B traffic");
   const void *rb = bus_ring_consume_begin(&sb->qpair.outbound);
   must(rb && *(const uint32_t *)rb == 0xBBBBBBBB, "B's slot shows B traffic");
   bus_ring_consume_commit(&sb->qpair.outbound);

   client_unmap(&a);
   client_unmap(&b);
   bus_host_destroy(&h);
   printf("  isolation: two clients, each sees only its own rings\n");
}

static void test_denials(void)
{
   bus_host_config_t cfg = default_cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cfg, admit_deny_666, NULL) == BUS_HOST_OK, "host create");

   client_t c;
   must(attach(&h, BUS_WIRE_VERSION, BUS_WIRE_VERSION, 666, &c) == BUS_ATTACH_DENIED_POLICY,
        "policy denial");
   must(bus_host_admitted(&h) == 0, "denied client not admitted");

   /* A version with no overlap is denied, with no descriptors. */
   must(attach(&h, 7, 9, 1, &c) == BUS_ATTACH_DENIED_VERSION, "version denial");
   must(bus_host_admitted(&h) == 0, "still none admitted");

   bus_host_destroy(&h);
   printf("  denials: policy and version refused with a typed reason and no fds\n");
}

static void test_full_host(void)
{
   bus_host_config_t cfg = default_cfg();
   cfg.max_slots = 2;
   bus_host_t h;
   must(bus_host_create(&h, &cfg, NULL, NULL) == BUS_HOST_OK, "host create");

   client_t c[3];
   must(attach(&h, BUS_WIRE_VERSION, BUS_WIRE_VERSION, 1, &c[0]) == BUS_ATTACH_OK,
        "first admitted");
   must(attach(&h, BUS_WIRE_VERSION, BUS_WIRE_VERSION, 2, &c[1]) == BUS_ATTACH_OK,
        "second admitted");
   must(attach(&h, BUS_WIRE_VERSION, BUS_WIRE_VERSION, 3, &c[2]) == BUS_ATTACH_DENIED_NOSLOT,
        "third refused, host full");

   client_unmap(&c[0]);
   client_unmap(&c[1]);
   bus_host_destroy(&h);
   printf("  full host: admission refuses past capacity\n");
}

static void test_reaping(void)
{
   bus_host_config_t cfg = default_cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cfg, NULL, NULL) == BUS_HOST_OK, "host create");

   client_t a, b;
   must(attach(&h, BUS_WIRE_VERSION, BUS_WIRE_VERSION, 1, &a) == BUS_ATTACH_OK, "A admitted");
   must(attach(&h, BUS_WIRE_VERSION, BUS_WIRE_VERSION, 2, &b) == BUS_ATTACH_OK, "B admitted");

   /* Both clients beat once at t=100 so the host has a baseline. */
   atomic_store_explicit(&a.qp.hdr->client_heartbeat, 1, memory_order_release);
   atomic_store_explicit(&b.qp.hdr->client_heartbeat, 1, memory_order_release);
   must(bus_host_reap(&h, 100, 50) == 0, "baseline: nobody stale yet");

   /* B keeps beating; A goes quiet. Advance the clock past the stale window. */
   atomic_store_explicit(&b.qp.hdr->client_heartbeat, 2, memory_order_release);
   must(bus_host_reap(&h, 130, 50) == 0, "still within the window");

   /* A's arena footprint should be released on reap: give A a lease first. */
   uint32_t lease;
   must(bus_arena_alloc(&h.arena, a.reply.handle_id, 64, &lease) == BUS_ARENA_OK,
        "A holds a lease");
   must(bus_arena_live_leases(&h.arena, a.reply.handle_id) == 1, "A has a live lease");

   /* Now past the window with no advance from A: A is reaped, B is not. */
   atomic_store_explicit(&b.qp.hdr->client_heartbeat, 3, memory_order_release);
   uint32_t reaped = bus_host_reap(&h, 200, 50);
   must(reaped == 1, "exactly one reaped");
   must(bus_host_admitted(&h) == 1, "one remains");
   must(!h.slots[a.reply.handle_id].in_use, "A's slot freed");
   must(h.slots[b.reply.handle_id].in_use, "B's slot kept");
   must(bus_arena_live_leases(&h.arena, a.reply.handle_id) == 0,
        "A's arena leases released on reap");

   client_unmap(&a);
   client_unmap(&b);
   bus_host_destroy(&h);
   printf("  reaping: a stalled client is reaped and its leases released\n");
}

static void test_epoch(void)
{
   bus_host_config_t cfg = default_cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cfg, NULL, NULL) == BUS_HOST_OK, "host create");
   client_t c;
   must(attach(&h, BUS_WIRE_VERSION, BUS_WIRE_VERSION, 1, &c) == BUS_ATTACH_OK, "admitted");

   uint64_t at = bus_control_epoch(c.ctl);
   must(!bus_control_epoch_changed(c.ctl, at), "no change yet");
   bus_host_bump_epoch(&h);
   must(bus_control_epoch_changed(c.ctl, at), "client sees the restart via the control region");

   client_unmap(&c);
   bus_host_destroy(&h);
   printf("  epoch: a host restart is visible through the control region\n");
}

typedef struct
{
   uint32_t kind;
   uint32_t slot;
   int calls;
} hook_state_t;

static bus_attach_status_t bind_capabilities(void *ctx, bus_host_t *host, uint32_t slot,
                                             const bus_attach_request_t *request)
{
   hook_state_t *state = ctx;
   state->calls++;
   state->slot = slot;
   if (request->principal_ref == 777)
      return BUS_ATTACH_DENIED_POLICY;
   return bus_host_subscribe(host, slot, state->kind) == BUS_HOST_OK ? BUS_ATTACH_OK
                                                                     : BUS_ATTACH_DENIED_POLICY;
}

static void test_capability_hook(void)
{
   bus_host_config_t cfg = default_cfg();
   bus_host_t h;
   hook_state_t state = {.kind = 9090, .slot = UINT32_MAX, .calls = 0};
   must(bus_host_create(&h, &cfg, NULL, NULL) == BUS_HOST_OK, "host create");
   bus_host_set_attach_hook(&h, bind_capabilities, &state);
   client_t admitted, denied;
   must(attach(&h, BUS_WIRE_VERSION, BUS_WIRE_VERSION, 42, &admitted) == BUS_ATTACH_OK,
        "hook admits and binds");
   must(state.calls == 1 && state.slot == admitted.reply.handle_id, "hook sees assigned slot");
   must(attach(&h, BUS_WIRE_VERSION, BUS_WIRE_VERSION, 777, &denied) == BUS_ATTACH_DENIED_POLICY,
        "hook denial grants no descriptors");
   must(state.calls == 2 && bus_host_admitted(&h) == 1, "denied hook slot rolled back");
   client_unmap(&admitted);
   bus_host_destroy(&h);
   printf("  capability hook: grants bind before descriptors; denial rolls back\n");
}

int main(void)
{
   printf("test_bus_host:\n");
   test_admit_and_traffic();
   test_two_clients_isolated();
   test_denials();
   test_full_host();
   test_reaping();
   test_epoch();
   test_capability_hook();
   printf("test_bus_host: OK\n");
   return 0;
}
