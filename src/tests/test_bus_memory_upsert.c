/* test_bus_memory_upsert.c: a real MUTATING module operation over the bus.
 *
 * The recall slice proved a read crosses the bus and agrees with a direct call.
 * A read is the easy case: it changes nothing, so "the reply came back" and "the
 * work happened" are the same event. A write is the honest test. This puts the
 * real db1_user_memory_upsert on the bus as a request/reply, and then proves the
 * write actually LANDED by reading the real DB1 store back with a direct call —
 * the reply alone is not accepted as evidence the state changed.
 *
 * It covers both an insert (a new key appears) and an update (an existing key's
 * content changes), each driven over the bus and each confirmed against the real
 * store afterwards. Same real memory write path, reached across the bus, required
 * to have really mutated the store.
 *
 * A test/integration harness. The bus is linked into no shipping binary by it (D7);
 * the migration that puts memory on the bus for real is a separate tree.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_ring.h>
#include <aimee/core/event_bus/bus_wire.h>
#include "db1/db1.h"
#include "db1/user_memory.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      abort();
   }
}

#define KIND_MEM_UPSERT 2001
#define MAX_ROWS        8

/* ---- wire form of an upsert request: the fields db1_user_memory_upsert takes ---- */

static uint32_t serialize_upsert(const char *kind, const char *tier, const char *key,
                                 const char *content, double confidence, const char *source,
                                 uint8_t *buf, uint32_t cap)
{
   uint32_t off = 0;
#define PUT(p, len)                                                                                \
   do                                                                                              \
   {                                                                                               \
      if (off + (len) > cap)                                                                       \
         return 0;                                                                                 \
      memcpy(buf + off, (p), (len));                                                               \
      off += (len);                                                                                \
   } while (0)
#define PUTS(s)                                                                                    \
   do                                                                                              \
   {                                                                                               \
      uint32_t l = (uint32_t)strlen(s);                                                            \
      PUT(&l, 4);                                                                                  \
      PUT((s), l);                                                                                 \
   } while (0)
   PUTS(kind);
   PUTS(tier);
   PUTS(key);
   PUTS(content);
   PUT(&confidence, 8);
   PUTS(source);
#undef PUTS
#undef PUT
   return off;
}

/* Read a length-prefixed string into out (NUL-terminated, bounded). */
static int get_str(const uint8_t *buf, uint32_t len, uint32_t *off, char *out, uint32_t outcap)
{
   uint32_t l;
   if (*off + 4 > len)
      return -1;
   memcpy(&l, buf + *off, 4);
   *off += 4;
   if (*off + l > len || l >= outcap)
      return -1;
   memcpy(out, buf + *off, l);
   out[l] = '\0';
   *off += l;
   return 0;
}

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

/* The memory server: on an upsert request, run the REAL upsert and reply the rc. */
static void memory_server_step(bus_client_t *server)
{
   bus_event_t ev;
   while (bus_client_poll(server, &ev) == BUS_CLIENT_OK)
   {
      if (ev.frame.event_kind != KIND_MEM_UPSERT || !(ev.frame.hdr_flags & BUS_F_REQUEST))
         continue;

      char kind[16] = {0}, tier[4] = {0}, key[512] = {0}, content[2048] = {0}, source[128] = {0};
      double confidence = 0;
      uint32_t off = 0;
      const uint8_t *p = ev.payload;
      uint32_t plen = ev.payload_len;
      int ok = get_str(p, plen, &off, kind, sizeof kind) == 0 &&
               get_str(p, plen, &off, tier, sizeof tier) == 0 &&
               get_str(p, plen, &off, key, sizeof key) == 0 &&
               get_str(p, plen, &off, content, sizeof content) == 0 && off + 8 <= plen;
      int32_t rc = -1;
      if (ok)
      {
         memcpy(&confidence, p + off, 8);
         off += 8;
         if (get_str(p, plen, &off, source, sizeof source) == 0)
            rc = db1_user_memory_upsert(kind, tier, key, content, confidence, source);
      }
      must(bus_client_reply(server, KIND_MEM_UPSERT, ev.frame.correlation_id, &rc, sizeof rc) ==
               BUS_CLIENT_OK,
           "server reply");
   }
}

/* One full upsert over the bus: request -> route -> real upsert -> route -> reply. */
static int32_t bus_upsert(bus_host_t *h, bus_client_t *req, bus_client_t *server, uint64_t corr,
                          const char *kind, const char *tier, const char *key, const char *content,
                          double confidence, const char *source)
{
   uint8_t buf[4096];
   uint32_t len = serialize_upsert(kind, tier, key, content, confidence, source, buf, sizeof buf);
   must(len > 0, "upsert request fits");
   must(bus_client_request(req, KIND_MEM_UPSERT, corr, buf, len) == BUS_CLIENT_OK, "request");
   bus_host_pump(h);           /* route the request to the server */
   memory_server_step(server); /* server runs the real upsert and replies */
   bus_host_pump(h);           /* route the reply back to the requester */

   bus_event_t ev;
   must(bus_client_poll(req, &ev) == BUS_CLIENT_OK, "reply arrived");
   must(ev.frame.event_kind == KIND_MEM_UPSERT && ev.frame.correlation_id == corr,
        "reply is the upsert");
   int32_t rc = -1;
   must(ev.payload_len == sizeof rc, "reply carries an rc");
   memcpy(&rc, ev.payload, sizeof rc);
   return rc;
}

/* Direct read-back of a single key's content from the real store; NULL if absent. */
static int find_content(const char *key, char *out, int outcap)
{
   db1_user_memory_row_t rows[MAX_ROWS];
   int n = db1_user_memory_list_recall(DB1_USER_RECALL_IDENTITY, rows, MAX_ROWS);
   for (int i = 0; i < n; i++)
      if (strcmp(rows[i].key, key) == 0)
      {
         snprintf(out, (size_t)outcap, "%s", rows[i].content);
         return 1;
      }
   return 0;
}

int main(void)
{
   printf("test_bus_memory_upsert:\n");

   char home[256];
   snprintf(home, sizeof home, "%s/aimee-busups-XXXXXX", platform_tmpdir());
   must(mkdtemp(home) != NULL, "tmp home");
   setenv("AIMEE_HOME", home, 1);
   must(db1_init(":memory:") == 0, "db1 init");

   bus_host_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.max_slots = 4;
   cfg.slot_size = 8192; /* large slots so a real content field fits inline */
   cfg.inline_budget = 8000;
   cfg.queue_capacity = 16;
   cfg.arena_size = 256 * 1024;

   bus_host_t h;
   must(bus_host_create(&h, &cfg, NULL, NULL) == BUS_HOST_OK, "host");
   bus_client_t req, server;
   attach_client(&h, &req);
   attach_client(&h, &server);
   must(bus_host_serve_kind(&h, server.reply.handle_id, KIND_MEM_UPSERT) == BUS_HOST_OK,
        "server serves upsert");

   /* Insert: the key does not exist, then an upsert over the bus creates it, and a
    * direct read of the real store must show it — the write really landed. */
   char got[2048];
   must(find_content("name:full", got, sizeof got) == 0, "key absent before the bus write");
   int32_t rc = bus_upsert(&h, &req, &server, 1, "fact", "L2", "name:full",
                           "The user is called Jordan.", 0.9, "sess-1");
   must(rc == 0, "bus upsert (insert) succeeded");
   must(find_content("name:full", got, sizeof got) == 1, "key present after the bus write");
   must(strcmp(got, "The user is called Jordan.") == 0, "stored content is what the bus carried");
   printf("  insert over the bus: key 'name:full' now in the real store, content matches\n");

   /* Update: the same key, new content, driven over the bus; the real store must
    * reflect the change (ON CONFLICT DO UPDATE ran on the far side of the bus). */
   rc = bus_upsert(&h, &req, &server, 2, "fact", "L3", "name:full",
                   "The user is called Jordan Bailes.", 0.95, "sess-2");
   must(rc == 0, "bus upsert (update) succeeded");
   must(find_content("name:full", got, sizeof got) == 1, "key still present after update");
   must(strcmp(got, "The user is called Jordan Bailes.") == 0,
        "update over the bus changed the store");
   printf("  update over the bus: same key, content changed in the real store\n");

   bus_client_detach(&req);
   bus_client_detach(&server);
   bus_host_destroy(&h);
   db1_shutdown();
   printf("test_bus_memory_upsert: OK (a real mutating memory op ran over the bus and changed the "
          "store)\n");
   return 0;
}
