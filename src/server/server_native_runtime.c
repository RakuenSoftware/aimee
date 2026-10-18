/* Native children hold only a lifetime-scoped local capability. Existing HTTP
 * owners admit model/tool requests under the captured actor and registration. */
#include "server_native_runtime.h"
#include "cli_proxy.h"
#include "server_http.h"
#include "server_http_identity.h"
#include "db1_client/server_sessions.h"
#include "platform_random.h"
#include "aimee_sha256.h"
#include <aimee/ir/session_event.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct server_native_runtime
{
   cli_proxy_t *listener;
   agent_t selected;
   request_context_t owner;
   agent_request_creds_t credentials;
   char session[128], token[65], url[64], binding[65], lineage[65];
};

static int native_digest_valid(const char *digest)
{
   if (!digest || strlen(digest) != 64)
      return 0;
   for (const char *p = digest; *p; p++)
      if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f'))
         return 0;
   return 1;
}

typedef struct
{
   const cli_proxy_io_t *io;
   atomic_int finished, cancelled;
} native_cancellation_t;

static void *native_cancel_watch(void *context)
{
   native_cancellation_t *watch = context;
   const struct timespec tick = {.tv_nsec = 20000000};
   while (!atomic_load(&watch->finished))
   {
      if (watch->io->cancelled(watch->io->context))
      {
         atomic_store(&watch->cancelled, 1);
         break;
      }
      nanosleep(&tick, NULL);
   }
   return NULL;
}

static int native_route(const char *method, const char *path, void *context)
{
   const server_native_runtime_t *runtime = context;
   /* The child reaches model entry and the existing MCP tool owner only.
    * Operator controls, session replies and arbitrary CLI methods are never
    * native model authority, even when the parent transport is an operator. */
   if (!method || !path || strchr(path, '?') || strchr(path, '#'))
      return 0;
   int admitted = !strcmp(method, "GET") &&
                  (!strcmp(path, "/v1/models") || !strcmp(path, "/v1/cli/manifest") ||
                   !strcmp(path, "/v1/mcp/tools_list"));
   if (!strcmp(method, "POST"))
      admitted = !strcmp(path, "/v1/responses") || !strcmp(path, "/v1/responses/compact") ||
                 !strcmp(path, "/v1/messages") || !strcmp(path, "/v1/messages/count_tokens") ||
                 !strcmp(path, "/v1/chat/completions") || !strcmp(path, "/v1/mcp/call") ||
                 !strcmp(path, "/v1/mcp/recheck") || !strcmp(path, "/v1/mcp/audit");
   return admitted &&
          server_http_route_allowed_caps(0, runtime->owner.capabilities, method, path, 0);
}

static int native_forward(const char *method, const char *path, const char *headers,
                          const void *body, unsigned long length, const char *session,
                          const cli_proxy_io_t *io, void *context)
{
   /* The listener admits protocol headers only, never identity or credentials. */
   server_native_runtime_t *runtime = context;
   if (!session || strcmp(session, runtime->session) || length > 4u * 1024u * 1024u ||
       !native_route(method, path, runtime))
      return -1;
   db1_server_session_t source;
   if (db1_server_session_get(runtime->session, &source) ||
       strcmp(source.principal, runtime->owner.vault_principal))
      return -1;
   char *input = malloc((size_t)length + 1);
   if (!input)
      return -1;
   if (length)
      memcpy(input, body, (size_t)length);
   input[length] = 0;
   request_context_snapshot_t previous_request;
   request_context_snapshot(&previous_request);
   agent_request_creds_t previous_credentials;
   agent_request_creds_snapshot(&previous_credentials);
   const agent_t *previous_model = agent_registry_bind_ingress_model(&runtime->selected);
   aimee_ir_session_begin_task();
   request_context_t admitted = runtime->owner;
   snprintf(admitted.method, sizeof admitted.method, "%s", method);
   snprintf(admitted.path, sizeof admitted.path, "%s", path);
   /* The browser turn's dedup key cannot collapse distinct SDK requests. */
   admitted.idempotency_key[0] = 0;
   request_context_set(&admitted);
   server_http_identity_bind_request_owner(&admitted);
   agent_request_creds_restore(&runtime->credentials);
   agent_set_request_session(runtime->session);
   int rc = -1;
   agent_codex_credentials_t current = {0};
   if (runtime->lineage[0])
   {
      const char *registration = runtime->selected.registration[0] ? runtime->selected.registration
                                                                   : runtime->selected.name;
      if (agent_codex_connection_credentials(registration, &current) ||
          strcmp(current.source_digest, runtime->lineage))
         goto restore;
      agent_set_request_codex_creds_for(&runtime->selected, current.token, current.account_id);
   }
   native_cancellation_t watch = {.io = io};
   pthread_t monitor;
   if (pthread_create(&monitor, NULL, native_cancel_watch, &watch))
      goto restore;
   agent_set_request_cancel(&watch.cancelled);
   anthropic_http_capture_request_headers(headers);
   io->response_started(io->context);
   rc = server_http_native_forward(io->downstream_fd, method, path, input, (int)length);
   atomic_store(&watch.finished, 1);
   pthread_join(monitor, NULL);
   agent_set_request_cancel(NULL);
   anthropic_http_capture_request_headers(NULL);
restore:
   OPENSSL_cleanse(&current, sizeof current);
   agent_registry_bind_ingress_model(previous_model);
   agent_request_creds_restore(&previous_credentials);
   OPENSSL_cleanse(&previous_credentials, sizeof previous_credentials);
   request_context_restore(&previous_request);
   server_http_identity_bind_request_owner(request_context_get());
   request_context_snapshot_clear(&previous_request);
   free(input);
   return rc;
}

server_native_runtime_t *server_native_runtime_open(const agent_t *selected,
                                                    const request_context_t *owner,
                                                    const char *session_id)
{
   if (!selected || !selected->enabled || !owner || !owner->vault_principal[0] ||
       owner->vault_transport <= ATTEST_TCP_BEARER || owner->vault_transport > ATTEST_MTLS_CLIENT ||
       !(owner->capabilities & CAP_CHAT) || !session_id || !session_id[0] ||
       strlen(session_id) >= 128)
      return NULL;
   db1_server_session_t session;
   if (db1_server_session_get(session_id, &session) ||
       strcmp(session.principal, owner->vault_principal))
      return NULL;
   server_native_runtime_t *runtime = calloc(1, sizeof *runtime);
   if (!runtime)
      return NULL;
   runtime->selected = *selected;
   runtime->owner = *owner;
   /* A model/tool capability never carries independent operator authority:
    * human gates, grant/credential administration and cross-user audit views
    * remain with the original authenticated operator transport. */
   runtime->owner.capabilities &= CAPS_AUTHENTICATED;
   snprintf(runtime->session, sizeof runtime->session, "%s", session_id);
   snprintf(runtime->owner.session_key, sizeof runtime->owner.session_key, "%s", session_id);
   /* Only selected connection credentials may reach the model callback. A
    * previous worker's per-turn override is never inherited by this owner. */
   snprintf(runtime->credentials.session_id, sizeof runtime->credentials.session_id, "%s",
            session_id);
   snprintf(runtime->credentials.vault_principal, sizeof runtime->credentials.vault_principal, "%s",
            owner->vault_principal);
   agent_codex_credentials_t credentials = {0};
   if (!strcmp(selected->auth_type, "codex-oauth"))
   {
      const char *registration =
          selected->registration[0] ? selected->registration : selected->name;
      request_context_snapshot_t previous_request;
      request_context_snapshot(&previous_request);
      agent_request_creds_t previous_credentials;
      agent_request_creds_snapshot(&previous_credentials);
      request_context_set(&runtime->owner);
      agent_request_creds_restore(&runtime->credentials);
      int rc = agent_codex_connection_credentials(registration, &credentials);
      agent_request_creds_restore(&previous_credentials);
      OPENSSL_cleanse(&previous_credentials, sizeof previous_credentials);
      request_context_restore(&previous_request);
      request_context_snapshot_clear(&previous_request);
      if (rc || !native_digest_valid(credentials.source_digest))
         goto failed;
      snprintf(runtime->lineage, sizeof runtime->lineage, "%s", credentials.source_digest);
   }
   const char *parts[] = {owner->vault_principal, selected->registration, selected->name,
                          selected->provider,     selected->model,        selected->endpoint,
                          credentials.account_id, runtime->lineage};
   char binding[4096];
   size_t used = 0;
   for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++)
   {
      int n = snprintf(binding + used, sizeof binding - used, "%zu:%s", strlen(parts[i]), parts[i]);
      if (n < 0 || (size_t)n >= sizeof binding - used)
         goto failed;
      used += (size_t)n;
   }
   if (aimee_sha256_hex(binding, used, runtime->binding))
      goto failed;
   unsigned char random[32];
   if (platform_random_bytes(random, sizeof random))
      goto failed;
   for (size_t i = 0; i < sizeof random; i++)
      snprintf(runtime->token + 2 * i, 3, "%02x", random[i]);
   OPENSSL_cleanse(random, sizeof random);
   unsigned port = 0;
   runtime->listener = cli_proxy_start_scoped(0, runtime->token, session_id, &port, native_route,
                                              native_forward, runtime);
   if (!runtime->listener)
      goto failed;
   snprintf(runtime->url, sizeof runtime->url, "http://127.0.0.1:%u", port);
   OPENSSL_cleanse(&credentials, sizeof credentials);
   return runtime;
failed:
   OPENSSL_cleanse(&credentials, sizeof credentials);
   server_native_runtime_close(runtime);
   return NULL;
}

const char *server_native_runtime_url(const server_native_runtime_t *runtime)
{
   return runtime ? runtime->url : "";
}
const char *server_native_runtime_token(const server_native_runtime_t *runtime)
{
   return runtime ? runtime->token : "";
}
const char *server_native_runtime_binding(const server_native_runtime_t *runtime)
{
   return runtime ? runtime->binding : "";
}
void server_native_runtime_close(server_native_runtime_t *runtime)
{
   if (!runtime)
      return;
   cli_proxy_stop(runtime->listener);
   OPENSSL_cleanse(runtime, sizeof *runtime);
   free(runtime);
}
