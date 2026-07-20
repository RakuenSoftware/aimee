/* openai_runs_store.c: in-process *live* store for /v1/runs records (see header).
 *
 * Each record holds a GET snapshot, an append-only event buffer, a status, and a
 * cancel flag. A single global mutex + condition variable guard every record;
 * subscribers block in openai_runs_store_wait() and the run worker wakes them by
 * broadcasting on every state change. */
#include "openai_runs_store.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OPENAI_RUNS_STORE_MAX 256
#define OPENAI_RUNS_ID_MAX    128

typedef struct
{
   int in_use;
   char id[OPENAI_RUNS_ID_MAX];
   openai_run_status_t status;
   int cancel_requested;
   char *snapshot_json; /* GET /v1/runs/{id} body */
   char **ev_name;      /* event buffer (parallel arrays) */
   char **ev_data;
   int ev_count;
   int ev_cap;
   long seq; /* creation order, for oldest-slot reuse */
} run_record_t;

static run_record_t g_entries[OPENAI_RUNS_STORE_MAX];
static int g_count = 0;
static long g_seq = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_generation_once = PTHREAD_ONCE_INIT;
static char g_generation[48];

static void init_generation(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
   {
      ts.tv_sec = time(NULL);
      ts.tv_nsec = 0;
   }
   snprintf(g_generation, sizeof(g_generation), "g%llx%lx", (unsigned long long)ts.tv_sec,
            (unsigned long)ts.tv_nsec);
}

const char *openai_runs_store_generation(void)
{
   pthread_once(&g_generation_once, init_generation);
   return g_generation;
}

static int all_digits(const char *s, size_t n)
{
   if (!s || n == 0)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (s[i] < '0' || s[i] > '9')
         return 0;
   return 1;
}

openai_runs_missing_t openai_runs_store_classify_missing(const char *run_id)
{
   static const char prefix[] = "oprun_";
   if (!run_id || strncmp(run_id, prefix, sizeof(prefix) - 1) != 0)
      return OPENAI_RUNS_MISSING_UNKNOWN;
   const char *token = run_id + sizeof(prefix) - 1;
   const char *sep1 = strchr(token, '_');
   const char *sep2 = sep1 ? strchr(sep1 + 1, '_') : NULL;
   if (!sep1 || !sep1[1])
      return OPENAI_RUNS_MISSING_UNKNOWN;
   /* Pre-generation ids were oprun_<created>_<seq>. A replacement running this
    * version cannot retain them, so a well-formed legacy handle is interrupted. */
   if (!sep2)
      return all_digits(token, (size_t)(sep1 - token)) &&
                     all_digits(sep1 + 1, strlen(sep1 + 1))
                 ? OPENAI_RUNS_MISSING_INTERRUPTED
                 : OPENAI_RUNS_MISSING_UNKNOWN;
   if (!sep2[1] || strchr(sep2 + 1, '_'))
      return OPENAI_RUNS_MISSING_UNKNOWN;
   if (!all_digits(sep1 + 1, (size_t)(sep2 - sep1 - 1)) ||
       !all_digits(sep2 + 1, strlen(sep2 + 1)))
      return OPENAI_RUNS_MISSING_UNKNOWN;

   size_t token_n = (size_t)(sep1 - token);
   const char *current = openai_runs_store_generation();
   if (token_n == strlen(current) && memcmp(token, current, token_n) == 0)
      return OPENAI_RUNS_MISSING_EVICTED;
   if (token[0] == 'g' && token_n > 1)
      return OPENAI_RUNS_MISSING_INTERRUPTED;
   return OPENAI_RUNS_MISSING_UNKNOWN;
}

const char *openai_run_status_str(openai_run_status_t status)
{
   switch (status)
   {
   case OPENAI_RUN_QUEUED:
      return "queued";
   case OPENAI_RUN_IN_PROGRESS:
      return "in_progress";
   case OPENAI_RUN_COMPLETED:
      return "completed";
   case OPENAI_RUN_CANCELLED:
      return "cancelled";
   case OPENAI_RUN_FAILED:
      return "failed";
   }
   return "queued";
}

int openai_run_status_terminal(openai_run_status_t status)
{
   return status == OPENAI_RUN_COMPLETED || status == OPENAI_RUN_CANCELLED ||
          status == OPENAI_RUN_FAILED;
}

/* Caller must hold g_lock. Returns the entry index for run_id or -1. */
static int find_locked(const char *run_id)
{
   if (!run_id || !run_id[0])
      return -1;
   for (int i = 0; i < g_count; i++)
      if (g_entries[i].in_use && strcmp(g_entries[i].id, run_id) == 0)
         return i;
   return -1;
}

/* Caller must hold g_lock. Free every heap field of an entry and zero it. */
static void free_entry_locked(run_record_t *e)
{
   free(e->snapshot_json);
   e->snapshot_json = NULL;
   for (int i = 0; i < e->ev_count; i++)
   {
      free(e->ev_name[i]);
      free(e->ev_data[i]);
   }
   free(e->ev_name);
   free(e->ev_data);
   e->ev_name = NULL;
   e->ev_data = NULL;
   e->ev_count = 0;
   e->ev_cap = 0;
   e->in_use = 0;
   e->cancel_requested = 0;
   e->id[0] = '\0';
}

int openai_runs_store_create(const char *run_id, const char *run_json)
{
   if (!run_id || !run_id[0] || !run_json || !run_json[0])
      return 0;
   pthread_mutex_lock(&g_lock);
   if (find_locked(run_id) >= 0)
   {
      pthread_mutex_unlock(&g_lock);
      return 0; /* duplicate */
   }
   int slot = -1;
   for (int i = 0; i < g_count; i++)
      if (!g_entries[i].in_use)
      {
         slot = i;
         break;
      }
   if (slot < 0 && g_count < OPENAI_RUNS_STORE_MAX)
      slot = g_count++;
   if (slot < 0)
   {
      /* Full: reuse the oldest (smallest seq) in-use slot. */
      slot = 0;
      for (int i = 1; i < g_count; i++)
         if (g_entries[i].seq < g_entries[slot].seq)
            slot = i;
      free_entry_locked(&g_entries[slot]);
   }
   run_record_t *e = &g_entries[slot];
   e->in_use = 1;
   snprintf(e->id, sizeof(e->id), "%s", run_id);
   e->status = OPENAI_RUN_QUEUED;
   e->cancel_requested = 0;
   e->snapshot_json = strdup(run_json);
   e->ev_name = NULL;
   e->ev_data = NULL;
   e->ev_count = 0;
   e->ev_cap = 0;
   e->seq = ++g_seq;
   pthread_cond_broadcast(&g_cond);
   pthread_mutex_unlock(&g_lock);
   return 1;
}

void openai_runs_store_update_json(const char *run_id, const char *run_json)
{
   if (!run_json)
      return;
   pthread_mutex_lock(&g_lock);
   int i = find_locked(run_id);
   if (i >= 0)
   {
      free(g_entries[i].snapshot_json);
      g_entries[i].snapshot_json = strdup(run_json);
   }
   pthread_mutex_unlock(&g_lock);
}

void openai_runs_store_set_status(const char *run_id, openai_run_status_t status)
{
   pthread_mutex_lock(&g_lock);
   int i = find_locked(run_id);
   if (i >= 0 && !openai_run_status_terminal(g_entries[i].status))
   {
      g_entries[i].status = status;
      pthread_cond_broadcast(&g_cond);
   }
   pthread_mutex_unlock(&g_lock);
}

void openai_runs_store_append_event(const char *run_id, const char *event_name,
                                    const char *data_json)
{
   pthread_mutex_lock(&g_lock);
   int i = find_locked(run_id);
   if (i < 0 || openai_run_status_terminal(g_entries[i].status))
   {
      pthread_mutex_unlock(&g_lock);
      return;
   }
   run_record_t *e = &g_entries[i];
   if (e->ev_count == e->ev_cap)
   {
      int ncap = e->ev_cap ? e->ev_cap * 2 : 16;
      char **nn = realloc(e->ev_name, (size_t)ncap * sizeof(*nn));
      char **nd = realloc(e->ev_data, (size_t)ncap * sizeof(*nd));
      if (!nn || !nd)
      {
         /* realloc may have grown one array; keep whichever succeeded so we do
          * not leak, then bail without appending. */
         if (nn)
            e->ev_name = nn;
         if (nd)
            e->ev_data = nd;
         pthread_mutex_unlock(&g_lock);
         return;
      }
      e->ev_name = nn;
      e->ev_data = nd;
      e->ev_cap = ncap;
   }
   /* Copy the name and the (bounded) data payload. */
   e->ev_name[e->ev_count] = strdup(event_name ? event_name : "");
   size_t dn = data_json ? strnlen(data_json, OPENAI_RUNS_EVENT_MAX) : 0;
   char *dcopy = malloc(dn + 1);
   if (dcopy)
   {
      if (dn)
         memcpy(dcopy, data_json, dn);
      dcopy[dn] = '\0';
   }
   e->ev_data[e->ev_count] = dcopy;
   if (e->ev_name[e->ev_count] && e->ev_data[e->ev_count])
   {
      e->ev_count++;
      pthread_cond_broadcast(&g_cond);
   }
   else
   {
      /* Allocation failed: drop this event cleanly. */
      free(e->ev_name[e->ev_count]);
      free(e->ev_data[e->ev_count]);
   }
   pthread_mutex_unlock(&g_lock);
}

void openai_runs_store_finalize(const char *run_id, openai_run_status_t status,
                                const char *final_run_json)
{
   pthread_mutex_lock(&g_lock);
   int i = find_locked(run_id);
   if (i >= 0)
   {
      if (final_run_json)
      {
         free(g_entries[i].snapshot_json);
         g_entries[i].snapshot_json = strdup(final_run_json);
      }
      g_entries[i].status = status;
      pthread_cond_broadcast(&g_cond);
   }
   pthread_mutex_unlock(&g_lock);
}

int openai_runs_store_request_cancel(const char *run_id)
{
   int ok = 0;
   pthread_mutex_lock(&g_lock);
   int i = find_locked(run_id);
   if (i >= 0 && !openai_run_status_terminal(g_entries[i].status))
   {
      g_entries[i].cancel_requested = 1;
      pthread_cond_broadcast(&g_cond);
      ok = 1;
   }
   pthread_mutex_unlock(&g_lock);
   return ok;
}

int openai_runs_store_cancel_requested(const char *run_id)
{
   int v = 0;
   pthread_mutex_lock(&g_lock);
   int i = find_locked(run_id);
   if (i >= 0)
      v = g_entries[i].cancel_requested;
   pthread_mutex_unlock(&g_lock);
   return v;
}

int openai_runs_store_status(const char *run_id, openai_run_status_t *out)
{
   int found = 0;
   pthread_mutex_lock(&g_lock);
   int i = find_locked(run_id);
   if (i >= 0)
   {
      if (out)
         *out = g_entries[i].status;
      found = 1;
   }
   pthread_mutex_unlock(&g_lock);
   return found;
}

int openai_runs_store_get(const char *run_id, char *out, size_t out_n)
{
   if (out && out_n)
      out[0] = '\0';
   if (!run_id || !run_id[0] || !out || !out_n)
      return 0;
   int found = 0;
   pthread_mutex_lock(&g_lock);
   int i = find_locked(run_id);
   if (i >= 0)
   {
      if (g_entries[i].snapshot_json)
         snprintf(out, out_n, "%s", g_entries[i].snapshot_json);
      found = 1;
   }
   pthread_mutex_unlock(&g_lock);
   return found;
}

openai_runs_wait_t openai_runs_store_wait(const char *run_id, size_t *cursor, int timeout_ms,
                                          char *ev_name, size_t ev_name_n, char *data,
                                          size_t data_n)
{
   if (!cursor)
      return OPENAI_RUNS_WAIT_GONE;

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
      int i = find_locked(run_id);
      if (i < 0)
      {
         pthread_mutex_unlock(&g_lock);
         return OPENAI_RUNS_WAIT_GONE;
      }
      run_record_t *e = &g_entries[i];
      if (*cursor < (size_t)e->ev_count)
      {
         if (ev_name && ev_name_n)
            snprintf(ev_name, ev_name_n, "%s", e->ev_name[*cursor] ? e->ev_name[*cursor] : "");
         if (data && data_n)
            snprintf(data, data_n, "%s", e->ev_data[*cursor] ? e->ev_data[*cursor] : "");
         (*cursor)++;
         pthread_mutex_unlock(&g_lock);
         return OPENAI_RUNS_WAIT_EVENT;
      }
      if (openai_run_status_terminal(e->status))
      {
         pthread_mutex_unlock(&g_lock);
         return OPENAI_RUNS_WAIT_TERMINAL;
      }
      if (timeout_ms <= 0)
      {
         pthread_mutex_unlock(&g_lock);
         return OPENAI_RUNS_WAIT_TIMEOUT;
      }
      int rc = pthread_cond_timedwait(&g_cond, &g_lock, &deadline);
      if (rc == ETIMEDOUT)
      {
         pthread_mutex_unlock(&g_lock);
         return OPENAI_RUNS_WAIT_TIMEOUT;
      }
      /* else: woken (real or spurious) — re-check the record. */
   }
}

void openai_runs_store_reset(void)
{
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < g_count; i++)
      if (g_entries[i].in_use)
         free_entry_locked(&g_entries[i]);
   g_count = 0;
   pthread_cond_broadcast(&g_cond);
   pthread_mutex_unlock(&g_lock);
}
