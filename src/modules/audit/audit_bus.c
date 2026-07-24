/* audit_bus.c: the governed-action audit row, carried over the event bus.
 * See audit_bus.h for the rationale and lifecycle.
 *
 * Shape: one in-process bus host, one producer client (published to by any number
 * of caller threads, serialized by a mutex so the SPSC producer ring has a single
 * logical writer), and one consumer client drained by a dedicated thread that
 * pumps the host and performs the real append via audit_action_log. The row is
 * off the answer's critical path, so the producer never blocks on the writer: it
 * publishes and returns; the consumer writes asynchronously.
 */
#define _GNU_SOURCE
#include "audit_bus.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bus_client.h"
#include "bus_host.h"
#include "log.h"

#define KIND_AUDIT_ACTION 3000

/* Per-field caps for the wire form. Generous vs the emitter's inputs (args_hash
 * 68, command preview 288, the rest short) so nothing a caller passes is clipped
 * before it reaches the writer, which re-escapes and bounds again. */
#define AB_ACTOR   128
#define AB_TOOL    256
#define AB_HASH    96
#define AB_COMMAND 512
#define AB_MODE    64
#define AB_REASON  128
#define AB_VERDICT 32

/* Publish retry cap on backpressure. At ~200us backoff this is ~5s of retry
 * before a row is treated as undeliverable — long past any transient burst, so
 * a drop means the consumer is genuinely stuck, not merely busy. */
#define AB_PUB_MAX 25000

static struct
{
   bus_host_t host;
   bus_client_t producer;
   bus_client_t consumer;
   pthread_t thread;
   pthread_mutex_t pub_lock; /* serializes the single producer ring */
   atomic_int emitting;      /* 1 while accepting emits */
   atomic_int stop;          /* 1 tells the consumer to final-drain and exit */
   atomic_uint_least64_t dropped;
   atomic_uint_least64_t written;
   int started;
} g;

/* ---------------------------------------------------------- wire form ---- */

static uint32_t put_str(uint8_t *buf, uint32_t off, uint32_t cap, const char *s, uint32_t maxlen)
{
   uint32_t l = s ? (uint32_t)strnlen(s, maxlen) : 0;
   if (off + 4 + l > cap)
      return 0; /* would overflow the slot; caller treats 0 as "does not fit" */
   memcpy(buf + off, &l, 4);
   off += 4;
   if (l)
      memcpy(buf + off, s, l);
   return off + l;
}

/* Read a length-prefixed string into out (NUL-terminated, bounded by outcap).
 * Returns the new offset, or 0 on malformed input. */
static uint32_t get_str(const uint8_t *buf, uint32_t off, uint32_t len, char *out, uint32_t outcap)
{
   uint32_t l;
   if (off + 4 > len)
      return 0;
   memcpy(&l, buf + off, 4);
   off += 4;
   if (off + l > len || l >= outcap)
      return 0;
   memcpy(out, buf + off, l);
   out[l] = '\0';
   return off + l;
}

static uint32_t serialize_row(uint8_t *buf, uint32_t cap, const char *actor, const char *tool,
                              const char *args_hash, const char *command, const char *mode,
                              const char *reason_code, const char *verdict, long long task_id)
{
   uint32_t off = 0;
   if (!(off = put_str(buf, off, cap, actor, AB_ACTOR)))
      return 0;
   if (!(off = put_str(buf, off, cap, tool, AB_TOOL)))
      return 0;
   if (!(off = put_str(buf, off, cap, args_hash, AB_HASH)))
      return 0;
   if (!(off = put_str(buf, off, cap, command, AB_COMMAND)))
      return 0;
   if (!(off = put_str(buf, off, cap, mode, AB_MODE)))
      return 0;
   if (!(off = put_str(buf, off, cap, reason_code, AB_REASON)))
      return 0;
   if (!(off = put_str(buf, off, cap, verdict, AB_VERDICT)))
      return 0;
   if (off + 8 > cap)
      return 0;
   int64_t t = (int64_t)task_id;
   memcpy(buf + off, &t, 8);
   return off + 8;
}

/* Deserialize a row and write it to the ledger. Returns 1 if written, 0 if the
 * payload was malformed (dropped with a rate-limited warning, never crashes). */
static int write_row(const uint8_t *buf, uint32_t len)
{
   char actor[AB_ACTOR], tool[AB_TOOL], hash[AB_HASH], command[AB_COMMAND];
   char mode[AB_MODE], reason[AB_REASON], verdict[AB_VERDICT];
   uint32_t off = 0;
   if (!(off = get_str(buf, off, len, actor, sizeof actor)) ||
       !(off = get_str(buf, off, len, tool, sizeof tool)) ||
       !(off = get_str(buf, off, len, hash, sizeof hash)) ||
       !(off = get_str(buf, off, len, command, sizeof command)) ||
       !(off = get_str(buf, off, len, mode, sizeof mode)) ||
       !(off = get_str(buf, off, len, reason, sizeof reason)) ||
       !(off = get_str(buf, off, len, verdict, sizeof verdict)) || off + 8 > len)
   {
      aimee_log(LOG_WARN, "audit_bus", "dropping malformed audit row (len=%u)", len);
      return 0;
   }
   int64_t task_id;
   memcpy(&task_id, buf + off, 8);
   audit_action_log(actor, tool, hash, command, mode, reason, verdict, (long long)task_id);
   return 1;
}

/* ---------------------------------------------------------- consumer ----- */

static uint32_t drain(void)
{
   uint32_t n = 0;
   bus_event_t ev;
   while (bus_client_poll(&g.consumer, &ev) == BUS_CLIENT_OK)
   {
      if (ev.frame.event_kind != KIND_AUDIT_ACTION)
         continue;
      if (write_row(ev.payload, ev.payload_len))
      {
         atomic_fetch_add_explicit(&g.written, 1, memory_order_relaxed);
         n++;
      }
   }
   return n;
}

static void *consumer_main(void *arg)
{
   (void)arg;
   /* Nap only when idle. During a burst the consumer must keep pace with the
    * producer or the ring backs up and the producer is forced to wait, so it
    * loops without napping as long as it is moving rows. */
   const struct timespec nap = {.tv_sec = 0, .tv_nsec = 200 * 1000}; /* 200 us */
   while (!atomic_load_explicit(&g.stop, memory_order_acquire))
   {
      bus_host_pump(&g.host);
      if (drain() == 0)
         nanosleep(&nap, NULL);
   }
   /* Final lossless drain: pump+drain until two consecutive empty rounds, so a
    * row published just before stop is written before the thread exits. */
   int empty = 0;
   while (empty < 2)
   {
      bus_host_pump(&g.host);
      empty = (drain() == 0) ? empty + 1 : 0;
   }
   return NULL;
}

/* ------------------------------------------------------ attach helper ---- */

struct serve_arg
{
   int fd;
};
static void *serve_thread(void *p)
{
   int fd = ((struct serve_arg *)p)->fd;
   bus_host_serve_attach(&g.host, fd);
   return NULL;
}

/* Attach one client over a socketpair the host serves on a short-lived thread
 * (the attach handshake passes the region fds via SCM_RIGHTS; after that the
 * rings live in shared memory and the sockets are no longer needed). */
static int attach(bus_client_t *c)
{
   int sv[2];
   if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0)
      return -1;
   struct serve_arg a = {.fd = sv[1]};
   pthread_t t;
   if (pthread_create(&t, NULL, serve_thread, &a) != 0)
   {
      close(sv[0]);
      close(sv[1]);
      return -1;
   }
   bus_client_result_t rc = bus_client_attach(sv[0], c);
   pthread_join(t, NULL);
   close(sv[0]);
   close(sv[1]);
   return rc == BUS_CLIENT_OK ? 0 : -1;
}

/* ------------------------------------------------------- lifecycle ------- */

int audit_bus_start(void)
{
   if (g.started)
      return 0;

   memset(&g, 0, sizeof g);
   pthread_mutex_init(&g.pub_lock, NULL);
   atomic_store(&g.emitting, 0);
   atomic_store(&g.stop, 0);
   atomic_store(&g.dropped, 0);
   atomic_store(&g.written, 0);

   bus_host_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.max_slots = 4;
   cfg.slot_size = 2048; /* an audit row (7 short strings + an int) fits inline */
   cfg.inline_budget = 1900;
   cfg.queue_capacity = 1024; /* absorb bursts between drain ticks */
   cfg.arena_size = 256 * 1024;

   if (bus_host_create(&g.host, &cfg, NULL, NULL) != BUS_HOST_OK)
   {
      aimee_log(LOG_ERROR, "audit_bus", "bus host create failed; audit rows will not be recorded");
      return -1;
   }
   if (attach(&g.producer) != 0 || attach(&g.consumer) != 0)
   {
      aimee_log(LOG_ERROR, "audit_bus", "audit bus client attach failed");
      bus_host_destroy(&g.host);
      return -1;
   }
   bus_host_subscribe(&g.host, g.consumer.reply.handle_id, KIND_AUDIT_ACTION);

   if (pthread_create(&g.thread, NULL, consumer_main, NULL) != 0)
   {
      aimee_log(LOG_ERROR, "audit_bus", "audit consumer thread spawn failed");
      bus_client_detach(&g.producer);
      bus_client_detach(&g.consumer);
      bus_host_destroy(&g.host);
      return -1;
   }

   g.started = 1;
   atomic_store_explicit(&g.emitting, 1, memory_order_release);
   return 0;
}

void audit_bus_emit(const char *actor, const char *tool, const char *args_hash, const char *command,
                    const char *mode, const char *reason_code, const char *verdict,
                    long long task_id)
{
   if (!atomic_load_explicit(&g.emitting, memory_order_acquire))
   {
      aimee_log(LOG_WARN, "audit_bus", "audit row emitted before start / after stop; not recorded");
      return;
   }

   uint8_t buf[2048];
   uint32_t len = serialize_row(buf, sizeof buf, actor, tool, args_hash, command, mode, reason_code,
                                verdict, task_id);
   if (len == 0)
   {
      atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
      aimee_log(LOG_WARN, "audit_bus", "audit row too large to serialize; not recorded");
      return;
   }

   /* Publish under the producer lock (single logical writer). Backpressure
    * (WOULD_BLOCK) is transient: the consumer drains aggressively, so a short
    * backoff sleep lets it free the ring and the retry lands. The migration is
    * lossless, so we retry WOULD_BLOCK until it clears — capped only high enough
    * to detect a genuinely stuck/dead consumer (AB_PUB_MAX * backoff ~= seconds),
    * at which point the row is recorded as a visible drop rather than blocking
    * forever. A non-WOULD_BLOCK result is a real error: drop immediately. */
   const struct timespec backoff = {.tv_sec = 0, .tv_nsec = 200 * 1000}; /* 200 us */
   pthread_mutex_lock(&g.pub_lock);
   bus_client_result_t rc = BUS_CLIENT_OK;
   int ok = 0;
   for (int attempt = 0; attempt < AB_PUB_MAX; attempt++)
   {
      rc = bus_client_publish(&g.producer, KIND_AUDIT_ACTION, buf, len);
      if (rc == BUS_CLIENT_OK)
      {
         ok = 1;
         break;
      }
      if (rc != BUS_CLIENT_WOULD_BLOCK)
         break; /* a real publish error, not backpressure */
      nanosleep(&backoff, NULL);
   }
   pthread_mutex_unlock(&g.pub_lock);

   if (!ok)
   {
      atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
      aimee_log(LOG_WARN, "audit_bus",
                "audit row not recorded (rc=%d) — consumer stuck or publish error", rc);
   }
}

void audit_bus_stop(void)
{
   if (!g.started)
      return;
   /* Reject new emits, then tell the consumer to final-drain and exit. */
   atomic_store_explicit(&g.emitting, 0, memory_order_release);
   atomic_store_explicit(&g.stop, 1, memory_order_release);
   pthread_join(g.thread, NULL);

   bus_client_detach(&g.producer);
   bus_client_detach(&g.consumer);
   bus_host_destroy(&g.host);
   pthread_mutex_destroy(&g.pub_lock);
   g.started = 0;
}

uint64_t audit_bus_dropped(void)
{
   return atomic_load_explicit(&g.dropped, memory_order_relaxed);
}

uint64_t audit_bus_written(void)
{
   return atomic_load_explicit(&g.written, memory_order_relaxed);
}
