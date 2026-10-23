/* Durable host-issued native delegation authority. Authorization snapshots are
 * encrypted by Vault and MAC-bound to this host; public Vault writes cannot mint
 * an attested actor. No provider credentials are copied into the snapshot. */
#include "server_native_delegate_source.h"
#include "request_context.h"
#include "util.h"
#include "db1_client/server_sessions.h"
#include "db1_client/wfe_store.h"
#include <pthread.h>
#include "vault_service.h"
#include "vault_server_key.h"
#include "modules/workspace/workspace_turn.h"
#include "aimee_sha256.h"
#include "cJSON.h"
#include "server.h"
#include "config.h"
#include "workflow_control_bus.h"
#include <aimee/ir/session_event.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <limits.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SOURCE_AGENT     "host-native-source"
#define SOURCE_MAX       (128u * 1024u)
#define SOURCE_PRINCIPAL 76u
struct server_native_delegate_source_scope
{
   request_context_snapshot_t previous_context;
   agent_request_creds_t previous_credentials;
   workspace_turn_scope_t *workspace;
   workspace_turn_scope_t *source_workspace;
   int source_directory;
   char previous_cwd[4096];
   int had_cwd;
   int directory;
   char handle[65], purpose[16], operation[257], role[64], registration[128], workdir[4096];
   char parent[128], actor[VAULT_PRINCIPAL_MAX], account[128];
   attested_transport_t transport;
   int tools, max_turns;
   long timeout_ms;
};

static int refuse(char *error, size_t capacity, const char *reason)
{
   if (error && capacity)
      snprintf(error, capacity, "%s", reason);
   return -1;
}
static int valid_handle(const char *handle)
{
   if (!handle || strlen(handle) != 64)
      return 0;
   for (int i = 0; i < 64; i++)
      if (!((handle[i] >= '0' && handle[i] <= '9') || (handle[i] >= 'a' && handle[i] <= 'f')))
         return 0;
   return 1;
}
static void clear_json(cJSON *value)
{
   for (cJSON *it = value; it; it = it->next)
   {
      if (it->valuestring)
         OPENSSL_cleanse(it->valuestring, strlen(it->valuestring));
      if (it->child)
         clear_json(it->child);
   }
}
static void free_secret_json(cJSON *value)
{
   clear_json(value);
   cJSON_Delete(value);
}
static void free_secret(char *value)
{
   if (value)
      OPENSSL_cleanse(value, strlen(value));
   free(value);
}
static int source_key(const char *handle, char key[64])
{
   char digest[65];
   if (!valid_handle(handle) || aimee_sha256_hex(handle, 64, digest))
      return -1;
   /* Credential identifiers are audited. Log only a one-way digest, never the
    * authorization handle itself. Full handle remains inside the sealed value. */
   snprintf(key, 64, "scope-%.48s", digest);
   return 0;
}
static int source_mac(const char *body, char output[65])
{
   static const char domain[] = "aimee.native-delegate-source.v1";
   unsigned char kek[VAULT_KEK_LEN] = {0}, signing[32] = {0}, mac[32] = {0};
   unsigned int length = 0;
   int valid = !vault_server_kek(kek) &&
               HMAC(EVP_sha256(), kek, sizeof kek, (const unsigned char *)domain, sizeof domain - 1,
                    signing, &length) &&
               length == sizeof signing &&
               HMAC(EVP_sha256(), signing, sizeof signing, (const unsigned char *)body,
                    strlen(body), mac, &length) &&
               length == sizeof mac;
   if (valid)
   {
      for (size_t i = 0; i < sizeof mac; i++)
         snprintf(output + i * 2, 3, "%02x", mac[i]);
   }
   OPENSSL_cleanse(kek, sizeof kek);
   OPENSSL_cleanse(signing, sizeof signing);
   OPENSSL_cleanse(mac, sizeof mac);
   return valid ? 0 : -1;
}
#define SOURCE_CONTEXT_STRINGS(X)                                                                  \
   X(principal)                                                                                    \
   X(caller_subject) X(caller_authorization) X(source) X(vault_principal) X(request_budget_limits)
#define SOURCE_CONTEXT_INTS(X)                                                                     \
   X(transport)                                                                                    \
   X(capabilities) X(vault_transport) X(trusted) X(compress_disabled) X(request_budget_present)
static cJSON *context_json(const request_context_t *context)
{
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;
#define ADD_STRING(field)                                                                          \
   if (!cJSON_AddStringToObject(out, #field, context->field))                                      \
      goto fail;
   SOURCE_CONTEXT_STRINGS(ADD_STRING)
#undef ADD_STRING
#define ADD_INT(field)                                                                             \
   if (!cJSON_AddNumberToObject(out, #field, context->field))                                      \
      goto fail;
   SOURCE_CONTEXT_INTS(ADD_INT)
#undef ADD_INT
   return out;
fail:
   free_secret_json(out);
   return NULL;
}
static int parse_context(const cJSON *json, request_context_t *context, int service)
{
   memset(context, 0, sizeof *context);
   if (!cJSON_IsObject(json))
      return -1;
#define GET_STRING(field)                                                                          \
   do                                                                                              \
   {                                                                                               \
      const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, #field));    \
      if (!value || strlen(value) >= sizeof context->field)                                        \
         return -1;                                                                                \
      memcpy(context->field, value, strlen(value) + 1);                                            \
   } while (0);
   SOURCE_CONTEXT_STRINGS(GET_STRING)
#undef GET_STRING
#define GET_INT(field)                                                                             \
   do                                                                                              \
   {                                                                                               \
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, #field);                         \
      if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble > INT_MAX ||      \
          value->valuedouble != value->valueint)                                                   \
         return -1;                                                                                \
      context->field = value->valueint;                                                            \
   } while (0);
   SOURCE_CONTEXT_INTS(GET_INT)
#undef GET_INT
   if (context->transport < REQ_TRANSPORT_UNKNOWN || context->transport > REQ_TRANSPORT_TCP ||
       context->vault_transport < ATTEST_NONE || context->vault_transport > ATTEST_MTLS_CLIENT ||
       (!service && context->vault_transport == ATTEST_NONE) ||
       (service && (context->vault_transport != ATTEST_NONE ||
                    strcmp(context->vault_principal, VAULT_SERVER_PRINCIPAL) ||
                    strcmp(context->principal, VAULT_SERVER_PRINCIPAL) || context->trusted)) ||
       !context->vault_principal[0] || (context->trusted != 0 && context->trusted != 1) ||
       context->request_budget_present > 1)
      return -1;
   context->model_execution = 1;
   context->peer_uid = -1;
   return 0;
}

static pthread_mutex_t source_claim_lock = PTHREAD_MUTEX_INITIALIZER;
static int source_store_box(const char *agent, const char *key, const cJSON *payload)
{
   char mac[65];
   char *body = cJSON_PrintUnformatted(payload);
   if (!body || source_mac(body, mac))
   {
      free_secret(body);
      return -1;
   }
   cJSON *sealed = cJSON_CreateObject();
   if (!sealed || !cJSON_AddStringToObject(sealed, "payload", body) ||
       !cJSON_AddStringToObject(sealed, "mac", mac))
   {
      free_secret(body);
      free_secret_json(sealed);
      return -1;
   }
   free_secret(body);
   char *wire = cJSON_PrintUnformatted(sealed);
   free_secret_json(sealed);
   int valid =
       wire && strlen(wire) < SOURCE_MAX && vault_service_set_server(agent, key, wire) == VAULT_OK;
   free_secret(wire);
   return valid ? 0 : -1;
}
static int source_store(const char *handle, const cJSON *payload)
{
   char key[64];
   return source_key(handle, key) ? -1 : source_store_box(SOURCE_AGENT, key, payload);
}
static cJSON *source_read_box(const char *agent, const char *key)
{
   char mac[65];
   char *wire = calloc(SOURCE_MAX, 1);
   cJSON *sealed = NULL, *payload = NULL;
   if (wire && vault_service_get_server_principal(agent, key, wire, SOURCE_MAX) == VAULT_OK)
   {
      sealed = cJSON_ParseWithOpts(wire, NULL, 1);
      const char *body = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sealed, "payload"));
      const char *signature = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sealed, "mac"));
      if (sealed && aimee_ir_json_unique(sealed) && body && signature && strlen(signature) == 64 &&
          !source_mac(body, mac) && CRYPTO_memcmp(mac, signature, 64) == 0)
         payload = cJSON_ParseWithOpts(body, NULL, 1);
   }
   free_secret_json(sealed);
   free_secret(wire);
   if (payload && !aimee_ir_json_unique(payload))
   {
      free_secret_json(payload);
      payload = NULL;
   }
   return payload;
}
static cJSON *source_read(const char *handle)
{
   char key[64];
   cJSON *payload = source_key(handle, key) ? NULL : source_read_box(SOURCE_AGENT, key);
   const char *saved = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "handle"));
   const char *schema = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "schema"));
   if (!payload || !aimee_ir_json_unique(payload) || !saved || strcmp(saved, handle) || !schema ||
       strcmp(schema, "aimee.execution-source.v1"))
   {
      free_secret_json(payload);
      return NULL;
   }
   return payload;
}
/* A root capability delegates derivation to the authenticated workflow owner.
 * Each admitted request has a separate MAC-bound claim, so parallel seats and
 * later stages retain their exact request/registration/role/workspace binding. */
static int source_claim(const char *handle, const char *purpose, const char *operation,
                        const char *role, const char *registration, const char *workdir,
                        const aimee_request_t *request, int tools, int max_turns, long timeout_ms,
                        const char *persona)
{
   cJSON *claim = cJSON_CreateObject(), *ir = aimee_ir_request_json(request);
   if (!claim || !ir)
   {
      cJSON_Delete(claim);
      cJSON_Delete(ir);
      return -1;
   }
   cJSON_AddStringToObject(claim, "operation", operation);
   cJSON_AddStringToObject(claim, "role", role);
   cJSON_AddStringToObject(claim, "persona", persona ? persona : "");
   cJSON_AddStringToObject(claim, "registration", registration);
   cJSON_AddStringToObject(claim, "workdir", workdir);
   cJSON_AddBoolToObject(claim, "tools", tools);
   cJSON_AddNumberToObject(claim, "max_turns", max_turns);
   cJSON_AddNumberToObject(claim, "timeout_ms", (double)timeout_ms);
   cJSON_AddItemToObject(claim, "ir", ir);
   char *wire = cJSON_PrintUnformatted(claim), digest[65];
   int valid = wire && !aimee_sha256_hex(wire, strlen(wire), digest);
   free_secret(wire);
   free_secret_json(claim);
   if (!valid)
      return -1;
   pthread_mutex_lock(&source_claim_lock);
   cJSON *payload = source_read(handle);
   const char *bound_purpose =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "purpose"));
   valid = payload && bound_purpose && !strcmp(purpose, bound_purpose);
   if (valid && !strcmp(purpose, "roundtable"))
   {
      const char *bound =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "operation"));
      valid = bound ? !strcmp(bound, operation)
                    : cJSON_AddStringToObject(payload, "operation", operation) != NULL;
   }
   cJSON *claims = cJSON_GetObjectItemCaseSensitive(payload, "claims");
   if (valid && !claims)
      claims = cJSON_AddObjectToObject(payload, "claims");
   if (!cJSON_IsObject(claims))
      valid = 0;
   if (valid && !cJSON_GetObjectItemCaseSensitive(claims, digest))
   {
      int count = 0;
      for (const cJSON *it = claims->child; it; it = it->next)
         count++;
      valid = count < 1024 && cJSON_AddStringToObject(claims, digest, operation);
   }
   if (valid)
      valid = !source_store(handle, payload);
   free_secret_json(payload);
   pthread_mutex_unlock(&source_claim_lock);
   return valid ? 0 : -1;
}
static int source_registered_trigger(const cJSON *registration);
/* The service branch is called only after the host has checked its sealed
 * operator registration. The held FD is a producer capability, not a pathname
 * fallback; the workspace owner rejects nonlocal/conflicting namespaces. */
static int source_workspace_begin(const char *path, int service, workspace_turn_scope_t **out)
{
   if (!service)
      return workspace_turn_scope_begin_background(path, out);
   int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return -1;
   int result = workspace_turn_scope_begin_owned_source(path, fd, out);
   close(fd);
   return result;
}
static int source_mint(const char *purpose, const char *workspace, char handle[65], char *error,
                       size_t capacity, int service);
int server_native_delegate_source_mint(const char *purpose, char handle[65], char *error,
                                       size_t capacity)
{
   return server_native_delegate_source_mint_for_workspace(purpose, run_cmd_get_cwd(), handle,
                                                           error, capacity);
}
int server_native_delegate_source_mint_for_workspace(const char *purpose, const char *workspace,
                                                     char handle[65], char *error, size_t capacity)
{
   return source_mint(purpose, workspace, handle, error, capacity, 0);
}
static int source_mint(const char *purpose, const char *workspace, char handle[65], char *error,
                       size_t capacity, int service)
{
   if (handle)
      handle[0] = 0;
   const request_context_t *context = request_context_get();
   if (!handle || !purpose || (strcmp(purpose, "workflow") && strcmp(purpose, "roundtable")) ||
       !context || (!service && context->vault_transport == ATTEST_NONE) ||
       (service && (context->vault_transport != ATTEST_NONE ||
                    strcmp(context->vault_principal, VAULT_SERVER_PRINCIPAL) ||
                    strcmp(context->principal, VAULT_SERVER_PRINCIPAL) || context->trusted)) ||
       !context->vault_principal[0] || context->context_refused ||
       context->request_budget_present < 0)
      return refuse(error, capacity, "native delegation requires a host-attested source");
   char *approved_workspace = workspace ? realpath(workspace, NULL) : NULL;
   workspace_turn_scope_t *workspace_scope = NULL;
   char identity[65] = "";
   int directory = -1;
   int custody = approved_workspace &&
                 source_workspace_begin(approved_workspace, service, &workspace_scope) >= 0 &&
                 !workspace_turn_source_identity(approved_workspace, &directory, identity);
   if (directory >= 0)
      close(directory);
   if (workspace_scope)
      (void)workspace_turn_scope_end(workspace_scope);
   if (!custody)
   {
      free(approved_workspace);
      return refuse(error, capacity, "native delegation requires an approved source workspace");
   }
   unsigned char random[32];
   if (RAND_bytes(random, sizeof random) != 1)
   {
      free(approved_workspace);
      return refuse(error, capacity, "cannot issue native execution source");
   }
   for (size_t i = 0; i < sizeof random; i++)
      snprintf(handle + i * 2, 3, "%02x", random[i]);
   OPENSSL_cleanse(random, sizeof random);
   char parent[128], digest[65];
   snprintf(parent, sizeof parent, "%s", agent_get_request_session());
   int created = 0;
   if (!parent[0])
   {
      if (aimee_sha256_hex(handle, 64, digest))
         goto fail;
      snprintf(parent, sizeof parent, "execution-source-%s", digest);
      if (db1_server_session_create(parent, "workflow", context->vault_principal))
         goto fail;
      created = 1;
   }
   db1_server_session_t owner;
   if (db1_server_session_get(parent, &owner) || strcmp(owner.principal, context->vault_principal))
      goto fail;
   cJSON *payload = cJSON_CreateObject(), *snapshot = context_json(context);
   if (!payload || !snapshot)
   {
      cJSON_Delete(payload);
      free_secret_json(snapshot);
      goto fail;
   }
   cJSON_AddStringToObject(payload, "schema", "aimee.execution-source.v1");
   cJSON_AddStringToObject(payload, "handle", handle);
   cJSON_AddStringToObject(payload, "purpose", purpose);
   cJSON_AddStringToObject(payload, "origin", service ? "host-service" : "attested");
   cJSON_AddStringToObject(payload, "parent", parent);
   cJSON_AddStringToObject(payload, "workspace", approved_workspace);
   cJSON_AddStringToObject(payload, "workspace_identity", identity);
   cJSON_AddItemToObject(payload, "context", snapshot);
   int stored = !source_store(handle, payload);
   free_secret_json(payload);
   if (stored)
   {
      free(approved_workspace);
      return 0;
   }
fail:
   free(approved_workspace);
   if (created)
      (void)db1_server_session_delete(parent);
   handle[0] = 0;
   return refuse(error, capacity, "cannot retain native execution source authority");
}

int server_native_delegate_source_bind(const char *handle, unsigned principal_ref,
                                       const char *operation, const char *role,
                                       const char *registration, const char *workdir,
                                       const aimee_request_t *request, int tools, int max_turns,
                                       long timeout_ms, server_native_delegate_source_scope_t **out,
                                       agent_config_t *config, agent_t *selected, char *error,
                                       size_t capacity)
{
   if (out)
      *out = NULL;
   if (!out || principal_ref != SOURCE_PRINCIPAL || !operation || !operation[0] ||
       strlen(operation) > 256 || !role || !role[0] || strlen(role) >= 64 || !registration ||
       !registration[0] || strlen(registration) >= 128 || !workdir || !workdir[0] ||
       strlen(workdir) >= 4096 || !config || !selected || aimee_ir_request_admit(request) ||
       (tools != 0 && tools != 1) || max_turns < 0 || max_turns > 10000 || timeout_ms <= 0 ||
       timeout_ms > INT_MAX)
      return refuse(error, capacity, "invalid source-bound native invocation");
   cJSON *payload = source_read(handle);
   request_context_t restored = {0};
   char parent[128] = "", source_workspace[4096] = "", source_identity[65] = "", purpose[16] = "";
   const char *saved_parent =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "parent"));
   const char *saved_workspace =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "workspace"));
   const char *saved_identity =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "workspace_identity"));
   const char *saved_purpose =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "purpose"));
   const char *origin = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "origin"));
   int service = origin && !strcmp(origin, "host-service");
   int verified =
       payload && origin && (service || !strcmp(origin, "attested")) && saved_parent &&
       saved_parent[0] && strlen(saved_parent) < sizeof parent && saved_workspace &&
       saved_workspace[0] && strlen(saved_workspace) < sizeof source_workspace && saved_identity &&
       strlen(saved_identity) == 64 && saved_purpose &&
       (!strcmp(saved_purpose, "workflow") || !strcmp(saved_purpose, "roundtable")) &&
       !parse_context(cJSON_GetObjectItemCaseSensitive(payload, "context"), &restored, service);
   if (verified && service &&
       !source_registered_trigger(
           cJSON_GetObjectItemCaseSensitive(payload, "trigger_registration")))
      verified = 0;
   if (verified)
   {
      snprintf(parent, sizeof parent, "%s", saved_parent);
      snprintf(source_workspace, sizeof source_workspace, "%s", saved_workspace);
      snprintf(source_identity, sizeof source_identity, "%s", saved_identity);
      snprintf(purpose, sizeof purpose, "%s", saved_purpose);
   }
   free_secret_json(payload);
   db1_server_session_t owner;
   if (!verified || db1_server_session_get(parent, &owner) ||
       strcmp(owner.principal, restored.vault_principal))
   {
      OPENSSL_cleanse(&restored, sizeof restored);
      return refuse(error, capacity, "native execution source is absent, changed or revoked");
   }
   char *target = realpath(workdir, NULL);
   int approved_target = 0;
   if (target && !strcmp(purpose, "workflow"))
   {
      db1_work_item_t item;
      if (db1_work_item_get(operation, &item) == 1 && !strcmp(item.execution_scope, handle) &&
          item.repo[0] && !strcmp(item.state, "active") && !item.pause_reason[0])
      {
         char *repo = realpath(item.repo, NULL);
         const char *expected = item.worktree[0] ? item.worktree : (!tools ? item.repo : "");
         char *owned = expected[0] ? realpath(expected, NULL) : NULL;
         approved_target =
             repo && !strcmp(repo, source_workspace) && owned && !strcmp(owned, target);
         free(repo);
         free(owned);
      }
   }
   else if (target && !strcmp(purpose, "roundtable"))
      approved_target = !strcmp(target, source_workspace);
   char approved_workdir[4096] = "";
   if (approved_target && target && strlen(target) < sizeof approved_workdir)
      snprintf(approved_workdir, sizeof approved_workdir, "%s", target);
   else
      approved_target = 0;
   free(target);
   if (!approved_target)
   {
      OPENSSL_cleanse(&restored, sizeof restored);
      return refuse(error, capacity,
                    "native task is outside its issued workflow or workspace scope");
   }
   if (agent_load_config(config))
   {
      OPENSSL_cleanse(&restored, sizeof restored);
      return refuse(error, capacity, "native execution registry unavailable");
   }
   agent_t *registered = agent_find(config, registration);
   if (!registered || !registered->enabled || registered->primary_only ||
       !agent_has_role(registered, role) ||
       (request->model && request->model[0] && strcmp(request->model, registered->model)))
   {
      OPENSSL_cleanse(&restored, sizeof restored);
      return refuse(error, capacity, "native registration or role is not admitted");
   }
   server_native_delegate_source_scope_t *scope = calloc(1, sizeof *scope);
   if (!scope)
   {
      OPENSSL_cleanse(&restored, sizeof restored);
      return refuse(error, capacity, "cannot retain native source scope");
   }
   scope->directory = scope->source_directory = -1;
   request_context_snapshot(&scope->previous_context);
   agent_request_creds_snapshot(&scope->previous_credentials);
   const char *previous = run_cmd_get_cwd();
   if (previous)
   {
      if (strlen(previous) >= sizeof scope->previous_cwd)
      {
         OPENSSL_cleanse(&restored, sizeof restored);
         OPENSSL_cleanse(scope, sizeof *scope);
         free(scope);
         return refuse(error, capacity, "native caller workspace exceeds the owned scope limit");
      }
      snprintf(scope->previous_cwd, sizeof scope->previous_cwd, "%s", previous);
      scope->had_cwd = 1;
   }
   request_context_set(&restored);
   OPENSSL_cleanse(&restored, sizeof restored);
   agent_request_creds_t credentials = {0};
   snprintf(credentials.session_id, sizeof credentials.session_id, "%s", parent);
   const request_context_t *active = request_context_get();
   snprintf(credentials.vault_principal, sizeof credentials.vault_principal, "%s",
            active->vault_principal);
   agent_request_creds_restore(&credentials);
   OPENSSL_cleanse(&credentials, sizeof credentials);
   run_cmd_set_cwd(approved_workdir);
   char workspace_identity[65];
   if (source_workspace_begin(source_workspace, service, &scope->source_workspace) < 0 ||
       workspace_turn_source_identity(source_workspace, &scope->source_directory,
                                      workspace_identity) ||
       strcmp(workspace_identity, source_identity) ||
       source_workspace_begin(approved_workdir, service, &scope->workspace) < 0 ||
       workspace_turn_resource_identity(approved_workdir, &scope->directory, workspace_identity) ||
       source_claim(handle, purpose, operation, role, registration, approved_workdir, request,
                    tools, max_turns, timeout_ms, NULL))
      goto restore;
   snprintf(scope->handle, sizeof scope->handle, "%s", handle);
   snprintf(scope->purpose, sizeof scope->purpose, "%s", purpose);
   snprintf(scope->operation, sizeof scope->operation, "%s", operation);
   snprintf(scope->role, sizeof scope->role, "%s", role);
   snprintf(scope->registration, sizeof scope->registration, "%s", registration);
   snprintf(scope->workdir, sizeof scope->workdir, "%s", approved_workdir);
   snprintf(scope->parent, sizeof scope->parent, "%s", parent);
   snprintf(scope->actor, sizeof scope->actor, "%s", request_context_get()->vault_principal);
   snprintf(scope->account, sizeof scope->account, "%s", request_context_get()->principal);
   scope->transport = request_context_get()->vault_transport;
   scope->tools = tools;
   scope->max_turns = max_turns;
   scope->timeout_ms = timeout_ms;
   *selected = *registered;
   if (max_turns > 0 && (selected->max_turns <= 0 || max_turns < selected->max_turns))
      selected->max_turns = max_turns;
   if (selected->timeout_ms <= 0 || timeout_ms < selected->timeout_ms)
      selected->timeout_ms = (int)timeout_ms;
   *out = scope;
   return 0;
restore:
   OPENSSL_cleanse(&restored, sizeof restored);
   server_native_delegate_source_unbind(scope);
   return refuse(error, capacity, "native source workspace custody unavailable");
}
int server_native_delegate_source_admit(server_native_delegate_source_scope_t *scope,
                                        const char *persona, const aimee_request_t *request,
                                        char *error, size_t capacity)
{
   const request_context_t *context = request_context_get();
   if (!scope || !persona || !persona[0] || strlen(persona) > 128 || !context ||
       context->context_refused || context->model_execution != 1 ||
       context->operator_script_origin || context->vault_transport != scope->transport ||
       strcmp(context->principal, scope->account) ||
       strcmp(agent_get_request_vault_principal(), scope->actor) ||
       strcmp(context->vault_principal, scope->actor) ||
       strcmp(agent_get_request_session(), scope->parent) || aimee_ir_request_admit(request) ||
       source_claim(scope->handle, scope->purpose, scope->operation, scope->role,
                    scope->registration, scope->workdir, request, scope->tools, scope->max_turns,
                    scope->timeout_ms, persona))
      return refuse(error, capacity, "effective native request lost its host-issued source");
   return 0;
}
void server_native_delegate_source_unbind(server_native_delegate_source_scope_t *scope)
{
   if (!scope)
      return;
   if (scope->directory >= 0)
      close(scope->directory);
   if (scope->workspace)
      (void)workspace_turn_scope_end(scope->workspace);
   if (scope->source_directory >= 0)
      close(scope->source_directory);
   if (scope->source_workspace)
      (void)workspace_turn_scope_end(scope->source_workspace);
   run_cmd_set_cwd(scope->had_cwd ? scope->previous_cwd : NULL);
   agent_request_creds_restore(&scope->previous_credentials);
   request_context_restore(&scope->previous_context);
   OPENSSL_cleanse(scope, sizeof *scope);
   free(scope);
}
int server_native_delegate_source_revoke(const char *handle)
{
   char key[64];
   return source_key(handle, key) ||
                  vault_service_delete(VAULT_SERVER_PRINCIPAL, SOURCE_AGENT, key) != VAULT_OK
              ? -1
              : 0;
}

/* The caller is an authenticated private host stage. Match the descriptor to
 * the current configuration owner; a caller-supplied origin or actor is never
 * evidence. Background work uses the honest server service identity. */
static const char *source_text(const cJSON *object, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}
static int source_same(const char *value, const char *expected)
{
   return value && expected && !strcmp(value, expected);
}
static int source_registered_trigger(const cJSON *registration)
{
   static const char *const names[] = {"origin", "source",   "event", "workspace",
                                       "ref",    "pipeline", "mode"};
   if (!cJSON_IsObject(registration) || !aimee_ir_json_unique(registration))
      return 0;
   int count = 0;
   for (const cJSON *item = registration->child; item; item = item->next)
      count++;
   if (count != 8)
      return 0;
   for (size_t i = 0; i < sizeof names / sizeof *names; i++)
   {
      const char *value = source_text(registration, names[i]);
      if (!value || strlen(value) >= 4096)
         return 0;
   }
   const cJSON *spend = cJSON_GetObjectItemCaseSensitive(registration, "max_spend_usd");
   if (!cJSON_IsNumber(spend) || !isfinite(spend->valuedouble) || spend->valuedouble < 0)
      return 0;
   const char *source = source_text(registration, "source");
   if (strcmp(source, "watch-dir") && strcmp(source, "proposals"))
      return 0;
   const char *origin = source_text(registration, "origin");
   if (strcmp(origin, "config") && strcmp(origin, "workflow"))
      return 0;
   const size_t limit = 16u * 1024u * 1024u;
   char *reply = calloc(limit, 1);
   if (!reply || agent_request_cancelled())
   {
      free(reply);
      return 0;
   }
   int status = workflow_control_request("GET", "/v1/workflow/triggers", "", NULL, 0,
                                         VAULT_SERVER_PRINCIPAL, 0, reply, (int)limit);
   cJSON *registered = status == 200 ? cJSON_ParseWithOpts(reply, NULL, 1) : NULL;
   free(reply);
   int valid = registered && aimee_ir_json_unique(registered) && !agent_request_cancelled();
   if (!strcmp(origin, "config") && cJSON_GetObjectItemCaseSensitive(registered, "registry_error"))
      valid = 0;
   const cJSON *rows = cJSON_GetObjectItemCaseSensitive(registered, "triggers");
   int match = 0;
   if (valid && cJSON_IsArray(rows))
   {
      for (const cJSON *row = rows->child; row; row = row->next)
      {
         const cJSON *maximum = cJSON_GetObjectItemCaseSensitive(row, "max_spend_usd");
         double expected = cJSON_IsNumber(maximum) ? maximum->valuedouble : 0;
         if (source_same(origin, source_text(row, "origin")) &&
             source_same(source, source_text(row, "source")) &&
             source_same(source_text(registration, "event"), source_text(row, "event")) &&
             source_same(source_text(registration, "workspace"), source_text(row, "workspace")) &&
             source_same(source_text(registration, "ref"), source_text(row, "schedule")) &&
             source_same(source_text(registration, "pipeline"), source_text(row, "template")) &&
             source_same(source_text(registration, "mode"), source_text(row, "mode")) &&
             spend->valuedouble == expected)
         {
            match = 1;
            break;
         }
      }
   }
   cJSON_Delete(registered);
   return match;
}

int server_native_delegate_source_trigger(const cJSON *registration,
                                          const char *configuration_sha256, char handle[65],
                                          char *error, size_t capacity)
{
   if (handle)
      handle[0] = 0;
   if (!handle || agent_request_cancelled() || !valid_handle(configuration_sha256) ||
       !source_registered_trigger(registration))
      return refuse(error, capacity, "workflow trigger has no matching host registration");
   /* Compute the host's fingerprint from admitted semantic fields. The Go hash
    * is diagnostic: differing JSON float/HTML serializers cannot grant authority. */
   const char *fields[] = {"origin", "source", "event", "workspace", "ref", "pipeline", "mode"};
   cJSON *canonical = cJSON_CreateObject();
   if (!canonical)
      return refuse(error, capacity, "cannot retain registered trigger source");
   for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
      cJSON_AddStringToObject(canonical, fields[i], source_text(registration, fields[i]));
   cJSON_AddNumberToObject(
       canonical, "max_spend_usd",
       cJSON_GetObjectItemCaseSensitive(registration, "max_spend_usd")->valuedouble);
   char *wire = cJSON_PrintUnformatted(canonical), fingerprint[65], key[64];
   free_secret_json(canonical);
   if (!wire || aimee_sha256_hex(wire, strlen(wire), fingerprint))
   {
      free_secret(wire);
      return refuse(error, capacity, "cannot bind registered trigger source");
   }
   free_secret(wire);
   snprintf(key, sizeof key, "trigger-%.48s", fingerprint);
   static const char trigger_agent[] = "host-native-trigger";
   pthread_mutex_lock(&source_claim_lock);
   cJSON *mapping = source_read_box(trigger_agent, key);
   const char *existing = source_text(mapping, "handle");
   cJSON *source = existing && valid_handle(existing) ? source_read(existing) : NULL;
   int reusable = source_same(source_text(mapping, "schema"), "aimee.trigger-source.v1") &&
                  source_same(source_text(mapping, "fingerprint"), fingerprint) &&
                  source_same(source_text(source, "trigger_fingerprint"), fingerprint) &&
                  source_same(source_text(source, "origin"), "host-service");
   if (reusable)
   {
      const char *parent = source_text(source, "parent");
      const char *root = source_text(source, "workspace");
      const char *expected_identity = source_text(source, "workspace_identity");
      char *current_root = realpath(source_text(registration, "workspace"), NULL);
      db1_server_session_t owner;
      workspace_turn_scope_t *custody = NULL;
      int directory = -1;
      char actual_identity[65];
      reusable = parent && parent[0] && !db1_server_session_get(parent, &owner) &&
                 !strcmp(owner.principal, VAULT_SERVER_PRINCIPAL) && root && current_root &&
                 !strcmp(root, current_root) && expected_identity &&
                 cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(source, "trigger_registration")) &&
                 source_workspace_begin(current_root, 1, &custody) >= 0 &&
                 !workspace_turn_source_identity(current_root, &directory, actual_identity) &&
                 !strcmp(expected_identity, actual_identity);
      if (directory >= 0)
         close(directory);
      if (custody)
         (void)workspace_turn_scope_end(custody);
      free(current_root);
   }
   if (reusable)
      snprintf(handle, 65, "%s", existing);
   free_secret_json(source);
   free_secret_json(mapping);
   int rc = 0;
   if (!reusable)
   {
      if (agent_request_cancelled())
      {
         pthread_mutex_unlock(&source_claim_lock);
         return refuse(error, capacity, "registered trigger source was cancelled");
      }
      request_context_snapshot_t previous;
      request_context_snapshot(&previous);
      agent_request_creds_t credentials;
      agent_request_creds_snapshot(&credentials);
      request_context_t service = {0};
      snprintf(service.principal, sizeof service.principal, "%s", VAULT_SERVER_PRINCIPAL);
      snprintf(service.vault_principal, sizeof service.vault_principal, "%s",
               VAULT_SERVER_PRINCIPAL);
      snprintf(service.source, sizeof service.source, "registered-workflow-trigger");
      service.model_execution = 1;
      service.peer_uid = -1;
      service.capabilities = CAPS_READ_ONLY | CAP_DELEGATE | CAP_TOOL_EXECUTE | CAP_TOOL_BASH |
                             CAP_TOOL_WRITE | CAP_MEMORY_WRITE;
      request_context_set(&service);
      agent_request_creds_t empty = {0};
      agent_request_creds_restore(&empty);
      rc = source_mint("workflow", source_text(registration, "workspace"), handle, error, capacity,
                       1);
      agent_request_creds_restore(&credentials);
      request_context_restore(&previous);
      OPENSSL_cleanse(&credentials, sizeof credentials);
      OPENSSL_cleanse(&previous, sizeof previous);
      OPENSSL_cleanse(&service, sizeof service);
      if (!rc)
      {
         source = source_read(handle);
         int stored = source &&
                      cJSON_AddStringToObject(source, "trigger_fingerprint", fingerprint) &&
                      cJSON_AddItemToObject(source, "trigger_registration",
                                            cJSON_Duplicate(registration, 1)) &&
                      !source_store(handle, source);
         free_secret_json(source);
         mapping = cJSON_CreateObject();
         if (mapping)
         {
            cJSON_AddStringToObject(mapping, "schema", "aimee.trigger-source.v1");
            cJSON_AddStringToObject(mapping, "fingerprint", fingerprint);
            cJSON_AddStringToObject(mapping, "handle", handle);
         }
         stored = stored && mapping && !source_store_box(trigger_agent, key, mapping);
         free_secret_json(mapping);
         if (!stored)
         {
            (void)server_native_delegate_source_revoke(handle);
            handle[0] = 0;
            rc = refuse(error, capacity, "cannot retain registered trigger source");
         }
      }
   }
   pthread_mutex_unlock(&source_claim_lock);
   return rc;
}
