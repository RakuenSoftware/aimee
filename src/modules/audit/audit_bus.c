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

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bus_capture.h"
#include "bus_client.h"
#include "bus_host.h"
#include "bus_region.h" /* bus_control_epoch */
#include "config.h"     /* config_default_dir */
#include "log.h"

#define KIND_AUDIT_ACTION    AUDIT_BUS_KIND_ACTION
#define KIND_GUARDRAIL_EVENT AUDIT_BUS_KIND_GUARDRAIL

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
   /* Record+replay: the reason the audit row is on the bus at all. The host's
    * capture tap records every event, in seq order, into this sink; the consumer
    * thread flushes it to a per-session capture file. Owned by the consumer thread
    * (the tap fires inside bus_host_pump, which only the consumer calls), so no
    * lock guards it. */
   bus_capture_t capture;
   int cap_fd; /* -1 when capture is off (no writable home / open failed) */
   int started;
   int terminated; /* set by stop; blocks lazy resurrection after shutdown */
} g;

/* Guards start/stop transitions and the started/terminated fields. Separate from
 * g.pub_lock (which serializes producers) and never held across a producer wait. */
static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;

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

/* ---- guardrail-semantic event: wire form of guardrail_event_t ---- */

static uint32_t serialize_guardrail(uint8_t *buf, uint32_t cap, const guardrail_event_t *e)
{
   uint32_t off = 0;
   if (!(off = put_str(buf, off, cap, e->session_id, sizeof e->session_id)) ||
       !(off = put_str(buf, off, cap, e->tool_name, sizeof e->tool_name)))
      return 0;
   const double d[5] = {e->overall_risk, e->action_risk, e->diff_risk, e->drift_risk,
                        e->antipattern_similarity};
   if (off + sizeof d > cap)
      return 0;
   memcpy(buf + off, d, sizeof d);
   off += sizeof d;
   if (!(off = put_str(buf, off, cap, e->recommendation, sizeof e->recommendation)) ||
       !(off = put_str(buf, off, cap, e->labels, sizeof e->labels)) ||
       !(off = put_str(buf, off, cap, e->final_action, sizeof e->final_action)) ||
       !(off = put_str(buf, off, cap, e->explanation, sizeof e->explanation)))
      return 0;
   if (off + 4 > cap)
      return 0;
   int32_t dry = e->dry_run;
   memcpy(buf + off, &dry, 4);
   return off + 4;
}

/* Deserialize a guardrail event and write it to db1. Returns 1 if written. */
static int write_guardrail(const uint8_t *p, uint32_t len)
{
   guardrail_event_t e;
   memset(&e, 0, sizeof e);
   uint32_t off = 0;
   if (!(off = get_str(p, off, len, e.session_id, sizeof e.session_id)) ||
       !(off = get_str(p, off, len, e.tool_name, sizeof e.tool_name)) || off + 5 * 8 > len)
   {
      aimee_log(LOG_WARN, "audit_bus", "dropping malformed guardrail event (len=%u)", len);
      return 0;
   }
   double d[5];
   memcpy(d, p + off, sizeof d);
   off += sizeof d;
   e.overall_risk = d[0];
   e.action_risk = d[1];
   e.diff_risk = d[2];
   e.drift_risk = d[3];
   e.antipattern_similarity = d[4];
   if (!(off = get_str(p, off, len, e.recommendation, sizeof e.recommendation)) ||
       !(off = get_str(p, off, len, e.labels, sizeof e.labels)) ||
       !(off = get_str(p, off, len, e.final_action, sizeof e.final_action)) ||
       !(off = get_str(p, off, len, e.explanation, sizeof e.explanation)) || off + 4 > len)
   {
      aimee_log(LOG_WARN, "audit_bus", "dropping malformed guardrail event (len=%u)", len);
      return 0;
   }
   int32_t dry;
   memcpy(&dry, p + off, 4);
   e.dry_run = dry;
   db1_guardrail_event_insert(&e); /* db1 is SQLITE_OPEN_FULLMUTEX — safe off-thread */
   return 1;
}

/* ---------------------------------------------------------- consumer ----- */

static uint32_t drain(void)
{
   uint32_t n = 0;
   bus_event_t ev;
   while (bus_client_poll(&g.consumer, &ev) == BUS_CLIENT_OK)
   {
      int wrote = 0;
      if (ev.frame.event_kind == KIND_AUDIT_ACTION)
         wrote = write_row(ev.payload, ev.payload_len);
      else if (ev.frame.event_kind == KIND_GUARDRAIL_EVENT)
         wrote = write_guardrail(ev.payload, ev.payload_len);
      else
         continue;
      if (wrote)
      {
         atomic_fetch_add_explicit(&g.written, 1, memory_order_relaxed);
         n++;
      }
   }
   return n;
}

/* ---------------------------------------------------- capture / replay --- */

/* Threshold at which the in-memory capture sink is flushed to the file, so its
 * memory stays bounded during a burst instead of growing with the whole stream. */
#define AB_CAP_FLUSH_AT (32u * 1024u)

/* Append the sink's bytes to the capture file and reset it to empty WITHOUT
 * re-emitting the file header (header_written stays set), so the file remains one
 * valid, seq-contiguous stream across many flushes. Runs only on the consumer
 * thread. On a short/failed write the capture file is abandoned (closed) rather
 * than left half-written — the audit LEDGER is the durable record; capture is the
 * replay layer on top, so losing it degrades replay, never the audit itself. */
static void capture_flush(void)
{
   if (g.cap_fd < 0 || g.capture.len == 0)
      return;
   if (g.capture.broken)
   {
      aimee_log(LOG_WARN, "audit_bus", "capture sink broke (alloc); replay stream abandoned");
      close(g.cap_fd);
      g.cap_fd = -1;
      return;
   }
   size_t off = 0;
   while (off < g.capture.len)
   {
      ssize_t w = write(g.cap_fd, g.capture.buf + off, g.capture.len - off);
      if (w <= 0)
      {
         aimee_log(LOG_WARN, "audit_bus", "capture file write failed; replay stream abandoned");
         close(g.cap_fd);
         g.cap_fd = -1;
         g.capture.broken = 1; /* stop the tap appending to a sink we can no longer drain */
         return;
      }
      off += (size_t)w;
   }
   g.capture.len = 0; /* keep header_written/first_seq: the header is already on disk */
}

/* One capture file per host SESSION: the reader requires seq-contiguity, which a
 * new host (new epoch, seq restarting) would break, so sessions cannot share a
 * file. Files are named by start time + pid so a restart RETAINS prior sessions'
 * replayable records rather than clobbering them; the ledger already keeps the
 * durable rows, but the ordered, full-fidelity replay stream is worth keeping too.
 * Retention is bounded — the newest AB_CAP_KEEP files survive, older ones are
 * pruned on start — so the streams do not accumulate without limit. */
#define AB_CAP_PREFIX "audit-bus-capture-"
#define AB_CAP_SUFFIX ".aimeecap"
#define AB_CAP_KEEP   16

static int cmp_str_desc(const void *a, const void *b)
{
   return -strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Keep the newest AB_CAP_KEEP session files in `dir`, unlink older ones. Names
 * embed a fixed-width-ish start time so a lexical sort is chronological for the
 * current era; good enough for retention (a stale extra file is pruned next
 * start). Best-effort: any failure just leaves files in place. */
static void capture_prune(const char *dir, int keep)
{
   DIR *d = opendir(dir);
   if (!d)
      return;
   char *names[512];
   int n = 0;
   struct dirent *e;
   while ((e = readdir(d)) != NULL && n < (int)(sizeof names / sizeof names[0]))
   {
      size_t plen = strlen(AB_CAP_PREFIX);
      size_t nlen = strlen(e->d_name);
      if (strncmp(e->d_name, AB_CAP_PREFIX, plen) == 0 && nlen > plen &&
          strcmp(e->d_name + nlen - strlen(AB_CAP_SUFFIX), AB_CAP_SUFFIX) == 0)
      {
         char *dup = strdup(e->d_name);
         if (dup)
            names[n++] = dup;
      }
   }
   closedir(d);
   qsort(names, (size_t)n, sizeof names[0], cmp_str_desc); /* newest (largest) first */
   for (int i = keep; i < n; i++)
   {
      char path[4096];
      snprintf(path, sizeof path, "%s/%s", dir, names[i]);
      unlink(path);
   }
   for (int i = 0; i < n; i++)
      free(names[i]);
}

/* Open this session's capture file under the home dir and register the tap that
 * records every routed event into it. Best-effort: if there is no writable home
 * the tap is NOT registered (capture off; the sink would otherwise grow unbounded
 * with nowhere to flush), and audit still works — the ledger is the durable
 * record, capture is the replay layer on top. */
static void capture_open(void)
{
   g.cap_fd = -1;
   bus_capture_init(&g.capture, 1, 1, bus_control_epoch(g.host.control));

   const char *dir = config_default_dir();
   if (!dir || !dir[0])
   {
      aimee_log(LOG_WARN, "audit_bus", "no home dir; audit capture/replay stream disabled");
      return;
   }
   /* time+pid identifies the process/session; a per-process counter breaks ties
    * so restarting the bus twice within one second (same pid) cannot collide and
    * truncate the earlier file. capture_open runs under start_lock, so the counter
    * needs no atomic. */
   static unsigned session_seq = 0;
   char path[4096];
   snprintf(path, sizeof path, "%s/%s%010lld-%d-%03u%s", dir, AB_CAP_PREFIX,
            (long long)time(NULL), (int)getpid(), session_seq++, AB_CAP_SUFFIX);
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
   {
      aimee_log(LOG_WARN, "audit_bus", "cannot open audit capture file; replay stream disabled");
      return;
   }
   g.cap_fd = fd;
   bus_host_set_tap(&g.host, bus_capture_tap, &g.capture);
   capture_prune(dir, AB_CAP_KEEP); /* retain the newest sessions, prune older */
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
      bus_host_pump(&g.host); /* the tap records each routed event into g.capture */
      uint32_t n = drain();
      /* Flush the capture stream on the threshold (bound memory during a burst)
       * or when the flow goes idle (so a recorded row is not stranded in memory
       * waiting for more traffic). */
      if (g.capture.len >= AB_CAP_FLUSH_AT || (n == 0 && g.capture.len > 0))
         capture_flush();
      if (n == 0)
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
   capture_flush(); /* persist whatever the final drain recorded */
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

/* Bring the bus up. start_lock MUST be held and g.started MUST be false. */
static int start_locked(void)
{
   memset(&g, 0, sizeof g);
   pthread_mutex_init(&g.pub_lock, NULL);

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
   bus_host_subscribe(&g.host, g.consumer.reply.handle_id, KIND_GUARDRAIL_EVENT);

   /* Register the capture tap BEFORE the consumer thread starts pumping, so the
    * first routed event onward is recorded. */
   capture_open();

   if (pthread_create(&g.thread, NULL, consumer_main, NULL) != 0)
   {
      aimee_log(LOG_ERROR, "audit_bus", "audit consumer thread spawn failed");
      if (g.cap_fd >= 0)
         close(g.cap_fd);
      bus_capture_free(&g.capture);
      bus_client_detach(&g.producer);
      bus_client_detach(&g.consumer);
      bus_host_destroy(&g.host);
      return -1;
   }

   g.started = 1;
   atomic_store_explicit(&g.emitting, 1, memory_order_release);
   return 0;
}

int audit_bus_start(void)
{
   pthread_mutex_lock(&start_lock);
   int rc = g.started ? 0 : start_locked();
   pthread_mutex_unlock(&start_lock);
   return rc;
}

/* Lazy start on first emit, so audit_bus_emit is a drop-in for the old direct
 * audit_action_log in EVERY context (server, standalone agent, CLI) — a row is
 * never lost merely because no one called audit_bus_start(). atexit drains at a
 * graceful process exit. Once stop() has run (g.terminated), a late emit does
 * NOT resurrect the bus; only an explicit audit_bus_start() restarts it. */
static void ensure_started(void)
{
   if (atomic_load_explicit(&g.emitting, memory_order_acquire))
      return;
   pthread_mutex_lock(&start_lock);
   if (!g.started && !g.terminated && start_locked() == 0)
      atexit(audit_bus_stop);
   pthread_mutex_unlock(&start_lock);
}

/* Publish one already-serialized event of `kind` on the producer ring, under the
 * producer lock (single logical writer). Backpressure (WOULD_BLOCK) is transient:
 * the consumer drains aggressively, so a short backoff sleep lets it free the ring
 * and the retry lands. The migration is lossless, so we retry WOULD_BLOCK until it
 * clears — capped only high enough to detect a genuinely stuck/dead consumer
 * (AB_PUB_MAX * backoff ~= seconds), at which point the event is a visible drop
 * rather than blocking forever. A non-WOULD_BLOCK result is a real error: drop. */
static void publish(uint32_t kind, const uint8_t *buf, uint32_t len)
{
   const struct timespec backoff = {.tv_sec = 0, .tv_nsec = 200 * 1000}; /* 200 us */
   pthread_mutex_lock(&g.pub_lock);
   bus_client_result_t rc = BUS_CLIENT_OK;
   int ok = 0;
   for (int attempt = 0; attempt < AB_PUB_MAX; attempt++)
   {
      rc = bus_client_publish(&g.producer, kind, buf, len);
      if (rc == BUS_CLIENT_OK)
      {
         ok = 1;
         break;
      }
      if (rc != BUS_CLIENT_WOULD_BLOCK)
         break;
      nanosleep(&backoff, NULL);
   }
   pthread_mutex_unlock(&g.pub_lock);

   if (!ok)
   {
      atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
      aimee_log(LOG_WARN, "audit_bus",
                "event not recorded (kind=%u rc=%d) — consumer stuck or publish error", kind, rc);
   }
}

void audit_bus_emit(const char *actor, const char *tool, const char *args_hash, const char *command,
                    const char *mode, const char *reason_code, const char *verdict,
                    long long task_id)
{
   ensure_started();
   if (!atomic_load_explicit(&g.emitting, memory_order_acquire))
   {
      aimee_log(LOG_WARN, "audit_bus", "audit bus unavailable; row not recorded");
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
   publish(KIND_AUDIT_ACTION, buf, len);
}

void audit_bus_emit_guardrail(const guardrail_event_t *e)
{
   if (!e)
      return;
   ensure_started();
   if (!atomic_load_explicit(&g.emitting, memory_order_acquire))
   {
      aimee_log(LOG_WARN, "audit_bus", "audit bus unavailable; guardrail event not recorded");
      return;
   }

   uint8_t buf[2048];
   uint32_t len = serialize_guardrail(buf, sizeof buf, e);
   if (len == 0)
   {
      atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
      aimee_log(LOG_WARN, "audit_bus", "guardrail event too large to serialize; not recorded");
      return;
   }
   publish(KIND_GUARDRAIL_EVENT, buf, len);
}

void audit_bus_stop(void)
{
   pthread_mutex_lock(&start_lock);
   if (!g.started)
   {
      pthread_mutex_unlock(&start_lock);
      return;
   }
   /* Reject new emits, then tell the consumer to final-drain and exit. */
   atomic_store_explicit(&g.emitting, 0, memory_order_release);
   atomic_store_explicit(&g.stop, 1, memory_order_release);
   pthread_join(g.thread, NULL); /* the consumer does its final capture_flush here */

   if (g.cap_fd >= 0)
   {
      close(g.cap_fd);
      g.cap_fd = -1;
   }
   bus_capture_free(&g.capture);
   bus_client_detach(&g.producer);
   bus_client_detach(&g.consumer);
   bus_host_destroy(&g.host);
   pthread_mutex_destroy(&g.pub_lock);
   g.started = 0;
   g.terminated = 1; /* a lazy emit must not resurrect the bus after shutdown */
   pthread_mutex_unlock(&start_lock);
}

uint64_t audit_bus_dropped(void)
{
   return atomic_load_explicit(&g.dropped, memory_order_relaxed);
}

uint64_t audit_bus_written(void)
{
   return atomic_load_explicit(&g.written, memory_order_relaxed);
}
