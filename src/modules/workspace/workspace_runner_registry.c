/* workspace_runner_registry.c — which client is serving which tree, asked over
 * the bus. See workspace_runner_registry.h.
 *
 * The answer lives in the workspace module, not here. This file only frames the
 * question and carries it: no map of trees, no queues, no condition variables.
 * The handles below exist solely because callers hold a `ws_runner_queue_t *`;
 * each one carries an id and nothing else. */
#include "workspace_runner_registry.h"
#include "cJSON.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/workspace/module_api.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A submit waits for a client to run the op and answer, which is as slow as the
 * work itself. Bounded anyway: an unbounded wait cannot be told apart from a
 * client that has silently gone. */
#define WS_RUNNER_CALL_MS 600000ULL

static atomic_ullong g_trace = 1;

static uint64_t monotonic_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int ws_runner_bus_call(uint32_t event_kind, uint32_t stage_id, const void *request,
                       uint32_t request_len, void *response, uint32_t response_capacity,
                       uint32_t *response_len, uint64_t timeout_ms)
{
   uint64_t now = monotonic_ns();
   if (!now)
      return -1;
   uint64_t trace = atomic_fetch_add_explicit(&g_trace, 1, memory_order_relaxed);
   if (trace == 0)
      trace = atomic_fetch_add_explicit(&g_trace, 1, memory_order_relaxed);
   return (int)obs_bus_module_call(event_kind, stage_id, trace, now + timeout_ms * 1000000ULL,
                                   request, request_len, response, response_capacity, response_len,
                                   NULL, NULL);
}

/* Ask the module a register / forget / resolve question. On resolve, `out_id`
 * receives the serving tree's id, empty when nobody is serving it. */
static int runner_ask(unsigned op, const char *value, char *out_id, size_t id_cap)
{
   uint8_t request[AIMEE_WS_RUNNER_REQUEST_LEN], response[AIMEE_WS_RUNNER_RESPONSE_LEN];
   uint32_t response_len = 0;
   char scratch[AIMEE_WS_RUNNER_ID_MAX + 1];
   if (!value || !value[0])
      return -1;
   if (aimee_ws_runner_request_encode(op, value, strlen(value), request, sizeof(request)) != 0)
      return -1;
   if (ws_runner_bus_call(AIMEE_WORKSPACE_EVENT_RUNNER, AIMEE_WORKSPACE_STAGE_RUNNER, request,
                          sizeof(request), response, sizeof(response), &response_len, 5000ULL) != 0)
      return -1;
   return aimee_ws_runner_response_decode(response, response_len, out_id ? out_id : scratch,
                                          out_id ? id_cap : sizeof(scratch));
}

/* Handles are bookkeeping for the C API's pointer, not state about a tree: the
 * table maps an id to the one handle callers share for it. */
static ws_runner_queue_t g_handles[WS_RUNNER_REGISTRY_MAX];
static int g_handle_used[WS_RUNNER_REGISTRY_MAX];
static pthread_mutex_t g_handles_mu = PTHREAD_MUTEX_INITIALIZER;

/* Caller holds g_handles_mu. */
static ws_runner_queue_t *handle_find(const char *id)
{
   for (int i = 0; i < WS_RUNNER_REGISTRY_MAX; i++)
      if (g_handle_used[i] && strcmp(g_handles[i].id, id) == 0)
         return &g_handles[i];
   return NULL;
}

static ws_runner_queue_t *handle_intern(const char *id)
{
   if (!id || !id[0] || strlen(id) >= sizeof(g_handles[0].id))
      return NULL;
   pthread_mutex_lock(&g_handles_mu);
   ws_runner_queue_t *h = handle_find(id);
   if (!h)
   {
      for (int i = 0; i < WS_RUNNER_REGISTRY_MAX; i++)
         if (!g_handle_used[i])
         {
            g_handle_used[i] = 1;
            snprintf(g_handles[i].id, sizeof(g_handles[i].id), "%s", id);
            h = &g_handles[i];
            break;
         }
   }
   pthread_mutex_unlock(&g_handles_mu);
   return h;
}

static void handle_release(const char *id)
{
   pthread_mutex_lock(&g_handles_mu);
   ws_runner_queue_t *h = handle_find(id);
   if (h)
   {
      g_handle_used[h - g_handles] = 0;
      h->id[0] = '\0';
   }
   pthread_mutex_unlock(&g_handles_mu);
}

ws_runner_queue_t *ws_runner_registry_get_or_create(const char *id)
{
   /* Announce the tree, but hand back the handle either way. This answers "what
    * is the handle for this tree", not "is the module up": the queue this
    * replaced never proved a client existed either, it only allocated. Failing
    * here instead would turn an unreachable module into "this workspace is not
    * detached", and a turn would quietly act on the server's own filesystem
    * rather than the client's -- a fallback that defeats the thing it falls back
    * from. An unreachable module surfaces at the op that needs it, where it is
    * visible and attributable. */
   (void)runner_ask(AIMEE_WS_RUNNER_OP_REGISTER, id, NULL, 0);
   return handle_intern(id);
}

ws_runner_queue_t *ws_runner_registry_lookup(const char *id)
{
   char serving[AIMEE_WS_RUNNER_ID_MAX + 1] = "";
   if (!id || runner_ask(AIMEE_WS_RUNNER_OP_RESOLVE, id, serving, sizeof(serving)) != 0)
      return NULL;
   /* Resolve answers with the CLOSEST serving tree, which for an exact lookup
    * must be the tree itself; a parent answering does not mean this id is
    * registered. */
   if (strcmp(serving, id) != 0)
      return NULL;
   return handle_intern(serving);
}

ws_runner_queue_t *ws_runner_registry_lookup_for_path(const char *path)
{
   char serving[AIMEE_WS_RUNNER_ID_MAX + 1] = "";
   if (!path || runner_ask(AIMEE_WS_RUNNER_OP_RESOLVE, path, serving, sizeof(serving)) != 0)
      return NULL;
   if (!serving[0])
      return NULL; /* nobody is serving it: an answer, not a failure */
   return handle_intern(serving);
}

void ws_runner_registry_remove(const char *id)
{
   if (!id || !id[0])
      return;
   (void)runner_ask(AIMEE_WS_RUNNER_OP_UNREGISTER, id, NULL, 0);
   handle_release(id);
}

/* One handoff op, carrying `payload` and returning the module's chunk. The
 * caller owns *out (malloc'd) when this returns 0 and `out` is non-NULL. */
int ws_runner_io(unsigned op, const char *id, const char *payload, size_t payload_len, char **out,
                 size_t *out_len, int *more, uint64_t timeout_ms)
{
   size_t id_len = id ? strlen(id) : 0;
   size_t request_cap = AIMEE_WS_IO_HEADER_LEN + id_len + payload_len;
   uint8_t *request = malloc(request_cap ? request_cap : 1);
   if (!request)
      return -1;
   size_t request_len =
       aimee_ws_io_request_encode(op, id, payload, payload_len, request, request_cap);
   if (request_len == 0)
   {
      free(request);
      return -1;
   }

   uint32_t response_cap = AIMEE_WS_IO_RESP_HEADER_LEN + AIMEE_WS_IO_PAYLOAD_MAX;
   uint8_t *response = malloc(response_cap);
   if (!response)
   {
      free(request);
      return -1;
   }
   uint32_t response_len = 0;
   int rc =
       ws_runner_bus_call(AIMEE_WORKSPACE_EVENT_RUNNER_IO, AIMEE_WORKSPACE_STAGE_RUNNER_IO, request,
                          (uint32_t)request_len, response, response_cap, &response_len, timeout_ms);
   free(request);
   if (rc != 0)
   {
      free(response);
      /* Keep "nobody is serving this tree" distinguishable from every other
       * failure. The module answers it as CAPABILITY_ABSENT precisely so this
       * layer can pass it on; collapsing it into -1 here is what let the poll
       * loop treat a permanent condition as a transient one. */
      return (rc == (int)AIMEE_MODULE_CALL_CAPABILITY_ABSENT) ? WS_RUNNER_IO_UNSERVED : -1;
   }

   const uint8_t *body = NULL;
   size_t body_len = 0;
   if (aimee_ws_io_response_decode(response, response_len, &body, &body_len, more) != 0)
   {
      free(response);
      return -1;
   }
   if (out)
   {
      char *copy = malloc(body_len + 1);
      if (!copy)
      {
         free(response);
         return -1;
      }
      memcpy(copy, body, body_len);
      copy[body_len] = '\0';
      *out = copy;
      if (out_len)
         *out_len = body_len;
   }
   free(response);
   return 0;
}

cJSON *ws_runner_registry_poll(const char *id, int timeout_ms, int *unserved)
{
   char *body = NULL;
   size_t body_len = 0;
   if (unserved)
      *unserved = 0;
   if (!id || !id[0])
      return NULL;
   int rc = ws_runner_io(AIMEE_WS_IO_OP_POLL, id, NULL, 0, &body, &body_len, NULL,
                         timeout_ms > 0 ? (uint64_t)timeout_ms : 1000ULL);
   if (rc != 0)
   {
      /* Report the two apart: WS_RUNNER_IO_UNSERVED came back immediately and
       * will keep doing so, while any other failure includes the ordinary
       * "waited, nothing arrived". */
      if (rc == WS_RUNNER_IO_UNSERVED)
      {
         if (unserved)
            *unserved = 1;
         /* PACE IT HERE, so every poll caller is covered by construction. Both
          * poll endpoints are long polls whose clients re-poll the instant they
          * answer; the wait IS the pacing, and an unserved tree is refused
          * without one. Absorbing the caller's own budget costs a correct
          * client nothing and bounds an incorrect one — including clients too
          * old to know about the `served` flag. Capped, because a 25s socket
          * budget spent sleeping would read as a hang and would delay noticing
          * a runner that arrives late. */
         long pace_ms = (timeout_ms > 0 && (uint64_t)timeout_ms < WS_RUNNER_UNSERVED_PACE_MS)
                            ? (long)timeout_ms
                            : (long)WS_RUNNER_UNSERVED_PACE_MS;
         struct timespec pace = {.tv_sec = pace_ms / 1000, .tv_nsec = (pace_ms % 1000) * 1000000L};
         nanosleep(&pace, NULL);
      }
      return NULL;
   }
   cJSON *op = body_len ? cJSON_Parse(body) : NULL;
   free(body);
   return op;
}

int ws_runner_registry_respond(const char *id, cJSON *response)
{
   if (!response)
      return -1;
   char *text = cJSON_PrintUnformatted(response);
   cJSON_Delete(response); /* ownership taken, per the header */
   if (!text)
      return -1;
   int rc = ws_runner_io(AIMEE_WS_IO_OP_RESPOND, id, text, strlen(text), NULL, NULL, NULL,
                         WS_RUNNER_CALL_MS);
   free(text);
   return rc;
}
