/* workspace_runner_queue.c — blocking request/response handoff between the
 * detached provider's transport and the runner. See workspace_runner_queue.h. */
#include "workspace_runner_queue.h"
#include "cJSON.h"

#include <stdlib.h>
#include <time.h>

void ws_runner_queue_init(ws_runner_queue_t *q)
{
   pthread_mutex_init(&q->mu, NULL);
   pthread_cond_init(&q->req_cv, NULL);
   pthread_cond_init(&q->req_taken_cv, NULL);
   pthread_cond_init(&q->resp_cv, NULL);
   pthread_cond_init(&q->idle_cv, NULL);
   q->req = NULL;
   q->resp_head = NULL;
   q->resp_tail = NULL;
   q->has_req = 0;
   q->busy = 0;
   q->closed = 0;
   q->pickup_timeout_ms = 0; /* 0 -> WS_RUNNER_PICKUP_TIMEOUT_MS default */
}

/* Caller holds q->mu (or has exclusive access during destroy). Frees every
 * parked response and empties the FIFO. */
static void resp_fifo_clear(ws_runner_queue_t *q)
{
   ws_runner_resp_node_t *n = q->resp_head;
   while (n)
   {
      ws_runner_resp_node_t *next = n->next;
      if (n->resp)
         cJSON_Delete(n->resp);
      free(n);
      n = next;
   }
   q->resp_head = NULL;
   q->resp_tail = NULL;
}

/* Caller holds q->mu. Pop the oldest response, or NULL if the FIFO is empty. */
static cJSON *resp_fifo_pop(ws_runner_queue_t *q)
{
   ws_runner_resp_node_t *n = q->resp_head;
   if (!n)
      return NULL;
   q->resp_head = n->next;
   if (!q->resp_head)
      q->resp_tail = NULL;
   cJSON *r = n->resp;
   free(n);
   return r;
}

void ws_runner_queue_destroy(ws_runner_queue_t *q)
{
   /* Free anything still parked in the slots. */
   if (q->req)
      cJSON_Delete(q->req);
   q->req = NULL;
   resp_fifo_clear(q);
   pthread_mutex_destroy(&q->mu);
   pthread_cond_destroy(&q->req_cv);
   pthread_cond_destroy(&q->req_taken_cv);
   pthread_cond_destroy(&q->resp_cv);
   pthread_cond_destroy(&q->idle_cv);
}

void ws_runner_queue_close(ws_runner_queue_t *q)
{
   pthread_mutex_lock(&q->mu);
   q->closed = 1;
   pthread_cond_broadcast(&q->req_cv);
   pthread_cond_broadcast(&q->req_taken_cv);
   pthread_cond_broadcast(&q->resp_cv);
   pthread_cond_broadcast(&q->idle_cv);
   pthread_mutex_unlock(&q->mu);
}

/* Build an absolute CLOCK_REALTIME deadline `timeout_ms` from now. */
static void deadline_from_now(struct timespec *ts, int timeout_ms)
{
   clock_gettime(CLOCK_REALTIME, ts);
   ts->tv_sec += timeout_ms / 1000;
   ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
   if (ts->tv_nsec >= 1000000000L)
   {
      ts->tv_sec++;
      ts->tv_nsec -= 1000000000L;
   }
}

/* Default pickup deadline: how long the transport waits for *some* runner to
 * claim a posted request before concluding no client is serving the detached
 * workspace. Generous enough to ride out a client momentarily between long-poll
 * cycles, short enough that a truly absent client fails fast instead of wedging
 * the calling turn until the delegate monitor reaps it (~20 min). */
#define WS_RUNNER_PICKUP_TIMEOUT_MS 45000

/* Caller holds q->mu. Post `request` into the single-flight slot. Assumes the
 * slot is free (busy==0) and the queue is open. */
static void post_request_locked(ws_runner_queue_t *q, cJSON *request)
{
   q->busy = 1;
   q->req = request; /* queue owns it until poll takes it */
   q->has_req = 1;
   /* A fresh op must not see a stale response from a prior aborted cycle. */
   resp_fifo_clear(q);
   pthread_cond_signal(&q->req_cv);
}

/* Caller holds q->mu, a request was just posted. Block until the runner claims
 * the request (has_req 1->0), a response is already pending, or the queue
 * closes — bounded by the pickup deadline. Returns 0 if the op is live (claimed
 * / response pending / closed — the caller's normal response wait handles those)
 * or -1 if no runner claimed the request before the deadline (no serving
 * client). Only the unclaimed window is bounded; a claimed op then waits for its
 * response without a deadline so a legit long client command isn't cut off. */
static int wait_for_pickup_locked(ws_runner_queue_t *q)
{
   int pickup_ms = q->pickup_timeout_ms > 0 ? q->pickup_timeout_ms : WS_RUNNER_PICKUP_TIMEOUT_MS;
   struct timespec ts;
   deadline_from_now(&ts, pickup_ms);
   while (q->has_req && !q->resp_head && !q->closed)
   {
      if (pthread_cond_timedwait(&q->req_taken_cv, &q->mu, &ts) != 0)
      {
         /* deadline (or spurious wake): if the request is still unclaimed and
          * nothing else changed, no runner is serving this queue. */
         if (q->has_req && !q->resp_head && !q->closed)
            return -1;
      }
   }
   return 0;
}

int ws_runner_queue_transport(void *ctx, cJSON *request, cJSON **response)
{
   ws_runner_queue_t *q = (ws_runner_queue_t *)ctx;
   if (response)
      *response = NULL;

   pthread_mutex_lock(&q->mu);

   /* single-flight: wait for any in-flight op to finish */
   while (q->busy && !q->closed)
      pthread_cond_wait(&q->idle_cv, &q->mu);
   if (q->closed)
   {
      pthread_mutex_unlock(&q->mu);
      cJSON_Delete(request);
      return -1;
   }

   post_request_locked(q, request);

   /* Bound only the unclaimed window: if no runner picks the op up, fail fast
    * rather than block forever (no serving client). Once claimed, the response
    * wait below is unbounded so a legit long client command runs to completion.
    * A pickup timeout leaves has_req==1, so the reclaim path below frees it. */
   if (wait_for_pickup_locked(q) == 0)
   {
      while (!q->resp_head && !q->closed)
         pthread_cond_wait(&q->resp_cv, &q->mu);
   }

   int rc;
   cJSON *resp = resp_fifo_pop(q);
   if (resp)
   {
      if (response)
         *response = resp;
      else
         cJSON_Delete(resp);
      rc = 0;
   }
   else
   {
      /* closed, or no runner claimed the op before the pickup deadline; reclaim
       * an untaken request and fail the transport. */
      rc = -1;
      if (q->has_req)
      {
         cJSON_Delete(q->req);
         q->req = NULL;
         q->has_req = 0;
      }
   }

   q->busy = 0;
   pthread_cond_signal(&q->idle_cv);
   pthread_mutex_unlock(&q->mu);
   return rc;
}

int ws_runner_queue_transport_stream(void *ctx, cJSON *request, ws_runner_partial_fn on_partial,
                                     void *cb_ctx, cJSON **final)
{
   ws_runner_queue_t *q = (ws_runner_queue_t *)ctx;
   if (final)
      *final = NULL;

   pthread_mutex_lock(&q->mu);

   while (q->busy && !q->closed)
      pthread_cond_wait(&q->idle_cv, &q->mu);
   if (q->closed)
   {
      pthread_mutex_unlock(&q->mu);
      cJSON_Delete(request);
      return -1;
   }

   post_request_locked(q, request);

   int rc = -1;
   /* Bound only the unclaimed window (see ws_runner_queue_transport): a stream
    * with no serving runner fails fast instead of wedging the turn. Once a
    * runner claims the op, partials are drained without a deadline. */
   for (; wait_for_pickup_locked(q) == 0;)
   {
      while (!q->resp_head && !q->closed)
         pthread_cond_wait(&q->resp_cv, &q->mu);
      if (!q->resp_head)
         break; /* closed before a (further) response arrived */

      cJSON *resp = resp_fifo_pop(q);
      int is_partial = resp && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "partial"));
      if (is_partial)
      {
         /* Deliver the partial without holding the lock so a slow consumer
          * never stalls the respond endpoint; further partials queue behind. */
         pthread_mutex_unlock(&q->mu);
         int abort_stream = on_partial ? on_partial(cb_ctx, resp) : 0;
         cJSON_Delete(resp);
         pthread_mutex_lock(&q->mu);
         if (abort_stream)
         {
            rc = -1;
            break;
         }
         continue;
      }

      /* Final response: hand it back and end the stream. */
      if (final)
         *final = resp;
      else
         cJSON_Delete(resp);
      rc = 0;
      break;
   }

   if (rc != 0 && q->has_req)
   {
      cJSON_Delete(q->req);
      q->req = NULL;
      q->has_req = 0;
   }

   q->busy = 0;
   pthread_cond_signal(&q->idle_cv);
   pthread_mutex_unlock(&q->mu);
   return rc;
}

cJSON *ws_runner_queue_poll(ws_runner_queue_t *q, int timeout_ms)
{
   pthread_mutex_lock(&q->mu);

   if (timeout_ms > 0)
   {
      struct timespec ts;
      deadline_from_now(&ts, timeout_ms);
      while (!q->has_req && !q->closed)
         if (pthread_cond_timedwait(&q->req_cv, &q->mu, &ts) != 0)
            break; /* timeout (or spurious) — recheck then bail */
   }
   else
   {
      while (!q->has_req && !q->closed)
         pthread_cond_wait(&q->req_cv, &q->mu);
   }

   cJSON *r = NULL;
   if (q->has_req)
   {
      r = q->req; /* ownership transfers to the runner */
      q->req = NULL;
      q->has_req = 0;
      /* Wake the transport's bounded pickup wait so it proceeds to the
       * (unbounded) response wait the instant the op is claimed. */
      pthread_cond_signal(&q->req_taken_cv);
   }
   pthread_mutex_unlock(&q->mu);
   return r;
}

int ws_runner_queue_respond(ws_runner_queue_t *q, cJSON *response)
{
   ws_runner_resp_node_t *node = calloc(1, sizeof(*node));
   if (!node)
   {
      cJSON_Delete(response);
      return -1;
   }
   node->resp = response;

   pthread_mutex_lock(&q->mu);
   if (q->closed)
   {
      pthread_mutex_unlock(&q->mu);
      cJSON_Delete(response);
      free(node);
      return -1;
   }
   if (q->resp_tail)
      q->resp_tail->next = node;
   else
      q->resp_head = node;
   q->resp_tail = node;
   pthread_cond_signal(&q->resp_cv);
   pthread_mutex_unlock(&q->mu);
   return 0;
}
