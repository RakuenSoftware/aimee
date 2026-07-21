/* compute_pool.c: bounded thread pool for long-running server operations */
#include "compute_pool.h"
#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Per-worker stack. The compute turn (agent loop + tool dispatch + detached
 * marshalling) carries large on-stack frames; the glibc pthread default (~8 MB,
 * unaffected by `ulimit -s unlimited`) overflows on deep turns. 32 MB is well
 * clear of the deepest frame and cheap (reserved virtual address space, not
 * committed RAM). */
#define COMPUTE_POOL_THREAD_STACK ((size_t)32 * 1024 * 1024)

/* Per-worker startup descriptor: pool + this worker's slot index. Lives in
 * compute_pool_t::worker_args so it survives the thread's lifetime. */
typedef struct
{
   compute_pool_t *pool;
   int idx;
} worker_arg_t;

/* TLS key holding a pointer to the calling thread's pool_slot_t. */
static pthread_key_t g_slot_key;
static pthread_once_t g_slot_once = PTHREAD_ONCE_INIT;

static void slot_key_init(void)
{
   pthread_key_create(&g_slot_key, NULL);
}

static int64_t monotonic_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static void *worker_thread(void *arg)
{
   worker_arg_t *wa = (worker_arg_t *)arg;
   compute_pool_t *pool = wa->pool;

   pthread_once(&g_slot_once, slot_key_init);
   pthread_setspecific(g_slot_key, &pool->slots[wa->idx]);

   for (;;)
   {
      pthread_mutex_lock(&pool->mutex);

      /* Wait for work or shutdown */
      while (pool->queue_count == 0 && !pool->shutdown)
         pthread_cond_wait(&pool->work_available, &pool->mutex);

      if (pool->shutdown && pool->queue_count == 0)
      {
         pthread_mutex_unlock(&pool->mutex);
         break;
      }

      /* Dequeue work */
      work_item_t item = pool->queue[pool->queue_head];
      pool->queue_head = (pool->queue_head + 1) % COMPUTE_QUEUE_SIZE;
      pool->queue_count--;

      if (pool->high_water_logged &&
          pool->queue_count <= (COMPUTE_QUEUE_SIZE * COMPUTE_QUEUE_LOW_PCT / 100))
         pool->high_water_logged = 0;

      pthread_mutex_unlock(&pool->mutex);

      /* Execute */
      if (item.fn)
         item.fn(item.arg);

      /* Defensive: if the job didn't clear its slot, do it now so the slot
       * doesn't appear active to the next reader. */
      compute_pool_clear_job();
   }

   return NULL;
}

int compute_pool_init(compute_pool_t *pool, int num_threads)
{
   memset(pool, 0, sizeof(*pool));

   if (num_threads < 1)
      num_threads = 1;

   pool->threads = calloc((size_t)num_threads, sizeof(pthread_t));
   pool->slots = calloc((size_t)num_threads, sizeof(pool_slot_t));
   worker_arg_t *args = calloc((size_t)num_threads, sizeof(worker_arg_t));
   if (!pool->threads || !pool->slots || !args)
   {
      free(pool->threads);
      free(pool->slots);
      free(args);
      pool->threads = NULL;
      pool->slots = NULL;
      return -1;
   }
   pool->worker_args = args;

   pthread_mutex_init(&pool->mutex, NULL);
   pthread_cond_init(&pool->work_available, NULL);

   /* Compute-pool workers run the chat/tool turn (agent loop, tool dispatch,
    * detached-provider marshalling), whose call chains carry large on-stack
    * frames. glibc gives a pthread the default stack (commonly 8 MB, and NOT
    * widened by `ulimit -s unlimited`, which sizes only the main thread), so a
    * deep turn overflows it and SIGSEGVs the server when it is not run under
    * systemd's LimitSTACK. Give every worker an explicit, generous stack. */
   pthread_attr_t wattr;
   pthread_attr_t *wattr_p = NULL;
   if (pthread_attr_init(&wattr) == 0)
   {
      if (pthread_attr_setstacksize(&wattr, COMPUTE_POOL_THREAD_STACK) == 0)
         wattr_p = &wattr;
   }

   for (int i = 0; i < num_threads; i++)
   {
      args[i].pool = pool;
      args[i].idx = i;
      if (pthread_create(&pool->threads[i], wattr_p, worker_thread, &args[i]) != 0)
         break;
      pool->thread_count++;
   }

   if (wattr_p)
      pthread_attr_destroy(wattr_p);

   if (pool->thread_count > 0)
      return 0;

   free(pool->threads);
   free(pool->slots);
   free(args);
   pool->threads = NULL;
   pool->slots = NULL;
   pool->worker_args = NULL;
   pthread_mutex_destroy(&pool->mutex);
   pthread_cond_destroy(&pool->work_available);
   return -1;
}

int compute_pool_submit(compute_pool_t *pool, void (*fn)(void *), void *arg)
{
   pthread_mutex_lock(&pool->mutex);

   if (pool->shutdown || pool->queue_count >= COMPUTE_QUEUE_SIZE)
   {
      pthread_mutex_unlock(&pool->mutex);
      return -1;
   }

   pool->queue[pool->queue_tail].fn = fn;
   pool->queue[pool->queue_tail].arg = arg;
   pool->queue_tail = (pool->queue_tail + 1) % COMPUTE_QUEUE_SIZE;
   pool->queue_count++;

   int log_high = 0;
   int log_count = pool->queue_count;
   if (!pool->high_water_logged &&
       pool->queue_count >= (COMPUTE_QUEUE_SIZE * COMPUTE_QUEUE_HIGH_PCT / 100))
   {
      pool->high_water_logged = 1;
      log_high = 1;
   }

   pthread_cond_signal(&pool->work_available);
   pthread_mutex_unlock(&pool->mutex);

   if (log_high)
      LOG_WARN("compute_pool", "queue depth high: %d/%d (>=%d%%)", log_count, COMPUTE_QUEUE_SIZE,
               COMPUTE_QUEUE_HIGH_PCT);
   return 0;
}

void compute_pool_close(compute_pool_t *pool)
{
   pthread_mutex_lock(&pool->mutex);
   pool->shutdown = 1;
   pthread_cond_broadcast(&pool->work_available);
   pthread_mutex_unlock(&pool->mutex);
}

void compute_pool_shutdown(compute_pool_t *pool)
{
   compute_pool_close(pool);

   for (int i = 0; i < pool->thread_count; i++)
      pthread_join(pool->threads[i], NULL);

   free(pool->threads);
   free(pool->slots);
   free(pool->worker_args);
   pool->threads = NULL;
   pool->slots = NULL;
   pool->worker_args = NULL;
   pthread_mutex_destroy(&pool->mutex);
   pthread_cond_destroy(&pool->work_available);
}

/* ---- slot tracking ---- */

void compute_pool_set_job(pool_job_kind_t kind, const char *descriptor_fmt, ...)
{
   pool_slot_t *slot = (pool_slot_t *)pthread_getspecific(g_slot_key);
   if (!slot)
      return;

   char buf[POOL_SLOT_DESCRIPTOR_MAX];
   buf[0] = '\0';
   if (descriptor_fmt && descriptor_fmt[0])
   {
      va_list ap;
      va_start(ap, descriptor_fmt);
      vsnprintf(buf, sizeof(buf), descriptor_fmt, ap);
      va_end(ap);
   }

   /* Worker writes its own slot; the JSON dumper snapshots under the pool
    * mutex. We publish active=1 last with a memory barrier so readers see
    * the descriptor + kind fully populated before active flips. */
   slot->active = 0;
   __sync_synchronize();
   slot->kind = kind;
   slot->started_ms = monotonic_ms();
   strncpy(slot->descriptor, buf, sizeof(slot->descriptor) - 1);
   slot->descriptor[sizeof(slot->descriptor) - 1] = '\0';
   __sync_synchronize();
   slot->active = 1;
}

void compute_pool_clear_job(void)
{
   pool_slot_t *slot = (pool_slot_t *)pthread_getspecific(g_slot_key);
   if (!slot)
      return;
   slot->active = 0;
   __sync_synchronize();
   slot->kind = POOL_JOB_NONE;
   slot->descriptor[0] = '\0';
   slot->started_ms = 0;
}

const char *pool_job_kind_name(pool_job_kind_t kind)
{
   switch (kind)
   {
   case POOL_JOB_DELEGATE:
      return "delegate";
   case POOL_JOB_VERIFY:
      return "verify";
   case POOL_JOB_INGEST:
      return "ingest";
   case POOL_JOB_TOOL:
      return "tool";
   case POOL_JOB_CHAT:
      return "chat";
   case POOL_JOB_KB_CURATOR:
      return "kb_curator";
   case POOL_JOB_OTHER:
      return "other";
   case POOL_JOB_NONE:
   default:
      return "none";
   }
}

int compute_pool_thread_count(const compute_pool_t *pool)
{
   return pool ? pool->thread_count : 0;
}

static void json_append_escaped(char *buf, size_t cap, size_t *pos, const char *s)
{
   if (!s)
      return;
   for (const char *p = s; *p && *pos + 8 < cap; p++)
   {
      unsigned char c = (unsigned char)*p;
      if (c == '"' || c == '\\')
      {
         buf[(*pos)++] = '\\';
         buf[(*pos)++] = (char)c;
      }
      else if (c == '\n')
      {
         buf[(*pos)++] = '\\';
         buf[(*pos)++] = 'n';
      }
      else if (c == '\r')
      {
         buf[(*pos)++] = '\\';
         buf[(*pos)++] = 'r';
      }
      else if (c == '\t')
      {
         buf[(*pos)++] = '\\';
         buf[(*pos)++] = 't';
      }
      else if (c < 0x20)
      {
         continue; /* skip other control chars */
      }
      else
      {
         buf[(*pos)++] = (char)c;
      }
   }
}

char *compute_pool_slots_json(compute_pool_t *pool)
{
   if (!pool)
      return NULL;

   /* Snapshot under the mutex to get a coherent view across slots. */
   pthread_mutex_lock(&pool->mutex);
   int n = pool->thread_count;
   pool_slot_t *snap = calloc((size_t)n, sizeof(pool_slot_t));
   if (!snap)
   {
      pthread_mutex_unlock(&pool->mutex);
      return NULL;
   }
   for (int i = 0; i < n; i++)
      snap[i] = pool->slots[i];
   pthread_mutex_unlock(&pool->mutex);

   /* Worst-case: ~320 bytes per slot (kind + descriptor + escaping). */
   size_t cap = (size_t)n * 320 + 32;
   char *out = malloc(cap);
   if (!out)
   {
      free(snap);
      return NULL;
   }

   int64_t now = monotonic_ms();
   size_t pos = 0;
   out[pos++] = '[';
   for (int i = 0; i < n; i++)
   {
      if (i > 0 && pos + 1 < cap)
         out[pos++] = ',';
      int elapsed = 0;
      if (snap[i].active && snap[i].started_ms > 0)
         elapsed = (int)((now - snap[i].started_ms) / 1000);
      int w =
          snprintf(out + pos, cap - pos,
                   "{\"index\":%d,\"active\":%s,\"kind\":\"%s\","
                   "\"elapsed_secs\":%d,\"descriptor\":\"",
                   i, snap[i].active ? "true" : "false", pool_job_kind_name(snap[i].kind), elapsed);
      if (w < 0 || (size_t)w >= cap - pos)
      {
         free(snap);
         free(out);
         return NULL;
      }
      pos += (size_t)w;
      json_append_escaped(out, cap, &pos, snap[i].descriptor);
      if (pos + 2 >= cap)
      {
         free(snap);
         free(out);
         return NULL;
      }
      out[pos++] = '"';
      out[pos++] = '}';
   }
   if (pos + 1 >= cap)
   {
      free(snap);
      free(out);
      return NULL;
   }
   out[pos++] = ']';
   out[pos] = '\0';

   free(snap);
   return out;
}

/* ---- secondary pool registry ---- */

typedef struct
{
   compute_pool_t *pool;
   char name[COMPUTE_POOL_NAME_MAX];
} compute_pool_secondary_t;

static compute_pool_secondary_t g_secondary_pools[COMPUTE_POOL_REGISTRY_MAX];
static pthread_mutex_t g_secondary_lock = PTHREAD_MUTEX_INITIALIZER;

void compute_pool_register_secondary(compute_pool_t *pool, const char *name)
{
   if (!pool || !name || !name[0])
      return;
   pthread_mutex_lock(&g_secondary_lock);
   /* Replace existing entry for this pool, or take the first empty slot. */
   int slot = -1;
   for (int i = 0; i < COMPUTE_POOL_REGISTRY_MAX; i++)
   {
      if (g_secondary_pools[i].pool == pool)
      {
         slot = i;
         break;
      }
   }
   if (slot < 0)
   {
      for (int i = 0; i < COMPUTE_POOL_REGISTRY_MAX; i++)
      {
         if (g_secondary_pools[i].pool == NULL)
         {
            slot = i;
            break;
         }
      }
   }
   if (slot >= 0)
   {
      g_secondary_pools[slot].pool = pool;
      strncpy(g_secondary_pools[slot].name, name, sizeof(g_secondary_pools[slot].name) - 1);
      g_secondary_pools[slot].name[sizeof(g_secondary_pools[slot].name) - 1] = '\0';
   }
   pthread_mutex_unlock(&g_secondary_lock);
}

void compute_pool_unregister_secondary(compute_pool_t *pool)
{
   if (!pool)
      return;
   pthread_mutex_lock(&g_secondary_lock);
   for (int i = 0; i < COMPUTE_POOL_REGISTRY_MAX; i++)
   {
      if (g_secondary_pools[i].pool == pool)
      {
         g_secondary_pools[i].pool = NULL;
         g_secondary_pools[i].name[0] = '\0';
         break;
      }
   }
   pthread_mutex_unlock(&g_secondary_lock);
}

char *compute_pool_secondary_pools_json(void)
{
   /* Snapshot the registry under the registry lock so entries don't move
    * mid-iteration. The slot dump for each entry then takes its pool's own
    * mutex independently; this means a pool can shut down while we're
    * reading its previous-snapshot entry, but we hold a stable
    * compute_pool_t pointer and the pool's slots array is only freed in
    * compute_pool_shutdown — which is the same path that calls
    * compute_pool_unregister_secondary BEFORE shutdown by convention. */
   pthread_mutex_lock(&g_secondary_lock);
   compute_pool_secondary_t snap[COMPUTE_POOL_REGISTRY_MAX];
   for (int i = 0; i < COMPUTE_POOL_REGISTRY_MAX; i++)
      snap[i] = g_secondary_pools[i];
   pthread_mutex_unlock(&g_secondary_lock);

   /* Compute total worst-case capacity. */
   size_t cap = 64;
   for (int i = 0; i < COMPUTE_POOL_REGISTRY_MAX; i++)
   {
      if (snap[i].pool)
         cap += 96 + (size_t)snap[i].pool->thread_count * 320;
   }
   char *out = malloc(cap);
   if (!out)
      return NULL;

   size_t pos = 0;
   out[pos++] = '[';
   int first = 1;
   for (int i = 0; i < COMPUTE_POOL_REGISTRY_MAX; i++)
   {
      if (!snap[i].pool)
         continue;
      if (!first && pos + 1 < cap)
         out[pos++] = ',';
      first = 0;
      char *slots = compute_pool_slots_json(snap[i].pool);
      int w = snprintf(out + pos, cap - pos, "{\"name\":\"%s\",\"configured\":%d,\"slots\":%s}",
                       snap[i].name, compute_pool_thread_count(snap[i].pool), slots ? slots : "[]");
      free(slots);
      if (w < 0 || (size_t)w >= cap - pos)
      {
         free(out);
         return NULL;
      }
      pos += (size_t)w;
   }
   if (pos + 1 >= cap)
   {
      free(out);
      return NULL;
   }
   out[pos++] = ']';
   out[pos] = '\0';
   return out;
}
