/* External ACP execution: one admitted host task, not a provider SDK profile. */
#include "server_acp_execution.h"
#include "server_acp_resources.h"
#include "server_native_delegate_source.h"
#include "server_native_cli.h"
#include "server_http_internal.h"
#include <aimee/core/connection/auth.h>
#include "cli_acp.h"
#include "request_context.h"
#include "server_session_capture.h"
#include "server_session_control.h"
#include "modules/workspace/workspace_turn.h"
#include <aimee/tools/agent_tools.h>
#include "agent_exec.h"
#include "agent_config.h"
#include "server.h"
#include "util.h"
#include "wire_fence.h"
#include "cli_proxy.h"
#include "platform_random.h"
#include "cJSON.h"
#include <openssl/crypto.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ACP_COMMAND_ARGS 64
typedef struct acp_owner acp_owner_t;
typedef struct acp_shell acp_shell_t;
typedef struct
{
   char *body;
   char key[96];
   int listing, complete, processing, status;
   atomic_int cancelled;
   cJSON *response;
   acp_shell_t *shell;
} acp_mcp_request_t;
struct acp_shell
{
   acp_owner_t *owner;
   server_native_cli_t *grant;
   const aimee_request_t *catalog;
   acp_mcp_request_t *pending;
   char **environment;
   char digest[65];
};
struct acp_owner
{
   server_session_capture_t *capture;
   server_control_scope_t *controls;
   const request_context_t *context;
   char actor[VAULT_PRINCIPAL_MAX], session[128], cwd[4096], request_id[64];
   uint32_t required_caps;
   attested_transport_t transport;
   int executable_fd, cwd_fd;
   struct stat executable, directory;
   server_acp_resources_t *resources;
   cli_proxy_t *proxy;
   int mcp_fd, mcp_initialized, closing;
   struct stat mcp_executable;
   pthread_mutex_t mcp_lock;
   pthread_cond_t mcp_changed;
   acp_mcp_request_t *pending;
   atomic_int *parent_cancel;
   unsigned requests, mcp_calls;
   long long deadline;
   char token[65];
   cJSON *mcp_servers;
   char endpoint[64];
   const char *path, *home;
   acp_shell_t *shells[3], *current_shell;
};

static int same_file(const struct stat *a, const struct stat *b)
{
   return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}
static int same_executable(int fd, const struct stat *original)
{
   struct stat current;
   return !fstat(fd, &current) && same_file(&current, original) &&
          current.st_size == original->st_size &&
          current.st_mtim.tv_sec == original->st_mtim.tv_sec &&
          current.st_mtim.tv_nsec == original->st_mtim.tv_nsec;
}
static int acp_owner_valid(void *opaque)
{
   acp_owner_t *owner = opaque;
   const request_context_t *ctx = request_context_get();
   struct stat directory, named;
   const char *cwd = run_cmd_get_cwd();
   return !owner || !ctx || ctx != owner->context || ctx->context_refused ||
                  strcmp(ctx->vault_principal, owner->actor) ||
                  (ctx->capabilities & owner->required_caps) != owner->required_caps ||
                  ctx->vault_transport != owner->transport ||
                  strcmp(ctx->request_id, owner->request_id) || !session_id() ||
                  strcmp(session_id(), owner->session) ||
                  server_session_capture_current() != owner->capture ||
                  server_control_scope_current() != owner->controls ||
                  server_control_scope_failed(owner->controls) ||
                  !server_session_capture_accepting(owner->capture) || agent_request_cancelled() ||
                  util_now_ms() >= owner->deadline || !cwd || strcmp(cwd, owner->cwd) ||
                  !same_executable(owner->executable_fd, &owner->executable) ||
                  (owner->mcp_fd >= 0 && !same_executable(owner->mcp_fd, &owner->mcp_executable)) ||
                  fstat(owner->cwd_fd, &directory) || stat(owner->cwd, &named) ||
                  !same_file(&directory, &owner->directory) || !same_file(&directory, &named)
              ? -1
              : 0;
}
static int acp_mcp_write(const cli_proxy_io_t *io, int status, const cJSON *response)
{
   char *body = response ? cJSON_PrintUnformatted(response) : NULL;
   if (response && !body)
      return -1;
   size_t length = body ? strlen(body) : 0;
   char header[256];
   int count = snprintf(header, sizeof header,
                        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                        status, status == 200 ? "OK" : "Request Refused", length);
   int rc = count < 0 || (size_t)count >= sizeof header
                ? -1
                : io->write_response(header, (unsigned long)count, io->context);
   if (!rc && length)
      rc = io->write_response(body, (unsigned long)length, io->context);
   free(body);
   return rc;
}
static int acp_mcp_route(const char *method, const char *path, void *context)
{
   (void)context;
   return method && path &&
          ((!strcmp(method, "GET") && !strcmp(path, "/v1/mcp/tools_list")) ||
           (!strcmp(method, "POST") && !strcmp(path, "/v1/mcp/call")));
}
static int acp_cli_route(const char *method, const char *path)
{
   return method && path &&
          ((!strcmp(method, "GET") && !strcmp(path, "/v1/cli/manifest")) ||
           (!strcmp(method, "POST") && !strcmp(path, "/v1/native/cli")));
}
static acp_shell_t *acp_shell_find(acp_owner_t *owner, const char *token, int retain)
{
   for (unsigned i = 0; i < 3; i++)
      if (owner->shells[i] && server_native_cli_matches(owner->shells[i]->grant, token) &&
          (!retain || !server_native_cli_retain(owner->shells[i]->grant)))
         return owner->shells[i];
   return NULL; /* Caller holds the owner list lock through lookup/retain. */
}
static int acp_route(const char *method, const char *path, void *context)
{
   return acp_mcp_route(method, path, context) || acp_cli_route(method, path);
}
static int acp_authorize(const char *method, const char *path, const char *token, void *context)
{
   acp_owner_t *owner = context;
   pthread_mutex_lock(&owner->mcp_lock);
   int allowed =
       !owner->closing &&
       ((acp_mcp_route(method, path, owner) && aimee_core_credential_equal(token, owner->token)) ||
        (acp_cli_route(method, path) && acp_shell_find(owner, token, 0)));
   pthread_mutex_unlock(&owner->mcp_lock);
   return allowed;
}
static int acp_mcp_forward(const char *method, const char *path, const char *headers,
                           const void *body, unsigned long length, const char *session,
                           const cli_proxy_io_t *io, void *context)
{
   (void)headers;
   acp_owner_t *owner = context;
   if (!acp_route(method, path, owner) || !session || strcmp(session, owner->session) ||
       length > 4u * 1024u * 1024u)
      return acp_mcp_write(io, 403, NULL);
   acp_mcp_request_t work = {.listing = !strcmp(method, "GET"), .status = 503};
   atomic_init(&work.cancelled, 0);
   work.body = length && body ? strndup(body, length) : NULL;
   if (length && (!work.body || strlen(work.body) != length))
   {
      free(work.body);
      return acp_mcp_write(io, 400, NULL);
   }
   pthread_mutex_lock(&owner->mcp_lock);
   int cli = acp_cli_route(method, path);
   work.shell = cli ? acp_shell_find(owner, io->local_credential, 1) : NULL;
   acp_mcp_request_t **slot = work.shell ? &work.shell->pending : &owner->pending;
   if (owner->closing || (cli && !work.shell) || *slot || owner->requests == AIMEE_IR_MAX_BLOCKS)
   {
      pthread_mutex_unlock(&owner->mcp_lock);
      if (work.shell)
         server_native_cli_release(work.shell->grant);
      free(work.body);
      return acp_mcp_write(io, 429, NULL);
   }
   int key_length = snprintf(work.key, sizeof work.key, "%s:%u",
                             server_session_capture_turn(owner->capture), ++owner->requests);
   if (key_length < 0 || (size_t)key_length >= sizeof work.key)
   {
      pthread_mutex_unlock(&owner->mcp_lock);
      if (work.shell)
         server_native_cli_release(work.shell->grant);
      free(work.body);
      return acp_mcp_write(io, 403, NULL);
   }
   *slot = &work;
   while (!work.complete)
   {
      if (owner->closing || util_now_ms() >= owner->deadline ||
          (owner->parent_cancel && atomic_load(owner->parent_cancel)) ||
          (io->cancelled && io->cancelled(io->context)))
      {
         atomic_store(&work.cancelled, 1);
         if (!work.processing)
         {
            *slot = NULL;
            work.complete = 1;
            work.status = 408;
            break;
         }
      }
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 100000000;
      if (until.tv_nsec >= 1000000000)
      {
         until.tv_sec++;
         until.tv_nsec -= 1000000000;
      }
      pthread_cond_timedwait(&owner->mcp_changed, &owner->mcp_lock, &until);
   }
   if (*slot == &work)
      *slot = NULL;
   pthread_mutex_unlock(&owner->mcp_lock);
   int rc = acp_mcp_write(io, work.status, work.response);
   if (work.shell)
      server_native_cli_release(work.shell->grant);
   cJSON_Delete(work.response);
   free(work.body);
   return rc;
}
static cJSON *acp_mcp_list(const aimee_request_t *catalog)
{
   cJSON *response = cJSON_CreateObject();
   cJSON *tools = response ? cJSON_AddArrayToObject(response, "tools") : NULL;
   if (!catalog || !tools || !cJSON_AddStringToObject(response, "status", "ok"))
      goto failed;
   for (int i = 0; i < catalog->n_tools; i++)
   {
      const aimee_tool_t *definition = &catalog->tools[i];
      cJSON *tool = cJSON_CreateObject();
      cJSON *schema = cJSON_Duplicate(definition->schema, 1);
      if (definition->kind != AIMEE_TOOL_FUNCTION || !tool || !schema ||
          !cJSON_AddStringToObject(tool, "name", definition->name) ||
          !cJSON_AddStringToObject(tool, "description",
                                   definition->description ? definition->description : "") ||
          !cJSON_AddItemToObject(tool, "inputSchema", schema))
      {
         cJSON_Delete(schema);
         cJSON_Delete(tool);
         goto failed;
      }
      if (!cJSON_AddItemToArray(tools, tool))
      {
         cJSON_Delete(tool);
         goto failed;
      }
   }
   return response;
failed:
   cJSON_Delete(response);
   return NULL;
}
static cJSON *acp_mcp_result(const aimee_block_t *result)
{
   cJSON *response = cJSON_CreateObject();
   cJSON *content = response ? cJSON_AddArrayToObject(response, "content") : NULL;
   cJSON *text = content ? cJSON_CreateObject() : NULL;
   char *wire = cJSON_IsString(result->tool_result) ? strdup(result->tool_result->valuestring)
                                                    : cJSON_PrintUnformatted(result->tool_result);
   if (!text || !wire || !cJSON_AddStringToObject(response, "status", "ok") ||
       !cJSON_AddBoolToObject(response, "isError", result->tool_is_error) ||
       !cJSON_AddStringToObject(text, "type", "text") ||
       !cJSON_AddStringToObject(text, "text", wire) || !cJSON_AddItemToArray(content, text))
   {
      cJSON_Delete(text);
      cJSON_Delete(response);
      response = NULL;
   }
   free(wire);
   return response;
}
static int acp_bash_open(const char *digest, const aimee_request_t *catalog,
                         agent_tools_bash_scope_t *scope, void **lease, void *context);
static void acp_bash_close(void *lease, void *context);
static int acp_cli_progress(void *context)
{
   acp_owner_t *owner = context;
   if (acp_owner_valid(owner))
      return -1;
   pthread_mutex_lock(&owner->mcp_lock);
   acp_mcp_request_t *work = NULL;
   for (unsigned i = 0; i < 3; i++)
      if (owner->shells[i] && owner->shells[i]->pending && !owner->shells[i]->pending->processing &&
          !owner->shells[i]->pending->complete)
      {
         work = owner->shells[i]->pending;
         break;
      }
   if (!work)
   {
      pthread_mutex_unlock(&owner->mcp_lock);
      return 0;
   }
   work->processing = 1;
   pthread_mutex_unlock(&owner->mcp_lock);
   acp_shell_t *previous = owner->current_shell;
   owner->current_shell = work->shell;
   int failed =
       atomic_load(&work->cancelled) ||
       !server_native_cli_owner(work->shell->grant, owner->session, owner->actor, owner->cwd);
   if (!failed && work->listing)
   {
      char *wire = calloc(1, 1024u * 1024u);
      if (wire && rh_cli_manifest(NULL, wire, 1024u * 1024u) == 200)
         work->response = aimee_ir_json_parse_frame(wire, strlen(wire));
      free(wire);
   }
   else if (!failed)
   {
      cJSON *request =
          work->body ? aimee_ir_json_parse_frame(work->body, strlen(work->body)) : NULL;
      char tool[128], nonce[33];
      cJSON *arguments = NULL;
      const aimee_tool_t *definition = NULL;
      if (!server_native_cli_project(request, tool, &arguments, nonce))
         definition = aimee_ir_find_function(work->shell->catalog, NULL, tool);
      cJSON_Delete(arguments);
      long long remaining = owner->deadline - util_now_ms();
      agent_request_cancel_scope_t cancel = {.flag = &work->cancelled,
                                             .parent = agent_request_cancel_scope_current()};
      const agent_request_cancel_scope_t *prior = agent_request_cancel_scope_bind(&cancel);
      policy_action_scope_t *action_scope = NULL;
      agent_tools_effect_scope_t *effect_scope = NULL;
      failed = policy_action_scope_push(&action_scope);
      if (!failed)
         failed = agent_tools_effect_scope_push(&effect_scope);
      void *child = NULL;
      agent_tools_bash_scope_t child_scope = {0}, previous_scope = {0};
      int is_bash = definition && !strcmp(tool, "bash");
      if (!failed)
         failed = is_bash && acp_bash_open(work->shell->digest, work->shell->catalog, &child_scope,
                                           &child, owner);
      if (is_bash && !failed)
         previous_scope = agent_tools_bind_bash_scope(&child_scope);
      if (!failed)
         failed = remaining <= 0 ||
                  server_native_cli_execute(work->shell->grant, request, definition,
                                            remaining < 120000 ? (int)remaining : 120000,
                                            &work->response);
      if (is_bash && child)
      {
         agent_tools_bind_bash_scope(&previous_scope);
         acp_bash_close(child, owner);
      }
      int restore_failed = 0;
      if (effect_scope)
         restore_failed |= agent_tools_effect_scope_pop(effect_scope);
      if (action_scope)
         restore_failed |= policy_action_scope_pop(action_scope);
      if (restore_failed)
      {
         server_session_capture_fail(owner->capture);
         failed = 1;
      }
      agent_request_cancel_scope_bind(prior);
      cJSON_Delete(request);
   }
   owner->current_shell = previous;
   failed |= !work->response;
   pthread_mutex_lock(&owner->mcp_lock);
   work->status = failed ? 403 : 200;
   work->complete = 1;
   work->shell->pending = NULL;
   pthread_cond_broadcast(&owner->mcp_changed);
   pthread_mutex_unlock(&owner->mcp_lock);
   return acp_owner_valid(owner); /* Policy errors fail this CLI request, not unrelated shell IO. */
}
static void acp_bash_close(void *lease, void *context)
{
   acp_shell_t *shell = lease;
   acp_owner_t *owner = context;
   if (!shell)
      return;
   pthread_mutex_lock(&owner->mcp_lock);
   for (unsigned i = 0; i < 3; i++)
      if (owner->shells[i] == shell)
         owner->shells[i] = NULL;
   if (shell->pending && !shell->pending->processing)
   {
      atomic_store(&shell->pending->cancelled, 1);
      shell->pending->status = 403;
      shell->pending->complete = 1;
      shell->pending = NULL;
   }
   pthread_cond_broadcast(&owner->mcp_changed);
   pthread_mutex_unlock(&owner->mcp_lock);
   server_native_cli_close(shell->grant);
   if (shell->environment)
   {
      for (unsigned i = 0; shell->environment[i]; i++)
      {
         OPENSSL_cleanse(shell->environment[i], strlen(shell->environment[i]));
         free(shell->environment[i]);
      }
   }
   free(shell->environment);
   free(shell);
}
static int acp_bash_open(const char *digest, const aimee_request_t *catalog,
                         agent_tools_bash_scope_t *scope, void **lease, void *context)
{
   acp_owner_t *owner = context;
   unsigned depth =
       owner->current_shell ? server_native_cli_depth(owner->current_shell->grant) + 1 : 0;
   if (!scope || !lease || !digest || strlen(digest) != 64 || !catalog || !catalog->n_tools ||
       depth > 2 || acp_owner_valid(owner))
      return -1;
   *lease = NULL;
   char attempt[75];
   snprintf(attempt, sizeof attempt, "native-ir:%s", digest);
   acp_shell_t *shell = calloc(1, sizeof *shell);
   if (!shell)
      return -1;
   shell->owner = owner;
   shell->catalog = catalog;
   memcpy(shell->digest, digest, sizeof shell->digest);
   shell->grant = server_native_cli_open(owner->endpoint, owner->session, owner->actor, owner->cwd,
                                         attempt, depth);
   if (!shell->grant)
   {
      free(shell);
      return -1;
   }
   shell->environment = calloc(10, sizeof(char *));
   if (!shell->environment)
      goto failed;
   const char *names[] = {"PATH", "LANG", "HOME"};
   const char *values[] = {owner->path, "C.UTF-8", owner->home};
   for (unsigned i = 0; i < 3; i++)
   {
      size_t size = strlen(names[i]) + strlen(values[i]) + 2;
      shell->environment[i] = malloc(size);
      if (!shell->environment[i])
         goto failed;
      snprintf(shell->environment[i], size, "%s=%s", names[i], values[i]);
   }
   char *const *grant_env = server_native_cli_environment(shell->grant);
   for (unsigned i = 0; i < 6; i++)
      if (!(shell->environment[i + 3] = strdup(grant_env[i])))
         goto failed;
   pthread_mutex_lock(&owner->mcp_lock);
   if (owner->closing || owner->shells[depth])
   {
      pthread_mutex_unlock(&owner->mcp_lock);
      goto failed;
   }
   owner->shells[depth] = shell;
   pthread_mutex_unlock(&owner->mcp_lock);
   *scope = (agent_tools_bash_scope_t){.environment = shell->environment,
                                       .replace_environment = 1,
                                       .progress = acp_cli_progress,
                                       .context = owner};
   *lease = shell;
   return 0;
failed:
   acp_bash_close(shell, owner);
   return -1;
}
/* The listener never borrows producer TLS. This callback runs synchronously on
 * the exact admitted producer before its next bounded ACP pipe operation. */
static int acp_mcp_service(aimee_harness_ir_t *state, void *context)
{
   acp_owner_t *owner = context;
   pthread_mutex_lock(&owner->mcp_lock);
   acp_mcp_request_t *work = owner->pending;
   if (!work || work->processing || work->complete)
   {
      pthread_mutex_unlock(&owner->mcp_lock);
      return 0;
   }
   work->processing = 1;
   pthread_mutex_unlock(&owner->mcp_lock);
   int failed = acp_owner_valid(owner) || atomic_load(&work->cancelled);
   if (!failed && work->listing)
      work->response = acp_mcp_list(server_acp_resources_mcp_catalog(owner->resources));
   else if (!failed)
   {
      cJSON *request =
          work->body ? aimee_ir_json_parse_frame(work->body, strlen(work->body)) : NULL;
      const char *method =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "method"));
      const char *tool = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "tool"));
      const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(request, "arguments");
      const cJSON *sid = cJSON_GetObjectItemCaseSensitive(request, "session_id");
      const cJSON *cwd = cJSON_GetObjectItemCaseSensitive(request, "cwd");
      if (!cJSON_IsObject(request) || !method || strcmp(method, "mcp.call") || !tool || !tool[0] ||
          strlen(tool) >= 128 || !cJSON_IsObject(arguments) ||
          (sid && (!cJSON_IsString(sid) || strcmp(sid->valuestring, owner->session))) ||
          (cwd && (!cJSON_IsString(cwd) || strcmp(cwd->valuestring, owner->cwd))))
         failed = 1;
      const cJSON *field;
      cJSON_ArrayForEach(
          field, request) if (!field->string ||
                              (strcmp(field->string, "method") && strcmp(field->string, "tool") &&
                               strcmp(field->string, "arguments") &&
                               strcmp(field->string, "session_id") && strcmp(field->string, "cwd")))
          failed = 1;
      long long remaining = owner->deadline - util_now_ms();
      agent_request_cancel_scope_t cancelled = {.flag = &work->cancelled,
                                                .parent = agent_request_cancel_scope_current()};
      const agent_request_cancel_scope_t *previous = agent_request_cancel_scope_bind(&cancelled);
      aimee_block_t result = {0};
      if (!failed && remaining > 0 && !aimee_acp_before_host_tool(state) &&
          !server_acp_resources_mcp_execute(owner->resources, work->key, tool, arguments,
                                            remaining < 120000 ? (int)remaining : 120000, &result))
      {
         owner->mcp_calls++;
         if (aimee_acp_observe_host_tool_result(state, &result))
            failed = 1;
         else
            work->response = acp_mcp_result(&result);
      }
      else
         failed = 1;
      aimee_block_free_contents(&result);
      agent_request_cancel_scope_bind(previous);
      cJSON_Delete(request);
   }
   if (!work->response)
      failed = 1;
   pthread_mutex_lock(&owner->mcp_lock);
   work->status = failed ? 403 : 200;
   work->complete = 1;
   owner->pending = NULL;
   pthread_cond_broadcast(&owner->mcp_changed);
   pthread_mutex_unlock(&owner->mcp_lock);
   return failed ? -1 : 0;
}
static void acp_mcp_close(acp_owner_t *owner)
{
   if (owner->mcp_initialized)
   {
      pthread_mutex_lock(&owner->mcp_lock);
      owner->closing = 1;
      if (owner->pending)
      {
         atomic_store(&owner->pending->cancelled, 1);
         if (!owner->pending->processing)
         {
            owner->pending->status = 403;
            owner->pending->complete = 1;
            owner->pending = NULL;
         }
      }
      pthread_cond_broadcast(&owner->mcp_changed);
      pthread_mutex_unlock(&owner->mcp_lock);
   }
   /* No callback or token remains after stop; borrowed producer state is then
    * safe to release. No detached worker borrows its request TLS. */
   cli_proxy_stop(owner->proxy);
   owner->proxy = NULL;
   cJSON_Delete(owner->mcp_servers);
   owner->mcp_servers = NULL;
   if (owner->mcp_fd >= 0)
      close(owner->mcp_fd);
   owner->mcp_fd = -1;
   if (owner->mcp_initialized)
   {
      pthread_cond_destroy(&owner->mcp_changed);
      pthread_mutex_destroy(&owner->mcp_lock);
      owner->mcp_initialized = 0;
   }
   OPENSSL_cleanse(owner->token, sizeof owner->token);
}
static int acp_mcp_env(cJSON *env, const char *name, const char *value)
{
   cJSON *entry = cJSON_CreateObject();
   if (!entry || !cJSON_AddStringToObject(entry, "name", name) ||
       !cJSON_AddStringToObject(entry, "value", value) || !cJSON_AddItemToArray(env, entry))
   {
      cJSON_Delete(entry);
      return -1;
   }
   return 0;
}
static int open_command(const char *name, const char *search_path)
{
   if (!name || !name[0])
      return -1;
   int fd = -1;
   if (strchr(name, '/'))
   {
      if (name[0] != '/')
         return -1; /* Relative executable lookup must never depend on a task cwd. */
      fd = open(name, O_RDONLY | O_CLOEXEC);
   }
   else
   {
      /* Resolve the explicitly selected command once in the producer's frozen
       * startup PATH. Relative/empty entries never turn task files into code. */
      char *paths = strdup(search_path);
      if (!paths)
         return -1;
      char *save = NULL;
      for (char *directory = strtok_r(paths, ":", &save); directory && fd < 0;
           directory = strtok_r(NULL, ":", &save))
      {
         char path[4096];
         if (directory[0] == '/' &&
             snprintf(path, sizeof path, "%s/%s", directory, name) < (int)sizeof path)
            fd = open(path, O_RDONLY | O_CLOEXEC);
      }
      free(paths);
   }
   struct stat st;
   if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & 0111)))
   {
      close(fd);
      fd = -1;
   }
   return fd;
}
static int acp_mcp_open(acp_owner_t *owner, const char *path, const char *home)
{
#if !defined(__linux__)
   (void)owner;
   (void)path;
   (void)home;
   return -1; /* No unpinned executable or ambient UID endpoint fallback. */
#else
   unsigned char random[32];
   unsigned port = 0;
   if (pthread_mutex_init(&owner->mcp_lock, NULL))
      return -1;
   if (pthread_cond_init(&owner->mcp_changed, NULL))
   {
      pthread_mutex_destroy(&owner->mcp_lock);
      return -1;
   }
   owner->mcp_initialized = 1;
   if ((owner->mcp_fd = open_command("aimee", path)) < 0 ||
       fstat(owner->mcp_fd, &owner->mcp_executable) || platform_random_bytes(random, sizeof random))
      return -1;
   for (size_t i = 0; i < sizeof random; i++)
      snprintf(owner->token + i * 2, 3, "%02x", random[i]);
   OPENSSL_cleanse(random, sizeof random);
   owner->proxy = cli_proxy_start_authorized(0, owner->token, owner->session, &port, acp_route,
                                             acp_authorize, acp_mcp_forward, owner);
   if (!owner->proxy)
      return -1;
   char command[128], endpoint[64];
   snprintf(command, sizeof command, "/proc/%ld/fd/%d", (long)getpid(), owner->mcp_fd);
   snprintf(endpoint, sizeof endpoint, "http://127.0.0.1:%u", port);
   snprintf(owner->endpoint, sizeof owner->endpoint, "%s", endpoint);
   owner->path = path;
   owner->home = home;
   if (server_acp_resources_set_bash_owner(owner->resources, acp_bash_open, acp_bash_close, owner))
      return -1;
   struct stat linked;
   if (stat(command, &linked) || !same_file(&linked, &owner->mcp_executable))
      return -1;
   owner->mcp_servers = cJSON_CreateArray();
   cJSON *server = cJSON_CreateObject();
   cJSON *arguments = server ? cJSON_AddArrayToObject(server, "args") : NULL;
   cJSON *env = server ? cJSON_AddArrayToObject(server, "env") : NULL;
   if (!owner->mcp_servers || !server || !arguments || !env ||
       !cJSON_AddStringToObject(server, "name", "aimee-owned") ||
       !cJSON_AddStringToObject(server, "command", command) ||
       !cJSON_AddItemToArray(arguments, cJSON_CreateString("mcp-serve")) ||
       acp_mcp_env(env, "PATH", path) || acp_mcp_env(env, "LANG", "C.UTF-8") ||
       acp_mcp_env(env, "HOME", home) || acp_mcp_env(env, "AIMEE_API_ENDPOINT", endpoint) ||
       acp_mcp_env(env, "AIMEE_API_BEARER", owner->token) ||
       acp_mcp_env(env, "AIMEE_NATIVE_CLI", "1") ||
       acp_mcp_env(env, "AIMEE_SESSION_ID", owner->session) ||
       acp_mcp_env(env, "AIMEE_SERVER_URL", "") || acp_mcp_env(env, "AIMEE_SERVER_TOKEN", "") ||
       !cJSON_AddItemToArray(owner->mcp_servers, server))
   {
      cJSON_Delete(server);
      return -1;
   }
   return 0;
#endif
}
int server_acp_execute_owned(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   agent_t selected = {0};
   provider_cli_cfg_t selection = {0};
   if (cfg && cfg->agent)
   {
      selected = *cfg->agent;
      selection = *cfg;
      selection.agent = &selected;
      cfg = &selection; /* Freeze the producer's entire selected command once. */
   }
   const char *failure = "ACP execution owner unavailable";
   const request_context_t *ctx = request_context_get();
   acp_owner_t owner = {.executable_fd = -1, .cwd_fd = -1, .mcp_fd = -1};
   char *argv[ACP_COMMAND_ARGS + 1] = {0};
   int argc = 0, rc = AGENT_RC_CONTEXT_REFUSED, model_bound = 0, tools_bound = 0;
   int correlation_bound = 0, session_bound = 0, previous_session_active = 0;
   char previous_session[128] = "";
   char previous_request_id[sizeof ctx->request_id];
   request_context_t original_context = {0};
   agent_tools_scope_snapshot_t previous_tools = {0}, tools = {0};
   server_acp_resources_t *resources = NULL;
   aimee_request_t captured = {0};
   aimee_message_t *latest = NULL;
   aimee_block_t host_system = {0}, *system_view = NULL;
   aimee_request_t task = {0};
   cJSON *history = NULL;
   void *previous_sink_context = NULL;
   aimee_ir_session_sink_fn previous_sink = NULL;
   unsigned long long prompt_lease = 0;
   char home[4096] = "", home_env[4128], path_env[8192];
   agent_request_creds_t credentials;
   memset(&credentials, 0, sizeof credentials);
   if (!cfg || !cfg->agent || !cfg->agent->enabled ||
       strnlen(selected.cli_kind, sizeof selected.cli_kind) == sizeof selected.cli_kind ||
       strnlen(selected.cli_cmd, sizeof selected.cli_cmd) == sizeof selected.cli_cmd ||
       strnlen(selected.name, sizeof selected.name) == sizeof selected.name || !selected.name[0] ||
       (cfg->role && strlen(cfg->role) >= sizeof tools.role) || strcmp(selected.cli_kind, "acp") ||
       !ctx || !ctx->principal[0] || !ctx->vault_principal[0] || ctx->context_refused)
      goto done;
   agent_t registered = {0};
   int configured = !agent_registry_find(selected.name, &registered);
   if (configured &&
       (!registered.enabled ||
        strnlen(registered.name, sizeof registered.name) == sizeof registered.name ||
        strnlen(registered.cli_kind, sizeof registered.cli_kind) == sizeof registered.cli_kind ||
        strnlen(registered.cli_cmd, sizeof registered.cli_cmd) == sizeof registered.cli_cmd ||
        strcmp(registered.name, selected.name) || strcmp(registered.cli_kind, "acp") ||
        strcmp(registered.cli_cmd, selected.cli_cmd)))
      goto done;
   owner.required_caps = configured && cfg->primary ? CAP_CHAT : CAP_DELEGATE;
   if ((ctx->capabilities & owner.required_caps) != owner.required_caps ||
       (ctx->vault_transport == ATTEST_NONE && !configured) ||
       strnlen(ctx->request_id, sizeof ctx->request_id) >= sizeof owner.request_id)
      goto done;
   owner.transport = ctx->vault_transport;
   owner.deadline = util_now_ms() + cli_acp_timeout_ms(&selected);
   owner.parent_cancel = agent_get_request_cancel();
   if (!ctx->request_id[0])
   {
      memcpy(previous_request_id, ctx->request_id, sizeof previous_request_id);
      unsigned char nonce[16];
      if (platform_random_bytes(nonce, sizeof nonce))
         goto done;
      request_context_t correlated = *ctx;
      for (size_t i = 0; i < sizeof nonce; i++)
         snprintf(correlated.request_id + i * 2, 3, "%02x", nonce[i]);
      OPENSSL_cleanse(nonce, sizeof nonce);
      request_context_set(&correlated); /* Local correlation, never source authority. */
      correlation_bound = 1;
   }
   snprintf(owner.request_id, sizeof owner.request_id, "%s", ctx->request_id);
   if (wire_fence_external_backend() || !cli_acp_request_supported(cfg))
   {
      failure = "ACP requested history or hidden generation/token/tool guarantees are unsupported";
      goto done;
   }
   /* The selected command is an execution-capable host producer's immutable
    * input, including an explicitly authorized inline command. Its display name
    * and ACP-provided fields confer no command authority. */
   owner.context = ctx;
   snprintf(owner.actor, sizeof owner.actor, "%s", ctx->vault_principal);
   agent_request_creds_snapshot(&credentials);
   size_t session_length = strnlen(credentials.session_id, sizeof credentials.session_id);
   if (!session_length || session_length >= sizeof owner.session ||
       strnlen(credentials.vault_principal, sizeof credentials.vault_principal) ==
           sizeof credentials.vault_principal ||
       strcmp(credentials.vault_principal, owner.actor))
      goto done;
   memcpy(owner.session, credentials.session_id, session_length + 1);
   OPENSSL_cleanse(&credentials, sizeof credentials);
   if (server_session_capture_retain_current(owner.session, owner.actor, &owner.capture) ||
       !owner.capture ||
       server_control_scope_capture(owner.session, owner.actor, &owner.controls) ||
       !owner.controls || !cfg->cwd || strlen(cfg->cwd) >= sizeof owner.cwd)
      goto done;
   previous_session_active = session_id_override_active();
   if (previous_session_active)
   {
      const char *previous = session_id();
      if (!previous || strlen(previous) >= sizeof previous_session)
         goto done;
      snprintf(previous_session, sizeof previous_session, "%s", previous);
   }
   session_id_set_override(owner.session);
   session_bound = 1;
   failure = "ACP canonical history is unavailable or unsupported";
   if (server_session_capture_history(owner.capture, &history) ||
       aimee_ir_history_admit_host(history) || !cJSON_IsArray(history) ||
       !(captured.n_messages = cJSON_GetArraySize(history)) ||
       captured.n_messages > AIMEE_IR_MAX_BLOCKS ||
       !(captured.messages = calloc((size_t)captured.n_messages, sizeof *captured.messages)))
      goto done;
   for (int i = 0; i < captured.n_messages; i++)
      if (aimee_ir_message_parse(cJSON_GetArrayItem(history, i), &captured.messages[i]))
         goto done;
   latest = &captured.messages[captured.n_messages - 1];
   if (strcmp(latest->role, "user") || latest->n_blocks != 1 ||
       latest->blocks[0].type != AIMEE_BLK_TEXT || !latest->blocks[0].text)
      goto done;
   if (cfg->canonical_request)
   {
      const aimee_request_t *seed = cfg->canonical_request;
      if (seed->n_messages != 1 && seed->n_messages != captured.n_messages)
         goto done;
      for (int i = 0; i < seed->n_messages; i++)
      {
         cJSON *expected = aimee_ir_message_json(&seed->messages[i]);
         const cJSON *actual =
             cJSON_GetArrayItem(history, seed->n_messages == 1 ? captured.n_messages - 1 : i);
         int exact = expected && cJSON_Compare(expected, actual, 1);
         cJSON_Delete(expected);
         if (!exact)
            goto done;
      }
      task.system = cfg->canonical_request->system;
      task.n_system = cfg->canonical_request->n_system;
      task.model = cfg->canonical_request->model;
      task.stream = cfg->canonical_request->stream;
      task.frontend = cfg->canonical_request->frontend;
      if (cfg->system_prompt && cfg->system_prompt[0])
      {
         aimee_response_t system_text = {.content = task.system, .n_content = task.n_system};
         char *joined = aimee_ir_response_text_alloc(&system_text);
         if (!joined)
            goto done;
         int present = !strcmp(joined, cfg->system_prompt);
         free(joined);
         for (int i = 0; i < task.n_system; i++)
            present |= !strcmp(task.system[i].text, cfg->system_prompt);
         if (!present)
         {
            if (task.n_system >= AIMEE_IR_MAX_BLOCKS ||
                !(system_view = calloc((size_t)task.n_system + 1, sizeof *system_view)))
               goto done;
            if (task.n_system)
               memcpy(system_view, task.system, (size_t)task.n_system * sizeof *system_view);
            system_view[task.n_system++] =
                (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = (char *)cfg->system_prompt};
            task.system = system_view;
         }
      }
   }
   else
   {
      if (strcmp(latest->blocks[0].text, cfg->user_prompt))
         goto done;
      if (cfg->system_prompt && cfg->system_prompt[0])
      {
         host_system = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = (char *)cfg->system_prompt};
         task.system = &host_system;
         task.n_system = 1;
      }
   }
   task.messages = captured.messages;
   task.n_messages = captured.n_messages;
   provider_cli_cfg_t represented = *cfg;
   represented.canonical_request = &task;
   if (!cli_acp_request_supported(&represented))
      goto done;
   failure = "ACP execution owner unavailable";
   snprintf(owner.cwd, sizeof owner.cwd, "%s", cfg->cwd);
   char workspace_identity[65];
   if (workspace_turn_resource_identity(owner.cwd, &owner.cwd_fd, workspace_identity) ||
       owner.cwd_fd < 0 || fstat(owner.cwd_fd, &owner.directory))
   {
      failure = "ACP requires an owned local workspace; no host execution fallback";
      goto done;
   }
   const char *startup_path = getenv("PATH");
   if (!startup_path || !startup_path[0])
      startup_path = "/usr/local/bin:/usr/bin:/bin";
   if (strlen(startup_path) >= sizeof path_env - 5)
      goto done;
   char *paths = strdup(startup_path), *saved_path = NULL;
   if (!paths)
      goto done;
   size_t used = 5;
   memcpy(path_env, "PATH=", 6);
   for (char *entry = strtok_r(paths, ":", &saved_path); entry;
        entry = strtok_r(NULL, ":", &saved_path))
      if (entry[0] == '/')
      {
         if (used > 5)
            path_env[used++] = ':';
         size_t length = strlen(entry);
         memcpy(path_env + used, entry, length + 1);
         used += length;
      }
   free(paths);
   if (used == 5)
      goto done;
   char command_error[128];
   argc = provider_cli_split_command(cfg->agent->cli_cmd, argv, ACP_COMMAND_ARGS, command_error,
                                     sizeof command_error);
   if (argc < 0 || (owner.executable_fd = open_command(argv[0], path_env + 5)) < 0 ||
       fstat(owner.executable_fd, &owner.executable))
   {
      failure = "ACP selected command cannot be opened as an owned executable";
      goto done;
   }
   if (acp_owner_valid(&owner) || server_session_capture_model_enter(owner.capture, &prompt_lease))
      goto done;
   agent_tools_scope_bookmark(&previous_tools);
   if (agent_tools_scope_capture_owned(&tools))
      goto done;
   snprintf(tools.role, sizeof tools.role, "%s", cfg->role ? cfg->role : "");
   agent_tools_scope_restore(&tools);
   tools_bound = 1;
   original_context = *ctx;
   request_context_t model_context = *ctx;
   model_context.model_execution = 1;
   model_context.operator_script_origin = 0;
   model_context.capabilities &= CAPS_AUTHENTICATED;
   request_context_set(&model_context);
   model_bound = 1;
   previous_sink = aimee_ir_session_get_sink(&previous_sink_context);
   if (!previous_sink)
      goto done;
   if (!cfg->no_tools && cfg->agent->tools_enabled)
   {
      resources =
          server_acp_resources_open(owner.capture, owner.session, owner.actor, owner.cwd, 1);
      if (!resources)
         goto done;
      aimee_ir_session_set_sink(server_acp_resources_sink, resources);
   }
   if (ctx->vault_transport == ATTEST_NONE)
   {
      /* This is the existing consumed host-service source, not a transport or
       * SDK credential inferred from a public registration name. */
      aimee_request_t source_profile = task;
      const aimee_request_t *mcp = server_acp_resources_mcp_catalog(resources);
      const aimee_request_t *fs = server_acp_resources_catalog(resources);
      int count = (mcp ? mcp->n_tools : 0) + (fs ? fs->n_tools : 0);
      if (count > AIMEE_IR_MAX_BLOCKS)
         goto done;
      aimee_tool_t *tools_view = count ? calloc((size_t)count, sizeof *tools_view) : NULL;
      if (count && !tools_view)
         goto done;
      if (mcp && mcp->n_tools)
         memcpy(tools_view, mcp->tools, (size_t)mcp->n_tools * sizeof *tools_view);
      if (fs && fs->n_tools)
         memcpy(tools_view + (mcp ? mcp->n_tools : 0), fs->tools,
                (size_t)fs->n_tools * sizeof *tools_view);
      source_profile.tools = tools_view;
      source_profile.n_tools = count;
      source_profile.model = selected.model;
      source_profile.mutated = 1;
      int source_refused =
          server_native_delegate_source_runtime_prepare(cfg->role, cfg->agent, &selected,
                                                        cfg->canonical_request, &source_profile,
                                                        owner.session) ||
          server_native_delegate_source_runtime_consume(&selected, ctx, &source_profile,
                                                        owner.session) != 1;
      free(tools_view);
      if (source_refused)
         goto done;
   }
   const char *temporary = getenv("TMPDIR");
   if (!temporary || temporary[0] != '/')
      temporary = P_tmpdir;
   if (snprintf(home, sizeof home, "%s/aimee-acp-XXXXXX", temporary) >= (int)sizeof home ||
       !mkdtemp(home))
      goto done;
   snprintf(home_env, sizeof home_env, "HOME=%s", home);
   owner.resources = resources;
   const aimee_request_t *mcp_catalog = server_acp_resources_mcp_catalog(resources);
   if ((resources && !mcp_catalog) ||
       (mcp_catalog && mcp_catalog->n_tools && acp_mcp_open(&owner, path_env + 5, home)))
   {
      failure = "ACP scoped MCP executable or listener unavailable";
      goto done;
   }
   char *environment[] = {path_env, "LANG=C.UTF-8", home_env, NULL};
   cli_acp_host_launch_t launch = {.executable_fd = owner.executable_fd,
                                   .cwd_fd = owner.cwd_fd,
                                   .argv = argv,
                                   .environment = environment,
                                   .cwd = owner.cwd,
                                   .mcp_servers = owner.mcp_servers,
                                   .deadline_ms = owner.deadline,
                                   .validate = acp_owner_valid,
                                   .service = owner.proxy ? acp_mcp_service : NULL,
                                   .context = &owner};
   provider_cli_cfg_t owned = *cfg;
   owned.agent = &selected;
   owned.canonical_request = &task;
   owned.user_prompt = latest->blocks[0].text;
   owned.acp_host = &launch;
   owned.cwd = owner.cwd;
   owned.session_id = owner.session;
   owned.tool_catalog = server_acp_resources_catalog(resources);
   owned.tool_call = resources ? server_acp_resources_execute : NULL;
   owned.tool_context = resources;
   owned.control_channel = server_control_scope_can_reply(owner.controls);
   owned.home = NULL;
   const provider_cli_adapter_t *adapter = provider_cli_adapter_get("acp");
   if (!adapter || !adapter->execute || acp_owner_valid(&owner))
      goto done;
   rc = adapter->execute(&owned, out);
   out->tool_calls += (int)owner.mcp_calls; /* Actual admitted host requests only. */
   if (!rc && out->success && cfg->canonical_response &&
       aimee_ir_response_clone(&out->canonical_response, cfg->canonical_response))
   {
      rc = AGENT_RC_CAPTURE_FAILED;
      failure = "ACP canonical response retention failed";
   }
done:
   OPENSSL_cleanse(&credentials, sizeof credentials);
   acp_mcp_close(&owner);
   if (previous_sink)
   {
      int failed = aimee_ir_session_capture_failed();
      aimee_ir_session_restore_sink(previous_sink, previous_sink_context, failed);
   }
   server_acp_resources_close(resources);
   if (model_bound)
   {
      /* Restore only producer privilege/intent; keep context refusals, receipts
       * and accounting accumulated by the actual model-origin resource calls. */
      request_context_t restored = *request_context_get();
      restored.capabilities = original_context.capabilities;
      restored.model_execution = original_context.model_execution;
      restored.operator_script_origin = original_context.operator_script_origin;
      request_context_set(&restored);
   }
   if (tools_bound)
      agent_tools_scope_restore(&previous_tools);
   agent_tools_scope_destroy_owned(&tools);
   if (prompt_lease && server_session_capture_model_leave(owner.capture, prompt_lease))
      rc = AGENT_RC_CAPTURE_FAILED;
   if (owner.controls)
      server_control_scope_release(owner.controls);
   server_session_capture_release(owner.capture);
   if (session_bound)
   {
      if (previous_session_active)
         session_id_set_override(previous_session);
      else
         session_id_clear_override();
   }
   if (correlation_bound)
   {
      request_context_t restored = *request_context_get();
      memcpy(restored.request_id, previous_request_id, sizeof restored.request_id);
      request_context_set(&restored);
   }
   free(system_view);
   aimee_request_free(&captured);
   cJSON_Delete(history);
   if (owner.executable_fd >= 0)
      close(owner.executable_fd);
   if (owner.cwd_fd >= 0)
      close(owner.cwd_fd);
   if (argc > 0)
      provider_cli_free_tokens(argv, argc);
   if (home[0])
      (void)rmdir(home); /* Only this empty owned HOME; never recurse into agent-created paths. */
   if (rc || !out->success)
   {
      out->success = 0;
      if (!out->error[0])
         snprintf(out->error, sizeof out->error, "%s", failure);
      if (!out->stop_reason[0])
         snprintf(out->stop_reason, sizeof out->stop_reason, "context_refused");
      (void)request_context_refuse_assembly("acp_execution_refused");
      return rc ? rc : AGENT_RC_CAPTURE_FAILED;
   }
   return 0;
}
