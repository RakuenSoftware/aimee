#include "headers/git_verify.h"
#include "headers/git_verify_jobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VERIFY_JOBS 32

static verify_job_t g_verify_jobs[MAX_VERIFY_JOBS];
static pthread_mutex_t g_jobs_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
   int active;
   char session_id[SERVER_SESSION_ID_MAX];
   volatile int *cancel_requested;
} verify_session_cancel_t;

static verify_session_cancel_t g_session_cancels[MAX_VERIFY_JOBS];
static pthread_mutex_t g_session_cancels_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_next_job_id = 1;

static void verify_job_reset_locked(verify_job_t *job)
{
   if (!job)
      return;
   free(job->output);
   free(job->verdict_json);
   job->id = 0;
   job->active = 0;
   job->cancel_requested = 0;
   job->passed = 0;
   job->failed = 0;
   job->total = 0;
   job->output = NULL;
   job->verdict_json = NULL;
   job->file_hash[0] = '\0';
   job->session_id[0] = '\0';
}

static void verify_reap_finished_locked(const char *session_id)
{
   for (int i = 0; i < MAX_VERIFY_JOBS; i++)
   {
      verify_job_t *job = &g_verify_jobs[i];
      if (job->active != 2)
         continue;
      if (session_id && session_id[0] && strcmp(job->session_id, session_id) != 0)
         continue;
      pthread_mutex_lock(&job->lock);
      verify_job_reset_locked(job);
      pthread_mutex_unlock(&job->lock);
   }
}

static int verify_session_running_locked(const char *session_id)
{
   if (!session_id || !session_id[0])
      return 0;

   for (int i = 0; i < MAX_VERIFY_JOBS; i++)
   {
      verify_job_t *job = &g_verify_jobs[i];
      if (job->active == 1 && strcmp(job->session_id, session_id) == 0)
         return 1;
   }

   for (int i = 0; i < MAX_VERIFY_JOBS; i++)
   {
      verify_session_cancel_t *slot = &g_session_cancels[i];
      if (slot->active && strcmp(slot->session_id, session_id) == 0)
         return 1;
   }

   return 0;
}

verify_job_t *verify_job_get(int id)
{
   pthread_mutex_lock(&g_jobs_lock);
   for (int i = 0; i < MAX_VERIFY_JOBS; i++)
   {
      if (g_verify_jobs[i].active && g_verify_jobs[i].id == id)
      {
         pthread_mutex_unlock(&g_jobs_lock);
         return &g_verify_jobs[i];
      }
   }
   pthread_mutex_unlock(&g_jobs_lock);
   return NULL;
}

static verify_job_t *verify_job_alloc_locked(const char *session_id)
{
   for (int i = 0; i < MAX_VERIFY_JOBS; i++)
   {
      if (!g_verify_jobs[i].active)
      {
         verify_job_t *job = &g_verify_jobs[i];
         if (!job->lock_ready)
         {
            pthread_mutex_init(&job->lock, NULL);
            job->lock_ready = 1;
         }
         job->id = g_next_job_id++;
         job->active = 1;
         if (session_id && session_id[0])
            snprintf(job->session_id, sizeof(job->session_id), "%s", session_id);
         return job;
      }
   }
   return NULL;
}

verify_job_t *verify_job_alloc(void)
{
   pthread_mutex_lock(&g_jobs_lock);
   verify_reap_finished_locked(NULL);
   verify_job_t *job = verify_job_alloc_locked(NULL);
   pthread_mutex_unlock(&g_jobs_lock);
   return job;
}

verify_job_t *verify_job_alloc_for_session(const char *session_id, int *busy)
{
   if (busy)
      *busy = 0;

   pthread_mutex_lock(&g_jobs_lock);
   pthread_mutex_lock(&g_session_cancels_lock);
   verify_reap_finished_locked(NULL);
   if (verify_session_running_locked(session_id))
   {
      if (busy)
         *busy = 1;
      pthread_mutex_unlock(&g_session_cancels_lock);
      pthread_mutex_unlock(&g_jobs_lock);
      return NULL;
   }

   verify_job_t *job = verify_job_alloc_locked(session_id);
   pthread_mutex_unlock(&g_session_cancels_lock);
   pthread_mutex_unlock(&g_jobs_lock);
   return job;
}

void verify_job_release(verify_job_t *job)
{
   if (!job)
      return;

   pthread_mutex_lock(&g_jobs_lock);
   pthread_mutex_lock(&job->lock);
   verify_job_reset_locked(job);
   pthread_mutex_unlock(&job->lock);
   pthread_mutex_unlock(&g_jobs_lock);
}

int verify_register_session_cancel(const char *session_id, volatile int *cancel_requested)
{
   if (!session_id || !session_id[0] || !cancel_requested)
      return 0;

   pthread_mutex_lock(&g_jobs_lock);
   pthread_mutex_lock(&g_session_cancels_lock);
   verify_reap_finished_locked(NULL);
   if (verify_session_running_locked(session_id))
   {
      pthread_mutex_unlock(&g_session_cancels_lock);
      pthread_mutex_unlock(&g_jobs_lock);
      return -1;
   }

   for (int i = 0; i < MAX_VERIFY_JOBS; i++)
   {
      verify_session_cancel_t *slot = &g_session_cancels[i];
      if (!slot->active)
      {
         slot->active = 1;
         snprintf(slot->session_id, sizeof(slot->session_id), "%s", session_id);
         slot->cancel_requested = cancel_requested;
         pthread_mutex_unlock(&g_session_cancels_lock);
         pthread_mutex_unlock(&g_jobs_lock);
         return 1;
      }
   }
   pthread_mutex_unlock(&g_session_cancels_lock);
   pthread_mutex_unlock(&g_jobs_lock);
   return 0;
}

void verify_unregister_session_cancel(volatile int *cancel_requested)
{
   if (!cancel_requested)
      return;

   pthread_mutex_lock(&g_session_cancels_lock);
   for (int i = 0; i < MAX_VERIFY_JOBS; i++)
   {
      verify_session_cancel_t *slot = &g_session_cancels[i];
      if (slot->active && slot->cancel_requested == cancel_requested)
      {
         memset(slot, 0, sizeof(*slot));
         break;
      }
   }
   pthread_mutex_unlock(&g_session_cancels_lock);
}

int verify_session_has_active_job(const char *session_id)
{
   if (!session_id || !session_id[0])
      return 0;
   pthread_mutex_lock(&g_jobs_lock);
   pthread_mutex_lock(&g_session_cancels_lock);
   for (int i = 0; i < MAX_VERIFY_JOBS; i++)
   {
      verify_job_t *job = &g_verify_jobs[i];
      if (job->active == 1 && strcmp(job->session_id, session_id) == 0)
      {
         pthread_mutex_unlock(&g_session_cancels_lock);
         pthread_mutex_unlock(&g_jobs_lock);
         return 1;
      }
   }
   for (int i = 0; i < MAX_VERIFY_JOBS; i++)
   {
      verify_session_cancel_t *slot = &g_session_cancels[i];
      if (slot->active && strcmp(slot->session_id, session_id) == 0)
      {
         pthread_mutex_unlock(&g_session_cancels_lock);
         pthread_mutex_unlock(&g_jobs_lock);
         return 1;
      }
   }
   pthread_mutex_unlock(&g_session_cancels_lock);
   pthread_mutex_unlock(&g_jobs_lock);
   return 0;
}

int verify_cancel_session(const char *session_id)
{
   if (!session_id || !session_id[0])
      return 0;

   int cancelled = 0;
   pthread_mutex_lock(&g_jobs_lock);
   verify_reap_finished_locked(session_id);
   for (int i = 0; i < MAX_VERIFY_JOBS; i++)
   {
      verify_job_t *job = &g_verify_jobs[i];
      if (job->active == 1 && strcmp(job->session_id, session_id) == 0)
      {
         pthread_mutex_lock(&job->lock);
         if (!job->cancel_requested)
         {
            job->cancel_requested = 1;
            cancelled++;
         }
         pthread_mutex_unlock(&job->lock);
      }
   }
   pthread_mutex_unlock(&g_jobs_lock);

   pthread_mutex_lock(&g_session_cancels_lock);
   for (int i = 0; i < MAX_VERIFY_JOBS; i++)
   {
      verify_session_cancel_t *slot = &g_session_cancels[i];
      if (slot->active && slot->cancel_requested && strcmp(slot->session_id, session_id) == 0 &&
          !*slot->cancel_requested)
      {
         *slot->cancel_requested = 1;
         cancelled++;
      }
   }
   pthread_mutex_unlock(&g_session_cancels_lock);

   return cancelled;
}
