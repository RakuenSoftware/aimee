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
#include "config.h" /* config_default_dir */
#include "log.h"
#include "headers/aimee_sha256.h" /* aimee_sha256_raw — obs_bus_key_fingerprint */

#define KIND_AUDIT_ACTION     OBS_BUS_KIND_ACTION
#define KIND_GUARDRAIL_EVENT  OBS_BUS_KIND_GUARDRAIL
#define KIND_DURABILITY_EVENT OBS_BUS_KIND_DURABILITY

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

#define AB_DUR_ACTION  128
#define AB_DUR_SUBJECT 192
#define AB_DUR_DETAIL  768

typedef enum
{
   CAPTURE_NOT_STARTED = 0,
   CAPTURE_OK,
   CAPTURE_NO_HOME,
   CAPTURE_OPEN_FAILED,
   CAPTURE_WRITE_FAILED,
   CAPTURE_SINK_BROKEN
} capture_state_t;

typedef struct durable_pending
{
   char action[AB_DUR_ACTION];
   char subject[AB_DUR_SUBJECT];
   char verdict[AB_VERDICT];
   char detail[AB_DUR_DETAIL];
   struct durable_pending *next;
} durable_pending_t;

/* PostgreSQL can be unavailable during KB startup. Retain audit rows for retry,
 * but never let a broken durable sink turn into unbounded daemon memory. */
#define AB_DUR_PENDING_MAX 4096u

/* Publish retry cap on backpressure. At ~200us backoff this is ~5s of retry
 * before a row is treated as undeliverable — long past any transient burst, so
 * a drop means the consumer is genuinely stuck, not merely busy. */
#define AB_PUB_MAX 25000

/* Concurrent C->module calls. Sized for the short fixed-contract stages the
 * gateway makes per request plus room for a few long-running ones. */
#define OBS_BUS_MODULE_CLIENTS 8

/* The guardrail writer's queue.
 *
 * BOUNDED, and a full queue drops rather than blocks: blocking would put the
 * consumer back to waiting on the store, which is the whole defect this queue
 * exists to remove. A drop is counted and visible in obs_bus_dropped(); a stall
 * would be neither.
 *
 * 4096 is well past any burst the ring itself can hold, so reaching it means
 * the store has stopped answering rather than that the queue is small. */
#define AB_WRITER_QUEUE 4096
/* One guardrail payload, as it arrives off the ring. */
#define AB_GUARDRAIL_MAX 2048

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
   int module_in_flight;      /* calls currently holding a client */
   int module_peak_in_flight; /* high-water mark, for diagnosing serialization */
   pthread_t thread;
   pthread_mutex_t pub_lock;  /* serializes the single producer ring */
   pthread_mutex_t host_lock; /* serializes pump/reap with external admission */
   bus_runtime_t *runtime;
   bus_runtime_policy_t *runtime_policy;
   atomic_int emitting;        /* 1 while accepting emits */
   atomic_int stop;            /* 1 tells the consumer to final-drain */
   atomic_int consumer_exit;   /* 1 tells it to leave, once the writer is done */
   atomic_int drain_done;      /* set by the consumer when its final drain is complete */
   atomic_int publishers;      /* # producers inside the emit window (see enter_emit) */
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
   atomic_int capture_state;
   atomic_uint_least64_t capture_last_seq; /* last seq successfully flushed to disk */
   char capture_session[128];
   durable_pending_t *durable_head;
   durable_pending_t *durable_tail;
   uint32_t durable_pending_count;
   uint64_t durable_retry_after_ns;
   int started;
   int terminated; /* set by stop; blocks lazy resurrection after shutdown */

   /* The guardrail writer. See guardrail_writer_main: the sink is a module call
    * now, and it may not be made from the thread that pumps the host. */
   pthread_t writer;
   int writer_running;
   pthread_mutex_t writer_lock;
   pthread_cond_t writer_ready; /* work arrived, or finishing */
   pthread_cond_t writer_drained;
   atomic_int writer_finish; /* 1 tells the writer to exit once the queue empties */
   struct
   {
      uint8_t payload[AB_GUARDRAIL_MAX];
      uint32_t len;
   } writer_q[AB_WRITER_QUEUE];
   unsigned writer_head;
   unsigned writer_count;
   /* 1 between taking an event off the queue and finishing its write. The queue
    * being empty does NOT mean the writer is idle: it decrements the count under
    * the lock and then writes outside it, so there is a window where the event
    * is in neither place. A waiter that watched only the count would return in
    * that window, which is the whole failure obs_bus_flush exists to avoid. */
   int writer_busy;
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
   obs_bus_durable_sink_fn durable;
   void *durable_ctx;
   char module_socket[108];
   char module_policy_dir[4096];
   char capture_fault[24]; /* deterministic unit-test seam; empty in production */
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

/* ---- generic WORM durability event ------------------------------------- */

static uint32_t serialize_durable(uint8_t *buf, uint32_t cap, const char *action,
                                  const char *subject, const char *verdict, const char *detail)
{
   uint32_t off = 0;
   if (!(off = put_str(buf, off, cap, action, AB_DUR_ACTION)) ||
       !(off = put_str(buf, off, cap, subject, AB_DUR_SUBJECT)) ||
       !(off = put_str(buf, off, cap, verdict, AB_VERDICT)) ||
       !(off = put_str(buf, off, cap, detail, AB_DUR_DETAIL)))
      return 0;
   return off;
}

static int persist_durable(const char *action, const char *subject, const char *verdict,
                           const char *detail)
{
   return sinks.durable ? sinks.durable("system", "event-bus", action, subject, verdict, detail,
                                        sinks.durable_ctx) == 0
                        : 0;
}

static int queue_durable(const char *action, const char *subject, const char *verdict,
                         const char *detail)
{
   if (g.durable_pending_count >= AB_DUR_PENDING_MAX)
      return 0;
   durable_pending_t *p = calloc(1, sizeof *p);
   if (!p)
      return 0;
   snprintf(p->action, sizeof p->action, "%s", action ? action : "");
   snprintf(p->subject, sizeof p->subject, "%s", subject ? subject : "");
   snprintf(p->verdict, sizeof p->verdict, "%s", verdict ? verdict : "");
   snprintf(p->detail, sizeof p->detail, "%s", detail ? detail : "");
   if (g.durable_tail)
      g.durable_tail->next = p;
   else
      g.durable_head = p;
   g.durable_tail = p;
   g.durable_pending_count++;
   return 1;
}

/* Runs on the consumer thread, except during capture_open before that thread is
 * spawned. A startup marker that reaches aimee-kb before PostgreSQL is ready is
 * retained and retried; it never turns into a transient WARN and disappears. */
static int persist_or_queue_durable(const char *action, const char *subject, const char *verdict,
                                    const char *detail)
{
   /* Standalone binaries and legacy unit tests can use the diagnostic bus
    * without owning a WORM store. Production daemons install this sink before
    * start; without one there is nowhere honest to claim the row is durable. */
   if (!sinks.durable)
      return 0;
   if (persist_durable(action, subject, verdict, detail))
      return 1;
   if (queue_durable(action, subject, verdict, detail))
      return -1;
   atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
   aimee_log(LOG_ERROR, "obs_bus", "durable record could not be persisted or queued: %s",
             action ? action : "");
   return 0;
}

static void discard_pending_durable(void)
{
   while (g.durable_head)
   {
      durable_pending_t *next = g.durable_head->next;
      free(g.durable_head);
      g.durable_head = next;
   }
   g.durable_tail = NULL;
   g.durable_pending_count = 0;
}

static uint32_t flush_pending_durable(void)
{
   uint64_t now = bus_runtime_monotonic_ns();
   if (g.durable_head && now < g.durable_retry_after_ns)
      return 0;
   uint32_t n = 0;
   while (g.durable_head)
   {
      durable_pending_t *p = g.durable_head;
      if (!persist_durable(p->action, p->subject, p->verdict, p->detail))
      {
         /* PostgreSQL may not be ready when the KB daemon starts. Retry the
          * retained row without turning the audit sink into a 5 kHz poller. */
         g.durable_retry_after_ns = now + 1000000000ull;
         break;
      }
      g.durable_head = p->next;
      if (!g.durable_head)
         g.durable_tail = NULL;
      g.durable_pending_count--;
      free(p);
      atomic_fetch_add_explicit(&g.written, 1, memory_order_relaxed);
      n++;
   }
   if (!g.durable_head)
      g.durable_retry_after_ns = 0;
   return n;
}

static int write_durable(const uint8_t *p, uint32_t len)
{
   char action[AB_DUR_ACTION], subject[AB_DUR_SUBJECT], verdict[AB_VERDICT], detail[AB_DUR_DETAIL];
   uint32_t off = 0;
   if (!(off = get_str(p, off, len, action, sizeof action)) ||
       !(off = get_str(p, off, len, subject, sizeof subject)) ||
       !(off = get_str(p, off, len, verdict, sizeof verdict)) ||
       !(off = get_str(p, off, len, detail, sizeof detail)))
   {
      aimee_log(LOG_WARN, "obs_bus", "dropping malformed durability event (len=%u)", len);
      return 0;
   }
   return persist_or_queue_durable(action, subject, verdict, detail);
}

/* ------------------------------------------------------- guardrail writer -- */

/* Queue one guardrail payload for the writer. Returns 1 when it was taken.
 *
 * A payload too large for the slot, or a full queue, is DROPPED and counted --
 * never blocked on. Blocking here would return the consumer to waiting on the
 * store, which is exactly what this queue exists to prevent. */
static int guardrail_enqueue(const uint8_t *payload, uint32_t len)
{
   if (len > AB_GUARDRAIL_MAX)
   {
      atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
      return 0;
   }
   pthread_mutex_lock(&g.writer_lock);
   if (!g.writer_running || g.writer_count == AB_WRITER_QUEUE)
   {
      pthread_mutex_unlock(&g.writer_lock);
      atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
      return 0;
   }
   unsigned slot = (g.writer_head + g.writer_count) % AB_WRITER_QUEUE;
   memcpy(g.writer_q[slot].payload, payload, len);
   g.writer_q[slot].len = len;
   g.writer_count++;
   pthread_cond_signal(&g.writer_ready);
   pthread_mutex_unlock(&g.writer_lock);
   return 1;
}

/* The writer thread: the only place the guardrail sink is called.
 *
 * It exists because that sink makes a module call and the consumer thread pumps
 * the host, so a call from there waits for a reply it is itself responsible for
 * routing. Any thread that is not the consumer will do; this one is dedicated so
 * a slow store cannot delay the ring. */
static void *guardrail_writer_main(void *arg)
{
   (void)arg;
   for (;;)
   {
      pthread_mutex_lock(&g.writer_lock);
      while (g.writer_count == 0 && !atomic_load_explicit(&g.writer_finish, memory_order_acquire))
         pthread_cond_wait(&g.writer_ready, &g.writer_lock);
      if (g.writer_count == 0)
      {
         pthread_mutex_unlock(&g.writer_lock);
         break; /* asked to finish, and nothing left */
      }
      uint8_t payload[AB_GUARDRAIL_MAX];
      uint32_t len = g.writer_q[g.writer_head].len;
      memcpy(payload, g.writer_q[g.writer_head].payload, len);
      g.writer_head = (g.writer_head + 1) % AB_WRITER_QUEUE;
      g.writer_count--;
      g.writer_busy = 1;
      pthread_mutex_unlock(&g.writer_lock);

      /* Outside the lock: this is the call that can take milliseconds, and
       * holding the queue lock across it would stall the consumer's handoff. */
      if (write_guardrail(payload, len))
         atomic_fetch_add_explicit(&g.written, 1, memory_order_relaxed);
      else
         atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);

      pthread_mutex_lock(&g.writer_lock);
      g.writer_busy = 0;
      if (g.writer_count == 0)
         pthread_cond_broadcast(&g.writer_drained);
      pthread_mutex_unlock(&g.writer_lock);
   }
   pthread_mutex_lock(&g.writer_lock);
   g.writer_busy = 0;
   pthread_cond_broadcast(&g.writer_drained);
   pthread_mutex_unlock(&g.writer_lock);
   return NULL;
}

/* Finish the writer: let it empty the queue, then join it.
 *
 * MUST BE CALLED WHILE THE CONSUMER IS STILL PUMPING. The writer's calls need
 * their replies routed, and the consumer is what routes them -- so stopping the
 * consumer first would leave every remaining event to time out, which is the
 * lossy shutdown this whole change is about. */
static void guardrail_writer_finish(void)
{
   pthread_mutex_lock(&g.writer_lock);
   if (!g.writer_running)
   {
      pthread_mutex_unlock(&g.writer_lock);
      return;
   }
   g.writer_running = 0;
   pthread_mutex_unlock(&g.writer_lock);
   atomic_store_explicit(&g.writer_finish, 1, memory_order_release);
   pthread_mutex_lock(&g.writer_lock);
   pthread_cond_broadcast(&g.writer_ready);
   pthread_mutex_unlock(&g.writer_lock);
   pthread_join(g.writer, NULL);
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
      {
         /* The audit sink stays here: audit_action_log writes the WORM ledger,
          * a file, and makes no module call, so it cannot wait on this thread. */
         wrote = write_row(ev.payload, ev.payload_len);
         if (wrote)
         {
            atomic_fetch_add_explicit(&g.written, 1, memory_order_relaxed);
            n++;
         }
      }
      else if (ev.frame.event_kind == KIND_GUARDRAIL_EVENT)
      {
         /* HANDED OFF, not written here. The guardrail sink is a module call,
          * and this thread pumps the host that would deliver its reply. The
          * writer counts it written once the store has it, so obs_bus_written()
          * still means durable. */
         wrote = guardrail_enqueue(ev.payload, ev.payload_len);
         if (wrote)
            n++;
      }
      else if (ev.frame.event_kind == KIND_DURABILITY_EVENT)
      {
         wrote = write_durable(ev.payload, ev.payload_len);
         if (wrote > 0)
         {
            atomic_fetch_add_explicit(&g.written, 1, memory_order_relaxed);
            n++;
         }
         else if (wrote < 0)
            n++; /* accepted into the durable retry queue */
      }
      else
         continue;
   }
   return n;
}

/* ---------------------------------------------------- capture / replay --- */

/* Threshold at which the in-memory capture sink is flushed to the file, so its
 * memory stays bounded during a burst instead of growing with the whole stream. */
#define AB_CAP_FLUSH_AT (32u * 1024u)

static const char *capture_state_name(capture_state_t state)
{
   switch (state)
   {
   case CAPTURE_OK:
      return "ok";
   case CAPTURE_NO_HOME:
      return "no_home";
   case CAPTURE_OPEN_FAILED:
      return "open_failed";
   case CAPTURE_WRITE_FAILED:
      return "write_failed";
   case CAPTURE_SINK_BROKEN:
      return "sink_broken";
   case CAPTURE_NOT_STARTED:
   default:
      return "not_started";
   }
}

static void capture_mark_gap(capture_state_t state)
{
   capture_state_t old =
       (capture_state_t)atomic_exchange_explicit(&g.capture_state, state, memory_order_acq_rel);
   if (old == state)
      return;
   uint64_t last = atomic_load_explicit(&g.capture_last_seq, memory_order_acquire);
   time_t wall = time(NULL);
   char detail[AB_DUR_DETAIL];
   snprintf(detail, sizeof detail,
            "{\"session_id\":\"%s\",\"last_seq\":%llu,\"reason\":\"%s\","
            "\"wall_time\":%lld}",
            g.capture_session, (unsigned long long)last, capture_state_name(state),
            (long long)wall);
   (void)persist_or_queue_durable("bus.capture.gap", g.capture_session, capture_state_name(state),
                                  detail);
}

/* Append the sink's bytes to the capture file and reset it to empty WITHOUT
 * re-emitting the file header (header_written stays set), so the file remains one
 * valid, seq-contiguous stream across many flushes. Runs only on the consumer
 * thread. On a short/failed write the capture file is abandoned (closed) rather
 * than left half-written — the audit LEDGER is the durable record; capture is the
 * replay layer on top, so losing it degrades replay, never the audit itself. */
static void capture_flush(void)
{
   if (g.cap_fd < 0)
      return;
   if (g.capture.broken)
   {
      aimee_log(LOG_WARN, "obs_bus", "capture sink broke (alloc); replay stream abandoned");
      close(g.cap_fd);
      g.cap_fd = -1;
      capture_mark_gap(CAPTURE_SINK_BROKEN);
      return;
   }
   if (g.capture.len == 0)
      return;
   size_t off = 0;
   while (off < g.capture.len)
   {
      ssize_t w = strcmp(sinks.capture_fault, "write_failed") == 0
                      ? -1
                      : write(g.cap_fd, g.capture.buf + off, g.capture.len - off);
      if (w <= 0)
      {
         aimee_log(LOG_WARN, "obs_bus", "capture file write failed; replay stream abandoned");
         close(g.cap_fd);
         g.cap_fd = -1;
         g.capture.broken = 1; /* stop the tap appending to a sink we can no longer drain */
         capture_mark_gap(CAPTURE_WRITE_FAILED);
         return;
      }
      off += (size_t)w;
   }
   atomic_store_explicit(&g.capture_last_seq, g.capture.last_seq, memory_order_release);
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
      if (unlink(path) == 0)
      {
         char detail[AB_DUR_DETAIL];
         snprintf(detail, sizeof detail, "{\"session_id\":\"%s\",\"wall_time\":%lld}", names[i],
                  (long long)time(NULL));
         (void)persist_or_queue_durable("bus.capture.pruned", names[i], "pruned", detail);
      }
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

   static unsigned session_seq = 0;
   snprintf(g.capture_session, sizeof g.capture_session, "%s%010lld-%d-%03u%s", AB_CAP_PREFIX,
            (long long)time(NULL), (int)getpid(), session_seq++, AB_CAP_SUFFIX);

   /* Capture may live on a dedicated diagnostic volume. Keeping that path
    * separate from AIMEE_HOME also lets an operator make the losable layer
    * read-only without disabling the daemon or its durable WORM store. */
   const char *capture_dir = getenv("AIMEE_CAPTURE_DIR");
   const char *dir = (capture_dir && capture_dir[0]) ? capture_dir : config_default_dir();
   if (!dir || !dir[0] || strcmp(sinks.capture_fault, "no_home") == 0)
   {
      aimee_log(LOG_WARN, "obs_bus", "no home dir; audit capture/replay stream disabled");
      capture_mark_gap(CAPTURE_NO_HOME);
      return;
   }
   /* time+pid identifies the process/session; a per-process counter breaks ties
    * so restarting the bus twice within one second (same pid) cannot collide and
    * truncate the earlier file. capture_open runs under start_lock, so the counter
    * needs no atomic. */
   char path[4096];
   snprintf(path, sizeof path, "%s/%s", dir, g.capture_session);
   int fd = strcmp(sinks.capture_fault, "open_failed") == 0
                ? -1
                : open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
   {
      aimee_log(LOG_WARN, "obs_bus", "cannot open audit capture file; replay stream disabled");
      capture_mark_gap(CAPTURE_OPEN_FAILED);
      return;
   }
   g.cap_fd = fd;
   atomic_store_explicit(&g.capture_state, CAPTURE_OK, memory_order_release);
   bus_host_set_tap(&g.host, bus_capture_tap, &g.capture);
   if (strcmp(sinks.capture_fault, "sink_broken") == 0)
      g.capture.broken = 1;
   capture_prune(dir, AB_CAP_KEEP); /* retain the newest sessions, prune older */
}

/* Loss evidence is independent of capture. bus_route calls this only for the
 * two rare loss controls, so a disabled capture does not put a callback on the
 * ordinary route path and cannot suppress the WORM row. */
void obs_bus_record_loss(void *ctx, const bus_frame_t *frame, const uint8_t *payload,
                         uint32_t payload_len)
{
   (void)ctx;
   char subject[AB_DUR_SUBJECT];
   char detail[AB_DUR_DETAIL];
   if (frame->event_kind == BUS_KIND_OVERFLOW && payload_len == sizeof(bus_overflow_t))
   {
      bus_overflow_t loss;
      memcpy(&loss, payload, sizeof loss);
      snprintf(subject, sizeof subject, "seq:%llu", (unsigned long long)loss.shed_seq);
      snprintf(detail, sizeof detail,
               "{\"lost_seq\":%llu,\"event_kind\":%u,\"dst_slot\":%u,"
               "\"control_seq\":%llu,\"wall_time\":%lld}",
               (unsigned long long)loss.shed_seq, loss.shed_kind, loss.dst_slot,
               (unsigned long long)frame->seq, (long long)time(NULL));
      (void)persist_or_queue_durable("bus.overflow", subject, "dropped", detail);
   }
   else if (frame->event_kind == BUS_KIND_PRODUCER_REAPED &&
            payload_len == sizeof(bus_producer_reaped_t))
   {
      bus_producer_reaped_t loss;
      memcpy(&loss, payload, sizeof loss);
      snprintf(subject, sizeof subject, "seq:%llu", (unsigned long long)loss.lost_seq);
      snprintf(detail, sizeof detail,
               "{\"lost_seq\":%llu,\"event_kind\":%u,\"src_slot\":%u,"
               "\"control_seq\":%llu,\"wall_time\":%lld}",
               (unsigned long long)loss.lost_seq, loss.lost_kind, loss.src_slot,
               (unsigned long long)frame->seq, (long long)time(NULL));
      (void)persist_or_queue_durable("bus.producer_reaped", subject, "dropped", detail);
   }
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
      n += flush_pending_durable();
      /* Flush the capture stream on the threshold (bound memory during a burst)
       * or when the flow goes idle (so a recorded row is not stranded in memory
       * waiting for more traffic). */
      if (g.capture.broken || g.capture.len >= AB_CAP_FLUSH_AT || (n == 0 && g.capture.len > 0))
         capture_flush();
      if (n == 0)
         nanosleep(&nap, NULL);
   }
   /* Final lossless drain: pump+drain until two consecutive empty rounds, so a
    * row published just before stop is handed to its sink before this thread
    * stops draining. */
   int empty = 0;
   while (empty < 2)
   {
      pthread_mutex_lock(&g.host_lock);
      bus_host_pump(&g.host);
      pthread_mutex_unlock(&g.host_lock);
      empty = (drain() == 0) ? empty + 1 : 0;
   }
   capture_flush();              /* persist whatever the final drain recorded */
   g.durable_retry_after_ns = 0; /* make one final attempt regardless of cadence */
   (void)flush_pending_durable();

   /* The ring is empty and everything on it has been handed to a sink. Say so,
    * because stop cannot finish the writer until this is true -- doing it
    * earlier would meet a writer that is already closing and drop exactly the
    * events this drain just rescued. That race is what "written 0, dropped 3"
    * looked like with no settle window. */
   atomic_store_explicit(&g.drain_done, 1, memory_order_release);

   /* KEEP PUMPING UNTIL THE WRITER IS DONE. The ring is empty, but the writer
    * still has queued guardrail events and each one is a module call whose
    * reply only this thread routes. Leaving now would strand exactly the events
    * the drain above just rescued -- they would time out one deadline at a time
    * and be counted dropped, which is the lossy shutdown this phase exists to
    * prevent.
    *
    * Pump only: draining again would find nothing, and handing anything to a
    * writer that is finishing would race its exit. */
   const struct timespec settle = {.tv_sec = 0, .tv_nsec = 200 * 1000}; /* 200 us */
   while (!atomic_load_explicit(&g.consumer_exit, memory_order_acquire))
   {
      pthread_mutex_lock(&g.host_lock);
      if (g.runtime)
         (void)bus_runtime_maintain(g.runtime, bus_runtime_monotonic_ns());
      bus_host_pump(&g.host);
      pthread_mutex_unlock(&g.host_lock);
      nanosleep(&settle, NULL);
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
   cfg.max_slots = 64;   /* three internal clients plus separately shipped modules */
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
   bus_host_set_loss_sink(&g.host, obs_bus_record_loss, NULL);
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
   bus_host_subscribe(&g.host, g.consumer.reply.handle_id, KIND_DURABILITY_EVENT);

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
      const bus_runtime_grant_t *grants = bus_runtime_policy_grants(g.runtime_policy, &grant_count);
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

   /* The writer first, so a guardrail event handed off by the consumer's very
    * first drain has somewhere to go. */
   pthread_mutex_init(&g.writer_lock, NULL);
   pthread_cond_init(&g.writer_ready, NULL);
   pthread_cond_init(&g.writer_drained, NULL);
   g.writer_head = 0;
   g.writer_count = 0;
   atomic_store(&g.writer_finish, 0);
   atomic_store(&g.consumer_exit, 0);
   atomic_store(&g.drain_done, 0);
   if (pthread_create(&g.writer, NULL, guardrail_writer_main, NULL) != 0)
   {
      aimee_log(LOG_ERROR, "obs_bus", "guardrail writer thread failed; events will not be stored");
      pthread_cond_destroy(&g.writer_drained);
      pthread_cond_destroy(&g.writer_ready);
      pthread_mutex_destroy(&g.writer_lock);
      module_clients_destroy();
      bus_client_detach(&g.consumer);
      bus_client_detach(&g.producer);
      bus_host_destroy(&g.host);
      pthread_mutex_destroy(&g.host_lock);
      pthread_mutex_destroy(&g.pub_lock);
      return -1;
   }
   g.writer_running = 1;

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
   pthread_cond_destroy(&g.writer_drained);
   pthread_cond_destroy(&g.writer_ready);
   pthread_mutex_destroy(&g.writer_lock);
   bus_capture_free(&g.capture);
   bus_client_detach(&g.producer);
   bus_client_detach(&g.consumer);
   module_clients_destroy();
   bus_host_destroy(&g.host);
   discard_pending_durable();
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

/* Say WHICH failure, once, where every caller shares it.
 *
 * Both daemons collapse this result to -1 and each caller then reports its own
 * generic line -- "retraction scan gave no answer", "pattern extraction gave no
 * answer", "rerank confidence unavailable", "DB1 pki is unreachable". Those read
 * identically whether the module is absent, the grant does not cover the stage,
 * the deadline passed, or the module rejected the request, so every one of them
 * has had to be diagnosed by guessing. The reason is known here and costs
 * nothing to keep.
 *
 * It belongs in the shared bus rather than in either daemon's adapter: both call
 * through here, and a copy per adapter is a second place to forget. */
static const char *module_call_result_str(aimee_module_call_result_t rc)
{
   switch (rc)
   {
   case AIMEE_MODULE_CALL_OK:
      return "ok";
   case AIMEE_MODULE_CALL_CAPABILITY_ABSENT:
      return "capability_absent";
   case AIMEE_MODULE_CALL_CAPABILITY_DENIED:
      return "capability_denied";
   case AIMEE_MODULE_CALL_CANCELLED:
      return "cancelled";
   case AIMEE_MODULE_CALL_DEADLINE_EXCEEDED:
      return "deadline_exceeded";
   case AIMEE_MODULE_CALL_INVALID_REQUEST:
      return "invalid_request";
   case AIMEE_MODULE_CALL_INTERNAL:
      return "internal";
   case AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE:
      return "response_too_large";
   case AIMEE_MODULE_CALL_TRANSPORT:
      return "transport";
   case AIMEE_MODULE_CALL_PROTOCOL:
      return "protocol";
   case AIMEE_MODULE_CALL_INVALID_ARGUMENT:
      return "invalid_argument";
   }
   /* An out-of-range code is a bug, and a bug must not read as success. */
   return "unknown";
}

static void obs_bus_log_module_call_failure(uint32_t event_kind, uint32_t stage_id,
                                            aimee_module_call_result_t rc)
{
   aimee_log(LOG_WARN, "obs_bus", "module stage call failed: event=%u stage=%u result=%s",
             event_kind, stage_id, module_call_result_str(rc));
}

/* C2's runtime half. The lint gate requires every ledger declaration in
 * process-contracts.json to appear here, and every entry here to remain
 * declared. Keeping the table beside the sole production C->module call seam
 * makes the emitter path mechanically checkable. */
static const struct
{
   uint32_t kind;
   const char *name;
} LEDGER_EVENT_KINDS[] = {
    {4609u, "config.config-store"},
    {5889u, "memory.structured-extraction-indexing"},
    {5890u, "memory.memory-write"},
    {5892u, "memory.candidate-retrieval"},
    {5893u, "memory.reranking"},
    {6145u, "learning.learning-observation"},
    {6401u, "routing.route-selection"},
    {6657u, "delegates.delegate-invocation"},
    {6658u, "delegates.delegate-capability-inference"},
    {6659u, "delegates.delegate-chain-depth"},
    {6660u, "delegates.delegate-named-paths"},
    {6661u, "delegates.delegate-handoff-validation"},
    {6662u, "delegates.delegate-tool-call-rescue"},
    {6663u, "delegates.delegate-verify-outcome"},
    {6664u, "delegates.delegate-economics-report"},
    {6665u, "delegates.delegate-patch-coordination"},
    {6666u, "delegates.delegate-role-policy"},
    {6667u, "delegates.delegate-worktree-plan"},
    {6668u, "delegates.delegate-launch-args"},
    {6669u, "delegates.delegate-image-spec"},
    {6670u, "delegates.delegate-isolation-verdict"},
    {6671u, "delegates.delegate-permissions"},
    {6672u, "delegates.delegate-image-gc"},
    {6673u, "delegates.delegate-route-filter"},
    {6674u, "delegates.delegate-noop-write"},
    {6675u, "delegates.delegate-launch-plan"},
    {6676u, "delegates.delegate-review-evidence"},
    {6677u, "delegates.delegate-named-file-drift"},
    {6678u, "delegates.delegate-group-plan"},
    {6913u, "tools.tool-dispatch"},
    {7169u, "workspace.workspace-access"},
    {7170u, "workspace.workspace-runner"},
    {7171u, "workspace.workspace-runner-io"},
    {7425u, "git.git-operation"},
    {7426u, "git.git-ref-validation"},
    {7427u, "git.git-ci-grade"},
    {7428u, "git.git-forge-request"},
    {7429u, "git.git-credential-resolve"},
    {7430u, "git.git-verify-run"},
    {7682u, "skills.skill-trigger-match"},
    {8449u, "execution-policy.tool-policy-decision"},
    {8961u, "governance.governance-evaluation"},
    {9217u, "workflows.workflow-advance-decision"},
    {9218u, "workflows.workflow-control"},
    {9219u, "workflows.workflow-gate-decision"},
    {9220u, "workflows.workflow-autonomous-route"},
    {9475u, "roundtable.roundtable-chunk-plan"},
    {9729u, "kb-synthesis.kb-grounding-decision"},
    {10241u, "control-web.proxy-route-authorization"},
    {10753u, "sandbox.sandbox-learned-observe"},
    {10754u, "sandbox.sandbox-learned-load"},
    {10755u, "sandbox.sandbox-proxy-request-policy"},
    {10756u, "sandbox.sandbox-proxy-address-policy"},
    {11521u, "db2.db2-lifecycle"},
    {11522u, "db2.db2-tenancy"},
    {11523u, "db2.db2-memory"},
    {11524u, "db2.db2-index"},
    {11525u, "db2.db2-learning"},
    {11526u, "db2.db2-organization"},
    {11527u, "db2.db2-custody"},
    {11528u, "db2.db2-maintenance"},
    {11777u, "aimee.aimee-economizer-state"},
    {11778u, "aimee.aimee-git-ownership"},
    {11779u, "aimee.aimee-conversation"},
    {11780u, "aimee.aimee-agent-work"},
    {11781u, "aimee.aimee-delegation"},
    {11782u, "aimee.aimee-sessions"},
    {11783u, "aimee.aimee-runtime"},
    {11784u, "aimee.aimee-telemetry"},
    {11785u, "aimee.aimee-guardrail-state"},
    {11786u, "aimee.aimee-ensemble"},
    {11787u, "aimee.aimee-workflow"},
    {11788u, "aimee.aimee-roundtable"},
    {11789u, "aimee.aimee-identity"},
    {11790u, "aimee.aimee-checkpoints"},
    {11791u, "aimee.aimee-jti-replay"},
    {11792u, "aimee.aimee-lifecycle"},
    {11793u, "aimee.aimee-mgmt-jwks"},
    {11794u, "aimee.aimee-mgmt-nonce"},
    {11795u, "aimee.aimee-pki"},
    {11796u, "aimee.aimee-peer-delivery"},
    {11797u, "aimee.aimee-peer-inbox"},
    {11798u, "aimee.aimee-peer-grant"},
    {11799u, "aimee.aimee-peer-channel"},
};

/* Sampled declarations use integer parts-per-million so the checked contract
 * has no floating-point ambiguity. The sentinel keeps this valid ISO C while
 * the initial catalog has no sampled kinds; lint requires future declarations
 * to add an exact row here. */
static const struct
{
   uint32_t kind;
   const char *name;
   uint32_t parts_per_million;
} SAMPLED_EVENT_KINDS[] = {{0u, NULL, 0u}};

static int sampled_event_selected(uint32_t kind, uint32_t parts_per_million)
{
   static atomic_uint_least64_t occurrence = 0;
   uint64_t x = atomic_fetch_add_explicit(&occurrence, 1, memory_order_relaxed) +
                ((uint64_t)kind << 32) + 0x9e3779b97f4a7c15ULL;
   /* SplitMix64: stable, cheap distribution without global random state. */
   x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
   x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
   x ^= x >> 31;
   return x % 1000000u < parts_per_million;
}

static const char *durable_event_name(uint32_t kind)
{
   for (size_t i = 0; i < sizeof LEDGER_EVENT_KINDS / sizeof LEDGER_EVENT_KINDS[0]; ++i)
      if (LEDGER_EVENT_KINDS[i].kind == kind)
         return LEDGER_EVENT_KINDS[i].name;
   for (size_t i = 0; i < sizeof SAMPLED_EVENT_KINDS / sizeof SAMPLED_EVENT_KINDS[0]; ++i)
      if (SAMPLED_EVENT_KINDS[i].kind == kind &&
          sampled_event_selected(kind, SAMPLED_EVENT_KINDS[i].parts_per_million))
         return SAMPLED_EVENT_KINDS[i].name;
   return NULL;
}

aimee_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   if (response_len)
      *response_len = 0;
   atomic_fetch_add(&g.module_callers, 1); /* seq_cst: pairs with stop admission gate */
   if (!atomic_load(&g.accepting_calls))
   {
      atomic_fetch_sub(&g.module_callers, 1);
      /* Logged like every other failure below: a caller that reports "no answer"
       * because the bus is shutting down looks exactly like one whose module
       * refused it. */
      obs_bus_log_module_call_failure(event_kind, stage_id, AIMEE_MODULE_CALL_TRANSPORT);
      return AIMEE_MODULE_CALL_TRANSPORT;
   }
   int slot = module_client_acquire(deadline_ns);
   if (slot < 0)
   {
      atomic_fetch_sub(&g.module_callers, 1);
      obs_bus_log_module_call_failure(event_kind, stage_id, AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
      return AIMEE_MODULE_CALL_DEADLINE_EXCEEDED;
   }
   module_cancel_context_t state = {.external = cancelled, .context = cancel_context};
   const char *durable_name = sinks.durable ? durable_event_name(event_kind) : NULL;
   if (durable_name)
   {
      char subject[AB_DUR_SUBJECT], detail[AB_DUR_DETAIL];
      snprintf(subject, sizeof subject, "%s:%u", durable_name, stage_id);
      snprintf(detail, sizeof detail,
               "{\"event_kind\":%u,\"stage_id\":%u,\"trace_id\":%llu,"
               "\"request_len\":%u}",
               event_kind, stage_id, (unsigned long long)trace_id, request_len);
      obs_bus_emit_durable_event("bus.module.request", subject, "intent", detail);
   }
   aimee_module_call_result_t result = aimee_module_client_call(
       &g.module_clients[slot].client, event_kind, stage_id, trace_id, deadline_ns, request_body,
       request_len, response_body, response_capacity, response_len, module_call_cancelled, &state);
   if (durable_name)
   {
      char subject[AB_DUR_SUBJECT], detail[AB_DUR_DETAIL];
      char response_digest[65] = "";
      uint32_t actual_response_len = response_len ? *response_len : 0;
      if (result == AIMEE_MODULE_CALL_OK && response_body && actual_response_len > 0)
         (void)aimee_sha256_hex(response_body, actual_response_len, response_digest);
      snprintf(subject, sizeof subject, "%s:%u", durable_name, stage_id);
      snprintf(detail, sizeof detail,
               "{\"event_kind\":%u,\"stage_id\":%u,\"trace_id\":%llu,"
               "\"response_len\":%u,\"response_sha256\":\"%s\"}",
               event_kind, stage_id, (unsigned long long)trace_id, actual_response_len,
               response_digest);
      obs_bus_emit_durable_event("bus.module.reply", subject, aimee_module_call_result_name(result),
                                 detail);
   }
   module_client_release(slot);
   atomic_fetch_sub(&g.module_callers, 1);
   if (result != AIMEE_MODULE_CALL_OK)
      obs_bus_log_module_call_failure(event_kind, stage_id, result);
   return result;
}

int obs_bus_module_peak_concurrency(void)
{
   pthread_mutex_lock(&g.module_client_lock);
   int peak = g.module_peak_in_flight;
   pthread_mutex_unlock(&g.module_client_lock);
   return peak;
}

/* Is a kind served right now?
 *
 * NO start_lock, AND THAT IS THE WHOLE POINT. This used to take it, which
 * deadlocked the daemon on shutdown the moment a guardrail event was in flight:
 *
 *   obs_bus_stop()  holds start_lock, then waits for module_callers to drain
 *                   and joins the consumer
 *   the consumer    is inside persist_guardrail -> db1_guardrail_event_insert,
 *                   whose first act is to ask HERE whether the kind is served
 *                   -- and blocks on start_lock, which stop is holding
 *
 * Neither moves again. It could not happen while the store was in-process
 * SQLite: persist_guardrail wrote to a local database and made no bus call at
 * all, which is exactly what its comment in obs_bus_adapter.c said. The store
 * becoming a module turned that comment false without touching either file.
 *
 * Registering in module_callers is what makes dropping start_lock safe: stop
 * clears accepting_calls before it waits, so a probe arriving after that
 * returns 0 without touching the host, and one already inside is waited for
 * before the host is torn down. Exactly the discipline obs_bus_module_call
 * uses, for exactly the same reason. */
int obs_bus_module_available(uint32_t event_kind)
{
   int available = 0;
   atomic_fetch_add(&g.module_callers, 1); /* seq_cst: pairs with stop's gate */
   if (atomic_load_explicit(&g.accepting_calls, memory_order_acquire))
   {
      pthread_mutex_lock(&g.host_lock);
      available = bus_host_kind_has_server(&g.host, event_kind);
      pthread_mutex_unlock(&g.host_lock);
   }
   atomic_fetch_sub(&g.module_callers, 1);
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

int obs_bus_set_durable_sink(obs_bus_durable_sink_fn sink, void *ctx)
{
   pthread_mutex_lock(&start_lock);
   if (g.started)
   {
      pthread_mutex_unlock(&start_lock);
      return -1;
   }
   sinks.durable = sink;
   sinks.durable_ctx = sink ? ctx : NULL;
   pthread_mutex_unlock(&start_lock);
   return 0;
}

int obs_bus_test_capture_fault(const char *reason)
{
   static const char *const allowed[] = {"", "no_home", "open_failed", "write_failed",
                                         "sink_broken"};
   const char *value = reason ? reason : "";
   int valid = 0;
   for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; ++i)
      if (strcmp(value, allowed[i]) == 0)
         valid = 1;
   if (!valid || strlen(value) >= sizeof sinks.capture_fault)
      return -1;
   pthread_mutex_lock(&start_lock);
   if (g.started)
   {
      pthread_mutex_unlock(&start_lock);
      return -1;
   }
   snprintf(sinks.capture_fault, sizeof sinks.capture_fault, "%s", value);
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

int obs_bus_configure_daemon_module_runtime(const char *daemon_name, const char *config_directory)
{
   if (!daemon_name || !daemon_name[0] || strchr(daemon_name, '/') || !config_directory ||
       config_directory[0] != '/')
      return -1;
   const char *socket_override = getenv("AIMEE_MODULE_BUS_SOCKET");
   const char *policy_override = getenv("AIMEE_MODULE_POLICY_DIR");
   char socket_path[108], policy_dir[4096];
   int socket_length = socket_override && socket_override[0]
                           ? snprintf(socket_path, sizeof(socket_path), "%s", socket_override)
                           : snprintf(socket_path, sizeof(socket_path), "%s/%s-module-bus.sock",
                                      config_directory, daemon_name);
   int policy_length = policy_override && policy_override[0]
                           ? snprintf(policy_dir, sizeof(policy_dir), "%s", policy_override)
                           : snprintf(policy_dir, sizeof(policy_dir), "%s/modules.d/%s",
                                      config_directory, daemon_name);
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

void obs_bus_emit_durable_event(const char *action, const char *subject, const char *verdict,
                                const char *detail)
{
   if (!sinks.durable)
   {
      aimee_log(LOG_WARN, "obs_bus", "durability event has no configured WORM sink: %s",
                action ? action : "");
      return;
   }
   if (!enter_emit())
   {
      aimee_log(LOG_WARN, "obs_bus", "audit bus unavailable; durability event not recorded");
      return;
   }
   uint8_t buf[2048];
   uint32_t len = serialize_durable(buf, sizeof buf, action, subject, verdict, detail);
   if (len == 0)
   {
      atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
      aimee_log(LOG_WARN, "obs_bus", "durability event too large to serialize; not recorded");
   }
   else
      publish(KIND_DURABILITY_EVENT, buf, len);
   leave_emit();
}

void obs_bus_capture_health(obs_bus_capture_health_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof *out);
   capture_state_t state =
       (capture_state_t)atomic_load_explicit(&g.capture_state, memory_order_acquire);
   out->capture_ok = state == CAPTURE_OK;
   out->reason = capture_state_name(state);
   out->last_seq = atomic_load_explicit(&g.capture_last_seq, memory_order_acquire);
   pthread_mutex_lock(&start_lock);
   snprintf(out->session_id, sizeof out->session_id, "%s", g.capture_session);
   pthread_mutex_unlock(&start_lock);
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
   const struct timespec nap = {.tv_sec = 0, .tv_nsec = 50 * 1000}; /* 50 us */
   while (atomic_load(&g.publishers) > 0)
      nanosleep(&nap, NULL);

   /* Now no producer will touch the ring again — final-drain, then hold.
    *
    * THE CALL PATH IS STILL OPEN HERE, deliberately. The consumer's final drain
    * hands every queued event to the sink, and the sink is a MODULE CALL now:
    * closing accepting_calls before this join made the drain process each event
    * and fail to persist it, so a shutdown discarded whatever was still queued.
    * Measured as "emitted 3, written 0, dropped 3".
    *
    * It was correct while the store was in-process SQLite -- that sink made no
    * module call at all, so the ordering below could not affect it. The store
    * becoming a module is what made this ordering wrong, with nothing in either
    * file changing to say so. */
   atomic_store_explicit(&g.stop, 1, memory_order_release);

   /* WAIT FOR THAT DRAIN TO FINISH before touching the writer. The consumer is
    * still pumping throughout, so nothing is stalled by waiting here. */
   while (!atomic_load_explicit(&g.drain_done, memory_order_acquire))
      nanosleep(&nap, NULL);

   /* The consumer is now pumping without draining.
    * Finish the writer against that still-pumping consumer: its queued calls
    * need their replies routed, and this thread is the only one routing.
    *
    * The ordering is forced from both ends. Finishing the writer BEFORE the
    * drain loses the events the drain hands over; finishing it AFTER the
    * consumer is joined loses them to timeouts. Between the two is the only
    * place it works. */
   guardrail_writer_finish();

   /* Nothing is left to route. Release the consumer and collect it. */
   atomic_store_explicit(&g.consumer_exit, 1, memory_order_release);
   pthread_join(g.thread, NULL);

   /* The drain is over: nothing else will call, so close the path and wait for
    * anything an external caller still has in flight.
    *
    * bus_runtime_stop MOVED HERE TOO, and for the same reason as the line below
    * it. It stops the listener that services attached peers, so a sink call made
    * during the final drain went out and its reply never came back --
    * DEADLINE_EXCEEDED, two seconds at a time, with the events dropped. The
    * comment it carried ("no new process may receive mappings once shutdown
    * begins") is still true here: nothing has been torn down, and the drain that
    * needed the listener has finished. */
   bus_runtime_stop(&g.runtime);
   atomic_store(&g.accepting_calls, 0); /* seq_cst: no new caller can pass re-check */
   atomic_store_explicit(&g.module_stop, 1, memory_order_release);
   while (atomic_load(&g.module_callers) > 0)
      nanosleep(&nap, NULL);

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
   if (g.durable_head)
   {
      uint64_t pending = g.durable_pending_count;
      atomic_fetch_add_explicit(&g.dropped, pending, memory_order_relaxed);
      aimee_log(LOG_ERROR, "obs_bus", "%llu durable records remained unwritten at shutdown",
                (unsigned long long)pending);
      discard_pending_durable();
   }
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
   int i = 0;
   for (; i < 50000; i++) /* ~5 s cap */
   {
      if (atomic_load_explicit(&g.processed, memory_order_acquire) >= target)
         break;
      nanosleep(&nap, NULL);
   }
   /* SECOND WAIT, and the reason this function was not enough on its own. The
    * consumer counts an event `processed` when it DISPATCHES it, and dispatching
    * a guardrail event means putting it on the writer's queue -- the sink is a
    * module call and the consumer may not make one, since it is the thread that
    * routes the reply. So the first loop above proves the handoff happened and
    * says nothing about the write.
    *
    * A caller that emits a guardrail event, flushes, and reads the store back
    * would therefore race the writer and usually lose: the read is a few
    * microseconds away and the module call is milliseconds. That is what
    * obs_bus_flush promises not to do.
    *
    * Bounded like the first loop, and against the SAME budget rather than a
    * fresh one, so a flush cannot take twice as long as its documented cap. */
   pthread_mutex_lock(&g.writer_lock);
   while (i < 50000 && (g.writer_count > 0 || g.writer_busy))
   {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_nsec += nap.tv_nsec;
      if (deadline.tv_nsec >= 1000000000L)
      {
         deadline.tv_sec++;
         deadline.tv_nsec -= 1000000000L;
      }
      pthread_cond_timedwait(&g.writer_drained, &g.writer_lock, &deadline);
      i++;
   }
   pthread_mutex_unlock(&g.writer_lock);
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
