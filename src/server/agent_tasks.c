/* Durable job context for the active canonical execution owners. */
#include "agent_tasks.h"
#include "db1_client/agent_jobs.h"
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

struct agent_job_progress
{
   int job_id, transport_attempts;
   atomic_uint references;
   pthread_mutex_t mutex;
};

static _Thread_local agent_job_progress_t *g_progress;
static _Thread_local wire_fence_transport_observer_t previous_transport;
static _Thread_local int transport_bound;
static void progress_transport_completed(void *context, int status)
{
   (void)status;
   /* The callback context is the same retained owner as the current job TLS. */
   if (context == g_progress)
      agent_job_progress_observe_transport(1);
}

static void progress_retain(agent_job_progress_t *progress)
{
   if (progress)
      atomic_fetch_add(&progress->references, 1);
}

void agent_job_progress_release(agent_job_progress_t *progress)
{
   if (progress && atomic_fetch_sub(&progress->references, 1) == 1)
   {
      pthread_mutex_destroy(&progress->mutex);
      free(progress);
   }
}

int agent_durable_job_begin(int job_id)
{
   if (g_progress && g_progress->job_id == job_id)
      return 0;
   agent_job_progress_t *next = NULL;
   if (job_id > 0)
   {
      next = calloc(1, sizeof *next);
      if (!next)
         return -1;
      db1_agent_job_t stored = {0};
      if (db1_agent_job_get(job_id, &stored) != 0 || stored.id != job_id ||
          stored.api_call_count < 0)
      {
         db1_agent_job_free(&stored);
         free(next);
         return -1;
      }
      next->transport_attempts = stored.api_call_count;
      db1_agent_job_free(&stored);
      next->job_id = job_id;
      atomic_init(&next->references, 1);
      if (pthread_mutex_init(&next->mutex, NULL))
      {
         free(next);
         return -1;
      }
   }
   if (!transport_bound && next)
   {
      previous_transport = wire_fence_transport_observer_bind(
          (wire_fence_transport_observer_t){progress_transport_completed, next});
      transport_bound = 1;
   }
   else if (next)
      (void)wire_fence_transport_observer_bind(
          (wire_fence_transport_observer_t){progress_transport_completed, next});
   else if (transport_bound)
   {
      (void)wire_fence_transport_observer_bind(previous_transport);
      transport_bound = 0;
   }
   agent_job_progress_release(g_progress);
   g_progress = next;
   return 0;
}

void agent_set_durable_job(int job_id)
{
   (void)agent_durable_job_begin(job_id);
}

int agent_get_durable_job_id(void)
{
   return g_progress ? g_progress->job_id : 0;
}

agent_job_progress_t *agent_job_progress_capture(void)
{
   progress_retain(g_progress);
   return g_progress;
}

agent_job_progress_binding_t agent_job_progress_bind(agent_job_progress_t *progress)
{
   agent_job_progress_binding_t previous = {
       .previous = g_progress,
       .previous_transport = previous_transport,
       .transport_bound = transport_bound,
       .observer = wire_fence_transport_observer_bind((wire_fence_transport_observer_t){
           progress ? progress_transport_completed : NULL, progress})};
   previous_transport = previous.observer;
   transport_bound = progress != NULL;
   progress_retain(progress);
   g_progress = progress;
   return previous;
}

void agent_job_progress_restore(agent_job_progress_binding_t previous)
{
   agent_job_progress_release(g_progress);
   g_progress = previous.previous;
   previous_transport = previous.previous_transport;
   transport_bound = previous.transport_bound;
   (void)wire_fence_transport_observer_bind(previous.observer);
}

void agent_job_progress_heartbeat(const char *current_tool)
{
   if (!g_progress)
      return;
   pthread_mutex_lock(&g_progress->mutex);
   db1_agent_job_heartbeat_ext(g_progress->job_id, current_tool, g_progress->transport_attempts);
   pthread_mutex_unlock(&g_progress->mutex);
}

void agent_job_progress_observe_transport(int attempts)
{
   if (!g_progress || attempts <= 0)
      return;
   pthread_mutex_lock(&g_progress->mutex);
   g_progress->transport_attempts = attempts > INT_MAX - g_progress->transport_attempts
                                        ? INT_MAX
                                        : g_progress->transport_attempts + attempts;
   /* Keep the write under the same mutex: the DB operation is SET, so even
    * two correctly counted concurrent callbacks must not publish 2 then 1. */
   db1_agent_job_heartbeat_ext(g_progress->job_id, "", g_progress->transport_attempts);
   pthread_mutex_unlock(&g_progress->mutex);
}

int agent_job_progress_transport_attempts(void)
{
   if (!g_progress)
      return 0;
   pthread_mutex_lock(&g_progress->mutex);
   int attempts = g_progress->transport_attempts;
   pthread_mutex_unlock(&g_progress->mutex);
   return attempts;
}
