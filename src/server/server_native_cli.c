/* Lifetime-scoped child CLI capability; no provider credentials, global server
 * endpoint or alternate conversation store. */
#include "server_native_cli.h"
#include "server_native_tool_admission.h"
#include "server_session_capture.h"
#include "request_context.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "util.h"
#include <aimee/tools/agent_tools.h>
#include "platform_random.h"
#include "aimee_sha256.h"
#include <aimee/core/connection/auth.h>
#include <aimee/protocols/mcp/mcp_tools.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLI_CALLS 16
struct cli_call
{
   char nonce[33], digest[65];
   cJSON *response;
};
struct server_native_cli
{
   pthread_mutex_t lock;
   pthread_cond_t joined;
   atomic_int revoked;
   unsigned refs, count, busy, depth;
   char token[65], session[128], actor[160], cwd[4096], parent[96];
   char **environment;
   struct cli_call calls[CLI_CALLS];
};
static int cli_nonce(const char *value)
{
   if (!value || strlen(value) != 32)
      return 0;
   for (const char *p = value; *p; p++)
      if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f'))
         return 0;
   return 1;
}
server_native_cli_t *server_native_cli_open(const char *endpoint, const char *session,
                                            const char *actor, const char *cwd,
                                            const char *parent_attempt, unsigned depth)
{
   if (!endpoint || strncmp(endpoint, "http://127.0.0.1:", 17) || !session || !*session ||
       strlen(session) >= 128 || !actor || !*actor || strlen(actor) >= 160 || !cwd ||
       cwd[0] != '/' || strlen(cwd) >= 4096 || !parent_attempt ||
       strncmp(parent_attempt, "native-ir:", 10) || strlen(parent_attempt) != 74 || depth > 2)
      return NULL;
   server_native_cli_t *grant = calloc(1, sizeof *grant);
   if (!grant)
      return NULL;
   if (pthread_mutex_init(&grant->lock, NULL))
   {
      free(grant);
      return NULL;
   }
   if (pthread_cond_init(&grant->joined, NULL))
   {
      pthread_mutex_destroy(&grant->lock);
      free(grant);
      return NULL;
   }
   atomic_init(&grant->revoked, 0);
   unsigned char bytes[32];
   if (platform_random_bytes(bytes, sizeof bytes))
      goto failed;
   for (unsigned i = 0; i < sizeof bytes; i++)
      snprintf(grant->token + 2 * i, 3, "%02x", bytes[i]);
   OPENSSL_cleanse(bytes, sizeof bytes);
   snprintf(grant->session, sizeof grant->session, "%s", session);
   snprintf(grant->actor, sizeof grant->actor, "%s", actor);
   snprintf(grant->cwd, sizeof grant->cwd, "%s", cwd);
   snprintf(grant->parent, sizeof grant->parent, "%s", parent_attempt);
   grant->depth = depth;
   /* Prepared assignments only; inherited environment is overlaid after fork,
    * never read/copied while other host request threads can change it. */
   grant->environment = calloc(7, sizeof(char *));
   if (!grant->environment)
      goto failed;
   const char *keys[] = {"AIMEE_API_ENDPOINT", "AIMEE_API_BEARER",   "AIMEE_NATIVE_CLI",
                         "AIMEE_SESSION_ID",   "AIMEE_SERVER_TOKEN", "AIMEE_SERVER_URL"};
   const char *values[] = {endpoint, grant->token, "1", session, "", ""};
   for (unsigned i = 0; i < 6; i++)
   {
      size_t size = strlen(keys[i]) + strlen(values[i]) + 2;
      grant->environment[i] = malloc(size);
      if (!grant->environment[i])
         goto failed;
      snprintf(grant->environment[i], size, "%s=%s", keys[i], values[i]);
   }
   return grant;
failed:
   server_native_cli_close(grant);
   return NULL;
}
const char *server_native_cli_token(const server_native_cli_t *grant)
{
   return grant->token;
}
char *const *server_native_cli_environment(const server_native_cli_t *grant)
{
   return grant->environment;
}
int server_native_cli_matches(const server_native_cli_t *grant, const char *token)
{
   return grant && !atomic_load(&grant->revoked) && token &&
          aimee_core_credential_equal(grant->token, token);
}
int server_native_cli_retain(server_native_cli_t *grant)
{
   pthread_mutex_lock(&grant->lock);
   int rc = atomic_load(&grant->revoked) ? -1 : 0;
   if (!rc)
      grant->refs++;
   pthread_mutex_unlock(&grant->lock);
   return rc;
}
void server_native_cli_release(server_native_cli_t *grant)
{
   pthread_mutex_lock(&grant->lock);
   if (grant->refs)
      grant->refs--;
   pthread_cond_broadcast(&grant->joined);
   pthread_mutex_unlock(&grant->lock);
}
int server_native_cli_cancelled(const server_native_cli_t *grant)
{
   return atomic_load(&grant->revoked);
}
const atomic_int *server_native_cli_cancel_flag(const server_native_cli_t *grant)
{
   return &grant->revoked;
}
unsigned server_native_cli_depth(const server_native_cli_t *grant)
{
   return grant->depth;
}
void server_native_cli_close(server_native_cli_t *grant)
{
   if (!grant)
      return;
   pthread_mutex_lock(&grant->lock);
   atomic_store(&grant->revoked, 1);
   while (grant->refs)
      pthread_cond_wait(&grant->joined, &grant->lock);
   pthread_mutex_unlock(&grant->lock);
   if (grant->environment)
      for (char **p = grant->environment; *p; p++)
      {
         OPENSSL_cleanse(*p, strlen(*p));
         free(*p);
      }
   free(grant->environment);
   for (unsigned i = 0; i < grant->count; i++)
      cJSON_Delete(grant->calls[i].response);
   pthread_cond_destroy(&grant->joined);
   pthread_mutex_destroy(&grant->lock);
   OPENSSL_cleanse(grant, sizeof *grant);
   free(grant);
}
int server_native_cli_owner(const server_native_cli_t *grant, const char *session,
                            const char *actor, const char *cwd)
{
   return grant && !atomic_load(&grant->revoked) && session && actor && cwd &&
          !strcmp(grant->session, session) && !strcmp(grant->actor, actor) &&
          !strcmp(grant->cwd, cwd);
}
static int cli_keys_unique(const cJSON *value, unsigned depth, unsigned *remaining)
{
   if (depth > 32 || !*remaining)
      return 0;
   (*remaining)--;
   int children = cJSON_GetArraySize(value);
   if (children < 0 || (unsigned)children > *remaining)
      return 0;
   const cJSON *a, *b;
   cJSON_ArrayForEach(a, value)
   {
      if (cJSON_IsObject(value))
         for (b = a->next; b; b = b->next)
            if (!a->string || !b->string || !strcmp(a->string, b->string))
               return 0;
      if ((cJSON_IsObject(a) || cJSON_IsArray(a)) && !cli_keys_unique(a, depth + 1, remaining))
         return 0;
   }
   return 1;
}
int server_native_cli_project(const cJSON *request, char tool[128], cJSON **arguments,
                              char request_id[33])
{
   if (arguments)
      *arguments = NULL;
   unsigned remaining = 4096;
   if (!cJSON_IsObject(request) || !arguments || !cli_keys_unique(request, 0, &remaining))
      return -1;
   *arguments = NULL;
   /* Repeated JSON keys cannot select a different method/authority downstream. */
   const cJSON *a, *b;
   cJSON_ArrayForEach(a, request)
   {
      for (b = a->next; b; b = b->next)
         if (!strcmp(a->string, b->string))
            return -1;
      if (!strcmp(a->string, "_principal") || !strcmp(a->string, "actor") ||
          !strcmp(a->string, "native_owner") || !strcmp(a->string, "_meta"))
         return -1;
   }
   const char *nonce =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "_cli_request_id"));
   const char *method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "method"));
   const cJSON *version = cJSON_GetObjectItemCaseSensitive(request, "protocol_version");
   if (version && (!cJSON_IsNumber(version) || version->valuedouble != 1))
      return -1;
   const cJSON *sid = cJSON_GetObjectItemCaseSensitive(request, "session_id");
   const cJSON *cwd = cJSON_GetObjectItemCaseSensitive(request, "cwd");
   if (!cli_nonce(nonce) || !method || (sid && !cJSON_IsString(sid)) ||
       (cwd && !cJSON_IsString(cwd)))
      return -1;
   snprintf(request_id, 33, "%s", nonce);
   if (!strcmp(method, "mcp.call"))
   {
      cJSON_ArrayForEach(
          a, request) if (strcmp(a->string, "method") && strcmp(a->string, "tool") &&
                          strcmp(a->string, "arguments") && strcmp(a->string, "_cli_request_id") &&
                          strcmp(a->string, "session_id") && strcmp(a->string, "cwd") &&
                          strcmp(a->string, "protocol_version")) return -1;
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "tool"));
      const cJSON *args = cJSON_GetObjectItemCaseSensitive(request, "arguments");
      if (!name || !*name || strlen(name) >= 128 || !cJSON_IsObject(args))
         return -1;
      snprintf(tool, 128, "%s", name);
      *arguments = cJSON_Duplicate(args, 1);
   }
   else
   {
      const char *dot = strchr(method, '.');
      if (!dot || dot == method || !dot[1] || (size_t)(dot - method) >= 64 || strchr(dot + 1, '.'))
         return -1;
      char family[64];
      memcpy(family, method, (size_t)(dot - method));
      family[dot - method] = 0;
      const char *discriminator = mcp_family_discriminator(family);
      if (!discriminator)
         return -1;
      cJSON *args = cJSON_Duplicate(request, 1);
      if (!args)
         return -1;
      cJSON_DeleteItemFromObjectCaseSensitive(args, "method");
      cJSON_DeleteItemFromObjectCaseSensitive(args, "protocol_version");
      cJSON_DeleteItemFromObjectCaseSensitive(args, "_cli_request_id");
      cJSON_DeleteItemFromObjectCaseSensitive(args, "session_id");
      cJSON_DeleteItemFromObjectCaseSensitive(args, "cwd");
      if (cJSON_GetObjectItemCaseSensitive(args, discriminator) ||
          !cJSON_AddStringToObject(args, discriminator, dot + 1) ||
          mcp_family_demux(family, args, tool, 128) != 1)
      {
         cJSON_Delete(args);
         return -1;
      }
      cJSON_DeleteItemFromObjectCaseSensitive(args, discriminator);
      *arguments = args;
   }
   return *arguments ? 0 : -1;
}
int server_native_cli_begin(server_native_cli_t *grant, const cJSON *request, const char *nonce,
                            char attempt[128], cJSON **replay)
{
   *replay = NULL;
   char *wire = cJSON_PrintUnformatted(request);
   char digest[65];
   int invalid =
       !wire || !cli_nonce(nonce) || aimee_sha256_hex(wire, wire ? strlen(wire) : 0, digest);
   free(wire);
   if (invalid)
      return -1;
   pthread_mutex_lock(&grant->lock);
   int rc = -1;
   if (atomic_load(&grant->revoked) || grant->busy)
      goto done;
   for (unsigned i = 0; i < grant->count; i++)
      if (!strcmp(grant->calls[i].nonce, nonce))
      {
         if (!strcmp(grant->calls[i].digest, digest) && grant->calls[i].response)
         {
            *replay = cJSON_Duplicate(grant->calls[i].response, 1);
            rc = *replay ? 1 : -1;
         }
         goto done;
      }
   if (grant->count == CLI_CALLS)
      goto done;
   struct cli_call *call = &grant->calls[grant->count++];
   snprintf(call->nonce, sizeof call->nonce, "%s", nonce);
   snprintf(call->digest, sizeof call->digest, "%s", digest);
   grant->busy = 1;
   /* Direct parent linkage to the actual native-ir source attempt, not a new
    * assistant/tool claim. Child correlation never replaces original call ID. */
   snprintf(attempt, 128, "native-cli:%s:%s", grant->parent + 10, nonce);
   rc = 0;
done:
   pthread_mutex_unlock(&grant->lock);
   return rc;
}
int server_native_cli_finish(server_native_cli_t *grant, const cJSON *response)
{
   /* Cache only complete bounded receipts. Unknown/oversized completion is
    * consumed, never repeated and never represented as a successful replay. */
   char *wire = response ? cJSON_PrintUnformatted(response) : NULL;
   int valid = wire && strlen(wire) <= 1024u * 1024u;
   free(wire);
   pthread_mutex_lock(&grant->lock);
   cJSON *saved = valid && grant->busy ? cJSON_Duplicate(response, 1) : NULL;
   if (grant->busy)
      grant->calls[grant->count - 1].response = saved;
   grant->busy = 0;
   pthread_mutex_unlock(&grant->lock);
   return saved ? 0 : -1;
}

/* Shared source-derived CLI operation; callers select the exact advertised
 * definition from the retained shell catalog, never from the CLI manifest. */
int server_native_cli_execute(server_native_cli_t *grant, const cJSON *request,
                              const aimee_tool_t *advertised, int timeout_ms, cJSON **out)
{
   const request_context_t *owner = request_context_get();
   server_session_capture_t *capture = server_session_capture_current();
   if (!out)
      return -1;
   *out = NULL;
   if (!owner || !capture || !owner->model_execution || owner->operator_script_origin ||
       timeout_ms <= 0 ||
       !server_native_cli_owner(grant, session_id(), owner->vault_principal, run_cmd_get_cwd()))
      return -1;
   char tool[128], nonce[33], attempt[128], detail[512] = "";
   cJSON *arguments = NULL, *response = NULL;
   aimee_tool_t definition = {0};
   char *wire = NULL, *text = NULL;
   int reserved = 0, is_error = 1, rc = -1;
   if (server_native_cli_project(request, tool, &arguments, nonce))
      goto done;
   const char *submitted_session =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "session_id"));
   const char *submitted_cwd =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "cwd"));
   if ((submitted_session && strcmp(submitted_session, session_id())) ||
       (submitted_cwd && strcmp(submitted_cwd, run_cmd_get_cwd())))
      goto done;
   if (!advertised || agent_tools_execution_definition(tool, &definition) ||
       !(wire = cJSON_PrintUnformatted(arguments)) ||
       tool_validate_ir(advertised, wire, detail, sizeof detail) ||
       tool_validate_ir(&definition, wire, detail, sizeof detail))
      goto done;
   int begun = server_native_cli_begin(grant, request, nonce, attempt, &response);
   if (begun < 0)
      goto done;
   if (begun == 1)
   {
      *out = response;
      response = NULL;
      rc = 0;
      goto done;
   }
   reserved = 1;
   const char *refusal = server_native_tool_admit_attempt(tool, arguments, session_id(), attempt,
                                                          detail, sizeof detail);
   if (agent_request_cancelled() || server_native_cli_cancelled(grant) || owner->context_refused ||
       !server_session_capture_accepting(capture))
      goto done;
   if (refusal)
      text = strdup(detail[0] ? detail : "tool policy refused execution");
   else
   {
      /* Independent callback TLS/action state: the producer remains inside the
       * real outer Bash call. No producer queue, borrowed sink or fake model ID. */
      agent_tools_set_effect_authorized(1);
      text = dispatch_tool_call_ir_ctx_status(&definition, wire, timeout_ms, &is_error);
      agent_tools_set_effect_authorized(0);
      text = policy_annotate_indexed(tool, wire, attempt, text);
   }
   if (!text)
      goto done;
   /* This is the advertised tool contract, not an operator REST handler
    * with a different batch envelope. Return complete owned text unchanged. */
   response = cJSON_CreateObject();
   cJSON *content = response ? cJSON_AddArrayToObject(response, "content") : NULL;
   cJSON *item = content ? cJSON_CreateObject() : NULL;
   if (!item || !cJSON_AddStringToObject(item, "type", "text") ||
       !cJSON_AddStringToObject(item, "text", text) ||
       !cJSON_AddBoolToObject(response, "isError", is_error))
   {
      cJSON_Delete(item);
      goto done;
   }
   if (!cJSON_AddItemToArray(content, item))
   {
      cJSON_Delete(item);
      goto done;
   }
   if (!response)
      goto done;
   int complete = server_native_cli_finish(grant, response);
   reserved = 0;
   if (complete)
   {
      server_session_capture_fail(capture);
      goto done;
   }
   *out = response;
   response = NULL;
   rc = 0;
done:
   if (reserved)
   {
      server_native_cli_finish(grant, NULL);
      server_session_capture_fail(capture);
   }
   agent_tools_set_effect_authorized(0);
   cJSON_Delete(arguments);
   cJSON_Delete(response);
   free(wire);
   free(text);
   aimee_tool_free(&definition);
   return rc;
}
