/* presence.c: unified-presence registry. See src/headers/presence.h for the
 * model (presence / session / attachment, turn arbitration, workspace leases,
 * presence-event ring, outbound routing).
 *
 * Concurrency model mirrors server/openai_runs_store.c: a single global mutex
 * guards the whole registry and one global condition variable wakes event /
 * turn waiters, which re-check their own session on wake. The tables are fixed
 * and bounded — no per-call allocation on the hot path — and nothing is durable
 * across restarts. The module intentionally depends only on delivery_target so
 * it links into a minimal unit-test binary; the gateway delivery callback is
 * injected (presence_set_delivery_fn), keeping link dependencies out. */
#include "presence.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- internal record types ---------------------------------------------- */

typedef struct
{
   int active; /* slot in use (live or routing-only) */
   int live;   /* has an attached live stream         */
   char attach_id[48];
   char surface[PRESENCE_SURFACE_MAX];
   delivery_target_t target; /* parsed delivery target              */
   int has_target;           /* non-origin deliverable target       */
   unsigned subscribe_mask;
   int persistent;
} pres_attach_t;

typedef struct
{
   int active;
   char workspace_id[PRESENCE_WORKSPACE_MAX];
   char holder[48]; /* attach_id holding the single-writer lease */
} pres_lease_t;

typedef struct
{
   uint64_t seq;
   unsigned cls;
   char name[PRESENCE_EVENT_NAME_MAX];
   char data[PRESENCE_EVENT_DATA_MAX];
} pres_event_t;

typedef struct
{
   int in_use;
   char session_id[PRESENCE_SESSION_ID_MAX];
   char owner[PRESENCE_OWNER_MAX];

   pres_attach_t attach[PRESENCE_ATTACH_MAX];

   /* turn arbitration */
   int turn_in_flight;
   char turn_id[PRESENCE_TURN_ID_MAX];
   char turn_holder[48]; /* attach_id holding the live turn */
   char queue[PRESENCE_QUEUE_MAX][48];
   int queue_len;

   /* workspace single-writer leases */
   pres_lease_t lease[PRESENCE_LEASE_MAX];

   /* presence-event ring (absolute-seq cursors survive ring wrap) */
   pres_event_t ring[PRESENCE_EVENT_RING];
   uint64_t ev_total; /* count of events ever published (next seq) */
   uint64_t ev_base;  /* seq of the oldest retained event          */
} presence_t;

static presence_t g_pres[PRESENCE_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static uint64_t g_id_counter = 0;

static presence_delivery_fn g_delivery_fn = NULL;
static void *g_delivery_user = NULL;

/* ---- small helpers (caller holds g_lock unless noted) -------------------- */

static int find_locked(const char *session_id)
{
   if (!session_id || !session_id[0])
      return -1;
   for (int i = 0; i < PRESENCE_MAX; i++)
      if (g_pres[i].in_use && strcmp(g_pres[i].session_id, session_id) == 0)
         return i;
   return -1;
}

static int alloc_locked(const char *session_id, const char *owner)
{
   for (int i = 0; i < PRESENCE_MAX; i++)
   {
      if (g_pres[i].in_use)
         continue;
      memset(&g_pres[i], 0, sizeof(g_pres[i]));
      g_pres[i].in_use = 1;
      snprintf(g_pres[i].session_id, sizeof(g_pres[i].session_id), "%s", session_id);
      snprintf(g_pres[i].owner, sizeof(g_pres[i].owner), "%s", owner ? owner : "");
      return i;
   }
   return -1;
}

static pres_attach_t *attach_find_locked(presence_t *p, const char *attach_id)
{
   if (!attach_id || !attach_id[0])
      return NULL;
   for (int i = 0; i < PRESENCE_ATTACH_MAX; i++)
      if (p->attach[i].active && strcmp(p->attach[i].attach_id, attach_id) == 0)
         return &p->attach[i];
   return NULL;
}

/* Append an event to the ring and wake waiters. Caller holds g_lock. */
static void publish_locked(presence_t *p, unsigned cls, const char *name, const char *data)
{
   pres_event_t *slot = &p->ring[p->ev_total % PRESENCE_EVENT_RING];
   slot->seq = p->ev_total;
   slot->cls = cls;
   snprintf(slot->name, sizeof(slot->name), "%s", name ? name : "");
   snprintf(slot->data, sizeof(slot->data), "%s", data ? data : "{}");
   p->ev_total++;
   if (p->ev_total - p->ev_base > PRESENCE_EVENT_RING)
      p->ev_base = p->ev_total - PRESENCE_EVENT_RING;
   pthread_cond_broadcast(&g_cond);
}

/* Drop a leased-by / turn-held-by attach_id when it goes away. Caller locked. */
static void release_holdings_locked(presence_t *p, const char *attach_id)
{
   for (int i = 0; i < PRESENCE_LEASE_MAX; i++)
      if (p->lease[i].active && strcmp(p->lease[i].holder, attach_id) == 0)
         p->lease[i].active = 0;
}

/* Remove attach_id from the turn queue (shift the tail down). No-op if absent. */
static void queue_remove_locked(presence_t *p, const char *attach_id)
{
   for (int i = 0; i < p->queue_len; i++)
   {
      if (strcmp(p->queue[i], attach_id) != 0)
         continue;
      for (int j = i + 1; j < p->queue_len; j++)
         memcpy(p->queue[j - 1], p->queue[j], sizeof(p->queue[0]));
      p->queue_len--;
      return;
   }
}

/* True if any slot is still active (live stream or routing-only persistent). */
static int has_any_attachment_locked(const presence_t *p)
{
   for (int i = 0; i < PRESENCE_ATTACH_MAX; i++)
      if (p->attach[i].active)
         return 1;
   return 0;
}

static void teardown_locked(presence_t *p)
{
   p->in_use = 0;
   pthread_cond_broadcast(&g_cond); /* wake its event waiters with GONE */
}

/* Minimal JSON string escaping for the controlled identifiers we emit. */
static void json_append_escaped(char *buf, size_t n, size_t *off, const char *s)
{
   if (!s)
      s = "";
   for (; *s && *off + 2 < n; s++)
   {
      char c = *s;
      if (c == '"' || c == '\\')
      {
         buf[(*off)++] = '\\';
         buf[(*off)++] = c;
      }
      else if ((unsigned char)c < 0x20)
      {
         *off += (size_t)snprintf(buf + *off, n - *off, "\\u%04x", (unsigned char)c);
      }
      else
      {
         buf[(*off)++] = c;
      }
   }
   if (*off < n)
      buf[*off] = '\0';
}

#define J_PUT(...)                                                                                 \
   do                                                                                              \
   {                                                                                               \
      off += (size_t)snprintf(out + off, (off < out_n ? out_n - off : 0), __VA_ARGS__);            \
   } while (0)

/* ====================================================================== */
/* Lifecycle                                                              */
/* ====================================================================== */

void presence_init(void)
{
   pthread_mutex_lock(&g_lock);
   memset(g_pres, 0, sizeof(g_pres));
   g_id_counter = 0;
   pthread_cond_broadcast(&g_cond);
   pthread_mutex_unlock(&g_lock);
}

void presence_shutdown(void)
{
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < PRESENCE_MAX; i++)
      g_pres[i].in_use = 0;
   pthread_cond_broadcast(&g_cond);
   pthread_mutex_unlock(&g_lock);
}

/* ====================================================================== */
/* Attachment registry                                                    */
/* ====================================================================== */

int presence_attach(const char *session_id, const char *owner, const char *surface,
                    const char *target_spec, unsigned subscribe_mask, int persistent,
                    char *out_attach_id, size_t out_n)
{
   if (!session_id || !session_id[0] || !surface || !surface[0])
      return 0;

   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   if (idx < 0)
   {
      idx = alloc_locked(session_id, owner);
      if (idx < 0)
      {
         pthread_mutex_unlock(&g_lock);
         return 0; /* registry full */
      }
   }
   presence_t *p = &g_pres[idx];

   /* Owner reconciliation: the first non-empty owner fixes the presence; a
    * later attach with a conflicting non-empty owner is refused. */
   if (owner && owner[0])
   {
      if (p->owner[0] && strcmp(p->owner, owner) != 0)
      {
         pthread_mutex_unlock(&g_lock);
         return 0; /* owner mismatch */
      }
      if (!p->owner[0])
         snprintf(p->owner, sizeof(p->owner), "%s", owner);
   }

   pres_attach_t *slot = NULL;
   for (int i = 0; i < PRESENCE_ATTACH_MAX; i++)
      if (!p->attach[i].active)
      {
         slot = &p->attach[i];
         break;
      }
   if (!slot)
   {
      if (!has_any_attachment_locked(p))
         teardown_locked(p); /* just-allocated but unusable */
      pthread_mutex_unlock(&g_lock);
      return 0; /* attachment table full */
   }

   memset(slot, 0, sizeof(*slot));
   slot->active = 1;
   slot->live = 1;
   snprintf(slot->attach_id, sizeof(slot->attach_id), "att-%llu",
            (unsigned long long)++g_id_counter);
   snprintf(slot->surface, sizeof(slot->surface), "%s", surface);
   slot->subscribe_mask = subscribe_mask;
   slot->persistent = persistent ? 1 : 0;
   slot->has_target = 0;
   if (target_spec && target_spec[0] && delivery_target_parse(target_spec, &slot->target) == 0 &&
       !delivery_target_is_origin(&slot->target) && slot->target.platform[0] &&
       strcmp(slot->target.platform, "local") != 0)
      slot->has_target = 1;

   if (out_attach_id && out_n)
      snprintf(out_attach_id, out_n, "%s", slot->attach_id);

   pthread_mutex_unlock(&g_lock);
   return 1;
}

int presence_detach(const char *session_id, const char *attach_id)
{
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   if (idx < 0)
   {
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   presence_t *p = &g_pres[idx];
   pres_attach_t *slot = attach_find_locked(p, attach_id);
   if (!slot)
   {
      pthread_mutex_unlock(&g_lock);
      return 0;
   }

   /* If this attachment held the live turn, release it (promotes the queue). */
   if (p->turn_in_flight && strcmp(p->turn_holder, attach_id) == 0)
   {
      release_holdings_locked(p, attach_id);
      char data[96];
      snprintf(data, sizeof(data), "{\"turn_id\":\"%s\"}", p->turn_id);
      publish_locked(p, PRESENCE_EV_TURN, "turn_done", data);
      p->turn_in_flight = 0;
      p->turn_id[0] = '\0';
      p->turn_holder[0] = '\0';
   }
   release_holdings_locked(p, attach_id);

   slot->live = 0;
   /* A persistent delivery target keeps receiving routed events after its live
    * stream detaches; a plain attachment's slot is freed outright. */
   if (slot->persistent && slot->has_target)
      slot->live = 0; /* routing-only: keep active=1 */
   else
      slot->active = 0;

   if (!has_any_attachment_locked(p))
      teardown_locked(p);
   else
      pthread_cond_broadcast(&g_cond);

   pthread_mutex_unlock(&g_lock);
   return 1;
}

int presence_attachment_count(const char *session_id)
{
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   int n = 0;
   if (idx >= 0)
   {
      presence_t *p = &g_pres[idx];
      for (int i = 0; i < PRESENCE_ATTACH_MAX; i++)
         if (p->attach[i].active && p->attach[i].live)
            n++;
   }
   pthread_mutex_unlock(&g_lock);
   return n;
}

int presence_session_close(const char *session_id)
{
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   if (idx < 0)
   {
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   /* teardown_locked clears in_use and broadcasts; waiters in presence_wait
    * re-check find_locked, miss, and return PRESENCE_WAIT_GONE. Zeroing on the
    * next alloc handles the rest of the slot state. */
   teardown_locked(&g_pres[idx]);
   pthread_mutex_unlock(&g_lock);
   return 1;
}

int presence_list_json(const char *owner, char *out, size_t out_n)
{
   size_t off = 0;
   int count = 0;
   pthread_mutex_lock(&g_lock);
   J_PUT("[");
   for (int i = 0; i < PRESENCE_MAX; i++)
   {
      presence_t *p = &g_pres[i];
      if (!p->in_use)
         continue;
      if (owner && owner[0] && strcmp(p->owner, owner) != 0)
         continue;
      int live = 0;
      for (int a = 0; a < PRESENCE_ATTACH_MAX; a++)
         if (p->attach[a].active && p->attach[a].live)
            live++;
      J_PUT("%s{\"session_id\":\"", count ? "," : "");
      json_append_escaped(out, out_n, &off, p->session_id);
      J_PUT("\",\"owner\":\"");
      json_append_escaped(out, out_n, &off, p->owner);
      J_PUT("\",\"attachments\":%d,\"turn_in_flight\":%s,\"turn_id\":\"%s\"}", live,
            p->turn_in_flight ? "true" : "false", p->turn_id);
      count++;
   }
   J_PUT("]");
   pthread_mutex_unlock(&g_lock);
   if (out && out_n && off >= out_n)
      out[out_n - 1] = '\0';
   return count;
}

int presence_session_json(const char *session_id, char *out, size_t out_n)
{
   size_t off = 0;
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   if (idx < 0)
   {
      pthread_mutex_unlock(&g_lock);
      J_PUT("{\"error\":\"no such session\"}");
      return 0;
   }
   presence_t *p = &g_pres[idx];
   J_PUT("{\"session_id\":\"");
   json_append_escaped(out, out_n, &off, p->session_id);
   J_PUT("\",\"owner\":\"");
   json_append_escaped(out, out_n, &off, p->owner);
   J_PUT("\",\"turn_in_flight\":%s,\"turn_id\":\"%s\",\"attachments\":[",
         p->turn_in_flight ? "true" : "false", p->turn_id);
   int first = 1;
   for (int i = 0; i < PRESENCE_ATTACH_MAX; i++)
   {
      pres_attach_t *a = &p->attach[i];
      if (!a->active)
         continue;
      char tbuf[256];
      tbuf[0] = '\0';
      if (a->has_target)
         delivery_target_format(&a->target, tbuf, sizeof(tbuf));
      J_PUT("%s{\"attach_id\":\"%s\",\"surface\":\"", first ? "" : ",", a->attach_id);
      json_append_escaped(out, out_n, &off, a->surface);
      J_PUT("\",\"target\":\"");
      json_append_escaped(out, out_n, &off, tbuf);
      J_PUT("\",\"subscribe_mask\":%u,\"persistent\":%s,\"live\":%s}", a->subscribe_mask,
            a->persistent ? "true" : "false", a->live ? "true" : "false");
      first = 0;
   }
   J_PUT("]}");
   pthread_mutex_unlock(&g_lock);
   if (out && out_n && off >= out_n)
      out[out_n - 1] = '\0';
   return 1;
}

int presence_session_owner(const char *session_id, char *out, size_t out_n)
{
   if (out && out_n)
      out[0] = '\0';
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   if (idx < 0 || !g_pres[idx].owner[0])
   {
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   if (out && out_n)
      snprintf(out, out_n, "%s", g_pres[idx].owner);
   pthread_mutex_unlock(&g_lock);
   return 1;
}

/* ====================================================================== */
/* Turn arbitration                                                       */
/* ====================================================================== */

presence_turn_result_t presence_turn_acquire(const char *session_id, const char *attach_id,
                                             int want_queue, char *out_turn_id, size_t id_n,
                                             char *out_inflight_turn_id, size_t infl_n,
                                             int *out_position)
{
   if (!attach_id || !attach_id[0])
      return PRESENCE_TURN_ERR;

   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   if (idx < 0)
   {
      pthread_mutex_unlock(&g_lock);
      return PRESENCE_TURN_ERR;
   }
   presence_t *p = &g_pres[idx];
   if (!attach_find_locked(p, attach_id))
   {
      pthread_mutex_unlock(&g_lock);
      return PRESENCE_TURN_ERR;
   }

   int caller_is_head = (p->queue_len > 0 && strcmp(p->queue[0], attach_id) == 0);

   if (p->turn_in_flight || (p->queue_len > 0 && !caller_is_head))
   {
      /* A turn is in flight, or the caller is not yet at the head of the
       * waiting queue — it cannot jump the line. */
      if (want_queue && p->queue_len < PRESENCE_QUEUE_MAX)
      {
         /* Idempotent: do not double-enqueue the same attachment. */
         int already = 0;
         for (int i = 0; i < p->queue_len; i++)
            if (strcmp(p->queue[i], attach_id) == 0)
            {
               already = 1;
               if (out_position)
                  *out_position = i + 1;
               break;
            }
         if (!already)
         {
            snprintf(p->queue[p->queue_len], sizeof(p->queue[0]), "%s", attach_id);
            p->queue_len++;
            if (out_position)
               *out_position = p->queue_len;
         }
         pthread_mutex_unlock(&g_lock);
         return PRESENCE_TURN_QUEUED;
      }
      if (out_inflight_turn_id && infl_n)
         snprintf(out_inflight_turn_id, infl_n, "%s", p->turn_id);
      pthread_mutex_unlock(&g_lock);
      return PRESENCE_TURN_BUSY;
   }

   /* Turn is free and the caller is permitted (queue empty, or caller is the
    * head). Consume the head slot if it is the caller, then acquire. */
   if (caller_is_head)
   {
      for (int i = 1; i < p->queue_len; i++)
         memcpy(p->queue[i - 1], p->queue[i], sizeof(p->queue[0]));
      p->queue_len--;
   }

   snprintf(p->turn_id, sizeof(p->turn_id), "turn-%llu", (unsigned long long)++g_id_counter);
   snprintf(p->turn_holder, sizeof(p->turn_holder), "%s", attach_id);
   p->turn_in_flight = 1;
   if (out_turn_id && id_n)
      snprintf(out_turn_id, id_n, "%s", p->turn_id);

   char data[96];
   snprintf(data, sizeof(data), "{\"turn_id\":\"%s\"}", p->turn_id);
   publish_locked(p, PRESENCE_EV_TURN, "turn_started", data);

   pthread_mutex_unlock(&g_lock);
   return PRESENCE_TURN_ACQUIRED;
}

presence_turn_result_t presence_turn_acquire_wait(const char *session_id, const char *attach_id,
                                                  int timeout_ms, char *out_turn_id, size_t id_n,
                                                  char *out_inflight_turn_id, size_t infl_n)
{
   if (!attach_id || !attach_id[0])
      return PRESENCE_TURN_ERR;

   struct timespec deadline;
   clock_gettime(CLOCK_REALTIME, &deadline);
   if (timeout_ms > 0)
   {
      deadline.tv_sec += timeout_ms / 1000;
      deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
      if (deadline.tv_nsec >= 1000000000L)
      {
         deadline.tv_sec += 1;
         deadline.tv_nsec -= 1000000000L;
      }
   }

   pthread_mutex_lock(&g_lock);
   for (;;)
   {
      /* Re-resolve the presence each iteration: a wakeup may follow a teardown
       * that freed/reused the slot. */
      int idx = find_locked(session_id);
      if (idx < 0)
      {
         pthread_mutex_unlock(&g_lock);
         return PRESENCE_TURN_ERR;
      }
      presence_t *p = &g_pres[idx];
      if (!attach_find_locked(p, attach_id))
      {
         pthread_mutex_unlock(&g_lock);
         return PRESENCE_TURN_ERR;
      }

      int caller_is_head = (p->queue_len > 0 && strcmp(p->queue[0], attach_id) == 0);

      /* Acquirable now: turn free and the caller is permitted (queue empty or
       * the caller is the head). Same fairness rule as presence_turn_acquire. */
      if (!p->turn_in_flight && (p->queue_len == 0 || caller_is_head))
      {
         if (caller_is_head)
         {
            for (int i = 1; i < p->queue_len; i++)
               memcpy(p->queue[i - 1], p->queue[i], sizeof(p->queue[0]));
            p->queue_len--;
         }
         snprintf(p->turn_id, sizeof(p->turn_id), "turn-%llu", (unsigned long long)++g_id_counter);
         snprintf(p->turn_holder, sizeof(p->turn_holder), "%s", attach_id);
         p->turn_in_flight = 1;
         if (out_turn_id && id_n)
            snprintf(out_turn_id, id_n, "%s", p->turn_id);
         char data[96];
         snprintf(data, sizeof(data), "{\"turn_id\":\"%s\"}", p->turn_id);
         publish_locked(p, PRESENCE_EV_TURN, "turn_started", data);
         pthread_mutex_unlock(&g_lock);
         return PRESENCE_TURN_ACQUIRED;
      }

      /* Not acquirable. Without a wait budget, report busy immediately. */
      if (timeout_ms <= 0)
      {
         if (out_inflight_turn_id && infl_n)
            snprintf(out_inflight_turn_id, infl_n, "%s", p->turn_id);
         pthread_mutex_unlock(&g_lock);
         return PRESENCE_TURN_BUSY;
      }

      /* Take a place in line (idempotent), then wait for a release/teardown to
       * wake us. The wait happens on the caller's own (handler) thread, never a
       * session-pool thread, so it cannot block the turn holder → no deadlock. */
      int already = 0;
      for (int i = 0; i < p->queue_len; i++)
         if (strcmp(p->queue[i], attach_id) == 0)
         {
            already = 1;
            break;
         }
      if (!already)
      {
         if (p->queue_len >= PRESENCE_QUEUE_MAX)
         {
            if (out_inflight_turn_id && infl_n)
               snprintf(out_inflight_turn_id, infl_n, "%s", p->turn_id);
            pthread_mutex_unlock(&g_lock);
            return PRESENCE_TURN_BUSY; /* queue full */
         }
         snprintf(p->queue[p->queue_len], sizeof(p->queue[0]), "%s", attach_id);
         p->queue_len++;
      }

      int rc = pthread_cond_timedwait(&g_cond, &g_lock, &deadline);
      if (rc == ETIMEDOUT)
      {
         int j = find_locked(session_id);
         if (j >= 0)
         {
            queue_remove_locked(&g_pres[j], attach_id);
            if (out_inflight_turn_id && infl_n)
               snprintf(out_inflight_turn_id, infl_n, "%s", g_pres[j].turn_id);
         }
         pthread_mutex_unlock(&g_lock);
         return PRESENCE_TURN_BUSY;
      }
      /* Woken (release/publish broadcast, or spurious) — loop and re-check. */
   }
}

int presence_turn_release(const char *session_id, const char *turn_id)
{
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   if (idx < 0)
   {
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   presence_t *p = &g_pres[idx];
   if (!p->turn_in_flight || !turn_id || strcmp(p->turn_id, turn_id) != 0)
   {
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   release_holdings_locked(p, p->turn_holder);
   char data[96];
   snprintf(data, sizeof(data), "{\"turn_id\":\"%s\"}", p->turn_id);
   publish_locked(p, PRESENCE_EV_TURN, "turn_done", data);
   p->turn_in_flight = 0;
   p->turn_id[0] = '\0';
   p->turn_holder[0] = '\0';
   pthread_cond_broadcast(&g_cond); /* wake the queue head so it can acquire */
   pthread_mutex_unlock(&g_lock);
   return 1;
}

int presence_turn_is_live(const char *session_id, const char *turn_id)
{
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   int live = 0;
   if (idx >= 0 && g_pres[idx].turn_in_flight && turn_id && turn_id[0] &&
       strcmp(g_pres[idx].turn_id, turn_id) == 0)
      live = 1;
   pthread_mutex_unlock(&g_lock);
   return live;
}

/* ====================================================================== */
/* Workspace single-writer leases                                         */
/* ====================================================================== */

int presence_workspace_lease_acquire(const char *session_id, const char *workspace_id,
                                     const char *attach_id)
{
   if (!workspace_id || !workspace_id[0] || !attach_id || !attach_id[0])
      return 0;
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   if (idx < 0)
   {
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   presence_t *p = &g_pres[idx];
   int freeslot = -1;
   for (int i = 0; i < PRESENCE_LEASE_MAX; i++)
   {
      if (p->lease[i].active && strcmp(p->lease[i].workspace_id, workspace_id) == 0)
      {
         int ok = strcmp(p->lease[i].holder, attach_id) == 0; /* reentrant */
         pthread_mutex_unlock(&g_lock);
         return ok ? 1 : 0;
      }
      if (!p->lease[i].active && freeslot < 0)
         freeslot = i;
   }
   if (freeslot < 0)
   {
      pthread_mutex_unlock(&g_lock);
      return 0; /* lease table full */
   }
   p->lease[freeslot].active = 1;
   snprintf(p->lease[freeslot].workspace_id, sizeof(p->lease[freeslot].workspace_id), "%s",
            workspace_id);
   snprintf(p->lease[freeslot].holder, sizeof(p->lease[freeslot].holder), "%s", attach_id);
   pthread_mutex_unlock(&g_lock);
   return 1;
}

int presence_workspace_lease_release(const char *session_id, const char *workspace_id,
                                     const char *attach_id)
{
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   int released = 0;
   if (idx >= 0)
   {
      presence_t *p = &g_pres[idx];
      for (int i = 0; i < PRESENCE_LEASE_MAX; i++)
         if (p->lease[i].active && strcmp(p->lease[i].workspace_id, workspace_id) == 0 &&
             strcmp(p->lease[i].holder, attach_id ? attach_id : "") == 0)
         {
            p->lease[i].active = 0;
            released = 1;
            break;
         }
   }
   pthread_mutex_unlock(&g_lock);
   return released;
}

int presence_workspace_lease_holder(const char *session_id, const char *workspace_id, char *out,
                                    size_t out_n)
{
   if (out && out_n)
      out[0] = '\0';
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   int held = 0;
   if (idx >= 0)
   {
      presence_t *p = &g_pres[idx];
      for (int i = 0; i < PRESENCE_LEASE_MAX; i++)
         if (p->lease[i].active && strcmp(p->lease[i].workspace_id, workspace_id) == 0)
         {
            if (out && out_n)
               snprintf(out, out_n, "%s", p->lease[i].holder);
            held = 1;
            break;
         }
   }
   pthread_mutex_unlock(&g_lock);
   return held;
}

/* ====================================================================== */
/* Presence-event ring + SSE wait/publish                                 */
/* ====================================================================== */

void presence_publish(const char *session_id, presence_event_class_t cls, const char *event_name,
                      const char *data_json)
{
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   if (idx >= 0)
      publish_locked(&g_pres[idx], (unsigned)cls, event_name, data_json);
   pthread_mutex_unlock(&g_lock);
}

presence_wait_t presence_wait(const char *session_id, uint64_t *cursor, int timeout_ms, char *ev,
                              size_t ev_n, char *data, size_t data_n)
{
   if (!cursor)
      return PRESENCE_WAIT_GONE;

   struct timespec deadline;
   clock_gettime(CLOCK_REALTIME, &deadline);
   if (timeout_ms > 0)
   {
      deadline.tv_sec += timeout_ms / 1000;
      deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
      if (deadline.tv_nsec >= 1000000000L)
      {
         deadline.tv_sec += 1;
         deadline.tv_nsec -= 1000000000L;
      }
   }

   pthread_mutex_lock(&g_lock);
   for (;;)
   {
      int idx = find_locked(session_id);
      if (idx < 0)
      {
         pthread_mutex_unlock(&g_lock);
         return PRESENCE_WAIT_GONE;
      }
      presence_t *p = &g_pres[idx];
      if (*cursor < p->ev_base)
         *cursor = p->ev_base; /* skip events already evicted from the ring */
      if (*cursor < p->ev_total)
      {
         pres_event_t *e = &p->ring[*cursor % PRESENCE_EVENT_RING];
         if (ev && ev_n)
            snprintf(ev, ev_n, "%s", e->name);
         if (data && data_n)
            snprintf(data, data_n, "%s", e->data);
         (*cursor)++;
         pthread_mutex_unlock(&g_lock);
         return PRESENCE_WAIT_EVENT;
      }
      if (timeout_ms <= 0)
      {
         pthread_mutex_unlock(&g_lock);
         return PRESENCE_WAIT_TIMEOUT;
      }
      int rc = pthread_cond_timedwait(&g_cond, &g_lock, &deadline);
      if (rc == ETIMEDOUT)
      {
         pthread_mutex_unlock(&g_lock);
         return PRESENCE_WAIT_TIMEOUT;
      }
      /* woken (real or spurious) — re-check */
   }
}

void presence_emit_turn_started(const char *session_id, const char *turn_id)
{
   char d[96];
   snprintf(d, sizeof(d), "{\"turn_id\":\"%s\"}", turn_id ? turn_id : "");
   presence_publish(session_id, PRESENCE_EV_TURN, "turn_started", d);
}

void presence_emit_turn_delta(const char *session_id, const char *turn_id, const char *delta_json)
{
   (void)turn_id;
   presence_publish(session_id, PRESENCE_EV_TURN, "turn_delta", delta_json ? delta_json : "{}");
}

void presence_emit_turn_done(const char *session_id, const char *turn_id)
{
   char d[96];
   snprintf(d, sizeof(d), "{\"turn_id\":\"%s\"}", turn_id ? turn_id : "");
   presence_publish(session_id, PRESENCE_EV_TURN, "turn_done", d);
}

void presence_emit_busy(const char *session_id, const char *inflight_turn_id)
{
   char d[96];
   snprintf(d, sizeof(d), "{\"turn_id\":\"%s\"}", inflight_turn_id ? inflight_turn_id : "");
   presence_publish(session_id, PRESENCE_EV_TURN, "busy", d);
}

/* ====================================================================== */
/* Outbound routing                                                       */
/* ====================================================================== */

void presence_set_delivery_fn(presence_delivery_fn fn, void *user)
{
   pthread_mutex_lock(&g_lock);
   g_delivery_fn = fn;
   g_delivery_user = user;
   pthread_mutex_unlock(&g_lock);
}

int presence_route_event(const char *session_id, presence_event_class_t cls, const char *event_name,
                         const char *text)
{
   /* Snapshot the subscribed targets under the lock, then dispatch outside it
    * (never call the delivery callback while holding g_lock). */
   delivery_target_t targets[PRESENCE_ATTACH_MAX];
   int ntargets = 0;
   presence_delivery_fn fn = NULL;
   void *user = NULL;

   pthread_mutex_lock(&g_lock);
   int idx = find_locked(session_id);
   if (idx < 0)
   {
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   presence_t *p = &g_pres[idx];

   /* Publish to the live ring so any open SSE stream sees the routed event. */
   {
      char data[PRESENCE_EVENT_DATA_MAX];
      size_t off = 0;
      char *out = data;
      size_t out_n = sizeof(data);
      J_PUT("{\"event\":\"");
      json_append_escaped(out, out_n, &off, event_name);
      J_PUT("\",\"text\":\"");
      json_append_escaped(out, out_n, &off, text);
      J_PUT("\"}");
      publish_locked(p, (unsigned)cls, event_name, data);
   }

   fn = g_delivery_fn;
   user = g_delivery_user;
   for (int i = 0; i < PRESENCE_ATTACH_MAX && ntargets < PRESENCE_ATTACH_MAX; i++)
   {
      pres_attach_t *a = &p->attach[i];
      if (!a->active || !a->has_target)
         continue;
      if (!(a->subscribe_mask & (unsigned)cls))
         continue;
      /* Persistent targets receive even after their live stream detached;
       * non-persistent targets only while still live. */
      if (!a->persistent && !a->live)
         continue;
      targets[ntargets++] = a->target;
   }
   pthread_mutex_unlock(&g_lock);

   if (!fn)
      return 0;
   int dispatched = 0;
   for (int i = 0; i < ntargets; i++)
      if (fn(&targets[i], text ? text : "", user) == 0)
         dispatched++;
   return dispatched;
}
