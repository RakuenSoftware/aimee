/* test_bus_config_autonomy.c: a SECOND, distinct module over the bus.
 *
 * The memory slices proved the bus carries a real db1 module's read and write.
 * One module proves the wiring; it does not prove the pattern generalises. This
 * puts a different subsystem — config — on the bus: the real config_autonomy_lookup,
 * served as a request/reply, and required to agree with a direct call.
 *
 * config_autonomy_lookup is a good generalisation test because it is not a pure
 * function of its argument: it reads the live config snapshot and honours an
 * operator env override. So the test also flips an env override and requires the
 * bus answer to change with it — proving the real function ran on the far side of
 * the bus against live state, not a value captured when the request was framed.
 *
 * A test/integration harness. The bus is linked into no shipping binary by it (D7).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bus_client.h"
#include "bus_host.h"
#include "bus_ring.h"
#include "bus_wire.h"
#include "config.h"

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      abort();
   }
}

#define KIND_CFG_LOOKUP 2002

/* ---- reply wire form: found flag + the looked-up value ---- */
struct lookup_reply
{
   int32_t found;
   int64_t value;
};

/* ---- attach helper (serve on a thread so the one-shot handshake completes) ---- */

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
   must(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair");
   struct serve_arg a = {.h = h, .fd = sv[1]};
   pthread_t t;
   must(pthread_create(&t, NULL, serve_thread, &a) == 0, "spawn serve");
   must(bus_client_attach(sv[0], c) == BUS_CLIENT_OK, "attach");
   pthread_join(t, NULL);
   close(sv[0]);
   close(sv[1]);
}

/* The config server: on a lookup request (payload = env name), run the REAL
 * config_autonomy_lookup and reply {found, value}. */
static void config_server_step(bus_client_t *server)
{
   bus_event_t ev;
   while (bus_client_poll(server, &ev) == BUS_CLIENT_OK)
   {
      if (ev.frame.event_kind != KIND_CFG_LOOKUP || !(ev.frame.hdr_flags & BUS_F_REQUEST))
         continue;
      char name[128] = {0};
      uint32_t n = ev.payload_len < sizeof name - 1 ? ev.payload_len : sizeof name - 1;
      memcpy(name, ev.payload, n);

      long v = 0;
      struct lookup_reply rep = {0, 0};
      rep.found = config_autonomy_lookup(name, &v);
      rep.value = (int64_t)v;
      must(bus_client_reply(server, KIND_CFG_LOOKUP, ev.frame.correlation_id, &rep, sizeof rep) ==
               BUS_CLIENT_OK,
           "server reply");
   }
}

/* One lookup over the bus: request(name) -> route -> real lookup -> route -> reply. */
static struct lookup_reply bus_lookup(bus_host_t *h, bus_client_t *req, bus_client_t *server,
                                      uint64_t corr, const char *name)
{
   must(bus_client_request(req, KIND_CFG_LOOKUP, corr, name, (uint32_t)strlen(name)) ==
            BUS_CLIENT_OK,
        "request");
   bus_host_pump(h);
   config_server_step(server);
   bus_host_pump(h);

   bus_event_t ev;
   must(bus_client_poll(req, &ev) == BUS_CLIENT_OK, "reply arrived");
   must(ev.frame.event_kind == KIND_CFG_LOOKUP && ev.frame.correlation_id == corr,
        "reply is the lookup");
   struct lookup_reply rep = {0, 0};
   must(ev.payload_len == sizeof rep, "reply is a lookup reply");
   memcpy(&rep, ev.payload, sizeof rep);
   return rep;
}

/* Assert the bus answer equals a fresh direct call, for a given name. */
static void check_agrees(bus_host_t *h, bus_client_t *req, bus_client_t *server, uint64_t corr,
                         const char *name)
{
   long dv = 0;
   int dfound = config_autonomy_lookup(name, &dv);
   struct lookup_reply b = bus_lookup(h, req, server, corr, name);
   must(b.found == dfound, "bus and direct agree on found");
   must(!dfound || b.value == (int64_t)dv, "bus and direct agree on value");
   printf("  %-34s direct{found=%d,val=%ld} == bus{found=%d,val=%lld}\n", name, dfound, dv,
          b.found, (long long)b.value);
}

int main(void)
{
   printf("test_bus_config_autonomy:\n");

   /* Seed the live config snapshot the real lookup reads, with distinctive
    * autonomy values so a match cannot be a coincidence of zeros. */
   config_t c;
   memset(&c, 0, sizeof c);
   c.autonomy_max_turns = 42;
   c.autonomy_skeptics = 3;
   c.autonomy_concurrency = 7;
   config_snapshot_init(&c);
   unsetenv("AIMEE_AUTONOMY_MAX_TURNS"); /* no stray operator override for the first checks */

   bus_host_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.max_slots = 4;
   cfg.slot_size = 256;
   cfg.inline_budget = 192;
   cfg.queue_capacity = 16;
   cfg.arena_size = 256 * 1024;

   bus_host_t h;
   must(bus_host_create(&h, &cfg, NULL, NULL) == BUS_HOST_OK, "host");
   bus_client_t req, server;
   attach_client(&h, &req);
   attach_client(&h, &server);
   must(bus_host_serve_kind(&h, server.reply.handle_id, KIND_CFG_LOOKUP) == BUS_HOST_OK,
        "server serves lookup");

   /* Real config-backed autonomy vars: the bus answer must equal a direct call. */
   check_agrees(&h, &req, &server, 1, "AIMEE_AUTONOMY_MAX_TURNS");
   check_agrees(&h, &req, &server, 2, "AIMEE_AUTONOMY_SKEPTICS");
   check_agrees(&h, &req, &server, 3, "AIMEE_AUTONOMY_CONCURRENCY");
   /* A name the function does not own: both must report not-found (found=0). */
   check_agrees(&h, &req, &server, 4, "AIMEE_USD_PER_SEC");

   /* Live state: flip an operator override and require the BUS answer to move
    * with it. If the far side were echoing a value captured at request-framing
    * time rather than really running config_autonomy_lookup, this would fail. */
   setenv("AIMEE_AUTONOMY_MAX_TURNS", "99", 1);
   struct lookup_reply after = bus_lookup(&h, &req, &server, 5, "AIMEE_AUTONOMY_MAX_TURNS");
   must(after.found == 1 && after.value == 99,
        "bus reflects the live env override (real lookup ran on the far side)");
   printf("  env override AIMEE_AUTONOMY_MAX_TURNS=99 -> bus{found=%d,val=%lld} (was 42)\n",
          after.found, (long long)after.value);
   unsetenv("AIMEE_AUTONOMY_MAX_TURNS");

   bus_client_detach(&req);
   bus_client_detach(&server);
   bus_host_destroy(&h);
   printf("test_bus_config_autonomy: OK (a second, distinct module works over the bus)\n");
   return 0;
}
