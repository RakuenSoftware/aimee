/* test_bus_memory_recall.c: the first real module-on-bus integration.
 *
 * Everything until now proved the bus in isolation with synthetic clients. This
 * is the first slice that puts a REAL module operation on the bus: the DB1-backed
 * user-memory recall (db1_user_memory_list_recall) is served over the bus as a
 * request/reply, and the answer is verified against a direct call to the same
 * function. Same real memory logic, same real SQLite store, reached two ways —
 * directly and across the bus — and required to agree.
 *
 * It also measures the two paths, which is the honest pre-migration baseline the
 * performance budget wants: what does the bus add on top of the module's own
 * work for a real recall.
 *
 * This is a test/integration harness. The bus is not linked into a shipping
 * binary by it (the memory-migration that does that is a separate tree).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bus_client.h"
#include "bus_host.h"
#include "bus_ring.h"
#include "bus_wire.h"
#include "db1/db1.h"
#include "db1/user_memory.h"

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      abort();
   }
}

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

#define KIND_MEM_RECALL 2000
#define MAX_ROWS        8

/* ---- wire form of a recall reply: the real rows, serialized compactly ---- */

static uint32_t serialize_rows(const db1_user_memory_row_t *rows, int n, uint8_t *buf,
                               uint32_t cap)
{
   uint32_t off = 0;
#define PUT(p, len)                                                                               \
   do                                                                                             \
   {                                                                                              \
      if (off + (len) > cap)                                                                      \
         return 0;                                                                                \
      memcpy(buf + off, (p), (len));                                                              \
      off += (len);                                                                               \
   } while (0)
   uint32_t count = (uint32_t)n;
   PUT(&count, 4);
   for (int i = 0; i < n; i++)
   {
      PUT(&rows[i].id, 8);
      uint32_t kl = (uint32_t)strnlen(rows[i].kind, sizeof rows[i].kind);
      uint32_t keyl = (uint32_t)strnlen(rows[i].key, sizeof rows[i].key);
      uint32_t cl = (uint32_t)strnlen(rows[i].content, sizeof rows[i].content);
      PUT(&kl, 4);
      PUT(rows[i].kind, kl);
      PUT(&keyl, 4);
      PUT(rows[i].key, keyl);
      PUT(&cl, 4);
      PUT(rows[i].content, cl);
   }
#undef PUT
   return off;
}

static int deserialize_rows(const uint8_t *buf, uint32_t len, db1_user_memory_row_t *rows,
                            int cap)
{
   uint32_t off = 0;
#define GET(p, l)                                                                                 \
   do                                                                                             \
   {                                                                                              \
      if (off + (l) > len)                                                                        \
         return -1;                                                                               \
      memcpy((p), buf + off, (l));                                                                \
      off += (l);                                                                                 \
   } while (0)
   uint32_t count = 0;
   GET(&count, 4);
   if ((int)count > cap)
      return -1;
   for (uint32_t i = 0; i < count; i++)
   {
      memset(&rows[i], 0, sizeof rows[i]);
      GET(&rows[i].id, 8);
      uint32_t kl, keyl, cl;
      GET(&kl, 4);
      if (kl >= sizeof rows[i].kind)
         return -1;
      GET(rows[i].kind, kl);
      GET(&keyl, 4);
      if (keyl >= sizeof rows[i].key)
         return -1;
      GET(rows[i].key, keyl);
      GET(&cl, 4);
      if (cl >= sizeof rows[i].content)
         return -1;
      GET(rows[i].content, cl);
   }
#undef GET
   return (int)count;
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

/* The memory server: on a recall request, run the REAL recall and reply. */
static void memory_server_step(bus_client_t *server)
{
   bus_event_t ev;
   while (bus_client_poll(server, &ev) == BUS_CLIENT_OK)
   {
      if (ev.frame.event_kind != KIND_MEM_RECALL || !(ev.frame.hdr_flags & BUS_F_REQUEST))
         continue;
      int32_t section = DB1_USER_RECALL_IDENTITY;
      if (ev.payload_len >= 4)
         memcpy(&section, ev.payload, 4);

      db1_user_memory_row_t rows[MAX_ROWS];
      int n = db1_user_memory_list_recall((db1_user_recall_section_t)section, rows, MAX_ROWS);

      uint8_t out[8192];
      uint32_t len = serialize_rows(rows, n, out, sizeof out);
      must(len > 0, "serialize fits");
      must(bus_client_reply(server, KIND_MEM_RECALL, ev.frame.correlation_id, out, len) ==
               BUS_CLIENT_OK,
           "server reply");
   }
}

/* One full recall over the bus: request -> route -> real recall -> route -> reply. */
static int bus_recall(bus_host_t *h, bus_client_t *req, bus_client_t *server, uint64_t corr,
                      db1_user_memory_row_t *rows, int cap)
{
   int32_t section = DB1_USER_RECALL_IDENTITY;
   must(bus_client_request(req, KIND_MEM_RECALL, corr, &section, 4) == BUS_CLIENT_OK, "request");
   bus_host_pump(h);       /* route the request to the server */
   memory_server_step(server); /* server runs the real recall and replies */
   bus_host_pump(h);       /* route the reply back to the requester */

   bus_event_t ev;
   must(bus_client_poll(req, &ev) == BUS_CLIENT_OK, "reply arrived");
   must(ev.frame.event_kind == KIND_MEM_RECALL && ev.frame.correlation_id == corr,
        "reply is the recall");
   return deserialize_rows(ev.payload, ev.payload_len, rows, cap);
}

static void seed_memories(void)
{
   must(db1_init(":memory:") == 0, "db1 init");
   /* Real user-memory writes into the real DB1 store. The IDENTITY recall selects
    * kind='fact', an L2..L5 tier, and keys under identity/name/role/user/self —
    * so the seeds must use those conventions to be recalled. */
   must(db1_user_memory_upsert("fact", "L2", "name:full", "The user is called Jordan.", 0.9,
                               "sess-1") == 0,
        "upsert 1");
   must(db1_user_memory_upsert("fact", "L2", "role:job", "Jordan is a systems engineer.", 0.8,
                               "sess-1") == 0,
        "upsert 2");
   must(db1_user_memory_upsert("fact", "L2", "identity:location", "Jordan works from Perth.", 0.7,
                               "sess-1") == 0,
        "upsert 3");
}

static int rows_equal(const db1_user_memory_row_t *a, const db1_user_memory_row_t *b, int n)
{
   for (int i = 0; i < n; i++)
   {
      if (a[i].id != b[i].id || strcmp(a[i].kind, b[i].kind) != 0 ||
          strcmp(a[i].key, b[i].key) != 0 || strcmp(a[i].content, b[i].content) != 0)
         return 0;
   }
   return 1;
}

int main(void)
{
   printf("test_bus_memory_recall:\n");

   /* AIMEE_HOME so db1 has somewhere for its side files; :memory: keeps the db
    * itself in RAM. */
   char home[] = "/tmp/aimee-busmem-XXXXXX";
   must(mkdtemp(home) != NULL, "tmp home");
   setenv("AIMEE_HOME", home, 1);

   seed_memories();

   /* The direct path: the real recall, straight. This is the baseline. */
   db1_user_memory_row_t direct[MAX_ROWS];
   int dn = db1_user_memory_list_recall(DB1_USER_RECALL_IDENTITY, direct, MAX_ROWS);
   must(dn == 3, "direct recall returns the three seeded rows");
   printf("  direct recall: %d rows (real DB1 store)\n", dn);

   /* The bus path: same recall, reached over the bus. */
   bus_host_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.max_slots = 4;
   cfg.slot_size = 8192; /* a memory hub uses large slots so real rows fit inline */
   cfg.inline_budget = 8000;
   cfg.queue_capacity = 16;
   cfg.arena_size = 256 * 1024;

   bus_host_t h;
   must(bus_host_create(&h, &cfg, NULL, NULL) == BUS_HOST_OK, "host");
   bus_client_t req, server;
   attach_client(&h, &req);
   attach_client(&h, &server);
   must(bus_host_serve_kind(&h, server.reply.handle_id, KIND_MEM_RECALL) == BUS_HOST_OK,
        "server serves recall");

   db1_user_memory_row_t viabus[MAX_ROWS];
   int bn = bus_recall(&h, &req, &server, 1, viabus, MAX_ROWS);
   must(bn == dn, "bus recall returns the same number of rows");
   must(rows_equal(direct, viabus, dn), "bus rows match the direct rows exactly");
   printf("  bus recall:    %d rows, identical to the direct answer\n", bn);
   printf("    e.g. row[0] key=%s content=\"%s\"\n", viabus[0].key, viabus[0].content);

   /* Baseline timing: the module's own work, and the same work over the bus. */
   const int ITERS = 20000;
   uint64_t t0 = now_ns();
   for (int i = 0; i < ITERS; i++)
   {
      db1_user_memory_row_t tmp[MAX_ROWS];
      (void)db1_user_memory_list_recall(DB1_USER_RECALL_IDENTITY, tmp, MAX_ROWS);
   }
   uint64_t direct_ns = (now_ns() - t0) / ITERS;

   t0 = now_ns();
   for (int i = 0; i < ITERS; i++)
   {
      db1_user_memory_row_t tmp[MAX_ROWS];
      (void)bus_recall(&h, &req, &server, (uint64_t)(i + 2), tmp, MAX_ROWS);
   }
   uint64_t bus_ns = (now_ns() - t0) / ITERS;

   printf("  timing: direct recall %llu ns, over the bus %llu ns (bus adds %llu ns)\n",
          (unsigned long long)direct_ns, (unsigned long long)bus_ns,
          (unsigned long long)(bus_ns > direct_ns ? bus_ns - direct_ns : 0));
   printf("  -> pre-migration baseline: the bus request/reply adds ~%llu ns over a direct call\n",
          (unsigned long long)(bus_ns > direct_ns ? bus_ns - direct_ns : 0));

   bus_client_detach(&req);
   bus_client_detach(&server);
   bus_host_destroy(&h);
   db1_shutdown();
   printf("test_bus_memory_recall: OK (a real module operation works over the bus)\n");
   return 0;
}
