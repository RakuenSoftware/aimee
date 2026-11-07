#define _GNU_SOURCE
#include "obs_bus_call.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ouroboros-ring v0.4.0 handles cooperative cancellation, but its pool/client
 * mutexes and temporary request allocation have no POSIX cancellation cleanup.
 * Keep checkout and caller cancellation here until that upstream contract is
 * provided. The library still owns every request/reply byte, correlation,
 * deadline, cancellation frame, and stale-reply drain. At most pool->count
 * workers exist: each worker already owns one of the existing bounded slots.
 * Caller callbacks stay on the caller thread so their TLS authority is intact. */
typedef struct
{
   ouro_module_client_pool_t *pool;
   int index, started;
   pthread_t worker;
   atomic_int cancel, done;
   pthread_mutex_t completion_lock;
   pthread_cond_t completion;
   uint32_t kind, stage, request_len, capacity, *response_len;
   uint64_t trace, deadline;
   const void *request;
   void *response;
   ouro_module_call_result_t result;
} host_call;

static int worker_cancelled(void *opaque)
{
   host_call *call = opaque;
   return atomic_load(&call->cancel) || atomic_load(&call->pool->stopping);
}
static void *invoke(void *opaque)
{
   host_call *call = opaque;
   call->result = ouro_module_client_call(&call->pool->clients[call->index], call->kind,
                                          call->stage, call->trace, call->deadline, call->request,
                                          call->request_len, call->response, call->capacity,
                                          call->response_len, worker_cancelled, call);
   pthread_mutex_lock(&call->completion_lock);
   atomic_store_explicit(&call->done, 1, memory_order_release);
   pthread_cond_signal(&call->completion);
   pthread_mutex_unlock(&call->completion_lock);
   return NULL;
}
static void unlock(void *mutex)
{
   pthread_mutex_unlock(mutex);
}
static void release(void *opaque)
{
   host_call *call = opaque;
   int previous;
   pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous);
   if (call->started)
   {
      atomic_store(&call->cancel, 1);
      /* Cooperative ring cancellation returns before any caller-owned buffers
       * or source context leave scope; never detach an in-flight operation. */
      if (pthread_join(call->worker, NULL))
         abort(); /* Never release caller buffers without a joined transport. */
      call->started = 0;
   }
   if (call->index >= 0)
   {
      pthread_mutex_lock(&call->pool->lock);
      call->pool->in_use[call->index] = 0;
      call->pool->in_flight--;
      pthread_cond_signal(&call->pool->freed);
      pthread_mutex_unlock(&call->pool->lock);
      call->index = -1;
   }
   pthread_cond_destroy(&call->completion);
   pthread_mutex_destroy(&call->completion_lock);
   pthread_setcancelstate(previous, NULL);
}
static int acquire(host_call *call)
{
   struct timespec until;
   if (call->deadline)
      until = (struct timespec){(time_t)(call->deadline / 1000000000ULL),
                                (long)(call->deadline % 1000000000ULL)};
   else
   {
      if (clock_gettime(CLOCK_MONOTONIC, &until))
         return -1;
      until.tv_sec += 30; /* Same zero-deadline bounded pool wait as v0.4.0. */
   }
   pthread_mutex_lock(&call->pool->lock);
   pthread_cleanup_push(unlock, &call->pool->lock);
   while (!atomic_load(&call->pool->stopping))
   {
      for (size_t i = 0; i < call->pool->count; ++i)
         if (!call->pool->in_use[i])
         {
            call->pool->in_use[i] = 1;
            call->index = (int)i;
            if (++call->pool->in_flight > call->pool->peak_in_flight)
               call->pool->peak_in_flight = call->pool->in_flight;
            break;
         }
      if (call->index >= 0 ||
          pthread_cond_timedwait(&call->pool->freed, &call->pool->lock, &until) == ETIMEDOUT)
         break;
   }
   pthread_cleanup_pop(1);
   return call->index;
}

ouro_module_call_result_t aimee_obs_module_pool_call(
    ouro_module_client_pool_t *pool, uint32_t kind, uint32_t stage, uint64_t trace,
    uint64_t deadline, const void *request, uint32_t request_len, void *response, uint32_t capacity,
    uint32_t *response_len, ouro_module_cancelled_fn cancelled, void *cancel_context)
{
   if (response_len)
      *response_len = 0;
   if (!pool || !pool->initialized)
      return OURO_MODULE_CALL_INVALID_ARGUMENT;
   host_call call = {.pool = pool,
                     .index = -1,
                     .kind = kind,
                     .stage = stage,
                     .trace = trace,
                     .deadline = deadline,
                     .request = request,
                     .request_len = request_len,
                     .response = response,
                     .capacity = capacity,
                     .response_len = response_len,
                     .result = OURO_MODULE_CALL_TRANSPORT};
   atomic_init(&call.cancel, 0);
   atomic_init(&call.done, 0);
   if (pthread_mutex_init(&call.completion_lock, NULL))
      return OURO_MODULE_CALL_TRANSPORT;
   if (pthread_cond_init(&call.completion, NULL))
   {
      pthread_mutex_destroy(&call.completion_lock);
      return OURO_MODULE_CALL_TRANSPORT;
   }
   pthread_cleanup_push(release, &call);
   if (acquire(&call) < 0)
      call.result = atomic_load(&pool->stopping) ? OURO_MODULE_CALL_TRANSPORT
                                                 : OURO_MODULE_CALL_DEADLINE_EXCEEDED;
   else
   {
      if (cancelled && cancelled(cancel_context))
         atomic_store(&call.cancel, 1);
      int previous;
      pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous);
      call.started = pthread_create(&call.worker, NULL, invoke, &call) == 0;
      pthread_setcancelstate(previous, NULL);
      while (call.started && !atomic_load_explicit(&call.done, memory_order_acquire))
      {
         if (cancelled && cancelled(cancel_context))
            atomic_store(&call.cancel, 1);
         struct timespec until;
         if (clock_gettime(CLOCK_REALTIME, &until))
            break;
         until.tv_nsec += 20000000;
         if (until.tv_nsec >= 1000000000)
         {
            until.tv_nsec -= 1000000000;
            until.tv_sec++;
         }
         pthread_mutex_lock(&call.completion_lock);
         pthread_cleanup_push(unlock, &call.completion_lock);
         if (!atomic_load_explicit(&call.done, memory_order_acquire))
            pthread_cond_timedwait(&call.completion, &call.completion_lock, &until);
         pthread_cleanup_pop(1);
      }
   }
   pthread_cleanup_pop(1);
   return call.result;
}
