/* Credential-free SDK scheduler resources. The durable capture and existing
 * native-cache owner, not SDK files or process exit, own conversation truth. */
#include "server_native_execution.h"
#include "server_native_history.h"
#include "server_native_launch.h"
#include "server_native_sdk_profile.h"
#include "cli_claude_native.h"
#include "claude_headless.h"
#include "server_native_runtime.h"
#include "server_native_delegate_source.h"
#include "server_native_cache.h"
#include "server_native_cache_lease.h"
#include "server_native_containment.h"
#include "aimee_home.h"
#include "server_session_capture.h"
#include "request_context.h"
#include "agent_exec.h"
#include "agent_protocol.h"
#include "agent_request_build.h"
#include "agent_adapter.h"
#include <aimee/tools/agent_tools.h>
#include <aimee/ir/session_event.h>
#include "modules/workspace/workspace_turn.h"
#include "cli_session.h"
#include "db1_client/primary_sessions.h"
#include "aimee_sha256.h"
#include "log.h"
#include "cJSON.h"
#include <openssl/crypto.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

/* A known-quiescent child transfers only derived cache metadata to the turn.
 * No listener, credential, process or browser pointer survives in this resource. */
typedef struct
{
   server_native_cache_t *cache;
   server_native_cache_lease_t *lease;
   char native_id[128];
   server_native_containment_t *containment;
   cJSON *reservation;
   char actor[129];
   cli_session_t home_cleanup;
   server_native_runtime_t *runtime;
   int slot, success, cache_resolved, reservation_resolved;
} native_terminal_cache_t;
typedef struct
{
   server_session_capture_t *capture;
   agent_t selected;
   server_native_runtime_t *runtime;
   server_native_cache_t *cache;
   server_native_cache_lease_t *lease;
   server_native_containment_t *containment;
   cJSON *reservation;
   char epoch[33], effective_binding[65];
   aimee_request_t profile, adoption;
   cli_session_t home_cleanup;
   cli_codex_host_launch_t launch;
   cli_claude_host_launch_t claude;
   provider_cli_cfg_t launching;
   char *mcp_config;
   char claude_digest[65], sdk_identity[65];
   char home[4096], cwd[4096], build[65], workspace[65];
   int executable_fd, scheduler_fd, workspace_fd;
   cJSON *config;
   char *environment[16];
   char native_id[128];
   char source_session[128], actor[129];
   agent_tools_scope_snapshot_t previous_tools;
   int tools_bound, custody_unresolved, recovery_slot;
   native_terminal_cache_t *terminal_resource;
} native_execution_t;

static const char *native_string(const cJSON *value, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, key));
}
static int native_same(const cJSON *value, const char *key, const char *expected)
{
   const char *actual = native_string(value, key);
   return actual && expected && !strcmp(actual, expected);
}
/* DB admission supplies the authority. Filesystem markers never nominate a
 * process boundary to kill or a source turn to cancel. */
static cJSON *native_reservation_minimal(native_execution_t *execution)
{
   cJSON *value = cJSON_CreateObject();
   int valid = value && cJSON_AddNumberToObject(value, "version", 1) &&
               cJSON_AddStringToObject(value, "session_id",
                                       server_session_capture_session(execution->capture)) &&
               cJSON_AddStringToObject(value, "harness", execution->selected.cli_kind) &&
               cJSON_AddStringToObject(value, "binding_sha256",
                                       server_native_runtime_binding(execution->runtime));
   if (!valid)
   {
      cJSON_Delete(value);
      return NULL;
   }
   return value;
}
static int native_reservation_finish(native_terminal_cache_t *resource)
{
   cJSON *request = cJSON_Duplicate(resource->reservation, 1);
   cJSON *claim = resource->cache ? server_native_cache_claim_json(resource->cache) : NULL;
   const char *token = claim ? native_string(claim, "expected_token") : "";
   int valid = request && token && cJSON_AddStringToObject(request, "expected_token", token);
   char *wire = valid ? cJSON_PrintUnformatted(request) : NULL;
   char sequence[32], epoch[33];
   int absent = 0;
   /* Absent acknowledges only a real terminal-fenced prelaunch absence. */
   int rc = wire ? db1_session_native_reservation_finish_checked(resource->actor, wire, sequence,
                                                                 epoch, &absent)
                 : -1;
   free(wire);
   cJSON_Delete(request);
   cJSON_Delete(claim);
   return rc;
}
#define NATIVE_RECOVERY_SLOTS 32
static atomic_uint native_recovery_slots;
static pthread_mutex_t native_quarantine_lock = PTHREAD_MUTEX_INITIALIZER;
static native_terminal_cache_t *native_quarantine[NATIVE_RECOVERY_SLOTS];
static int native_quarantine_started, native_quarantine_stopping;
static unsigned native_terminal_attempts;
static pthread_t native_quarantine_worker;
static pthread_cond_t native_quarantine_wake = PTHREAD_COND_INITIALIZER;
/* Returns only after quiescence and durable ownership resolution. On refusal
 * the resource, deactivated bound port and exact durable descriptor stay owned. */
static int native_terminal_resolve_authoritative(native_terminal_cache_t *resource);
static int native_terminal_attempt(native_terminal_cache_t *resource)
{
   if (resource->runtime)
      server_native_runtime_deactivate_hold_port(resource->runtime);
   if (resource->containment && server_native_containment_quiesce(resource->containment, 1000))
      return -1;
   server_native_runtime_close(resource->runtime);
   resource->runtime = NULL;
   if (!resource->cache_resolved && resource->cache)
   {
      int resolved =
          resource->success && !server_native_cache_complete(resource->cache, resource->native_id);
      if (!resolved)
         resolved = !server_native_cache_abort(resource->cache);
      if (!resolved)
      {
         if (!resource->reservation || native_terminal_resolve_authoritative(resource))
            return -1;
         resource->reservation_resolved = 1;
      }
      resource->cache_resolved = 1;
   }
   if (resource->reservation && !resource->reservation_resolved &&
       native_reservation_finish(resource))
   {
      if (native_terminal_resolve_authoritative(resource))
         return -1;
      resource->reservation_resolved = 1;
   }
   return 0;
}
static void native_terminal_dispose(native_terminal_cache_t *resource, int resolved)
{
   if (resource->cache)
      server_native_cache_close(resource->cache);
   int marker_closed = !server_native_cache_lease_close(resource->lease, resolved);
   if (!marker_closed)
      aimee_log(LOG_WARN, "native-cache", "native cache owner marker remains unresolved");
   server_native_containment_close(resource->containment, resolved && marker_closed);
   /* A quarantine never reaches disposal while its native tree is unknown. */
   cli_session_destroy(&resource->home_cleanup);
   cJSON_Delete(resource->reservation);
   if (resource->slot)
      atomic_fetch_sub(&native_recovery_slots, 1);
   free(resource);
}
static void *native_quarantine_run(void *unused)
{
   (void)unused;
   for (;;)
   {
      pthread_mutex_lock(&native_quarantine_lock);
      int stopping = native_quarantine_stopping;
      pthread_mutex_unlock(&native_quarantine_lock);
      if (stopping)
         break;
      for (unsigned i = 0; i < NATIVE_RECOVERY_SLOTS; i++)
      {
         pthread_mutex_lock(&native_quarantine_lock);
         native_terminal_cache_t *resource =
             native_quarantine_stopping ? NULL : native_quarantine[i];
         pthread_mutex_unlock(&native_quarantine_lock);
         if (!resource)
            continue;
         if (!native_terminal_attempt(resource))
         {
            pthread_mutex_lock(&native_quarantine_lock);
            native_quarantine[i] = NULL;
            pthread_mutex_unlock(&native_quarantine_lock);
            native_terminal_dispose(resource, 1);
         }
      }
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += 5;
      pthread_mutex_lock(&native_quarantine_lock);
      if (!native_quarantine_stopping)
         pthread_cond_timedwait(&native_quarantine_wake, &native_quarantine_lock, &deadline);
      pthread_mutex_unlock(&native_quarantine_lock);
   }
   return NULL;
}
static int native_quarantine_ensure(void)
{
   pthread_mutex_lock(&native_quarantine_lock);
   if (!native_quarantine_started && !native_quarantine_stopping &&
       !pthread_create(&native_quarantine_worker, NULL, native_quarantine_run, NULL))
      native_quarantine_started = 1;
   int rc = native_quarantine_started && !native_quarantine_stopping ? 0 : -1;
   pthread_mutex_unlock(&native_quarantine_lock);
   return rc;
}
static void native_terminal_quarantine(native_terminal_cache_t *resource)
{
   if (resource->runtime)
      server_native_runtime_deactivate_hold_port(resource->runtime);
   pthread_mutex_lock(&native_quarantine_lock);
   unsigned i;
   for (i = 0; i < NATIVE_RECOVERY_SLOTS && native_quarantine[i]; i++)
      ;
   /* Every qualified launch reserved a preallocated capacity slot before DB40. */
   if (i < NATIVE_RECOVERY_SLOTS)
      native_quarantine[i] = resource;
   int stopping = native_quarantine_stopping;
   pthread_cond_signal(&native_quarantine_wake);
   pthread_mutex_unlock(&native_quarantine_lock);
   if (!stopping)
      aimee_log(LOG_WARN, "native-cache",
                "native custody quarantined; durable resolution will retry");
}
void server_native_execution_shutdown(void)
{
   pthread_mutex_lock(&native_quarantine_lock);
   native_quarantine_stopping = 1;
   int started = native_quarantine_started;
   pthread_t worker = native_quarantine_worker;
   pthread_cond_broadcast(&native_quarantine_wake);
   pthread_mutex_unlock(&native_quarantine_lock);
   if (started)
      pthread_join(worker, NULL);
   pthread_mutex_lock(&native_quarantine_lock);
   native_quarantine_started = 0;
   while (native_terminal_attempts)
      pthread_cond_wait(&native_quarantine_wake, &native_quarantine_lock);
   pthread_mutex_unlock(&native_quarantine_lock);
   /* Late callbacks may park already-reserved custody, but cannot start DB
    * retries after this join. Unknown trees/ports remain owned until exit. */
}
static void native_cache_terminal(int success, void *context)
{
   native_terminal_cache_t *resource = context;
   resource->success = success;
   int qualified = resource->slot;
   pthread_mutex_lock(&native_quarantine_lock);
   int stopping = native_quarantine_stopping;
   if (qualified && !stopping)
      native_terminal_attempts++;
   pthread_mutex_unlock(&native_quarantine_lock);
   if (qualified && stopping)
   {
      native_terminal_quarantine(resource);
      return;
   }
   int resolved = !native_terminal_attempt(resource);
   if (!resolved && qualified)
      native_terminal_quarantine(resource);
   else
      native_terminal_dispose(resource, resolved);
   if (qualified)
   {
      pthread_mutex_lock(&native_quarantine_lock);
      native_terminal_attempts--;
      pthread_cond_broadcast(&native_quarantine_wake);
      pthread_mutex_unlock(&native_quarantine_lock);
   }
}
/* Transfer failed qualified executions too. Only the durable terminal callback
 * may acknowledge the reservation; a dropped capture leaves it recoverable. */
static int native_terminal_transfer(native_execution_t *execution)
{
   native_terminal_cache_t *resource = execution->terminal_resource;
   if (!resource)
      resource = calloc(1, sizeof *resource);
   if (!resource)
      return -1;
   resource->cache = execution->cache;
   resource->lease = execution->lease;
   resource->containment = execution->containment;
   resource->reservation = execution->reservation;
   resource->runtime = execution->reservation ? execution->runtime : NULL;
   resource->slot = execution->recovery_slot;
   snprintf(resource->actor, sizeof resource->actor, "%s", execution->actor);
   snprintf(resource->native_id, sizeof resource->native_id, "%s", execution->native_id);
   if (execution->reservation)
      resource->home_cleanup = execution->home_cleanup;
   int deferred =
       server_session_capture_after_terminal(execution->capture, native_cache_terminal, resource);
   if (deferred && !execution->reservation)
   {
      free(resource);
      return -1;
   }
   if (execution->reservation)
   {
      execution->home_cleanup = (cli_session_t){0};
      execution->runtime = NULL;
   }
   execution->terminal_resource = NULL;
   execution->recovery_slot = 0;
   execution->cache = NULL;
   execution->lease = NULL;
   execution->containment = NULL;
   execution->reservation = NULL;
   execution->custody_unresolved = 0;
   if (deferred)
      native_terminal_quarantine(resource);
   return deferred ? -1 : 0;
}
static void native_execution_close(native_execution_t *execution)
{
   if (!execution)
      return;
   if (execution->containment)
      server_native_runtime_deactivate_hold_port(execution->runtime);
   if (execution->reservation && !native_terminal_transfer(execution))
   { /* The canonical terminal owner now holds all cleanup resources. */
   }
   int resolved = !execution->reservation && !execution->custody_unresolved;
   int quiescent =
       !execution->containment || !server_native_containment_quiesce(execution->containment, 10000);
   if (quiescent)
      server_native_runtime_close(execution->runtime);
   if (execution->cache && quiescent)
   {
      int aborted = !server_native_cache_abort(execution->cache);
      resolved = resolved && aborted;
   }
   if (execution->cache)
      server_native_cache_close(execution->cache);
   int marker_closed = !server_native_cache_lease_close(execution->lease, resolved);
   if (!marker_closed)
      aimee_log(LOG_WARN, "native-cache", "native cache owner marker remains unresolved");
   server_native_containment_close(execution->containment, resolved && quiescent && marker_closed);
   cJSON_Delete(execution->reservation);
   if (execution->recovery_slot)
      atomic_fetch_sub(&native_recovery_slots, 1);
   free(execution->terminal_resource);
   if (execution->executable_fd >= 0)
      close(execution->executable_fd);
   if (execution->scheduler_fd >= 0)
      close(execution->scheduler_fd);
   if (execution->workspace_fd >= 0)
      close(execution->workspace_fd);
   if (quiescent)
      cli_session_destroy(&execution->home_cleanup);
   else
      free(execution->home_cleanup.iso_home);
   aimee_request_free(&execution->profile);
   aimee_request_free(&execution->adoption);
   cJSON_Delete(execution->config);
   free(execution->mcp_config);
   for (size_t i = 0; i < sizeof execution->environment / sizeof execution->environment[0]; i++)
      free(execution->environment[i]);
   server_session_capture_release(execution->capture);
   if (execution->tools_bound)
      agent_tools_scope_restore(&execution->previous_tools);
   free(execution);
}
/* The descriptor launch is qualified for Linux's default configuration
 * hierarchy. A nonempty ambient/managed SDK configuration is outside this
 * credential-free profile; never let it load secrets before effective readback. */
static int native_ambient_config_absent(const char *sdk)
{
   const char *codex[] = {"/etc/codex/config.toml", "/etc/codex/managed_config.toml",
                          "/etc/codex/requirements.toml"};
   const char *claude[] = {"/etc/claude-code/managed-settings.json",
                           "/etc/claude-code/managed-mcp.json",
                           "/etc/claude-code/managed-settings.d"};
   const char *const *paths = !strcmp(sdk, "claude") ? claude : codex;
   for (size_t i = 0; i < 3; i++)
   {
      struct stat status;
      if (lstat(paths[i], &status) == 0)
      {
         if (!strcmp(sdk, "claude") && i == 2 && S_ISDIR(status.st_mode))
         {
            DIR *directory = opendir(paths[i]);
            if (!directory)
               return 0;
            struct dirent *entry;
            int empty = 1;
            while ((entry = readdir(directory)))
               if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
                  empty = 0;
            closedir(directory);
            if (!empty)
               return 0;
         }
         else if (!S_ISREG(status.st_mode) || status.st_size != 0)
            return 0;
      }
      else if (errno != ENOENT)
         return 0;
   }
   return 1;
}
static int native_environment(native_execution_t *execution)
{
   char codex[4200], temporary[4200];
   int claude = !strcmp(execution->selected.cli_kind, "claude");
   if (snprintf(codex, sizeof codex,
                claude ? "CLAUDE_CONFIG_DIR=%s/.claude" : "CODEX_HOME=%s/.codex",
                execution->home) >= (int)sizeof codex ||
       snprintf(temporary, sizeof temporary, "TMPDIR=%s/tmp", execution->home) >=
           (int)sizeof temporary)
      return -1;
   char home[4200];
   if (snprintf(home, sizeof home, "HOME=%s", execution->home) >= (int)sizeof home)
      return -1;
   const char *values[] = {home,
                           codex,
                           temporary,
                           "PATH=/usr/bin:/bin",
                           "LANG=C.UTF-8",
                           "OTEL_SDK_DISABLED=true",
                           "HTTP_PROXY=http://127.0.0.1:1",
                           "HTTPS_PROXY=http://127.0.0.1:1",
                           "NO_PROXY=127.0.0.1,localhost",
                           NULL};
   for (int i = 0; values[i]; i++)
   {
      execution->environment[i] = strdup(values[i]);
      if (!execution->environment[i])
         return -1;
   }
   if (claude)
   {
      char key[512], url[512];
      if (snprintf(key, sizeof key, "ANTHROPIC_API_KEY=%s",
                   server_native_runtime_token(execution->runtime)) >= (int)sizeof key ||
          snprintf(url, sizeof url, "ANTHROPIC_BASE_URL=%s",
                   server_native_runtime_url(execution->runtime)) >= (int)sizeof url)
         return -1;
      /* The canonical gateway replaces SDK system content. Claude's gateway
       * protocol requires client-side omission of its fingerprint block in
       * this case; neither stripping nor moving that block is supported. */
      /* Stream-json startup otherwise reports a pending MCP server with no
       * tools before discovery completes. Require the SDK's blocking startup
       * mode; the exact connected/catalog readback remains mandatory. */
      const char *extra[] = {key,
                             url,
                             "DISABLE_AUTOUPDATER=1",
                             "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1",
                             "CLAUDE_CODE_ATTRIBUTION_HEADER=0",
                             "MCP_CONNECTION_NONBLOCKING=0"};
      for (unsigned i = 0; i < sizeof extra / sizeof extra[0]; i++)
      {
         execution->environment[9 + i] = strdup(extra[i]);
         if (!execution->environment[9 + i])
            return -1;
      }
   }
   return 0;
}
static int native_prepare_directory(native_execution_t *execution)
{
   /* Claude locates its project transcript by cwd. Keep that private path
    * stable only within this admitted lease/epoch, while HOME/config/tmp stay
    * per-launch. The tool runtime retains the actual operator project cwd. */
   if (!strcmp(execution->selected.cli_kind, "claude"))
   {
      if (!server_native_cache_workspace_valid(execution->lease, execution->epoch,
                                               execution->scheduler_fd, execution->cwd))
         return -1;
   }
   else
   {
      if (snprintf(execution->cwd, sizeof execution->cwd, "%s/workspace", execution->home) >=
              (int)sizeof execution->cwd ||
          mkdir(execution->cwd, 0700))
         return -1;
      execution->scheduler_fd =
          open(execution->cwd, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   }
   char temporary[4200];
   return execution->scheduler_fd < 0 ||
                  snprintf(temporary, sizeof temporary, "%s/tmp", execution->home) >=
                      (int)sizeof temporary ||
                  mkdir(temporary, 0700)
              ? -1
              : 0;
}
static int native_write_config(native_execution_t *execution, const agent_t *selected)
{
   if (native_prepare_directory(execution))
      return -1;
   execution->config = cJSON_CreateObject();
   cJSON *config = execution->config;
   char url[320], authorization[320];
   if (!config ||
       snprintf(url, sizeof url, "%s/v1", server_native_runtime_url(execution->runtime)) >=
           (int)sizeof url ||
       snprintf(authorization, sizeof authorization, "Bearer %s",
                server_native_runtime_token(execution->runtime)) >= (int)sizeof authorization ||
       !cJSON_AddStringToObject(config, "model", selected->model) ||
       !cJSON_AddStringToObject(config, "model_provider", "aimee_ir") ||
       !cJSON_AddStringToObject(config, "approval_policy", "never") ||
       !cJSON_AddStringToObject(config, "sandbox_mode", "read-only") ||
       !cJSON_AddStringToObject(config, "web_search", "disabled") ||
       !cJSON_AddStringToObject(config, "cli_auth_credentials_store", "file") ||
       !cJSON_AddStringToObject(config, "mcp_oauth_credentials_store", "file"))
      return -1;
   cJSON *features = cJSON_AddObjectToObject(config, "features");
   const char *disabled[] = {"shell_tool",    "unified_exec",   "multi_agent", "apps",
                             "remote_plugin", "goals",          "view_image",  "image_generation",
                             "hooks",         "shell_snapshot", "memories",    "js_repl"};
   if (!features)
      return -1;
   for (size_t i = 0; i < sizeof disabled / sizeof disabled[0]; i++)
      if (!cJSON_AddFalseToObject(features, disabled[i]))
         return -1;
   cJSON *projects = cJSON_AddObjectToObject(config, "projects");
   cJSON *project = projects ? cJSON_AddObjectToObject(projects, execution->cwd) : NULL;
   cJSON *providers = cJSON_AddObjectToObject(config, "model_providers");
   cJSON *provider = providers ? cJSON_AddObjectToObject(providers, "aimee_ir") : NULL;
   cJSON *headers = provider ? cJSON_AddObjectToObject(provider, "http_headers") : NULL;
   if (!project || !cJSON_AddStringToObject(project, "trust_level", "untrusted") || !provider ||
       !headers || !cJSON_AddStringToObject(provider, "name", "Aimee canonical host") ||
       !cJSON_AddStringToObject(provider, "base_url", url) ||
       !cJSON_AddStringToObject(provider, "wire_api", "responses") ||
       !cJSON_AddFalseToObject(provider, "requires_openai_auth") ||
       !cJSON_AddStringToObject(headers, "Authorization", authorization))
      return -1;
   char path[4200];
   if (snprintf(path, sizeof path, "%s/.codex/config.toml", execution->home) >= (int)sizeof path)
      return -1;
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
   FILE *file = fd >= 0 ? fdopen(fd, "w") : NULL;
   if (!file)
   {
      if (fd >= 0)
         close(fd);
      return -1;
   }
   cJSON *value;
   int valid = 1;
   cJSON_ArrayForEach(value, config)
   {
      if (!cJSON_IsString(value))
         continue;
      char *text = cJSON_PrintUnformatted(value);
      valid &= text && fprintf(file, "%s=%s\n", value->string, text) > 0;
      free(text);
   }
   valid &= fprintf(file, "[features]\n") > 0;
   for (size_t i = 0; i < sizeof disabled / sizeof disabled[0]; i++)
      valid &= fprintf(file, "%s=false\n", disabled[i]) > 0;
   cJSON *cwd_value = cJSON_CreateString(execution->cwd), *url_value = cJSON_CreateString(url);
   cJSON *auth_value = cJSON_CreateString(authorization);
   char *cwd = cwd_value ? cJSON_PrintUnformatted(cwd_value) : NULL;
   char *base = url_value ? cJSON_PrintUnformatted(url_value) : NULL;
   char *auth = auth_value ? cJSON_PrintUnformatted(auth_value) : NULL;
   valid &= cwd && base && auth &&
            fprintf(file,
                    "[projects.%s]\ntrust_level=\"untrusted\"\n"
                    "[model_providers.aimee_ir]\nname=\"Aimee canonical host\"\n"
                    "base_url=%s\nwire_api=\"responses\"\nrequires_openai_auth=false\n"
                    "[model_providers.aimee_ir.http_headers]\nAuthorization=%s\n",
                    cwd, base, auth) > 0;
   free(cwd);
   free(base);
   free(auth);
   cJSON_Delete(cwd_value);
   cJSON_Delete(url_value);
   cJSON_Delete(auth_value);
   if (fclose(file))
      valid = 0;
   return valid ? native_environment(execution) : -1;
}
static int native_claim_publish(const cJSON *claim, void *context)
{
   native_execution_t *execution = context;
   cJSON *marker = cJSON_Duplicate(server_native_cache_lease_manifest(execution->lease), 1);
   cJSON *copy = cJSON_Duplicate(claim, 1);
   if (marker)
      cJSON_DeleteItemFromObjectCaseSensitive(marker, "claim_request");
   int rc = marker && copy && cJSON_AddItemToObject(marker, "claim_request", copy)
                ? server_native_cache_lease_update(execution->lease, marker)
                : -1;
   if (!marker)
      cJSON_Delete(copy);
   cJSON_Delete(marker);
   return rc;
}
static cJSON *native_reservation_action(const cJSON *authority)
{
   const char *keys[] = {"session_id",     "turn_id",       "harness",
                         "binding_sha256", "owner_id",      "containment_sha256",
                         "epoch",          "expected_token"};
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddNumberToObject(request, "version", 1))
      goto failed;
   for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
   {
      const char *value = native_string(authority, keys[i]);
      if (!value && !strcmp(keys[i], "expected_token") && !native_string(authority, "claim_id"))
         value = "";
      if (!value || !cJSON_AddStringToObject(request, keys[i], value))
         goto failed;
   }
   return request;
failed:
   cJSON_Delete(request);
   return NULL;
}
/* A lost cache CAS acknowledgement must not force repeated stale writes. The
 * authoritative owner lookup provides its actual exact token; finish preserves
 * an admitted complete cache, otherwise reconcile invalidates only this owner. */
static int native_terminal_resolve_authoritative(native_terminal_cache_t *resource)
{
   cJSON *minimal = cJSON_CreateObject();
   const char *keys[] = {"session_id", "harness", "binding_sha256", "owner_id"};
   int valid = minimal && cJSON_AddNumberToObject(minimal, "version", 1);
   for (size_t i = 0; i < sizeof keys / sizeof keys[0] && valid; i++)
   {
      const char *value = native_string(resource->reservation, keys[i]);
      valid = value && cJSON_AddStringToObject(minimal, keys[i], value);
   }
   char *wire = valid ? cJSON_PrintUnformatted(minimal) : NULL, *stored = NULL;
   int rc =
       wire ? db1_session_native_reservation_lookup_checked(resource->actor, wire, &stored) : -1;
   cJSON *authority = !rc && stored ? aimee_ir_json_parse_frame(stored, strlen(stored)) : NULL;
   const char *identity[] = {"session_id",     "turn_id",  "harness",
                             "binding_sha256", "owner_id", "containment_sha256",
                             "epoch"};
   valid = authority && native_same(authority, "actor", resource->actor);
   for (size_t i = 0; i < sizeof identity / sizeof identity[0] && valid; i++)
      valid =
          native_same(authority, identity[i], native_string(resource->reservation, identity[i]));
   cJSON *request = valid ? native_reservation_action(authority) : NULL;
   free(wire);
   wire = request ? cJSON_PrintUnformatted(request) : NULL;
   char sequence[32], epoch[33];
   int absent = 0;
   rc = wire ? db1_session_native_reservation_finish_checked(resource->actor, wire, sequence, epoch,
                                                             &absent)
             : -1;
   const char *token = native_string(authority, "expected_token");
   if (!token && !native_string(authority, "claim_id"))
      token = "";
   int unresolved =
       token && (!token[0] || !strncmp(token, "pending:", 8) || !strncmp(token, "finishing:", 10));
   if (rc && wire && unresolved)
      rc = db1_session_native_reservation_reconcile(resource->actor, wire, sequence, epoch);
   free(wire);
   free(stored);
   cJSON_Delete(minimal);
   cJSON_Delete(authority);
   cJSON_Delete(request);
   return rc;
}
static int native_custody_recover(const char *session, const char *actor, const char *active_turn,
                                  const char *sdk, const char *binding,
                                  server_native_cache_lease_t *lease, char out_epoch[33],
                                  const cJSON *authority, const cJSON *prior, char error[512])
{
   const char *turn = native_string(authority, "turn_id");
   const char *descriptor = native_string(authority, "containment_json");
   const char *hash = native_string(authority, "containment_sha256");
   if (!native_same(authority, "actor", actor) || !native_same(authority, "session_id", session) ||
       !native_same(authority, "harness", sdk) ||
       !native_same(authority, "binding_sha256", binding) || !turn ||
       (active_turn && !strcmp(turn, active_turn)) || !descriptor || !hash ||
       (prior && (!native_same(prior, "owner", native_string(authority, "owner_id")) ||
                  !native_same(prior, "turn_id", turn))))
      return -1;
   cJSON *manifest = aimee_ir_json_parse_frame(descriptor, strlen(descriptor));
   server_native_containment_t *old =
       manifest ? server_native_containment_reopen(manifest, error) : NULL;
   cJSON_Delete(manifest);
   char actual[65];
   int valid = old && !server_native_containment_sha256(old, actual) && !strcmp(actual, hash);
   /* Identity was admitted by DB43, not the writable marker. Only now may the
    * helper address this exact boundary, including descendants after restart. */
   int empty = valid && !server_native_containment_quiesce(old, 10000);
   cJSON *request = empty ? native_reservation_action(authority) : NULL;
   char *wire = request ? cJSON_PrintUnformatted(request) : NULL;
   char sequence[32], epoch[33];
   const char *token = native_string(authority, "expected_token");
   int rc = -1, absent = 0;
   /* A completed canonical prefix may only lack its final custody receipt.
    * Preserve that cache/epoch; unfinished ownership still uses atomic41. */
   if (wire && token && !strncmp(token, "complete:", 9))
      rc = db1_session_native_reservation_finish_checked(actor, wire, sequence, epoch, &absent);
   if (rc && wire && (!token || strncmp(token, "complete:", 9)))
      rc = db1_session_native_reservation_reconcile(actor, wire, sequence, epoch);
   if (!rc)
   {
      snprintf(out_epoch, 33, "%s", epoch);
      rc = server_native_cache_lease_rotate_unpublished(
          lease, active_turn && active_turn[0] ? active_turn : "native-prehistory", error);
   }
   server_native_containment_close(old, !rc);
   free(wire);
   cJSON_Delete(request);
   return rc;
}
static int native_hex_text(const char *value, size_t length)
{
   if (!value || strlen(value) != length)
      return 0;
   for (size_t i = 0; i < length; i++)
      if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
         return 0;
   return 1;
}
static int native_recovery_record_admit(const cJSON *record, const char *session, const char *actor,
                                        const char *active_turn)
{
   const char *sdk = native_string(record, "harness"), *turn = native_string(record, "turn_id");
   const char *descriptor = native_string(record, "containment_json"),
              *epoch = native_string(record, "epoch");
   const char *token = native_string(record, "expected_token"),
              *claim = native_string(record, "claim_id");
   const cJSON *version = cJSON_GetObjectItemCaseSensitive(record, "version");
   return cJSON_IsObject(record) && cJSON_IsNumber(version) && version->valuedouble == 1 &&
          native_same(record, "session_id", session) && native_same(record, "actor", actor) &&
          native_same(record, "state", "active") && sdk &&
          (!strcmp(sdk, "codex") || !strcmp(sdk, "claude")) && turn && turn[0] &&
          strlen(turn) <= 128 && (!active_turn || strcmp(turn, active_turn)) && descriptor &&
          descriptor[0] && strlen(descriptor) <= 8192 &&
          native_hex_text(native_string(record, "binding_sha256"), 64) &&
          native_hex_text(native_string(record, "owner_id"), 32) &&
          native_hex_text(native_string(record, "containment_sha256"), 64) && epoch &&
          (!strcmp(epoch, "0") || native_hex_text(epoch, 32)) &&
          ((!token && !claim) || (token && strlen(token) <= 4096));
}
/* Discovery is session/actor scoped and precedes model/account/SDK selection.
 * A physical lease plus re-read DB authority is required for every old owner;
 * neither a writable marker nor the new selected model nominates a boundary. */
static int native_session_reconcile(const char *session, const char *actor, const char *active_turn,
                                    char error[512], int browse)
{
   if (error)
      error[0] = 0;
   if (!session || !session[0] || strlen(session) > 128 || !actor || !actor[0] ||
       strlen(actor) >= (browse ? VAULT_PRINCIPAL_MAX : 129))
      return -1;
   pthread_mutex_lock(&native_quarantine_lock);
   int stopping = native_quarantine_stopping;
   pthread_mutex_unlock(&native_quarantine_lock);
   if (stopping)
      return -1;
   char *stored = NULL;
   int rc = db1_session_native_active_owned(actor, session, &stored);
   cJSON *owners = !rc && stored && strlen(stored) <= 512 * 1024
                       ? aimee_ir_json_parse_frame(stored, strlen(stored))
                       : NULL;
   free(stored);
   if (!cJSON_IsArray(owners) || cJSON_GetArraySize(owners) > 32)
   {
      rc = -1;
      goto done;
   }
   const cJSON *record, *other;
   cJSON_ArrayForEach(record, owners)
   {
      if (!native_recovery_record_admit(record, session, actor, active_turn))
      {
         rc = -1;
         goto done;
      }
      for (other = record->next; other; other = other->next)
         if (native_same(other, "harness", native_string(record, "harness")) &&
             native_same(other, "binding_sha256", native_string(record, "binding_sha256")))
         {
            rc = -1;
            goto done;
         }
   }
   cJSON_ArrayForEach(record, owners)
   {
      /* A durable record alone does not establish liveness. Only the exact
       * process-local retained writer can defer browse recovery; an arbitrary
       * locked lease, PID or unavailable authority remains a refusal. */
      if (browse &&
          server_session_capture_has_owner(session, actor, native_string(record, "turn_id")))
         continue;
      const char *sdk = native_string(record, "harness"),
                 *binding = native_string(record, "binding_sha256");
      cJSON *prior = NULL;
      server_native_cache_lease_t *lease = server_native_cache_lease_acquire_unpublished(
          aimee_home(), sdk, session, binding, actor,
          active_turn && active_turn[0] ? active_turn : "native-prehistory", &prior, error);
      cJSON *request = cJSON_CreateObject();
      int valid = lease && request && cJSON_AddNumberToObject(request, "version", 1) &&
                  cJSON_AddStringToObject(request, "session_id", session) &&
                  cJSON_AddStringToObject(request, "harness", sdk) &&
                  cJSON_AddStringToObject(request, "binding_sha256", binding) &&
                  cJSON_AddStringToObject(request, "owner_id", native_string(record, "owner_id"));
      char *wire = valid ? cJSON_PrintUnformatted(request) : NULL, *fresh = NULL;
      int found = wire ? db1_session_native_reservation_lookup_checked(actor, wire, &fresh) : -1;
      cJSON *authority = !found && fresh ? aimee_ir_json_parse_frame(fresh, strlen(fresh)) : NULL;
      char epoch[33];
      rc = authority && cJSON_Compare(record, authority, 1)
               ? native_custody_recover(session, actor, active_turn, sdk, binding, lease, epoch,
                                        authority, prior, error)
               : -1;
      int closed = server_native_cache_lease_close(lease, !rc);
      if (closed)
         rc = -1;
      cJSON_Delete(prior);
      cJSON_Delete(request);
      cJSON_Delete(authority);
      free(fresh);
      free(wire);
      if (rc)
         goto done;
   }
done:
   cJSON_Delete(owners);
   if (rc && error && !error[0])
      snprintf(error, 512, "native session ownership recovery refused");
   return rc;
}
int server_native_session_reconcile(const char *session, const char *actor, const char *active_turn,
                                    char error[512])
{
   return native_session_reconcile(session, actor, active_turn, error, 0);
}
int server_native_session_reconcile_browse(const char *session, const char *actor, char error[512])
{
   return native_session_reconcile(session, actor, NULL, error, 1);
}
static int native_custody_open(native_execution_t *execution, char error[512])
{
   const char *stage = "shared cache epoch";
   cJSON *minimal = native_reservation_minimal(execution);
   char *wire = minimal ? cJSON_PrintUnformatted(minimal) : NULL;
   int rc = wire ? db1_session_native_epoch(execution->actor, wire, execution->epoch) : -1;
   cJSON *prior = NULL, *authority = NULL;
   char *stored = NULL;
   if (rc)
      goto done;
   stage = "physical cache lease";
   execution->lease = server_native_cache_lease_acquire_unpublished(
       aimee_home(), execution->selected.cli_kind,
       server_session_capture_session(execution->capture),
       server_native_runtime_binding(execution->runtime), execution->actor,
       server_session_capture_turn(execution->capture), &prior, error);
   if (!execution->lease)
   {
      rc = -1;
      goto done;
   }
   execution->custody_unresolved = 1;
   stage = "durable cache owner lookup";
   int found = db1_session_native_reservation_lookup_checked(execution->actor, wire, &stored);
   /* Finished/reconciled receipts are only selected by the prior exact owner;
    * no active row and a legacy marker is never a recoverable authority. */
   if (found == 1 && prior)
   {
      const char *owner = native_string(prior, "owner");
      if (!owner || !cJSON_AddStringToObject(minimal, "owner_id", owner))
      {
         rc = -1;
         goto done;
      }
      free(wire);
      wire = cJSON_PrintUnformatted(minimal);
      found = wire ? db1_session_native_reservation_lookup_checked(execution->actor, wire, &stored)
                   : -1;
   }
   if (found < 0 || (found == 1 && prior))
   {
      rc = -1;
      goto done;
   }
   if (found == 0)
   {
      authority = stored ? aimee_ir_json_parse_frame(stored, strlen(stored)) : NULL;
      if (!authority ||
          native_custody_recover(server_session_capture_session(execution->capture),
                                 execution->actor, server_session_capture_turn(execution->capture),
                                 execution->selected.cli_kind,
                                 server_native_runtime_binding(execution->runtime),
                                 execution->lease, execution->epoch, authority, prior, error))
      {
         rc = -1;
         goto done;
      }
   }
   const char *root = getenv("AIMEE_NATIVE_CONTAINMENT_ROOT");
   const char *url = server_native_runtime_url(execution->runtime);
   char *end = NULL;
   const char *port_text = url && !strncmp(url, "http://127.0.0.1:", 17) ? url + 17 : NULL;
   unsigned long port = port_text ? strtoul(port_text, &end, 10) : 0;
   uint16_t ports[1] = {(uint16_t)port};
   int qualified = root && root[0] && port && port <= 65535 && end && !*end &&
                   !server_native_containment_probe(root, ports, 1, error);
   if (qualified)
   {
      stage = "bounded cleanup owner";
      if (native_quarantine_ensure())
      {
         rc = -1;
         goto done;
      }
      if (atomic_fetch_add(&native_recovery_slots, 1) >= NATIVE_RECOVERY_SLOTS)
      {
         atomic_fetch_sub(&native_recovery_slots, 1);
         rc = -1;
         goto done;
      }
      execution->recovery_slot = 1;
      execution->terminal_resource = calloc(1, sizeof *execution->terminal_resource);
      if (!execution->terminal_resource)
      {
         rc = -1;
         goto done;
      }
      stage = "kernel containment owner";
      execution->containment = server_native_containment_open(root, ports, 1, error);
      cJSON *request = native_reservation_minimal(execution);
      char hash[65];
      char *descriptor =
          execution->containment
              ? cJSON_PrintUnformatted(server_native_containment_manifest(execution->containment))
              : NULL;
      int valid = execution->containment && request && descriptor &&
                  !server_native_containment_sha256(execution->containment, hash) &&
                  cJSON_AddStringToObject(request, "turn_id",
                                          server_session_capture_turn(execution->capture)) &&
                  cJSON_AddStringToObject(request, "owner_id",
                                          server_native_cache_lease_owner(execution->lease)) &&
                  cJSON_AddStringToObject(request, "containment_sha256", hash) &&
                  cJSON_AddStringToObject(request, "containment_json", descriptor) &&
                  cJSON_AddStringToObject(request, "epoch", execution->epoch);
      char *reserve = valid ? cJSON_PrintUnformatted(request) : NULL;
      char epoch[33];
      if (reserve)
      {
         execution->reservation = cJSON_Duplicate(request, 1);
         if (execution->reservation)
            cJSON_DeleteItemFromObjectCaseSensitive(execution->reservation, "containment_json");
      }
      stage = "durable prelaunch reservation";
      pthread_mutex_lock(&native_quarantine_lock);
      rc = reserve && execution->reservation && !native_quarantine_stopping
               ? db1_session_native_reserve(execution->actor, reserve, epoch)
               : -1;
      pthread_mutex_unlock(&native_quarantine_lock);
      free(reserve);
      if (!rc)
      {
         /* Reserve acknowledgement precedes marker, HOME and SDK configuration.
          * Ambiguous acknowledgement remains discoverable through DB43. */
         cJSON_DeleteItemFromObjectCaseSensitive(request, "containment_json");
         cJSON_Delete(execution->reservation);
         execution->reservation = request;
         request = NULL;
         stage = "runtime source custody";
         if (server_native_runtime_set_native_owner(execution->runtime, execution->reservation))
         {
            rc = -1;
            free(descriptor);
            goto done;
         }
         stage = "cache owner publication";
         cJSON *marker = cJSON_Duplicate(server_native_cache_lease_manifest(execution->lease), 1);
         rc = !marker || !cJSON_AddStringToObject(marker, "containment_sha256", hash) ||
              !cJSON_AddStringToObject(marker, "containment_json", descriptor) ||
              !cJSON_AddStringToObject(marker, "epoch", epoch) ||
              server_native_cache_lease_update(execution->lease, marker);
         cJSON_Delete(marker);
      }
      free(descriptor);
      cJSON_Delete(request);
      if (rc)
         goto done;
   }
   else
   {
      /* Unavailable optional kernel policy preserves the established direct
       * launch. It never authorizes clearing an unresolved owner. */
      error[0] = 0;
      rc = server_native_cache_lease_update(execution->lease,
                                            server_native_cache_lease_manifest(execution->lease));
      if (rc)
         goto done;
   }
   stage = "epoch-specific cache binding";
   execution->custody_unresolved = 0;
   rc = server_native_cache_epoch_binding(server_native_runtime_binding(execution->runtime),
                                          execution->epoch, execution->effective_binding);
done:
   if (rc && !error[0])
      snprintf(error, 512, "native cache custody refused during %s", stage);
   free(stored);
   free(wire);
   cJSON_Delete(minimal);
   cJSON_Delete(prior);
   cJSON_Delete(authority);
   return rc;
}
static int native_registry_open(native_execution_t *execution, const request_context_t *owner,
                                const provider_cli_cfg_t *cfg)
{
   aimee_request_t *profile = &execution->profile;
   int claude = !strcmp(execution->selected.cli_kind, "claude");
   server_native_tool_binding_t *bindings =
       profile->n_tools ? calloc((size_t)profile->n_tools, sizeof *bindings) : NULL;
   if (profile->n_tools && !bindings)
      return -1;
   int rc = -1;
   for (int i = 0; i < profile->n_tools; i++)
   {
      aimee_tool_t *tool = profile->tools + i;
      if (tool->kind != AIMEE_TOOL_FUNCTION)
         goto done;
      bindings[i].registry_name = strdup(tool->name);
      if (!bindings[i].registry_name)
         goto done;
      if (claude)
      {
         size_t size = strlen(tool->name) + sizeof "mcp__aimee__";
         char *alias = malloc(size);
         if (!alias)
            goto done;
         snprintf(alias, size, "mcp__aimee__%s", tool->name);
         free(tool->name);
         tool->name = alias;
         bindings[i].mcp_name = bindings[i].registry_name;
      }
      bindings[i].name = tool->name;
   }
   if (owner->vault_transport == ATTEST_NONE &&
       server_native_delegate_source_runtime_prepare(cfg->role, cfg->agent, &execution->selected,
                                                     cfg->canonical_request, profile,
                                                     execution->source_session))
      goto done;
   execution->runtime = server_native_runtime_open_bound_request(
       &execution->selected, owner, execution->source_session, profile, bindings, profile->n_tools);
   rc = execution->runtime ? 0 : -1;
done:
   for (int i = 0; i < profile->n_tools; i++)
      free((void *)bindings[i].registry_name);
   free(bindings);
   return rc;
}
static int native_cache_ready(native_execution_t *execution, const char *launch_digest,
                              cli_codex_host_replay_t *replay)
{
   const char *sdk = execution->selected.cli_kind;
   server_native_execution_profile_t profile = {.sdk = sdk,
                                                .build_digest = execution->build,
                                                .launch_digest = launch_digest,
                                                .workspace_digest = execution->workspace};
   char scope[65];
   if (execution->cache || server_native_runtime_cache_scope(execution->runtime, &profile, scope))
      return -1;
   server_native_cache_owner_t owner = {
       .binding = server_native_runtime_binding(execution->runtime),
       .owner = server_native_cache_lease_owner(execution->lease),
       .containment = native_string(execution->reservation, "containment_sha256"),
       .publish = native_claim_publish,
       .context = execution};
   execution->cache = server_native_cache_begin_epoch(
       server_session_capture_session(execution->capture), execution->actor,
       server_session_capture_turn(execution->capture), sdk, scope, &execution->profile,
       execution->epoch, execution->reservation ? &owner : NULL);
   if (!execution->cache || aimee_ir_request_clone(&execution->profile, &execution->adoption))
      return -1;
   int offset = server_native_cache_history_offset(execution->cache);
   if (offset < 0 || offset > execution->adoption.n_messages)
      return -1;
   for (int i = 0; i < offset; i++)
      aimee_message_free(execution->adoption.messages + i);
   execution->adoption.n_messages -= offset;
   if (execution->adoption.n_messages)
      memmove(execution->adoption.messages, execution->adoption.messages + offset,
              (size_t)execution->adoption.n_messages * sizeof *execution->adoption.messages);
   replay->history = &execution->adoption;
   replay->resume_id = server_native_cache_resume(execution->cache);
   return 0;
}
static int native_ready(const char *digest, cli_codex_host_replay_t *replay, void *context)
{
   native_execution_t *execution = context;
   return server_native_runtime_attest_launch(execution->runtime, digest)
              ? -1
              : native_cache_ready(execution, digest, replay);
}
static int native_bind(const char *id, void *context)
{
   native_execution_t *execution = context;
   if (!id || !id[0] || strlen(id) >= sizeof execution->native_id || execution->native_id[0])
      return -1;
   const char *resume = server_native_cache_resume(execution->cache);
   if (resume && resume[0] && strcmp(resume, id))
      return -1;
   snprintf(execution->native_id, sizeof execution->native_id, "%s", id);
   return 0;
}
static native_execution_t *native_execution_open(const provider_cli_cfg_t *cfg, char error[512])
{
   const char *stage = "selected model identity";
   native_execution_t *execution = calloc(1, sizeof *execution);
   if (!execution)
      return NULL;
   execution->executable_fd = execution->scheduler_fd = execution->workspace_fd = -1;
   if (!cfg || !cfg->agent)
      goto refused;
   execution->selected = *cfg->agent;
   const char *adapter_name = NULL;
   if (!strcmp(execution->selected.cli_kind, "claude") &&
       (!strcmp(execution->selected.provider, "claude") ||
        !strcmp(execution->selected.provider, "claude-code")))
      adapter_name = "anthropic";
   else if (!strcmp(execution->selected.auth_type, "codex-oauth") &&
            (!execution->selected.provider[0] || !strcmp(execution->selected.provider, "codex") ||
             !strcmp(execution->selected.provider, "codex-oauth") ||
             !strcmp(execution->selected.provider, "codex-cli")))
      adapter_name = "codex";
   if (adapter_name)
   {
      const agent_adapter_t *adapter = agent_adapter_for_name(adapter_name);
      if (!adapter)
         goto refused;
      snprintf(execution->selected.provider, sizeof execution->selected.provider, "%s",
               adapter->provider);
      if (!execution->selected.endpoint[0])
         snprintf(execution->selected.endpoint, sizeof execution->selected.endpoint, "%s",
                  adapter->default_endpoint);
   }
   const agent_t *selected = &execution->selected;
   const request_context_t *owner = request_context_get();
   agent_request_creds_t credentials;
   agent_request_creds_snapshot(&credentials);
   size_t session_length = strnlen(credentials.session_id, sizeof credentials.session_id);
   size_t actor_length = strnlen(credentials.vault_principal, sizeof credentials.vault_principal);
   int bounded_identity = session_length && session_length < sizeof execution->source_session &&
                          actor_length && actor_length < sizeof execution->actor;
   if (bounded_identity)
   {
      memcpy(execution->source_session, credentials.session_id, session_length + 1);
      memcpy(execution->actor, credentials.vault_principal, actor_length + 1);
   }
   OPENSSL_cleanse(&credentials, sizeof credentials);
   if (!bounded_identity)
      goto refused;
   stage = "canonical source ownership";
   const char *cwd = run_cmd_get_cwd();
   if (!owner || !execution->source_session[0] ||
       strcmp(owner->vault_principal, execution->actor) ||
       server_session_capture_retain_current(execution->source_session, execution->actor,
                                             &execution->capture) ||
       !execution->capture)
      goto refused;
   stage = "prior native ownership reconciliation";
   if (server_native_session_reconcile(server_session_capture_session(execution->capture),
                                       execution->actor,
                                       server_session_capture_turn(execution->capture), error))
      goto refused;
   stage = "approved workspace custody";
   if (workspace_turn_resource_identity(cwd, &execution->workspace_fd, execution->workspace))
      goto refused;
   stage = "canonical model profile";
   agent_tools_scope_bookmark(&execution->previous_tools);
   execution->tools_bound = 1;
   agent_tools_set_dispatch_role(cfg->role);
   cJSON *history = NULL, *empty = cJSON_CreateArray();
   aimee_wire_t wire = is_anthropic_provider(selected) ? AIMEE_WIRE_ANTHROPIC
                       : is_chatgpt_provider(selected) ? AIMEE_WIRE_RESPONSES
                                                       : AIMEE_WIRE_OPENAI_CHAT;
   cJSON *tools = cfg->no_tools                  ? cJSON_CreateArray()
                  : wire == AIMEE_WIRE_ANTHROPIC ? build_tools_array_anthropic()
                  : wire == AIMEE_WIRE_RESPONSES ? build_tools_array_responses()
                                                 : build_tools_array();
   if (tools)
   {
      if (wire == AIMEE_WIRE_ANTHROPIC)
         agent_tools_filter_anthropic_for_role(tools, cfg->role);
      else
         agent_tools_filter_for_role(tools, cfg->role);
   }
   char *system =
       agent_build_exec_context_checked(selected, cfg->network, cfg->role, cfg->system_prompt,
                                        !agent_tools_knowledge_write_allowed(), error, 512);
   const aimee_request_t *seed = cfg->canonical_request;
   int prepared = empty && tools && system && (!seed || !aimee_ir_request_admit(seed)) &&
                  !agent_prepare_request_ir(selected, empty, tools, system,
                                            seed && seed->has_max_tokens ? seed->max_tokens : 0,
                                            seed && seed->has_temperature ? seed->temperature : -1,
                                            wire, cfg->role != NULL, &execution->profile);
   free(system);
   cJSON_Delete(empty);
   cJSON_Delete(tools);
   if (prepared && cfg->reasoning_effort &&
       aimee_ir_effort_parse(cfg->reasoning_effort, &execution->profile.reasoning_effort))
      prepared = 0;
   if (!prepared)
      goto refused;
   const char *sdk = execution->selected.cli_kind;
   stage = "SDK executable";
   execution->executable_fd = server_native_launch_open(
       sdk, cfg->agent->cli_cmd[0] ? cfg->agent->cli_cmd : sdk, execution->build, error);
   if (execution->executable_fd < 0)
      goto refused;
   stage = "owned SDK identity";
   if (server_native_sdk_profile_prepare(&execution->profile, sdk, execution->executable_fd))
      goto refused;
   stage = "native runtime preparation";
   if (native_registry_open(execution, owner, cfg))
      goto refused;
   if (server_native_runtime_set_turn_limit(execution->runtime, cfg->native_turn_limit))
      goto refused;
   stage = "native cache custody";
   if (native_custody_open(execution, error))
      goto refused;
   stage = "authenticated canonical history";
   cJSON *receipts = NULL;
   prepared = !server_native_history_load(
       server_session_capture_session(execution->capture), execution->actor,
       server_session_capture_turn(execution->capture), 0, &history, &receipts);
   cJSON_Delete(receipts);
   if (prepared)
   {
      for (int i = 0; i < execution->profile.n_messages; i++)
         aimee_message_free(&execution->profile.messages[i]);
      free(execution->profile.messages);
      execution->profile.messages = NULL;
      execution->profile.n_messages = 0;
      int count = cJSON_GetArraySize(history);
      execution->profile.messages =
          count ? calloc((size_t)count, sizeof *execution->profile.messages) : NULL;
      prepared = !count || execution->profile.messages != NULL;
      for (int i = 0; i < count && prepared; i++)
      {
         execution->profile.n_messages++;
         prepared = !aimee_ir_message_parse(cJSON_GetArrayItem(history, i),
                                            &execution->profile.messages[i]);
      }
   }
   cJSON_Delete(history);
   if (!prepared)
      goto refused;
   stage = "cache lease";
   if (!execution->lease)
      goto refused;
   stage = "ambient SDK configuration";
   if (!native_ambient_config_absent(sdk))
      goto refused;
   stage = "nonsecret SDK identity";
   if (!strcmp(sdk, "claude") &&
       server_native_cache_sdk_identity(execution->lease, execution->sdk_identity))
      goto refused;
   /* Seed Claude's project-scoped configuration for the actual held SDK cwd,
    * never for the distinct operator workspace used by host-owned tools. */
   stage = "private SDK workspace";
   if (!strcmp(sdk, "claude"))
   {
      execution->scheduler_fd = server_native_cache_workspace_open(
          execution->lease, execution->epoch, execution->cwd, sizeof execution->cwd);
      if (execution->scheduler_fd < 0)
         goto refused;
   }
   stage = "private SDK HOME";
   if (cli_session_native_home(sdk, !strcmp(sdk, "claude") ? execution->cwd : cwd,
                               server_session_capture_session(execution->capture),
                               execution->effective_binding,
                               !strcmp(sdk, "claude") ? execution->sdk_identity : NULL,
                               execution->home, sizeof execution->home, &execution->home_cleanup))
      goto refused;
   stage = "owned SDK configuration";
   if (!strcmp(sdk, "codex") ? native_write_config(execution, selected)
                             : native_prepare_directory(execution) || native_environment(execution))
      goto refused;
   if (!strcmp(sdk, "claude") && execution->containment &&
       server_native_containment_bind_home(execution->containment, execution->home))
      goto refused;
   execution->launch = (cli_codex_host_launch_t){
       .executable_fd = execution->executable_fd,
       .cwd_fd = execution->scheduler_fd,
       .home = execution->home,
       .cwd = execution->cwd,
       .build_digest = execution->build,
       .gateway_url = server_native_runtime_url(execution->runtime),
       .gateway_token = server_native_runtime_token(execution->runtime),
       .environment = execution->environment,
       .expected_config = execution->config,
       .spawn_guard = server_native_containment_guard(execution->containment),
       .ready = native_ready,
       .context = execution};
   return execution;
refused:
   if (!error[0])
      snprintf(error, 512, "native SDK launch refused during %s", stage);
   native_execution_close(execution);
   return NULL;
}
static int native_execution_result(native_execution_t *execution, agent_result_t *out,
                                   aimee_response_t *canonical)
{
   if (execution->containment && server_native_containment_quiesce(execution->containment, 1000))
   {
      server_native_runtime_deactivate_hold_port(execution->runtime);
      return -1;
   }
   server_native_runtime_quiesce(execution->runtime);
   aimee_response_t original = {0};
   cJSON *history = NULL;
   int valid = execution->cache && execution->native_id[0] &&
               !server_session_capture_history(execution->capture, &history) &&
               !server_native_history_admit_source(
                   server_session_capture_session(execution->capture), execution->actor,
                   server_session_capture_turn(execution->capture), 1, history) &&
               !server_native_runtime_result(execution->runtime, &original) &&
               !aimee_ir_response_complete_valid(&original);
   cJSON_Delete(history);
   char *text = valid ? aimee_ir_response_text_alloc(&original) : NULL;
   if (!text || native_terminal_transfer(execution))
   {
      free(text);
      aimee_response_free(&original);
      return -1;
   }
   agent_result_clear_response(out);
   out->response = text;
   out->prompt_tokens = (int)original.usage_in;
   out->completion_tokens = (int)original.usage_out;
   out->cache_read_tokens = (int)original.usage_cache_read;
   out->cache_write_tokens = (int)original.usage_cache_write;
   snprintf(out->stop_reason, sizeof out->stop_reason, "%s",
            original.raw_stop_reason ? original.raw_stop_reason
                                     : aimee_stop_reason_name(original.stop_reason));
   aimee_response_free(&out->canonical_response);
   out->canonical_response = original;
   original = (aimee_response_t){0};
   if (canonical && aimee_ir_response_clone(&out->canonical_response, canonical))
      return -1;
   return 0;
}
/* A failed execution may still have a durably observed provider response.
 * Preserve that original IR as diagnostics without transferring a successful
 * cache or trusting the SDK's normalized stdout/display. */
static void native_execution_diagnostic(native_execution_t *execution, agent_result_t *out,
                                        aimee_response_t *canonical)
{
   agent_result_clear_response(out);
   out->success = 0;
   if (!execution->runtime)
      return;
   if (execution->containment && server_native_containment_quiesce(execution->containment, 1000))
   {
      server_native_runtime_deactivate_hold_port(execution->runtime);
      return;
   }
   server_native_runtime_quiesce(execution->runtime);
   aimee_response_t original = {0};
   if (server_native_runtime_result(execution->runtime, &original))
      return;
   if (canonical && canonical != &out->canonical_response &&
       aimee_ir_response_clone(&original, canonical))
   {
      aimee_response_free(&original);
      return;
   }
   out->canonical_response = original;
   out->response = aimee_ir_response_text_alloc(&original);
   out->prompt_tokens = original.usage_in > INT_MAX ? INT_MAX : (int)original.usage_in;
   out->completion_tokens = original.usage_out > INT_MAX ? INT_MAX : (int)original.usage_out;
   out->cache_read_tokens =
       original.usage_cache_read > INT_MAX ? INT_MAX : (int)original.usage_cache_read;
   out->cache_write_tokens =
       original.usage_cache_write > INT_MAX ? INT_MAX : (int)original.usage_cache_write;
}
static int native_claude_init(const cJSON *frame, void *context)
{
   native_execution_t *execution = context;
   const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "session_id"));
   const char *resume = server_native_cache_resume(execution->cache);
   char proof[65];
   const char *reason = NULL;
   if (!server_native_cache_workspace_valid(execution->lease, execution->epoch,
                                            execution->scheduler_fd, execution->cwd))
      reason = "private workspace custody";
   else if (!id || !id[0] || strlen(id) >= sizeof execution->native_id)
      reason = "session identity";
   else if (resume && resume[0] && strcmp(resume, id))
      reason = "resume identity";
   else if (cli_claude_native_launch_digest(&execution->launching, proof))
      reason = "launch digest computation";
   else if (strcmp(proof, execution->claude_digest))
      reason = "launch digest mismatch";
   else if (server_native_runtime_attest_launch(execution->runtime, proof))
      reason = "runtime attestation";
   if (reason)
      aimee_log(LOG_ERROR, "native.claude", "initialization refused: %s", reason);
   return reason ? -1 : 0;
}
static int native_claude_execute(native_execution_t *execution, const provider_cli_cfg_t *cfg,
                                 agent_result_t *out)
{
   cJSON *config = cJSON_CreateObject();
   cJSON *servers = config ? cJSON_AddObjectToObject(config, "mcpServers") : NULL;
   int valid = servers != NULL;
   if (valid && execution->profile.n_tools)
   {
      cJSON *server = cJSON_AddObjectToObject(servers, "aimee");
      cJSON *headers = server ? cJSON_AddObjectToObject(server, "headers") : NULL;
      char url[512], authorization[512];
      valid =
          server && headers &&
          snprintf(url, sizeof url, "%s/v1/mcp/call",
                   server_native_runtime_url(execution->runtime)) < (int)sizeof url &&
          snprintf(authorization, sizeof authorization, "Bearer %s",
                   server_native_runtime_token(execution->runtime)) < (int)sizeof authorization &&
          cJSON_AddStringToObject(server, "type", "http") &&
          cJSON_AddStringToObject(server, "url", url) &&
          cJSON_AddStringToObject(headers, "Authorization", authorization);
   }
   execution->mcp_config = valid ? cJSON_PrintUnformatted(config) : NULL;
   cJSON_Delete(config);
   if (!execution->mcp_config)
      return -1;
   execution->claude = (cli_claude_host_launch_t){
       .executable_fd = execution->executable_fd,
       .cwd_fd = execution->scheduler_fd,
       .home = execution->home,
       .sdk_identity = execution->sdk_identity,
       .cwd = execution->cwd,
       .mcp_config = execution->mcp_config,
       .gateway_url = server_native_runtime_url(execution->runtime),
       .gateway_token = server_native_runtime_token(execution->runtime),
       .environment = execution->environment,
       .catalog = &execution->profile,
       .spawn_guard = server_native_containment_guard(execution->containment),
       .init = native_claude_init,
       .context = execution,
       .progress = cfg->native_progress,
       .progress_context = cfg->native_progress_context};
   execution->launching = *cfg;
   provider_cli_cfg_t *owned = &execution->launching;
   owned->agent = &execution->selected;
   owned->cwd = execution->cwd;
   owned->home = NULL;
   owned->claude_host = &execution->claude;
   owned->codex_host = NULL;
   /* SDK user/slash syntax is not an ingress. The original task and complete
    * history are already owned by the model gateway's canonical request. */
   owned->user_prompt = "Continue the server-owned canonical turn.";
   owned->system_prompt = NULL;
   owned->history = NULL;
   owned->resume_id = NULL;
   owned->tool_catalog = NULL;
   owned->tool_call = NULL;
   owned->tool_context = NULL;
   owned->bind_session = native_bind;
   owned->bind_context = execution;
   if (cli_claude_native_launch_digest(owned, execution->claude_digest))
      return -1;
   execution->claude.expected_profile = execution->claude_digest;
   cli_codex_host_replay_t replay = {0};
   if (native_cache_ready(execution, execution->claude_digest, &replay))
      return -1;
   owned->resume_id = replay.resume_id;
   if (!server_native_cache_workspace_valid(execution->lease, execution->epoch,
                                            execution->scheduler_fd, execution->cwd))
      return -1;
   return claude_headless_run(owned, native_bind, execution, out);
}

int provider_cli_adapter_host_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   if (!out)
      return -1;
   if (!cfg || !cfg->agent)
      return 1;
   memset(out, 0, sizeof *out);
   if (strcmp(cfg->agent->cli_kind, "codex") && strcmp(cfg->agent->cli_kind, "claude"))
   {
      snprintf(out->error, sizeof out->error,
               "canonical native execution owner unavailable for configured adapter");
      return AGENT_RC_CONTEXT_REFUSED;
   }
   native_execution_t *execution = native_execution_open(cfg, out->error);
   if (!execution)
      goto refused;
   provider_cli_cfg_t owned = *cfg;
   owned.cwd = execution->cwd;
   owned.home = NULL;
   owned.user_prompt = "Continue the server-owned canonical turn.";
   owned.system_prompt = NULL;
   owned.codex_host = &execution->launch;
   owned.history = NULL;
   owned.resume_id = NULL;
   owned.tool_catalog = &execution->profile;
   owned.tool_call = server_native_runtime_execute_tool;
   owned.tool_context = execution->runtime;
   owned.bind_session = native_bind;
   owned.bind_context = execution;
   int rc = !strcmp(execution->selected.cli_kind, "claude")
                ? native_claude_execute(execution, &owned, out)
                : codex_provider_cli_adapter.execute(&owned, out);
   if (server_native_runtime_turn_limit_reached(execution->runtime))
   {
      server_native_runtime_quiesce(execution->runtime);
      aimee_response_t capped = {0};
      if (!server_native_runtime_result(execution->runtime, &capped))
      {
         if (cfg->canonical_response)
         {
            aimee_response_free(cfg->canonical_response);
            *cfg->canonical_response = capped;
            capped = (aimee_response_t){0};
         }
         aimee_response_free(&capped);
      }
      snprintf(out->error, sizeof out->error, "native model generation turn cap reached");
      snprintf(out->stop_reason, sizeof out->stop_reason, "turn_cap");
      out->turns = server_native_runtime_model_turns(execution->runtime);
      out->success = 0;
      rc = AGENT_RC_CONTEXT_REFUSED;
   }
   if (!rc && out->success)
      rc = native_execution_result(execution, out, cfg->canonical_response);
   if (rc || !out->success)
      native_execution_diagnostic(execution, out, cfg->canonical_response);
   native_execution_close(execution);
   if (!rc && out->success)
      return 0;
refused:
   out->success = 0;
   aimee_ir_session_fail_capture();
   if (strcmp(out->stop_reason, "turn_cap"))
      snprintf(out->stop_reason, sizeof out->stop_reason, "capture_failed");
   if (!out->error[0])
      snprintf(out->error, sizeof out->error, "canonical native execution was refused");
   return AGENT_RC_CAPTURE_FAILED;
}
