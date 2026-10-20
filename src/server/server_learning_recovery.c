/* Bounded delivery of canonical source pointers. No provider/tool execution. */
#include "server_learning_outcome.h"
#include "db1_client/interaction_events.h"
#include "log.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_t worker;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static atomic_int stopping;
static int started;

static int delivery_stopping(void)
{
   return atomic_load(&stopping);
}

static void *recover_projections(void *unused)
{
   (void)unused;
   int reported = 0, unavailable = 0;
   while (!atomic_load(&stopping))
   {
      char *wire = db1_learning_projection_claim();
      cJSON *batch = wire ? cJSON_Parse(wire) : NULL;
      free(wire);
      const cJSON *items = cJSON_GetObjectItemCaseSensitive(batch, "items");
      if (cJSON_IsArray(items) && cJSON_GetArraySize(items) <= 4)
      {
         unavailable = 0;
         if (!reported)
         {
            const char *cutoff =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(batch, "cutoff"));
            LOG_INFO("learning.recovery",
                     "canonical append delivery active since %s; earlier sources require explicit "
                     "replay",
                     cutoff ? cutoff : "unknown cutoff");
            reported = 1;
         }
         const cJSON *item;
         cJSON_ArrayForEach(item, items)
         {
            if (atomic_load(&stopping))
               break;
            const char *lease =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "lease"));
            const cJSON *record = cJSON_GetObjectItemCaseSensitive(item, "record");
            if (!lease || strlen(lease) != 32 || !cJSON_IsObject(record))
               continue;
            int delivered = server_learning_deliver_record(record, delivery_stopping);
            if (db1_learning_projection_ack(lease, delivered ? "delivered" : "retry"))
               LOG_WARN("learning.recovery",
                        "source delivery acknowledgment deferred; canonical source retained");
         }
      }
      else if (!unavailable)
      {
         LOG_WARN("learning.recovery", "delivery queue unavailable; canonical sources retained");
         unavailable = 1;
      }
      cJSON_Delete(batch);
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += 3;
      pthread_mutex_lock(&lock);
      if (!atomic_load(&stopping))
         pthread_cond_timedwait(&wake, &lock, &deadline);
      pthread_mutex_unlock(&lock);
   }
   return NULL;
}
void server_learning_recovery_init(void)
{
   pthread_mutex_lock(&lock);
   if (!started)
   {
      atomic_store(&stopping, 0);
      if (!pthread_create(&worker, NULL, recover_projections, NULL))
         started = 1;
      else
         LOG_WARN("learning.recovery", "delivery worker unavailable; canonical sources retained");
   }
   pthread_mutex_unlock(&lock);
}
void server_learning_recovery_shutdown(void)
{
   pthread_mutex_lock(&lock);
   int join = started;
   atomic_store(&stopping, 1);
   pthread_cond_broadcast(&wake);
   pthread_mutex_unlock(&lock);
   if (join)
      pthread_join(worker, NULL);
   pthread_mutex_lock(&lock);
   started = 0;
   pthread_mutex_unlock(&lock);
}
