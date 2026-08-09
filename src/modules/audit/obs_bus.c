/* obs_bus.c: the governed-action audit row, carried over the event bus.
 * See obs_bus.h for the rationale and lifecycle.
 *
 * Shape: one in-process bus host, one producer client (published to by any number
 * of caller threads, serialized by a mutex so the SPSC producer ring has a single
 * logical writer), and one consumer client drained by a dedicated thread that
 * pumps the host and performs the real append via audit_action_log. The row is
 * off the answer's critical path, so the producer never blocks on the writer: it
 * publishes and returns; the consumer writes asynchronously.
 */
#define _GNU_SOURCE
#include <aimee/audit/obs_bus.h>

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

#include <aimee/core/event_bus/bus_capture.h>
#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_region.h> /* bus_control_epoch */
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/core/event_bus/module_client.h>
#include <errno.h>
#include "config.h"     /* config_default_dir */
#include "log.h"
#include "headers/aimee_sha256.h" /* aimee_sha256_raw — obs_bus_key_fingerprint */

#define KIND_AUDIT_ACTION    OBS_BUS_KIND_ACTION
#define KIND_GUARDRAIL_EVENT OBS_BUS_KIND_GUARDRAIL

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

/* Concurrent C->module calls. Sized for the short fixed-contract stages the
 * gateway makes per request plus room for a few long-running ones. */
#define OBS_BUS_MODULE_CLIENTS 8

static struct
{
   bus_host_t host;
   bus_client_t producer;
   bus_client_t consumer;
   /* A module call is synchronous and holds its client for the whole
    * request/reply, so one shared client serializes every C->module call in the
    * process. That is fatal for a long stage: a roundtable review holds the
    * client for minutes while the module it is running calls back into this
    * server to launch its seats -- and that callback needs a client too. The
    * review waits on its own callback and nothing moves until something times
    * out. Each concurrent call therefore gets its own client, which is what the
    * module client's "dedicated client" contract assumed all along. */
   struct
   {
      bus_client_t bus;
      aimee_module_client_t client;
      int attached;
      int in_use;
   } module_clients[OBS_BUS_MODULE_CLIENTS];
   pthread_mutex_t module_client_lock;
   pthread_cond_t module_client_free;
   int module_in_flight;   /* calls currently holding a client */
   int module_peak_in_flight; /* high-water mark, for diagnosing serialization */
   pthread_t thread;
   pthread_mutex_t pub_lock; /* serializes the single producer ring */
   pthread_mutex_t host_lock; /* serializes pump/reap with external admission */
   bus_runtime_t *runtime;
   bus_runtime_policy_t *runtime_policy;
   atomic_int emitting;      /* 1 while accepting emits */
   atomic_int stop;          /* 1 tells the consumer to final-drain and exit */
   atomic_int publishers;    /* # producers inside the emit window (see enter_emit) */
   atomic_int accepting_calls; /* module RPC admission during daemon lifetime */
   atomic_int module_stop;     /* cancels an in-flight module RPC on shutdown */
   atomic_int module_callers;  /* calls using module_client during teardown */
   atomic_uint_least64_t dropped;
   atomic_uint_least64_t written;
   atomic_uint_least64_t enqueued;  /* events successfully placed on the ring */
   atomic_uint_least64_t processed; /* events the consumer has polled + dispatched */
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

/* Service-owned sinks live outside `g` because start_locked() resets the bus
 * runtime. Configuration is immutable while the bus is running, guarded by
 * start_lock, so the consumer may read it without another lock. */
static struct
{
   obs_bus_guardrail_sink_fn guardrail;
   void *guardrail_ctx;
   char module_socket[108];
   char module_policy_dir[4096];
} sinks;

/* ---------------------------------------------------------- wire form ---- */

static uint32_t put_str(uint8_t *buf, uint32_t off, uint32_t cap, const char *s, uint32_t maxlen)
{
   uint32_t l = s ? (uint32_t)strnlen(s, maxlen) : 0;
   if (off + 4 + l > cap)
      return 0; /* would overflow the slot; caller treats 0 as "does not fit" */
   memcpy(buf + off, &l, 4);
   off += 4;
   /* Sanitize control bytes as we copy. Some fields (a served MCP tool name, a
    * caller-supplied session id) are attacker-influenceable identity; a raw
    * newline or ANSI escape would let a hostile value forge an extra row in the
    * capture stream or inject terminal escapes into an --audit-replay dump. Every
    * legitimate field is printable, so mapping bytes < 0x20 and 0x7f to '?' is a
    * no-op for real data and closes the injection at the one serializer feeding
    * both the ledger and the capture tap. */
   for (uint32_t i = 0; i < l; i++)
   {
      unsigned char c = (unsigned char)s[i];
      buf[off + i] = (c < 0x20 || c == 0x7f) ? '?' : c;
   }
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
      aimee_log(LOG_WARN, "obs_bus", "dropping malformed audit row (len=%u)", len);
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

/* Deserialize a guardrail event and hand it to the daemon-owned sink. The
 * shared bus has no DB1 dependency: aimee-server installs that adapter, while
 * aimee-kb never subscribes to this kind. Returns 1 if written. */
static int write_guardrail(const uint8_t *p, uint32_t len)
{
   guardrail_event_t e;
   memset(&e, 0, sizeof e);
   uint32_t off = 0;
   if (!(off = get_str(p, off, len, e.session_id, sizeof e.session_id)) ||
       !(off = get_str(p, off, len, e.tool_name, sizeof e.tool_name)) || off + 5 * 8 > len)
   {
      aimee_log(LOG_WARN, "obs_bus", "dropping malformed guardrail event (len=%u)", len);
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
      aimee_log(LOG_WARN, "obs_bus", "dropping malformed guardrail event (len=%u)", len);
      return 0;
   }
   int32_t dry;
   memcpy(&dry, p + off, 4);
   e.dry_run = dry;
   if (!sinks.guardrail || sinks.guardrail(&e, sinks.guardrail_ctx) != 0)
   {
      atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
      return 0;
   }
   return 1;
}

/* ---------------------------------------------------------- consumer ----- */

static uint32_t drain(void)
{
   uint32_t n = 0;
   bus_event_t ev;
   while (bus_client_poll(&g.consumer, &ev) == BUS_CLIENT_OK)
   {
      atomic_fetch_add_explicit(&g.processed, 1, memory_order_relaxed); /* this event is handled */
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
      aimee_log(LOG_WARN, "obs_bus", "capture sink broke (alloc); replay stream abandoned");
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
         aimee_log(LOG_WARN, "obs_bus", "capture file write failed; replay stream abandoned");
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
      aimee_log(LOG_WARN, "obs_bus", "no home dir; audit capture/replay stream disabled");
      return;
   }
   /* time+pid identifies the process/session; a per-process counter breaks ties
    * so restarting the bus twice within one second (same pid) cannot collide and
    * truncate the earlier file. capture_open runs under start_lock, so the counter
    * needs no atomic. */
   static unsigned session_seq = 0;
   char path[4096];
   snprintf(path, sizeof path, "%s/%s%010lld-%d-%03u%s", dir, AB_CAP_PREFIX, (long long)time(NULL),
            (int)getpid(), session_seq++, AB_CAP_SUFFIX);
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
   {
      aimee_log(LOG_WARN, "obs_bus", "cannot open audit capture file; replay stream disabled");
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
      uint64_t now = bus_runtime_monotonic_ns();
      bus_client_heartbeat(&g.producer, now);
      bus_client_heartbeat(&g.consumer, now);
      for (int i = 0; i < OBS_BUS_MODULE_CLIENTS; ++i)
         if (g.module_clients[i].attached)
            bus_client_heartbeat(&g.module_clients[i].bus, now);
      pthread_mutex_lock(&g.host_lock);
      if (g.runtime)
         (void)bus_runtime_maintain(g.runtime, now);
      bus_host_pump(&g.host); /* the tap records each routed event into g.capture */
      pthread_mutex_unlock(&g.host_lock);
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
      pthread_mutex_lock(&g.host_lock);
      bus_host_pump(&g.host);
      pthread_mutex_unlock(&g.host_lock);
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
static void module_clients_destroy(void)
{
   for (int i = 0; i < OBS_BUS_MODULE_CLIENTS; ++i)
   {
      if (!g.module_clients[i].attached)
         continue;
      aimee_module_client_destroy(&g.module_clients[i].client);
      bus_client_detach(&g.module_clients[i].bus);
      g.module_clients[i].attached = 0;
   }
}

/* Check out a client for one call. A caller waits only within its own deadline:
 * blocking past it is what turned a busy pool into the hang this pool exists to
 * prevent, so exhaustion is reported as a deadline rather than absorbed. */
static int module_client_acquire(uint64_t deadline_ns)
{
   pthread_mutex_lock(&g.module_client_lock);
   for (;;)
   {
      for (int i = 0; i < OBS_BUS_MODULE_CLIENTS; ++i)
      {
         if (g.module_clients[i].attached && !g.module_clients[i].in_use)
         {
            g.module_clients[i].in_use = 1;
            if (++g.module_in_flight > g.module_peak_in_flight)
               g.module_peak_in_flight = g.module_in_flight;
            pthread_mutex_unlock(&g.module_client_lock);
            return i;
         }
      }
      if (atomic_load(&g.module_stop))
         break;
      struct timespec wait;
      if (deadline_ns)
      {
         wait.tv_sec = (time_t)(deadline_ns / 1000000000ULL);
         wait.tv_nsec = (long)(deadline_ns % 1000000000ULL);
      }
      else
      {
         /* No caller deadline still gets a bound: an unbounded wait here is
          * indistinguishable from the deadlock this replaced. */
         if (clock_gettime(CLOCK_MONOTONIC, &wait) != 0)
            break;
         wait.tv_sec += 30;
      }
      if (pthread_cond_timedwait(&g.module_client_free, &g.module_client_lock, &wait) == ETIMEDOUT)
         break;
   }
   pthread_mutex_unlock(&g.module_client_lock);
   return -1;
}

static void module_client_release(int index)
{
   pthread_mutex_lock(&g.module_client_lock);
   g.module_clients[index].in_use = 0;
   g.module_in_flight--;
   pthread_cond_signal(&g.module_client_free);
   pthread_mutex_unlock(&g.module_client_lock);
}

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
   pthread_mutex_init(&g.host_lock, NULL);
   pthread_mutex_init(&g.module_client_lock, NULL);
   {
      /* Module deadlines are CLOCK_MONOTONIC, so the wait for a free client must
       * be too. A default condvar waits on CLOCK_REALTIME, which a clock step
       * would make honour the wrong instant. */
      pthread_condattr_t attr;
      pthread_condattr_init(&attr);
      pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
      pthread_cond_init(&g.module_client_free, &attr);
      pthread_condattr_destroy(&attr);
   }

   bus_host_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.max_slots = 64; /* three internal clients plus separately shipped modules */
   cfg.slot_size = 2048; /* an audit row (7 short strings + an int) fits inline */
   cfg.inline_budget = 1900;
   cfg.queue_capacity = 1024; /* absorb bursts between drain ticks */
   cfg.arena_size = 256 * 1024;

   if (bus_host_create(&g.host, &cfg, NULL, NULL) != BUS_HOST_OK)
   {
      aimee_log(LOG_ERROR, "obs_bus", "bus host create failed; audit rows will not be recorded");
      pthread_mutex_destroy(&g.host_lock);
      pthread_mutex_destroy(&g.pub_lock);
      return -1;
   }
   int module_clients_ready = 1;
   for (int i = 0; i < OBS_BUS_MODULE_CLIENTS && module_clients_ready; ++i)
   {
      if (attach(&g.module_clients[i].bus) != 0 ||
          aimee_module_client_init(&g.module_clients[i].client, &g.module_clients[i].bus) != 0)
         module_clients_ready = 0;
      else
         g.module_clients[i].attached = 1;
   }
   if (attach(&g.producer) != 0 || attach(&g.consumer) != 0 || !module_clients_ready)
   {
      aimee_log(LOG_ERROR, "obs_bus", "audit bus client attach failed");
      module_clients_destroy();
      bus_client_detach(&g.consumer);
      bus_client_detach(&g.producer);
      bus_host_destroy(&g.host);
      pthread_mutex_destroy(&g.host_lock);
      pthread_mutex_destroy(&g.pub_lock);
      return -1;
   }
   bus_host_subscribe(&g.host, g.consumer.reply.handle_id, KIND_AUDIT_ACTION);
   if (sinks.guardrail)
      bus_host_subscribe(&g.host, g.consumer.reply.handle_id, KIND_GUARDRAIL_EVENT);

   /* Register the capture tap BEFORE the consumer thread starts pumping, so the
    * first routed event onward is recorded. */
   capture_open();

   if (sinks.module_socket[0])
   {
      if (bus_runtime_policy_load_dir(sinks.module_policy_dir, &g.runtime_policy) != 0)
      {
         aimee_log(LOG_ERROR, "obs_bus", "module grant policy is invalid: %s",
                   sinks.module_policy_dir);
         goto start_fail;
      }
      size_t grant_count = 0;
      const bus_runtime_grant_t *grants =
          bus_runtime_policy_grants(g.runtime_policy, &grant_count);
      bus_runtime_config_t runtime_cfg = {.socket_path = sinks.module_socket,
                                          .socket_mode = 0600,
                                          .backlog = 32,
                                          .stale_after_ns = 30ULL * 1000000000ULL,
                                          .grants = grants,
                                          .grant_count = grant_count};
      g.runtime = bus_runtime_start(&g.host, &g.host_lock, &runtime_cfg);
      if (!g.runtime)
      {
         aimee_log(LOG_ERROR, "obs_bus", "module endpoint failed: %s", sinks.module_socket);
         goto start_fail;
      }
   }

   if (pthread_create(&g.thread, NULL, consumer_main, NULL) != 0)
   {
      aimee_log(LOG_ERROR, "obs_bus", "audit consumer thread spawn failed");
      goto start_fail;
   }

   g.started = 1;
   atomic_store_explicit(&g.emitting, 1, memory_order_release);
   atomic_store_explicit(&g.accepting_calls, 1, memory_order_release);
   return 0;

start_fail:
   bus_runtime_stop(&g.runtime);
   bus_runtime_policy_free(&g.runtime_policy);
   if (g.cap_fd >= 0)
      close(g.cap_fd);
   bus_capture_free(&g.capture);
   bus_client_detach(&g.producer);
   bus_client_detach(&g.consumer);
   module_clients_destroy();
   bus_host_destroy(&g.host);
   pthread_mutex_destroy(&g.host_lock);
   pthread_mutex_destroy(&g.pub_lock);
   return -1;
}

int obs_bus_start(void)
{
   pthread_mutex_lock(&start_lock);
   int rc = g.started ? 0 : start_locked();
   pthread_mutex_unlock(&start_lock);
   return rc;
}

typedef struct
{
   aimee_module_cancelled_fn external;
   void *context;
} module_cancel_context_t;

static int module_call_cancelled(void *context)
{
   module_cancel_context_t *state = context;
   return atomic_load_explicit(&g.module_stop, memory_order_acquire) ||
          (state->external && state->external(state->context));
}

aimee_module_call_result_t obs_bus_module_call(
    uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
    const void *request_body, uint32_t request_len, void *response_body,
    uint32_t response_capacity, uint32_t *response_len, aimee_module_cancelled_fn cancelled,
    void *cancel_context)
{
   if (response_len)
      *response_len = 0;
   atomic_fetch_add(&g.module_callers, 1); /* seq_cst: pairs with stop admission gate */
   if (!atomic_load(&g.accepting_calls))
   {
      atomic_fetch_sub(&g.module_callers, 1);
      return AIMEE_MODULE_CALL_TRANSPORT;
   }
   int slot = module_client_acquire(deadline_ns);
   if (slot < 0)
   {
      atomic_fetch_sub(&g.module_callers, 1);
      return AIMEE_MODULE_CALL_DEADLINE_EXCEEDED;
   }
   module_cancel_context_t state = {.external = cancelled, .context = cancel_context};
   aimee_module_call_result_t result = aimee_module_client_call(
       &g.module_clients[slot].client, event_kind, stage_id, trace_id, deadline_ns, request_body,
       request_len, response_body, response_capacity, response_len, module_call_cancelled, &state);
   module_client_release(slot);
   atomic_fetch_sub(&g.module_callers, 1);
   return result;
}

int obs_bus_module_peak_concurrency(void)
{
   pthread_mutex_lock(&g.module_client_lock);
   int peak = g.module_peak_in_flight;
   pthread_mutex_unlock(&g.module_client_lock);
   return peak;
}

int obs_bus_module_available(uint32_t event_kind)
{
   int available = 0;
   pthread_mutex_lock(&start_lock);
   if (g.started && atomic_load_explicit(&g.accepting_calls, memory_order_acquire))
   {
      pthread_mutex_lock(&g.host_lock);
      available = bus_host_kind_has_server(&g.host, event_kind);
      pthread_mutex_unlock(&g.host_lock);
   }
   pthread_mutex_unlock(&start_lock);
   return available;
}

int obs_bus_set_guardrail_sink(obs_bus_guardrail_sink_fn sink, void *ctx)
{
   pthread_mutex_lock(&start_lock);
   if (g.started)
   {
      pthread_mutex_unlock(&start_lock);
      return -1;
   }
   sinks.guardrail = sink;
   sinks.guardrail_ctx = sink ? ctx : NULL;
   pthread_mutex_unlock(&start_lock);
   return 0;
}

int obs_bus_configure_module_runtime(const char *socket_path, const char *policy_dir)
{
   if (!socket_path || socket_path[0] != '/' || !policy_dir || policy_dir[0] != '/' ||
       strlen(socket_path) >= sizeof(sinks.module_socket) ||
       strlen(policy_dir) >= sizeof(sinks.module_policy_dir))
      return -1;
   pthread_mutex_lock(&start_lock);
   if (g.started)
   {
      pthread_mutex_unlock(&start_lock);
      return -1;
   }
   snprintf(sinks.module_socket, sizeof(sinks.module_socket), "%s", socket_path);
   snprintf(sinks.module_policy_dir, sizeof(sinks.module_policy_dir), "%s", policy_dir);
   pthread_mutex_unlock(&start_lock);
   return 0;
}

int obs_bus_configure_daemon_module_runtime(const char *daemon_name,
                                            const char *config_directory)
{
   if (!daemon_name || !daemon_name[0] || strchr(daemon_name, '/') || !config_directory ||
       config_directory[0] != '/')
      return -1;
   const char *socket_override = getenv("AIMEE_MODULE_BUS_SOCKET");
   const char *policy_override = getenv("AIMEE_MODULE_POLICY_DIR");
   char socket_path[108], policy_dir[4096];
   int socket_length =
       socket_override && socket_override[0]
           ? snprintf(socket_path, sizeof(socket_path), "%s", socket_override)
           : snprintf(socket_path, sizeof(socket_path), "%s/%s-module-bus.sock", config_directory,
                      daemon_name);
   int policy_length =
       policy_override && policy_override[0]
           ? snprintf(policy_dir, sizeof(policy_dir), "%s", policy_override)
           : snprintf(policy_dir, sizeof(policy_dir), "%s/modules.d/%s", config_directory,
                      daemon_name);
   if (socket_length <= 0 || (size_t)socket_length >= sizeof(socket_path) || policy_length <= 0 ||
       (size_t)policy_length >= sizeof(policy_dir))
      return -1;
   return obs_bus_configure_module_runtime(socket_path, policy_dir);
}

/* Lazy start on first emit, so obs_bus_emit is a drop-in for the old direct
 * audit_action_log in EVERY context (server, standalone agent, CLI) — a row is
 * never lost merely because no one called obs_bus_start(). atexit drains at a
 * graceful process exit. Once stop() has run (g.terminated), a late emit does
 * NOT resurrect the bus; only an explicit obs_bus_start() restarts it. */
static void ensure_started(void)
{
   if (atomic_load_explicit(&g.emitting, memory_order_acquire))
      return;
   pthread_mutex_lock(&start_lock);
   if (!g.started && !g.terminated && start_locked() == 0)
      atexit(obs_bus_stop);
   pthread_mutex_unlock(&start_lock);
}

/* Enter the emit window. Returns 1 if the caller may publish (and MUST call
 * leave_emit afterwards), 0 if the bus is not accepting emits.
 *
 * The refcount + re-check is what makes shutdown safe: obs_bus_stop sets
 * emitting=0 and then waits for `publishers` to reach 0 before tearing down the
 * producer/host/pub_lock. A producer increments `publishers` BEFORE re-reading
 * emitting, so once stop observes publishers==0 (after storing emitting=0), no
 * producer is — or can newly get — inside publish(): the emitting store and the
 * publishers add are seq_cst, so stop cannot miss an in-flight producer, and no
 * new producer passes the re-check. Without this, emitting=0 gated only NEW
 * emits, leaving in-flight publish() calls racing teardown (use-after-free on the
 * detached producer / destroyed pub_lock, and silently lost rows). */
static int enter_emit(void)
{
   ensure_started();
   atomic_fetch_add(&g.publishers, 1); /* seq_cst */
   if (!atomic_load(&g.emitting))      /* seq_cst re-check after registering */
   {
      atomic_fetch_sub(&g.publishers, 1);
      return 0;
   }
   return 1;
}

static void leave_emit(void)
{
   atomic_fetch_sub(&g.publishers, 1);
}

/* Every event this module emits is INLINE, deliberately. An audit row and a
 * guardrail event are both fixed-schema: each field is bounded by its own cap
 * (serialize_row/serialize_guardrail via put_str's strnlen), so a serialized
 * event is at most ~1.3 KB — always inside the inline budget (1900). There is no
 * arena fallback here because there is no payload that could need one; adding one
 * would be unreachable code. A future producer whose payload can genuinely exceed
 * the budget (e.g. MCP tool-call args/results — see the feature tree) uses
 * bus_client_publish_arena over the now-thread-safe arena instead; this module is
 * simply not that producer.
 *
 * Publish one already-serialized event of `kind` on the producer ring, under the
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
         atomic_fetch_add_explicit(&g.enqueued, 1, memory_order_relaxed);
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
      aimee_log(LOG_WARN, "obs_bus",
                "event not recorded (kind=%u rc=%d) — consumer stuck or publish error", kind, rc);
   }
}

void obs_bus_emit(const char *actor, const char *tool, const char *args_hash, const char *command,
                  const char *mode, const char *reason_code, const char *verdict, long long task_id)
{
   if (!enter_emit())
   {
      aimee_log(LOG_WARN, "obs_bus", "audit bus unavailable; row not recorded");
      return;
   }

   uint8_t buf[2048];
   uint32_t len = serialize_row(buf, sizeof buf, actor, tool, args_hash, command, mode, reason_code,
                                verdict, task_id);
   if (len == 0)
   {
      atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
      aimee_log(LOG_WARN, "obs_bus", "audit row too large to serialize; not recorded");
   }
   else
      publish(KIND_AUDIT_ACTION, buf, len);
   leave_emit();
}

void obs_bus_emit_guardrail(const guardrail_event_t *e)
{
   if (!e)
      return;
   if (!sinks.guardrail)
   {
      aimee_log(LOG_WARN, "obs_bus", "guardrail event has no configured daemon sink; not recorded");
      return;
   }
   if (!enter_emit())
   {
      aimee_log(LOG_WARN, "obs_bus", "audit bus unavailable; guardrail event not recorded");
      return;
   }

   uint8_t buf[2048];
   uint32_t len = serialize_guardrail(buf, sizeof buf, e);
   if (len == 0)
   {
      atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
      aimee_log(LOG_WARN, "obs_bus", "guardrail event too large to serialize; not recorded");
   }
   else
      publish(KIND_GUARDRAIL_EVENT, buf, len);
   leave_emit();
}

void obs_bus_stop(void)
{
   pthread_mutex_lock(&start_lock);
   if (!g.started)
   {
      pthread_mutex_unlock(&start_lock);
      return;
   }
   /* Reject new emits, then WAIT for every in-flight producer to leave the emit
    * window before signalling the consumer or touching the producer/host. A
    * producer that already passed enter_emit()'s re-check is mid-publish and still
    * using g.pub_lock and g.producer; tearing those down under it is a
    * use-after-free (and its row would be lost). enter_emit registers in
    * `publishers` before re-reading emitting, and both are seq_cst, so once we
    * observe publishers==0 after storing emitting=0, no producer is or can get
    * inside publish(). The consumer is still running, so any producer mid-publish
    * still drains and completes. Bounded: a producer waits at most AB_PUB_MAX
    * backoffs. */
   atomic_store(&g.emitting, 0);                                    /* seq_cst */
   atomic_store(&g.accepting_calls, 0); /* seq_cst: no new caller can pass re-check */
   atomic_store_explicit(&g.module_stop, 1, memory_order_release);
   const struct timespec nap = {.tv_sec = 0, .tv_nsec = 50 * 1000}; /* 50 us */
   while (atomic_load(&g.publishers) > 0)
      nanosleep(&nap, NULL);
   while (atomic_load(&g.module_callers) > 0)
      nanosleep(&nap, NULL);

   /* No new process may receive mappings once shutdown begins. The listener is
    * joined before the host and regions are touched. */
   bus_runtime_stop(&g.runtime);

   /* Now no producer will touch the ring again — final-drain and exit. */
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
   module_clients_destroy();
   bus_host_destroy(&g.host);
   bus_runtime_policy_free(&g.runtime_policy);
   pthread_mutex_destroy(&g.host_lock);
   pthread_mutex_destroy(&g.pub_lock);
   g.started = 0;
   g.terminated = 1; /* a lazy emit must not resurrect the bus after shutdown */
   pthread_mutex_unlock(&start_lock);
}

void obs_bus_flush(void)
{
   if (!g.started)
      return;
   /* Wait until the consumer has processed every event enqueued as of now, so a
    * caller that just emitted can read the sink (the ledger / db1) and see them —
    * the write is asynchronous, so a synchronous read-after-emit would otherwise
    * race. The consumer drains aggressively, so this returns quickly; bounded so a
    * stuck consumer cannot hang the caller forever. Does NOT stop the bus. */
   uint64_t target = atomic_load_explicit(&g.enqueued, memory_order_acquire);
   const struct timespec nap = {.tv_sec = 0, .tv_nsec = 100 * 1000}; /* 100 us */
   for (int i = 0; i < 50000; i++)                                   /* ~5 s cap */
   {
      if (atomic_load_explicit(&g.processed, memory_order_acquire) >= target)
         return;
      nanosleep(&nap, NULL);
   }
}

uint64_t obs_bus_dropped(void)
{
   return atomic_load_explicit(&g.dropped, memory_order_relaxed);
}

uint64_t obs_bus_written(void)
{
   return atomic_load_explicit(&g.written, memory_order_relaxed);
}

void obs_bus_key_fingerprint(const char *kind, const char *key, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (out_len < 16)
      return;
   char buf[1200];
   int n = snprintf(buf, sizeof buf, "%s\x1f%s", kind ? kind : "", key ? key : "");
   size_t len = (n < 0) ? 0 : ((size_t)n < sizeof buf ? (size_t)n : sizeof buf);
   unsigned char dig[32];
   if (aimee_sha256_raw(buf, len, dig) != 0)
   {
      snprintf(out, out_len, "mk:?");
      return;
   }
   static const char hx[] = "0123456789abcdef";
   out[0] = 'm';
   out[1] = 'k';
   out[2] = ':';
   for (int i = 0; i < 6; i++)
   {
      out[3 + i * 2] = hx[(dig[i] >> 4) & 0xf];
      out[3 + i * 2 + 1] = hx[dig[i] & 0xf];
   }
   out[15] = '\0';
}
