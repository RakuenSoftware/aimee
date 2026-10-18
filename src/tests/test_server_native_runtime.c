#include "server_native_runtime.h"
#include "server_native_replay.h"
#include <aimee/translation/aimee_frontend.h>
#include "server_http.h"
#include "server_http_identity.h"
#include "db1_client/server_sessions.h"
#include <aimee/core/connection/socket.h>
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local agent_request_creds_t current_credentials;
static _Thread_local const agent_t *current_model;
static _Thread_local atomic_int *current_cancel;
static _Thread_local char identity[VAULT_PRINCIPAL_MAX];
static _Thread_local int protocol_captured;
static pthread_mutex_t ids_mutex = PTHREAD_MUTEX_INITIALIZER;
static char seen_ids[64][64];
static int seen_count;
static void probe_replay_scope(const char *path);
static atomic_int calls, cancelled, wait_for_cancel, source_changed, session_removed,
    malformed_source;

void agent_request_creds_snapshot(agent_request_creds_t *out)
{
   *out = current_credentials;
}
void agent_request_creds_restore(const agent_request_creds_t *value)
{
   current_credentials = *value;
}
void agent_set_request_session(const char *session)
{
   snprintf(current_credentials.session_id, sizeof current_credentials.session_id, "%s", session);
}
void agent_set_request_codex_creds_for(const agent_t *agent, const char *token, const char *account)
{
   assert(!strcmp(agent->registration, "selected-account"));
   snprintf(current_credentials.codex_token, sizeof current_credentials.codex_token, "%s", token);
   snprintf(current_credentials.codex_account_id, sizeof current_credentials.codex_account_id, "%s",
            account);
}
void agent_set_request_cancel(atomic_int *flag)
{
   current_cancel = flag;
}
const agent_t *agent_registry_bind_ingress_model(const agent_t *selected)
{
   const agent_t *previous = current_model;
   current_model = selected;
   return previous;
}
int agent_codex_connection_credentials(const char *registration, agent_codex_credentials_t *out)
{
   const request_context_t *owner = request_context_get();
   assert(owner && !strcmp(owner->vault_principal, current_credentials.vault_principal));
   assert(!strcmp(owner->session_key, current_credentials.session_id));
   assert(!(owner->capabilities & (CAP_WORKFLOW_ADMIN | CAP_GRANT_ADMIN | CAP_SHADOW_ADMIN)));
   if (strcmp(registration, "selected-account"))
      return -1;
   memset(out, 0, sizeof *out);
   strcpy(out->token, "host-only-upstream-secret");
   strcpy(out->account_id, "selected-vendor-account");
   memset(out->source_digest,
          atomic_load(&malformed_source) ? 'z'
          : atomic_load(&source_changed) ? 'b'
                                         : 'a',
          64);
   return 0;
}
int db1_server_session_get(const char *session, db1_server_session_t *out)
{
   memset(out, 0, sizeof *out);
   if (atomic_load(&session_removed))
      return -1;
   if (!strcmp(session, "session-a"))
      strcpy(out->principal, "webuser:alice");
   else if (!strcmp(session, "session-b"))
      strcpy(out->principal, "webuser:bob");
   else
      return -1;
   return 0;
}
void server_http_identity_bind_request_owner(const request_context_t *owner)
{
   snprintf(identity, sizeof identity, "%s", owner ? owner->vault_principal : "");
}
int server_http_route_allowed_caps(int tcp, uint32_t caps, const char *method, const char *path,
                                   int writes)
{
   (void)writes;
   assert(!tcp);
   if (!strcmp(method, "GET") && !strcmp(path, "/v1/cli/manifest"))
      return 1;
   if (strcmp(method, "POST"))
      return 0;
   if (!strcmp(path, "/v1/chat/control") || !strcmp(path, "/v1/config/set"))
      return 1; /* Parent authority must still be outside the child surface. */
   return !strcmp(path, "/v1/responses")  ? (caps & CAP_CHAT) != 0
          : !strcmp(path, "/v1/mcp/call") ? (caps & CAP_TOOL_EXECUTE) != 0
                                          : 0;
}
void anthropic_http_capture_request_headers(const char *headers)
{
   protocol_captured = headers != NULL;
   if (headers)
   {
      assert(strstr(headers, "Anthropic-Version: 2023-06-01"));
      assert(strstr(headers, "Anthropic-Beta: fixture-beta"));
      assert(!strstr(headers, "Authorization") && !strstr(headers, "mallory") &&
             !strstr(headers, "forged"));
   }
}
int server_http_native_forward(int fd, const char *method, const char *path, const char *body,
                               int length)
{
   (void)method;
   probe_replay_scope(path);
   assert(protocol_captured);
   assert(length == 2 && !strcmp(body, "{}"));
   const request_context_t *owner = request_context_get();
   assert(owner && owner->vault_transport == ATTEST_WEBCHAT_TRUSTED);
   assert(!strcmp(identity, owner->vault_principal));
   assert(!strcmp(owner->method, method) && !strcmp(owner->path, path));
   assert(owner->model_execution);
   assert(!owner->idempotency_key[0]);
   assert(strlen(owner->request_id) == 32);
   pthread_mutex_lock(&ids_mutex);
   assert(seen_count < 64);
   for (int i = 0; i < seen_count; i++)
      assert(strcmp(seen_ids[i], owner->request_id));
   strcpy(seen_ids[seen_count++], owner->request_id);
   pthread_mutex_unlock(&ids_mutex);
   assert(!strcmp(current_credentials.vault_principal, identity));
   assert(!strcmp(current_credentials.session_id, owner->session_key));
   assert(current_model && !strcmp(current_model->registration, "selected-account"));
   assert(!strcmp(current_credentials.codex_token, "host-only-upstream-secret"));
   atomic_fetch_add(&calls, 1);
   if (atomic_load(&wait_for_cancel))
   {
      for (int i = 0; i < 300 && !atomic_load(current_cancel); i++)
         usleep(10000);
      assert(atomic_load(current_cancel));
      atomic_store(&cancelled, 1);
      return -1;
   }
   char response[512], payload[256];
   int n =
       snprintf(payload, sizeof payload, "{\"owner\":\"%s\",\"session\":\"%s\",\"path\":\"%s\"}",
                identity, current_credentials.session_id, path);
   int size =
       snprintf(response, sizeof response,
                "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", n, payload);
   return aimee_core_socket_write_all(fd, response, (size_t)size);
}
void aimee_ir_session_begin_task(void)
{
}

/* The standalone replay fixture exercises the real durable proof owner. This
 * listener fixture verifies its guard/sink are confined to model requests. */
struct server_native_replay
{
   char actor[VAULT_PRINCIPAL_MAX];
};
static _Thread_local aimee_frontend_state_scope_t state_guard;
static _Thread_local aimee_ir_session_sink_fn installed_sink;
static _Thread_local void *sink_context;
server_native_replay_t *server_native_replay_open(const char *session, const char *actor,
                                                  const char *scope, const char *model)
{
   assert(session[0] && strlen(scope) == 64 && !strcmp(model, "selected-model"));
   server_native_replay_t *r = calloc(1, sizeof *r);
   assert(r);
   strcpy(r->actor, actor);
   return r;
}
void server_native_replay_close(server_native_replay_t *r)
{
   free(r);
}
int server_native_replay_restore(aimee_request_t *request, void *context)
{
   (void)request;
   server_native_replay_t *r = context;
   assert(!strcmp(r->actor, identity));
   return 0;
}
int server_native_replay_capture(const aimee_session_event_t *event, void *context)
{
   (void)event;
   return server_native_replay_restore(NULL, context);
}
aimee_frontend_state_scope_t aimee_frontend_bind_state_guard(aimee_frontend_state_guard_fn guard,
                                                             void *context)
{
   aimee_frontend_state_scope_t previous = state_guard;
   state_guard = (aimee_frontend_state_scope_t){guard, context};
   return previous;
}
aimee_ir_session_sink_fn aimee_ir_session_get_sink(void **context)
{
   *context = sink_context;
   return installed_sink;
}
void aimee_ir_session_set_sink(aimee_ir_session_sink_fn sink, void *context)
{
   installed_sink = sink;
   sink_context = context;
}
void aimee_ir_session_restore_sink(aimee_ir_session_sink_fn sink, void *context, int failed)
{
   assert(!failed);
   aimee_ir_session_set_sink(sink, context);
}
int aimee_ir_session_capture_failed(void)
{
   return 0;
}
static void probe_replay_scope(const char *path)
{
   if (!strcmp(path, "/v1/responses"))
   {
      assert(state_guard.guard && installed_sink);
      aimee_request_t request = {0};
      assert(!state_guard.guard(&request, state_guard.context));
      aimee_session_event_t event = {.kind = AIMEE_SESSION_PROJECTION};
      assert(!installed_sink(&event, sink_context));
   }
   else
      assert(!state_guard.guard && !installed_sink);
}
static int send_request(const server_native_runtime_t *runtime, const char *path, const char *token)
{
   const char *port = strrchr(server_native_runtime_url(runtime), ':') + 1;
   int fd = aimee_core_socket_connect("127.0.0.1", port, 2000);
   assert(fd >= 0);
   assert(!aimee_core_socket_set_timeouts(fd, 5000, 2000));
   char request[1024];
   int n = snprintf(request, sizeof request,
                    "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%s\r\nAuthorization: Bearer %s\r\n"
                    "X-Aimee-Webuser: mallory\r\naimee-session-id: forged\r\n"
                    "Anthropic-Version: 2023-06-01\r\nAnthropic-Beta: fixture-beta\r\n"
                    "Content-Length: 2\r\n\r\n{}",
                    path, port, token);
   assert(!aimee_core_socket_write_all(fd, request, (size_t)n));
   return fd;
}
static void request(const server_native_runtime_t *runtime, const char *path, const char *token,
                    char response[1024])
{
   int fd = send_request(runtime, path, token);
   size_t used = 0;
   while (used < 1023)
   {
      long n = aimee_core_socket_read(fd, response + used, 1023 - used);
      assert(n >= 0);
      if (!n)
         break;
      used += (size_t)n;
   }
   response[used] = 0;
   aimee_core_socket_close(fd);
}
int main(void)
{
   agent_t selected = {.enabled = 1};
   strcpy(selected.name, "selected");
   strcpy(selected.registration, "selected-account");
   strcpy(selected.model, "selected-model");
   strcpy(selected.auth_type, "codex-oauth");
   request_context_t a = {.vault_transport = ATTEST_WEBCHAT_TRUSTED,
                          .capabilities = CAP_CHAT | CAP_TOOL_EXECUTE};
   strcpy(a.vault_principal, "webuser:alice");
   strcpy(a.idempotency_key, "browser-turn-only");
   strcpy(a.request_id, "browser-request-only");
   a.capabilities |= CAP_WORKFLOW_ADMIN | CAP_GRANT_ADMIN | CAP_SHADOW_ADMIN;
   request_context_t b = a;
   strcpy(b.vault_principal, "webuser:bob");
   b.capabilities = CAP_CHAT;
   assert(!server_native_runtime_open(&selected, NULL, "session-a"));
   assert(!server_native_runtime_open(&selected, &b, "session-a"));
   atomic_store(&malformed_source, 1);
   assert(!server_native_runtime_open(&selected, &a, "session-a"));
   atomic_store(&malformed_source, 0);
   server_native_runtime_t *alice = server_native_runtime_open(&selected, &a, "session-a");
   server_native_runtime_t *bob = server_native_runtime_open(&selected, &b, "session-b");
   assert(alice && bob);
   assert(strcmp(server_native_runtime_binding(alice), server_native_runtime_binding(bob)));
   assert(strlen(server_native_runtime_token(alice)) == 64);
   char response[1024];
   for (int i = 0; i < 16; i++)
   {
      request(alice, "/v1/responses", server_native_runtime_token(alice), response);
      assert(strstr(response, "200 OK") && strstr(response, "webuser:alice") &&
             strstr(response, "session-a") && !strstr(response, "forged"));
      assert(!strstr(response, "upstream-secret"));
      request(bob, "/v1/responses", server_native_runtime_token(bob), response);
      assert(strstr(response, "webuser:bob") && strstr(response, "session-b"));
   }
   assert(!request_context_get());
   assert(!current_credentials.vault_principal[0]);
   int before = atomic_load(&calls);
   request(alice, "/v1/responses", server_native_runtime_token(bob), response);
   assert(strstr(response, "401") && atomic_load(&calls) == before);
   request(alice, "/v1/chat/control", server_native_runtime_token(alice), response);
   assert(strstr(response, "404") && atomic_load(&calls) == before);
   request(alice, "/v1/config/set", server_native_runtime_token(alice), response);
   assert(strstr(response, "404") && atomic_load(&calls) == before);
   request(bob, "/v1/mcp/call", server_native_runtime_token(bob), response);
   assert(strstr(response, "404") && atomic_load(&calls) == before);
   request(alice, "/v1/mcp/call", server_native_runtime_token(alice), response);
   assert(strstr(response, "200 OK") && strstr(response, "webuser:alice"));
   before = atomic_load(&calls);
   atomic_store(&source_changed, 1);
   request(alice, "/v1/responses", server_native_runtime_token(alice), response);
   assert(strstr(response, "502") && atomic_load(&calls) == before);
   atomic_store(&source_changed, 0);
   atomic_store(&session_removed, 1);
   request(alice, "/v1/responses", server_native_runtime_token(alice), response);
   assert(strstr(response, "502") && atomic_load(&calls) == before);
   atomic_store(&session_removed, 0);
   atomic_store(&wait_for_cancel, 1);
   int fd = send_request(alice, "/v1/responses", server_native_runtime_token(alice));
   for (int i = 0; i < 300 && atomic_load(&calls) == before; i++)
      usleep(10000);
   assert(atomic_load(&calls) > before);
   aimee_core_socket_shutdown(fd);
   aimee_core_socket_close(fd);
   for (int i = 0; i < 300 && !atomic_load(&cancelled); i++)
      usleep(10000);
   assert(atomic_load(&cancelled));
   server_native_runtime_close(alice);
   server_native_runtime_close(bob);
   puts("native runtime: owned actor/session/model, capability, lineage and cancellation passed");
   return 0;
}
