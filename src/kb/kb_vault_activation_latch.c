#include "kb_vault_activation_latch.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <time.h>

static int deadline_after(unsigned timeout_ms, struct timespec *deadline)
{
   if (!deadline || timeout_ms > INT_MAX || clock_gettime(CLOCK_MONOTONIC, deadline) != 0)
      return -1;
   deadline->tv_sec += (time_t)(timeout_ms / 1000u);
   long nanos = deadline->tv_nsec + (long)(timeout_ms % 1000u) * 1000000L;
   if (nanos >= 1000000000L)
   {
      deadline->tv_sec++;
      nanos -= 1000000000L;
   }
   deadline->tv_nsec = nanos;
   return 0;
}

int kb_vault_activation_latch_init(kb_vault_activation_latch_t *latch)
{
   if (!latch)
      return -1;
   memset(latch, 0, sizeof(*latch));
   if (pthread_mutex_init(&latch->mutex, NULL) != 0)
      return -1;
   pthread_condattr_t attr;
   if (pthread_condattr_init(&attr) != 0)
   {
      pthread_mutex_destroy(&latch->mutex);
      return -1;
   }
   int attr_ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0;
   int cond_rc = attr_ok ? pthread_cond_init(&latch->condition, &attr) : -1;
   pthread_condattr_destroy(&attr);
   if (cond_rc != 0)
   {
      pthread_mutex_destroy(&latch->mutex);
      return -1;
   }
   latch->initialized = 1;
   return 0;
}

int kb_vault_activation_latch_publish(kb_vault_activation_latch_t *latch,
                                      const kb_vault_operator_status_t *status)
{
   if (!latch || !latch->initialized || !status ||
       status->state != KB_VAULT_OPERATOR_STATE_OPERATIONAL ||
       !kb_vault_operator_status_validate(status) || pthread_mutex_lock(&latch->mutex) != 0)
      return -1;
   int result = 0;
   if (latch->activated)
      result = memcmp(&latch->status, status, sizeof(*status)) == 0 ? 0 : -1;
   else
   {
      latch->status = *status;
      latch->activated = 1;
      if (pthread_cond_broadcast(&latch->condition) != 0)
         result = -1;
   }
   pthread_mutex_unlock(&latch->mutex);
   return result;
}

int kb_vault_activation_latch_wait(kb_vault_activation_latch_t *latch, unsigned timeout_ms,
                                   kb_vault_operator_status_t *status)
{
   struct timespec deadline;
   if (!latch || !latch->initialized || !status || deadline_after(timeout_ms, &deadline) != 0 ||
       pthread_mutex_lock(&latch->mutex) != 0)
      return -1;
   int result = 1;
   while (!latch->activated)
   {
      int rc = pthread_cond_timedwait(&latch->condition, &latch->mutex, &deadline);
      if (rc == ETIMEDOUT)
      {
         result = 0;
         break;
      }
      if (rc != 0)
      {
         result = -1;
         break;
      }
   }
   if (result == 1)
      *status = latch->status;
   pthread_mutex_unlock(&latch->mutex);
   return result;
}

void kb_vault_activation_latch_destroy(kb_vault_activation_latch_t *latch)
{
   if (!latch || !latch->initialized)
      return;
   pthread_cond_destroy(&latch->condition);
   pthread_mutex_destroy(&latch->mutex);
   memset(latch, 0, sizeof(*latch));
}
