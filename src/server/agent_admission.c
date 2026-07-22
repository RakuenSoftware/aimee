/* agent_admission.c — the single, fail-closed agent admission controller.
 * See agent_admission.h for the model. One mutex guards everything; the data is a few
 * small flat tables (agents/models/contexts/waiters are all O(dozens)), scanned linearly
 * for readability. Fairness: when capacity frees, the highest-priority / oldest ADMITTABLE
 * waiter proceeds (a waiter blocked on its own agent/model cap never head-of-line-blocks a
 * waiter for a different, free agent). */
#include "agent_admission.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ADMIT_MAX_AGENTS   128
#define ADMIT_MAX_MODELS   128
#define ADMIT_MAX_CONTEXTS 512
#define ADMIT_MAX_WAITERS  256
#define ADMIT_NAME_LEN     64
#define ADMIT_CTX_LEN      128

typedef struct
{
   char name[ADMIT_NAME_LEN];
   int active;
} counter_t;

typedef struct
{
   char name[ADMIT_NAME_LEN];
   int limit; /* > 0 */
} model_limit_t;

typedef struct
{
   char ctx[ADMIT_CTX_LEN];
   char agent[ADMIT_NAME_LEN];
   char model[ADMIT_NAME_LEN];
   int refcount;        /* live holders of this context */
   unsigned generation; /* bumped on free; guards stale slot handles */
   int in_use;
} admission_ctx_t;

typedef struct
{
   int ticket;
   int priority;
   int waiting;
   char agent[ADMIT_NAME_LEN];
   char model[ADMIT_NAME_LEN];
   int per_agent_max;
} waiter_t;

struct agent_slot
{
   int ctx_index;
   unsigned generation;
};

static struct
{
   pthread_mutex_t lock;
   pthread_cond_t cond;
   int configured; /* 0 until agent_admission_configure sets a valid global_max */

   int global_max;
   int global_active;
   int default_model_limit;

   counter_t agents[ADMIT_MAX_AGENTS];
   int agent_count;
   model_limit_t model_limits[ADMIT_MAX_MODELS];
   int model_limit_count;
   counter_t models[ADMIT_MAX_MODELS];
   int model_count;

   admission_ctx_t ctxs[ADMIT_MAX_CONTEXTS];
   int next_ticket;
   waiter_t waiters[ADMIT_MAX_WAITERS];
} g = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

/* ---- small flat-table helpers (call with g.lock held) ---------------------- */

static counter_t *counter_find(counter_t *tbl, int n, const char *name)
{
   for (int i = 0; i < n; i++)
      if (strcmp(tbl[i].name, name) == 0)
         return &tbl[i];
   return NULL;
}

/* Find-or-append a counter row; returns NULL only if the table is full. */
static counter_t *counter_intern(counter_t *tbl, int *n, int cap, const char *name)
{
   counter_t *c = counter_find(tbl, *n, name);
   if (c)
      return c;
   if (*n >= cap)
      return NULL;
   c = &tbl[(*n)++];
   snprintf(c->name, sizeof(c->name), "%s", name);
   c->active = 0;
   return c;
}

static int model_limit_for(const char *model)
{
   for (int i = 0; i < g.model_limit_count; i++)
      if (strcmp(g.model_limits[i].name, model) == 0)
         return g.model_limits[i].limit;
   return g.default_model_limit;
}

static int agent_active_locked(const char *agent)
{
   counter_t *c = counter_find(g.agents, g.agent_count, agent);
   return c ? c->active : 0;
}

static int model_active_locked(const char *model)
{
   counter_t *c = counter_find(g.models, g.model_count, model);
   return c ? c->active : 0;
}

/* All three caps admit a NEW context for (agent, model, per_agent_max)? (g.lock held) */
static int caps_admit_locked(const char *agent, const char *model, int per_agent_max)
{
   return g.global_active < g.global_max && agent_active_locked(agent) < per_agent_max &&
          model_active_locked(model) < model_limit_for(model);
}

static admission_ctx_t *ctx_find_locked(const char *ctx_handle)
{
   for (int i = 0; i < ADMIT_MAX_CONTEXTS; i++)
      if (g.ctxs[i].in_use && strcmp(g.ctxs[i].ctx, ctx_handle) == 0)
         return &g.ctxs[i];
   return NULL;
}

/* ---- config -------------------------------------------------------------- */

void agent_admission_configure(int global_max, int default_model_limit,
                               const agent_admission_model_limit_t *overrides, int override_count)
{
   pthread_mutex_lock(&g.lock);
   g.global_max = global_max;
   g.default_model_limit = default_model_limit > 0 ? default_model_limit : 1;
   g.model_limit_count = 0;
   for (int i = 0; i < override_count && g.model_limit_count < ADMIT_MAX_MODELS; i++)
   {
      if (!overrides[i].model[0] || overrides[i].limit <= 0)
         continue;
      model_limit_t *m = &g.model_limits[g.model_limit_count++];
      snprintf(m->name, sizeof(m->name), "%s", overrides[i].model);
      m->limit = overrides[i].limit;
   }
   /* Configured only with a real global ceiling — otherwise stay fail-closed. */
   g.configured = global_max > 0;
   pthread_cond_broadcast(&g.cond); /* new limits may free waiters */
   pthread_mutex_unlock(&g.lock);
}

/* ---- fair blocking ------------------------------------------------------- */

/* Register a waiter; returns its slot index or -1 if the waiter table is full. */
static int waiter_add_locked(int priority, const char *agent, const char *model, int per_agent_max)
{
   for (int i = 0; i < ADMIT_MAX_WAITERS; i++)
   {
      if (g.waiters[i].waiting)
         continue;
      g.waiters[i].waiting = 1;
      g.waiters[i].ticket = g.next_ticket++;
      g.waiters[i].priority = priority;
      snprintf(g.waiters[i].agent, sizeof(g.waiters[i].agent), "%s", agent);
      snprintf(g.waiters[i].model, sizeof(g.waiters[i].model), "%s", model);
      g.waiters[i].per_agent_max = per_agent_max;
      return i;
   }
   return -1;
}

/* Is `me` the waiter that should proceed now: admittable AND best (priority, then oldest
 * ticket) among all currently-admittable waiters? (g.lock held) */
static int waiter_is_next_locked(int me)
{
   const waiter_t *w = &g.waiters[me];
   if (!caps_admit_locked(w->agent, w->model, w->per_agent_max))
      return 0;
   for (int i = 0; i < ADMIT_MAX_WAITERS; i++)
   {
      const waiter_t *o = &g.waiters[i];
      if (i == me || !o->waiting)
         continue;
      if (!caps_admit_locked(o->agent, o->model, o->per_agent_max))
         continue; /* not competing — it can't proceed yet */
      if (o->priority > w->priority || (o->priority == w->priority && o->ticket < w->ticket))
         return 0; /* someone better is also ready */
   }
   return 1;
}

/* ---- acquire / release --------------------------------------------------- */

static agent_slot_t *make_handle(int ctx_index)
{
   agent_slot_t *h = malloc(sizeof(*h));
   if (!h)
      return NULL;
   h->ctx_index = ctx_index;
   h->generation = g.ctxs[ctx_index].generation;
   return h;
}

/* Create+count a brand-new context (g.lock held, caps already checked). */
static agent_slot_t *admit_new_context_locked(const agent_admit_req_t *req)
{
   int idx = -1;
   for (int i = 0; i < ADMIT_MAX_CONTEXTS; i++)
      if (!g.ctxs[i].in_use)
      {
         idx = i;
         break;
      }
   if (idx < 0)
      return NULL; /* table full -> fail closed */
   counter_t *ac = counter_intern(g.agents, &g.agent_count, ADMIT_MAX_AGENTS, req->agent);
   counter_t *mc = counter_intern(g.models, &g.model_count, ADMIT_MAX_MODELS, req->model);
   if (!ac || !mc)
      return NULL; /* counter tables full -> fail closed */

   admission_ctx_t *c = &g.ctxs[idx];
   c->in_use = 1;
   c->refcount = 1;
   snprintf(c->ctx, sizeof(c->ctx), "%s", req->ctx_handle);
   snprintf(c->agent, sizeof(c->agent), "%s", req->agent);
   snprintf(c->model, sizeof(c->model), "%s", req->model);
   g.global_active++;
   ac->active++;
   mc->active++;
   return make_handle(idx);
}

static void set_status(agent_admit_status_t *out, agent_admit_status_t v)
{
   if (out)
      *out = v;
}

agent_slot_t *agent_admission_acquire(const agent_admit_req_t *req, agent_admit_status_t *status)
{
   /* Fail closed on every bad input — never wave a turn through ungated. */
   if (!req || !req->ctx_handle || !req->ctx_handle[0] || !req->agent || !req->agent[0] ||
       !req->model || !req->model[0] || req->per_agent_max <= 0)
   {
      set_status(status, AGENT_ADMIT_INVALID);
      return NULL;
   }

   pthread_mutex_lock(&g.lock);

   if (!g.configured)
   {
      pthread_mutex_unlock(&g.lock);
      set_status(status, AGENT_ADMIT_INVALID);
      return NULL;
   }

   /* Reentrant: a turn WITHIN an already-admitted context reuses its slot, no new count. */
   admission_ctx_t *existing = ctx_find_locked(req->ctx_handle);
   if (existing)
   {
      existing->refcount++;
      agent_slot_t *h = make_handle((int)(existing - g.ctxs));
      pthread_mutex_unlock(&g.lock);
      set_status(status, h ? AGENT_ADMIT_OK : AGENT_ADMIT_INVALID);
      if (!h)
      { /* OOM: undo the refcount we just took */
         pthread_mutex_lock(&g.lock);
         existing->refcount--;
         pthread_mutex_unlock(&g.lock);
      }
      return h;
   }

   /* New context. Try immediately; otherwise reject (non-blocking) or queue fairly. */
   if (caps_admit_locked(req->agent, req->model, req->per_agent_max))
   {
      agent_slot_t *h = admit_new_context_locked(req);
      pthread_mutex_unlock(&g.lock);
      set_status(status, h ? AGENT_ADMIT_OK : AGENT_ADMIT_INVALID);
      return h;
   }
   if (req->flags & AGENT_ADMIT_NONBLOCKING)
   {
      pthread_mutex_unlock(&g.lock);
      set_status(status, AGENT_ADMIT_AT_LIMIT);
      return NULL;
   }

   int me = waiter_add_locked(req->priority, req->agent, req->model, req->per_agent_max);
   if (me < 0)
   {
      pthread_mutex_unlock(&g.lock);
      set_status(status, AGENT_ADMIT_AT_LIMIT); /* waiter table full -> fail closed (reject) */
      return NULL;
   }

   agent_admit_status_t result = AGENT_ADMIT_OK;
   agent_slot_t *handle = NULL;
   for (;;)
   {
      if (waiter_is_next_locked(me))
      {
         handle = admit_new_context_locked(req);
         result = handle ? AGENT_ADMIT_OK : AGENT_ADMIT_INVALID;
         break;
      }
      if (req->cancel_fn && req->cancel_fn(req->cancel_ctx))
      {
         result = AGENT_ADMIT_CANCELLED;
         break;
      }
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 100 * 1000000L; /* 100ms poll so cancel is responsive */
      if (ts.tv_nsec >= 1000000000L)
      {
         ts.tv_sec++;
         ts.tv_nsec -= 1000000000L;
      }
      pthread_cond_timedwait(&g.cond, &g.lock, &ts);
   }
   g.waiters[me].waiting = 0;
   /* We consumed capacity or gave up; let the next-best waiter re-evaluate. */
   pthread_cond_broadcast(&g.cond);
   pthread_mutex_unlock(&g.lock);
   set_status(status, result);
   return handle;
}

void agent_admission_release(agent_slot_t *slot)
{
   if (!slot)
      return;
   pthread_mutex_lock(&g.lock);
   int idx = slot->ctx_index;
   if (idx >= 0 && idx < ADMIT_MAX_CONTEXTS)
   {
      admission_ctx_t *c = &g.ctxs[idx];
      /* Generation guard: a stale handle (context already torn down) is a safe no-op. */
      if (c->in_use && c->generation == slot->generation && c->refcount > 0)
      {
         if (--c->refcount == 0)
         {
            counter_t *ac = counter_find(g.agents, g.agent_count, c->agent);
            counter_t *mc = counter_find(g.models, g.model_count, c->model);
            if (g.global_active > 0)
               g.global_active--;
            if (ac && ac->active > 0)
               ac->active--;
            if (mc && mc->active > 0)
               mc->active--;
            c->in_use = 0;
            c->generation++;
            c->ctx[0] = c->agent[0] = c->model[0] = '\0';
            pthread_cond_broadcast(&g.cond); /* capacity freed */
         }
      }
   }
   pthread_mutex_unlock(&g.lock);
   free(slot);
}

/* ---- inspection ---------------------------------------------------------- */

int agent_admission_global_active(void)
{
   pthread_mutex_lock(&g.lock);
   int v = g.configured ? g.global_active : -1;
   pthread_mutex_unlock(&g.lock);
   return v;
}

int agent_admission_agent_active(const char *agent)
{
   if (!agent || !agent[0])
      return -1;
   pthread_mutex_lock(&g.lock);
   int v = g.configured ? agent_active_locked(agent) : -1;
   pthread_mutex_unlock(&g.lock);
   return v;
}
