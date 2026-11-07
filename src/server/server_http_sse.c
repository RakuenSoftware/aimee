/* server_http_sse.c: split from server_http.c into a real translation unit
 * (was server_http_sse.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_http_internal.h"
#include "agent_config.h"
#include "primary_session_adapter.h"
#include "server_learning_outcome.h"
#include "db1_client/server_sessions.h"
#include "db1_client/primary_sessions.h"
#include "server_http.h"
#include "server.h"         /* CAP_* / CAPS_* capability bits, server_capability_for_method */
#include "server_conn_io.h" /* transport-aware fd I/O (native-TLS phase 1) */
#include "server_tls.h"     /* native TLS termination (phase 1b) */
#include "modules/workspace/workspace_runner_registry.h" /* ws_runner_registry_poll/_respond for the /v1 reverse channel */
#include <time.h>
#include "persona.h"
#include "role_templates.h"
#include "util.h" /* safe_strdup, aimee_base64_* */
#include "cli_session_pty.h"
#include "config.h"
#include "prompts.h"
#include <aimee/delegates/delegate_role.h>
#include "log.h"
#include "aimee_version.h"
#include "openai_shape.h"
#include "ingress_preinject.h"
#include "openapi_server_data.h" /* AIMEE_OPENAPI_SERVER_YAML_STR (generated from api/openapi-server-v1.yaml) */
#include "openai_runs_store.h"
#if AIMEE_WITH_ROUNDTABLE
#include "roundtable_pipeline_capture.h" /* pipeline op-run capture seam (#18/#20) */
#endif
#include "presence.h"
#include "request_context.h"
#include "server_http_identity.h" /* WP-C.0 attested-identity capture/threading */
#include "server_workflow_api.h"  /* W7: /v1/workflow read+author handlers */
#include "cJSON.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

/* SSE event streams (handle_session_events / handle_run_events) are long-lived:
 * running them inline in handle_conn would block the single listener thread for
 * the stream's whole lifetime, starving every other /v1 connection (a presence
 * subscriber would freeze the entire HTTP surface). Offload each to a detached
 * thread on a dup'd fd — mirroring the chat.send_stream worker — so the listener
 * returns immediately. The listener's close(fd) only drops its own reference;
 * the worker streams on the dup until the client disconnects, then closes it. A
 * modest cap bounds concurrent streams against fd/thread exhaustion. */
typedef void (*sse_stream_fn)(int fd, const char *id, const char *request_id);
typedef struct
{
   int fd;
   sse_stream_fn fn;
   char id[256];
   char request_id[64];
   char principal[VAULT_PRINCIPAL_MAX];
   request_context_snapshot_t request_context;
} sse_offload_t;

static atomic_int g_sse_live = 0;
#define SSE_LIVE_DEFAULT 256
static int g_sse_max = SSE_LIVE_DEFAULT; /* configurable via aimee.api.max_event_streams */

/* Set the cap on concurrent SSE event streams. n <= 0 restores the default.
 * Called once at startup (server_http_start) before any stream is accepted. */
void server_http_set_max_event_streams(int n)
{
   g_sse_max = (n > 0) ? n : SSE_LIVE_DEFAULT;
}

static void *sse_offload_thread(void *arg)
{
   sse_offload_t *o = (sse_offload_t *)arg;
   request_context_restore(&o->request_context);
   server_http_identity_bind_request_owner(request_context_get());
   agent_set_request_vault_principal(o->principal);
   o->fn(o->fd, o->id, o->request_id);
   agent_set_request_vault_principal(NULL);
   server_http_identity_clear();
   request_context_clear();
   request_context_snapshot_clear(&o->request_context);
   close(o->fd);
   atomic_fetch_sub(&g_sse_live, 1);
   free(o);
   return NULL;
}

/* Hand an SSE stream to a detached worker on a dup'd fd. Returns 0 on success
 * (the caller must not touch fd afterward beyond the listener's own close).
 * Returns -1 if the live-stream cap is hit or resources are exhausted, in which
 * case the caller should send an error on fd and not stream. */
int server_http_sse_offload(int fd, sse_stream_fn fn, const char *id, const char *request_id)
{
   /* SSE offload dups the fd to a detached thread; a TLS conn's SSL is owned by
    * this conn's worker and cannot be shared with the dup (and the dup has no SSL
    * registered → it would emit plaintext on the TLS socket). Refuse over TLS; the
    * caller turns this into a 503. SSE-over-TLS is phase 1c. */
   if (server_conn_io_has_ssl(fd))
      return -1;
   if (atomic_fetch_add(&g_sse_live, 1) >= g_sse_max)
   {
      atomic_fetch_sub(&g_sse_live, 1);
      return -1;
   }
   int dfd = dup(fd);
   if (dfd < 0)
   {
      atomic_fetch_sub(&g_sse_live, 1);
      return -1;
   }
   sse_offload_t *o = (sse_offload_t *)calloc(1, sizeof(*o));
   if (!o)
   {
      close(dfd);
      atomic_fetch_sub(&g_sse_live, 1);
      return -1;
   }
   request_context_snapshot(&o->request_context);
   snprintf(o->principal, sizeof(o->principal), "%s", server_http_identity_principal());
   o->fd = dfd;
   o->fn = fn;
   snprintf(o->id, sizeof(o->id), "%s", id ? id : "");
   snprintf(o->request_id, sizeof(o->request_id), "%s", request_id ? request_id : "");
   pthread_t t;
   if (pthread_create(&t, NULL, sse_offload_thread, o) != 0)
   {
      close(dfd);
      request_context_snapshot_clear(&o->request_context);
      free(o);
      atomic_fetch_sub(&g_sse_live, 1);
      return -1;
   }
   pthread_detach(t);
   return 0;
}

typedef struct
{
   int fd;
   pthread_mutex_t mu;
   pthread_cond_t cv;
   int stop;
} sse_live_ctx_t;

static void sse_event_emit(void *ctx, const char *event, const char *data_json)
{
   sse_live_ctx_t *live = (sse_live_ctx_t *)ctx;
   if (!data_json)
      return;
   int need = server_http_sse_event_format(event, data_json, NULL, 0);
   char *frame = malloc((size_t)need + 1);
   if (!frame)
      return;
   server_http_sse_event_format(event, data_json, frame, (size_t)need + 1);
   pthread_mutex_lock(&live->mu);
   (void)write_all_fd(live->fd, frame, need);
   pthread_mutex_unlock(&live->mu);
   free(frame);
}

static void *sse_keepalive_main(void *arg)
{
   sse_live_ctx_t *live = (sse_live_ctx_t *)arg;
   pthread_mutex_lock(&live->mu);
   while (!live->stop)
   {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_sec += 15;
      int rc = pthread_cond_timedwait(&live->cv, &live->mu, &until);
      if (!live->stop && rc == ETIMEDOUT)
         (void)write_all_fd(live->fd, ": keep-alive\n\n", 14);
   }
   pthread_mutex_unlock(&live->mu);
   return NULL;
}

/* Keep long provider generations alive without changing the typed SSE event
 * stream. The same owner serves Responses and Anthropic Messages. */
void server_http_sse_live_run(int fd, const char *body, server_http_responses_stream_fn handler)
{
   sse_live_ctx_t live = {.fd = fd};
   pthread_mutex_init(&live.mu, NULL);
   pthread_cond_init(&live.cv, NULL);
   pthread_t keepalive;
   int started = pthread_create(&keepalive, NULL, sse_keepalive_main, &live) == 0;
   handler(body ? body : "", sse_event_emit, &live);
   if (started)
   {
      pthread_mutex_lock(&live.mu);
      live.stop = 1;
      pthread_cond_signal(&live.cv);
      pthread_mutex_unlock(&live.mu);
      pthread_join(keepalive, NULL);
   }
   pthread_cond_destroy(&live.cv);
   pthread_mutex_destroy(&live.mu);
}

static void handle_session_ir_events(int fd, const char *input, const char *request_id)
{
   char session[128], after[32] = "0";
   const char *query = strchr(input, '?');
   size_t n = query ? (size_t)(query - input) : strlen(input);
   if (!n || n >= sizeof(session))
   {
      send_response(fd, 400, "{\"error\":\"invalid session\"}", request_id);
      return;
   }
   memcpy(session, input, n);
   session[n] = '\0';
   const char *cursor = query ? strstr(query, "cursor=") : NULL;
   if (cursor)
      snprintf(after, sizeof(after), "%s", cursor + 7);
   const char *actor = agent_get_request_vault_principal();
   db1_server_session_owner_t row;
   if (!actor || !actor[0])
   {
      send_response(fd, 403, "{\"error\":\"session actor required\"}", request_id);
      return;
   }
   /* An attached browser already owns a durable session. A failed lookup is
    * not permission to skip history recovery and advertise a successful replay. */
   if (db1_server_session_owner_get(session, &row))
   {
      send_response(fd, 503, "{\"error\":\"session history unavailable\"}", request_id);
      return;
   }
   if (strcmp(row.principal, actor))
   {
      send_response(fd, 403, "{\"error\":\"session ownership mismatch\"}", request_id);
      return;
   }
   if (primary_session_history_open(session, actor))
   {
      send_response(fd, 503, "{\"error\":\"session history unavailable\"}", request_id);
      return;
   }
   write_sse_headers(fd, request_id);
   int seen = 0, quiet = 0, replay_complete = 0;
   for (;;)
   {
      if (db1_server_session_owner_get(session, &row) == 0)
      {
         if (strcmp(row.principal, actor))
            break;
         seen = 1;
         char *json = db1_session_ir_page(session, after, 64, actor);
         if (!json)
            break;
         cJSON *page = cJSON_Parse(json);
         free(json);
         if (!cJSON_IsArray(page))
         {
            cJSON_Delete(page);
            break;
         }
         int count = cJSON_GetArraySize(page), failed = 0;
         const cJSON *event = NULL;
         cJSON_ArrayForEach(event, page)
         {
            const cJSON *seq = cJSON_GetObjectItemCaseSensitive(event, "sequence");
            char *line = cJSON_PrintUnformatted(event);
            if (!cJSON_IsString(seq) || !line)
            {
               free(line);
               failed = 1;
               break;
            }
            /* Replay is also recovery for a lost derived observation. The
             * owner-checked durable event is the source; DB1 serializes and
             * deduplicates projection retries against that exact source. */
            const cJSON *source = cJSON_GetObjectItemCaseSensitive(event, "event");
            const cJSON *payload = cJSON_GetObjectItemCaseSensitive(source, "payload");
            const char *kind =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(source, "kind"));
            const char *status =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "status"));
            const char *event_actor =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "actor"));
            if (kind && !strcmp(kind, "execution") && status &&
                (!strcmp(status, "completed") || !strcmp(status, "failed") ||
                 !strcmp(status, "cancelled")) &&
                event_actor && !strcmp(event_actor, actor))
               server_learning_observe_terminal(
                   session,
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "turn_id")),
                   seq->valuestring,
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "event_id")), actor,
                   source);
            if (kind && !strcmp(kind, "child_execution") && event_actor &&
                !strcmp(event_actor, actor))
               server_learning_observe_child(
                   session,
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "turn_id")),
                   seq->valuestring,
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "event_id")), actor,
                   source);
            if (write_all_fd(fd, "id: ", 4) < 0 ||
                write_all_fd(fd, seq->valuestring, strlen(seq->valuestring)) < 0 ||
                write_all_fd(fd, "\nevent: ir\ndata: ", 17) < 0 ||
                write_all_fd(fd, line, strlen(line)) < 0 || write_all_fd(fd, "\n\n", 2) < 0)
               failed = 1;
            free(line);
            if (failed)
               break;
            snprintf(after, sizeof(after), "%s", seq->valuestring);
         }
         cJSON_Delete(page);
         if (failed)
            break;
         if (count)
         {
            quiet = 0;
            continue;
         }
      }
      else if (seen)
         break; /* deletion/store outage never becomes an empty successful replay */
      /* A browser must not offer historical controls while their saved replies
       * are still being replayed. This is transport readiness, not an IR event
       * or a new durable sequence. Existing consumers may ignore it. */
      if (!replay_complete && seen)
      {
         cJSON *ready = cJSON_CreateObject();
         if (!ready || !cJSON_AddStringToObject(ready, "session_id", session) ||
             !cJSON_AddStringToObject(ready, "cursor", after))
         {
            cJSON_Delete(ready);
            break;
         }
         char *line = cJSON_PrintUnformatted(ready);
         cJSON_Delete(ready);
         const char prefix[] = "event: replay_complete\ndata: ";
         int failed = !line || write_all_fd(fd, prefix, sizeof(prefix) - 1) < 0 ||
                      write_all_fd(fd, line, strlen(line)) < 0 || write_all_fd(fd, "\n\n", 2) < 0;
         free(line);
         if (failed)
            break;
         replay_complete = 1;
      }
      if ((quiet++ % 25) == 0 && write_all_fd(fd, ": keep-alive\n\n", 14) < 0)
         break;
      struct timespec pause = {0, 200000000L};
      nanosleep(&pause, NULL);
   }
}

void handle_session_events(int fd, const char *id_in, const char *request_id)
{
   if (strstr(id_in, "?format=ir&"))
   {
      handle_session_ir_events(fd, id_in, request_id);
      return;
   }
   /* id_in may carry a resume position as "<sid>?cursor=N" (threaded from the
    * dispatcher's query string). Split it: `id` is the bare session id, `cursor`
    * the absolute ring position to resume from (0 = replay from oldest retained,
    * the original behavior). */
   char id[128];
   snprintf(id, sizeof(id), "%s", id_in);
   uint64_t cursor = 0;
   char *q = strchr(id, '?');
   if (q)
   {
      *q = '\0';
      const char *c = strstr(q + 1, "cursor=");
      if (c)
         cursor = (uint64_t)strtoull(c + 7, NULL, 10);
   }

   char probe[80];
   if (presence_session_json(id, probe, sizeof(probe)) == 0)
   {
      send_response(fd, 404, "{\"error\":\"no such session\"}", request_id);
      return;
   }
   write_sse_headers(fd, request_id);
   char *data = (char *)malloc(PRESENCE_EVENT_DATA_MAX + 1);
   if (!data)
      return; /* headers already sent; just drop the stream */
   char ev[PRESENCE_EVENT_NAME_MAX];
   for (;;)
   {
      presence_wait_t w =
          presence_wait(id, &cursor, 1000, ev, sizeof(ev), data, PRESENCE_EVENT_DATA_MAX + 1);
      if (w == PRESENCE_WAIT_EVENT)
      {
         /* Emit the post-advance cursor as the SSE id so the client can persist
          * it and resume from exactly here after a reconnect/remount (?cursor=). */
         char idline[40];
         int idn = snprintf(idline, sizeof(idline), "id: %llu\n", (unsigned long long)cursor);
         if (idn > 0 && write_all_fd(fd, idline, idn) < 0)
            break;
         if (ev[0])
         {
            if (write_all_fd(fd, "event: ", 7) < 0)
               break;
            if (write_all_fd(fd, ev, (int)strlen(ev)) < 0)
               break;
            if (write_all_fd(fd, "\n", 1) < 0)
               break;
         }
         if (write_all_fd(fd, "data: ", 6) < 0)
            break;
         if (write_all_fd(fd, data, (int)strlen(data)) < 0)
            break;
         if (write_all_fd(fd, "\n\n", 2) < 0)
            break;
         continue;
      }
      if (w == PRESENCE_WAIT_GONE)
         break;
      /* PRESENCE_WAIT_TIMEOUT: heartbeat; a failed write means the client
       * disconnected, so stop streaming and free the listener. */
      if (write_all_fd(fd, ": keep-alive\n\n", 13) < 0)
         break;
   }
   free(data);
}
