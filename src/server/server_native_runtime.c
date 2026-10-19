/* Native children hold only a lifetime-scoped local capability. Existing HTTP
 * owners admit model/tool requests under the captured actor and registration. */
#include "server_native_runtime.h"
#include <aimee/tools/agent_tools.h>
#include "agent_source_authority.h"
#include "agent_tasks.h"
#include "server_delegate_scope.h"
#include "server_session_control.h"
#include "modules/workspace/workspace_provider.h"
#include "util.h"
#include "server_native_replay.h"
#include <aimee/translation/aimee_frontend.h>
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
   server_control_scope_t *controls;
   cli_proxy_t *listener;
   server_native_replay_t *replay;
   agent_t selected;
   request_context_t owner;
   agent_request_creds_t credentials;
   agent_tools_scope_snapshot_t tools;
   agent_source_authority_snapshot_t source;
   server_delegate_scope_t delegate;
   const workspace_provider_t *workspace;
   char cwd[MAX_PATH_LEN];
   int job_id, api_key_source, claude_source;
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
   int model_request = !strcmp(method, "POST") && (strncmp(path, "/v1/mcp/", 8) != 0);
   /* Model state requires the actual selected Vault source's stable lineage.
    * Unsupported credential sources cannot borrow a default account. */
   if (model_request && !runtime->replay)
      return -1;
   char *input = malloc((size_t)length + 1);
   if (!input)
      return -1;
   if (length)
      memcpy(input, body, (size_t)length);
   input[length] = 0;
   if (!model_request && !strcmp(method, "POST"))
   {
      /* Session and project scope are host identity, even inside the MCP body.
       * Tool arguments remain intact; exact numeric spelling is retained. */
      cJSON *request = aimee_ir_json_parse_frame(input, (size_t)length);
      if (!cJSON_IsObject(request))
      {
         cJSON_Delete(request);
         free(input);
         return -1;
      }
      cJSON_DeleteItemFromObjectCaseSensitive(request, "session_id");
      cJSON_DeleteItemFromObjectCaseSensitive(request, "cwd");
      if (!cJSON_AddStringToObject(request, "session_id", runtime->session) ||
          (runtime->cwd[0] && !cJSON_AddStringToObject(request, "cwd", runtime->cwd)))
      {
         cJSON_Delete(request);
         free(input);
         return -1;
      }
      free(input);
      input = cJSON_PrintUnformatted(request);
      cJSON_Delete(request);
      if (!input)
         return -1;
      length = (unsigned long)strlen(input);
      if (length > 4u * 1024u * 1024u)
      {
         free(input);
         return -1;
      }
   }
   /* These host leases outlive the child and this listener. Workers receive
    * owned permission/source copies and borrowed stable resource providers. */
   agent_source_authority_snapshot_t previous_source = {0}, bound_source = {0};
   if (agent_source_authority_tls_capture_checked(&previous_source) ||
       agent_source_authority_snapshot_clone(&runtime->source, &bound_source))
   {
      agent_source_authority_snapshot_destroy(&previous_source);
      agent_source_authority_snapshot_destroy(&bound_source);
      free(input);
      return -1;
   }
   agent_tools_scope_snapshot_t previous_tools;
   agent_tools_scope_bookmark(&previous_tools);
   server_delegate_scope_t previous_delegate;
   server_delegate_scope_capture(&previous_delegate);
   const workspace_provider_t *previous_workspace = workspace_provider_active();
   int previous_job = agent_get_durable_job_id();
   char previous_cwd[MAX_PATH_LEN], previous_session[128] = "";
   const char *cwd = run_cmd_get_cwd();
   snprintf(previous_cwd, sizeof previous_cwd, "%s", cwd ? cwd : "");
   if (session_id_override_active())
      snprintf(previous_session, sizeof previous_session, "%s", session_id());
   server_control_scope_t *previous_controls =
       server_control_scope_bind(!strncmp(path, "/v1/mcp/", 8) ? runtime->controls : NULL);
   agent_tools_scope_restore(&runtime->tools);
   agent_tools_set_effect_authorized(0);
   agent_source_authority_tls_restore(&bound_source);
   server_delegate_scope_restore(&runtime->delegate);
   workspace_provider_set_active(runtime->workspace);
   agent_set_durable_job(runtime->job_id);
   run_cmd_set_cwd(runtime->cwd);
   session_id_set_override(runtime->session);
   request_context_snapshot_t previous_request;
   request_context_snapshot(&previous_request);
   agent_request_creds_t previous_credentials;
   agent_request_creds_snapshot(&previous_credentials);
   const agent_t *previous_model = agent_registry_bind_ingress_model(&runtime->selected);
   aimee_ir_session_begin_task();
   int rc = -1;
   agent_codex_credentials_t current = {0};
   agent_api_credentials_t current_api = {0};
   agent_claude_credentials_t current_claude = {0};
   request_context_t admitted = runtime->owner;
   unsigned char request_nonce[16];
   if (platform_random_bytes(request_nonce, sizeof request_nonce))
      goto restore;
   for (unsigned i = 0; i < sizeof request_nonce; i++)
      snprintf(admitted.request_id + 2 * i, 3, "%02x", request_nonce[i]);
   snprintf(admitted.method, sizeof admitted.method, "%s", method);
   snprintf(admitted.path, sizeof admitted.path, "%s", path);
   /* The browser turn's dedup key cannot collapse distinct SDK requests. */
   admitted.idempotency_key[0] = 0;
   request_context_set(&admitted);
   server_http_identity_bind_request_owner(&admitted);
   agent_request_creds_restore(&runtime->credentials);
   agent_set_request_session(runtime->session);
   if (runtime->lineage[0])
   {
      const char *registration = runtime->selected.registration[0] ? runtime->selected.registration
                                                                   : runtime->selected.name;
      if (runtime->claude_source)
      {
         if (agent_claude_connection_credentials(registration, &current_claude) ||
             strcmp(current_claude.source_digest, runtime->lineage) ||
             (model_request &&
              agent_set_request_claude_creds_for(&runtime->selected, &current_claude)))
            goto restore;
      }
      else if (runtime->api_key_source)
      {
         if (agent_api_connection_credentials(&runtime->selected, &current_api) ||
             strcmp(current_api.source_digest, runtime->lineage) ||
             (model_request &&
              agent_set_request_api_credentials_for(&runtime->selected, &current_api)))
            goto restore;
      }
      else
      {
         if (agent_codex_connection_credentials(registration, &current) ||
             strcmp(current.source_digest, runtime->lineage))
            goto restore;
         if (model_request)
            agent_set_request_codex_creds_for(&runtime->selected, current.token,
                                              current.account_id);
      }
   }
   native_cancellation_t watch = {.io = io};
   pthread_t monitor;
   if (pthread_create(&monitor, NULL, native_cancel_watch, &watch))
      goto restore;
   agent_set_request_cancel(&watch.cancelled);
   anthropic_http_capture_request_headers(headers);
   void *previous_sink_context = NULL;
   aimee_ir_session_sink_fn previous_sink = aimee_ir_session_get_sink(&previous_sink_context);
   int previous_failed = aimee_ir_session_capture_failed();
   aimee_frontend_state_scope_t previous_guard = {0};
   if (model_request)
   {
      previous_guard =
          aimee_frontend_bind_state_guard(server_native_replay_restore, runtime->replay);
      aimee_ir_session_set_sink(server_native_replay_capture, runtime->replay);
   }
   io->response_started(io->context);
   rc = server_http_native_forward(io->downstream_fd, method, path, input, (int)length);
   if (model_request)
   {
      aimee_frontend_bind_state_guard(previous_guard.guard, previous_guard.context);
      aimee_ir_session_restore_sink(previous_sink, previous_sink_context,
                                    previous_failed || aimee_ir_session_capture_failed());
   }
   atomic_store(&watch.finished, 1);
   pthread_join(monitor, NULL);
   agent_set_request_cancel(NULL);
   anthropic_http_capture_request_headers(NULL);
restore:
   OPENSSL_cleanse(&current_claude, sizeof current_claude);
   OPENSSL_cleanse(&current_api, sizeof current_api);
   OPENSSL_cleanse(&current, sizeof current);
   agent_registry_bind_ingress_model(previous_model);
   agent_request_creds_restore(&previous_credentials);
   OPENSSL_cleanse(&previous_credentials, sizeof previous_credentials);
   request_context_restore(&previous_request);
   server_http_identity_bind_request_owner(request_context_get());
   request_context_snapshot_clear(&previous_request);
   agent_tools_set_effect_authorized(0);
   agent_tools_scope_restore(&previous_tools);
   agent_source_authority_tls_restore(&previous_source);
   server_delegate_scope_restore(&previous_delegate);
   workspace_provider_set_active(previous_workspace);
   agent_set_durable_job(previous_job);
   run_cmd_set_cwd(previous_cwd);
   session_id_set_override(previous_session);
   server_control_scope_bind(previous_controls);
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
   runtime->owner.model_execution = 1;
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
   agent_api_credentials_t api_credentials = {0};
   agent_claude_credentials_t claude_credentials = {0};
   if (agent_tools_scope_capture_owned(&runtime->tools) ||
       agent_source_authority_tls_capture_checked(&runtime->source) ||
       server_control_scope_capture(session_id, owner->vault_principal, &runtime->controls))
      goto failed;
   /* An ordinary primary has no delegate source overlay. Make that absence
    * explicit rather than falling back to a concurrent delegate's global env. */
   if (!runtime->source.active)
   {
      agent_source_authority_snapshot_destroy(&runtime->source);
      runtime->source.active = 1;
   }
   runtime->workspace = workspace_provider_active();
   runtime->job_id = agent_get_durable_job_id();
   server_delegate_scope_capture(&runtime->delegate);
   const char *cwd = run_cmd_get_cwd();
   snprintf(runtime->cwd, sizeof runtime->cwd, "%s", cwd ? cwd : "");
   {
      request_context_snapshot_t previous_request;
      request_context_snapshot(&previous_request);
      agent_request_creds_t previous_credentials;
      agent_request_creds_snapshot(&previous_credentials);
      request_context_set(&runtime->owner);
      agent_request_creds_restore(&runtime->credentials);
      int codex = !strcmp(selected->auth_type, "codex-oauth");
      int claude = !strcmp(selected->auth_type, "claude-oauth");
      const char *registration =
          selected->registration[0] ? selected->registration : selected->name;
      int rc = codex    ? agent_codex_connection_credentials(registration, &credentials)
               : claude ? agent_claude_connection_credentials(registration, &claude_credentials)
                        : agent_api_connection_credentials(selected, &api_credentials);
      agent_request_creds_restore(&previous_credentials);
      OPENSSL_cleanse(&previous_credentials, sizeof previous_credentials);
      request_context_restore(&previous_request);
      request_context_snapshot_clear(&previous_request);
      if (codex)
      {
         if (rc || !native_digest_valid(credentials.source_digest))
            goto failed;
         snprintf(runtime->lineage, sizeof runtime->lineage, "%s", credentials.source_digest);
      }
      else if (claude)
      {
         if (rc || !native_digest_valid(claude_credentials.source_digest))
            goto failed;
         runtime->claude_source = 1;
         snprintf(runtime->lineage, sizeof runtime->lineage, "%s",
                  claude_credentials.source_digest);
      }
      else if (!rc)
      {
         if (!native_digest_valid(api_credentials.source_digest))
            goto failed;
         runtime->api_key_source = 1;
         snprintf(runtime->lineage, sizeof runtime->lineage, "%s", api_credentials.source_digest);
      }
      /* Unsupported/unconfigured sources retain MCP-only operation. They cannot
       * forward model traffic or acquire a replay/cache scope without lineage. */
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
   if (runtime->lineage[0])
   {
      runtime->replay = server_native_replay_open(runtime->session, runtime->owner.vault_principal,
                                                  runtime->binding, runtime->selected.model);
      if (!runtime->replay)
         goto failed;
   }
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
   OPENSSL_cleanse(&claude_credentials, sizeof claude_credentials);
   OPENSSL_cleanse(&api_credentials, sizeof api_credentials);
   OPENSSL_cleanse(&credentials, sizeof credentials);
   return runtime;
failed:
   OPENSSL_cleanse(&claude_credentials, sizeof claude_credentials);
   OPENSSL_cleanse(&api_credentials, sizeof api_credentials);
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
static int native_name_order(const void *left, const void *right)
{
   return strcmp(*(const char *const *)left, *(const char *const *)right);
}

int server_native_runtime_cache_scope(const server_native_runtime_t *runtime,
                                      const server_native_execution_profile_t *profile,
                                      char scope[65])
{
   if (scope)
      scope[0] = 0;
   if (!scope || !runtime || !runtime->replay || !native_digest_valid(runtime->lineage) ||
       !runtime->workspace || !runtime->cwd[0] || !profile || !profile->sdk ||
       (strcmp(profile->sdk, "codex") && strcmp(profile->sdk, "claude")) ||
       !native_digest_valid(profile->build_digest) ||
       !native_digest_valid(profile->launch_digest) ||
       !native_digest_valid(profile->workspace_digest) || runtime->tools.denied_count < 0 ||
       runtime->tools.denied_count > 4096)
      return -1;
   cJSON *value = cJSON_CreateObject(), *denied = cJSON_CreateArray();
   if (!value || !denied)
   {
      cJSON_Delete(value);
      cJSON_Delete(denied);
      return -1;
   }
   int failed = 0;
#define PROFILE_TEXT(key, text)  failed |= cJSON_AddStringToObject(value, key, text) == NULL
#define PROFILE_INT(key, number) failed |= cJSON_AddNumberToObject(value, key, number) == NULL
   PROFILE_INT("version", 1);
   PROFILE_TEXT("account_model", runtime->binding);
   PROFILE_TEXT("auth_type", runtime->selected.auth_type);
   PROFILE_TEXT("session", runtime->session);
   PROFILE_TEXT("sdk", profile->sdk);
   PROFILE_TEXT("sdk_build", profile->build_digest);
   PROFILE_TEXT("launch", profile->launch_digest);
   PROFILE_TEXT("workspace", profile->workspace_digest);
   PROFILE_INT("workspace_kind", runtime->workspace->kind);
   PROFILE_TEXT("cwd", runtime->cwd);
   PROFILE_TEXT("role", runtime->tools.role);
   PROFILE_TEXT("toolset", runtime->tools.toolset);
   PROFILE_TEXT("read_root", runtime->tools.read_only_root);
   PROFILE_TEXT("write_root", runtime->tools.write_root);
   PROFILE_INT("write", runtime->tools.write_capable);
   PROFILE_INT("shell", runtime->tools.shell);
   PROFILE_INT("knowledge_write", runtime->tools.knowledge_write);
   PROFILE_INT("source_active", runtime->source.active);
   PROFILE_INT("source_authority", runtime->source.authority);
   PROFILE_TEXT("source_root", runtime->source.root);
   PROFILE_TEXT("source_paths", runtime->source.paths ? runtime->source.paths : "");
   PROFILE_INT("delegate_depth", runtime->delegate.depth);
   PROFILE_TEXT("delegate_parent", runtime->delegate.parent);
   PROFILE_INT("durable_job", runtime->job_id);
   char capabilities[32];
   snprintf(capabilities, sizeof capabilities, "%llu",
            (unsigned long long)runtime->owner.capabilities);
   PROFILE_TEXT("capabilities", capabilities);
#undef PROFILE_TEXT
#undef PROFILE_INT
   /* Denials are a set: order changes must not throw away a valid native prefix. */
   int count = runtime->tools.denied_count;
   const char **names = count ? calloc((size_t)count, sizeof *names) : NULL;
   if (count && !names)
      failed = 1;
   for (int i = 0; !failed && i < count; i++)
   {
      if (!runtime->tools.denied || !runtime->tools.denied[i])
         failed = 1;
      else
         names[i] = runtime->tools.denied[i];
   }
   if (!failed && count)
      qsort(names, (size_t)count, sizeof *names, native_name_order);
   for (int i = 0; !failed && i < count; i++)
   {
      if (i && !strcmp(names[i], names[i - 1]))
         continue;
      cJSON *name = cJSON_CreateString(names[i]);
      if (!name || !cJSON_AddItemToArray(denied, name))
      {
         cJSON_Delete(name);
         failed = 1;
      }
   }
   free(names);
   if (!cJSON_AddItemToObject(value, "denied", denied))
   {
      cJSON_Delete(denied);
      failed = 1;
   }
   char *wire = failed ? NULL : cJSON_PrintUnformatted(value);
   int rc = wire ? aimee_sha256_hex(wire, strlen(wire), scope) : -1;
   free(wire);
   cJSON_Delete(value);
   if (rc)
      scope[0] = 0;
   return rc;
}

void server_native_runtime_close(server_native_runtime_t *runtime)
{
   if (!runtime)
      return;
   cli_proxy_stop(runtime->listener);
   server_native_replay_close(runtime->replay);
   server_control_scope_release(runtime->controls);
   agent_tools_scope_destroy_owned(&runtime->tools);
   agent_source_authority_snapshot_destroy(&runtime->source);
   OPENSSL_cleanse(runtime, sizeof *runtime);
   free(runtime);
}
