/* Credential-free SDK scheduler resources. The durable capture and existing
 * native-cache owner, not SDK files or process exit, own conversation truth. */
#include "server_native_execution.h"
#include "server_native_launch.h"
#include "cli_claude_native.h"
#include "claude_headless.h"
#include "server_native_runtime.h"
#include "server_native_cache.h"
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

/* A known-quiescent child transfers only derived cache metadata to the turn.
 * No listener, credential, process or browser pointer survives in this resource. */
typedef struct
{
   server_native_cache_t *cache;
   char native_id[128];
} native_terminal_cache_t;
typedef struct
{
   server_session_capture_t *capture;
   agent_t selected;
   server_native_runtime_t *runtime;
   server_native_cache_t *cache;
   aimee_request_t profile, adoption;
   cli_session_t home_cleanup;
   cli_codex_host_launch_t launch;
   cli_claude_host_launch_t claude;
   provider_cli_cfg_t launching;
   char *mcp_config;
   char claude_digest[65];
   char home[4096], cwd[4096], build[65], workspace[65];
   int executable_fd, scheduler_fd, workspace_fd;
   cJSON *config;
   char *environment[16];
   char native_id[128];
   char source_session[128], actor[129];
   agent_tools_scope_snapshot_t previous_tools;
   int tools_bound;
} native_execution_t;

static void native_cache_terminal(int success, void *context)
{
   native_terminal_cache_t *resource = context;
   if (!success || server_native_cache_complete(resource->cache, resource->native_id))
   {
      if (server_native_cache_abort(resource->cache))
         aimee_log(LOG_WARN, "native-cache", "quiescent cache claim remains unresolved");
   }
   server_native_cache_close(resource->cache);
   free(resource);
}
static void native_execution_close(native_execution_t *execution)
{
   if (!execution)
      return;
   server_native_runtime_close(execution->runtime);
   if (execution->cache)
   {
      (void)server_native_cache_abort(execution->cache);
      server_native_cache_close(execution->cache);
   }
   if (execution->executable_fd >= 0)
      close(execution->executable_fd);
   if (execution->scheduler_fd >= 0)
      close(execution->scheduler_fd);
   if (execution->workspace_fd >= 0)
      close(execution->workspace_fd);
   cli_session_destroy(&execution->home_cleanup);
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
      const char *extra[] = {key, url, "DISABLE_AUTOUPDATER=1",
                             "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1"};
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
   if (snprintf(execution->cwd, sizeof execution->cwd, "%s/workspace", execution->home) >=
           (int)sizeof execution->cwd ||
       mkdir(execution->cwd, 0700))
      return -1;
   execution->scheduler_fd = open(execution->cwd, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
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
static int native_registry_open(native_execution_t *execution, const request_context_t *owner)
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
   execution->cache = server_native_cache_begin(
       server_session_capture_session(execution->capture), execution->actor,
       server_session_capture_turn(execution->capture), sdk, scope, &execution->profile);
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
   native_execution_t *execution = calloc(1, sizeof *execution);
   if (!execution)
      return NULL;
   execution->executable_fd = execution->scheduler_fd = execution->workspace_fd = -1;
   if (!cfg || !cfg->agent)
      goto refused;
   execution->selected = *cfg->agent;
   if (!strcmp(execution->selected.auth_type, "codex-oauth") &&
       (!execution->selected.provider[0] || !strcmp(execution->selected.provider, "codex") ||
        !strcmp(execution->selected.provider, "codex-oauth") ||
        !strcmp(execution->selected.provider, "codex-cli")))
   {
      const agent_adapter_t *adapter = agent_adapter_for_name("codex");
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
   snprintf(execution->source_session, sizeof execution->source_session, "%s",
            credentials.session_id);
   snprintf(execution->actor, sizeof execution->actor, "%s", credentials.vault_principal);
   OPENSSL_cleanse(&credentials, sizeof credentials);
   const char *cwd = run_cmd_get_cwd();
   if (!owner || !execution->source_session[0] ||
       strcmp(owner->vault_principal, execution->actor) ||
       server_session_capture_retain_current(execution->source_session, execution->actor,
                                             &execution->capture) ||
       !execution->capture ||
       workspace_turn_resource_identity(cwd, &execution->workspace_fd, execution->workspace))
      goto refused;
   agent_tools_scope_bookmark(&execution->previous_tools);
   execution->tools_bound = 1;
   agent_tools_set_dispatch_role(cfg->role);
   cJSON *history = NULL, *empty = cJSON_CreateArray();
   char *stored = NULL;
   int status = db1_session_ir_history_load(
       server_session_capture_session(execution->capture), "primary", execution->actor,
       server_session_capture_turn(execution->capture), &stored);
   history = status == 1             ? cJSON_CreateArray()
             : status == 0 && stored ? aimee_ir_json_parse_frame(stored, strlen(stored))
                                     : NULL;
   free(stored);
   aimee_wire_t wire = is_anthropic_provider(selected) ? AIMEE_WIRE_ANTHROPIC
                       : is_chatgpt_provider(selected) ? AIMEE_WIRE_RESPONSES
                                                       : AIMEE_WIRE_OPENAI_CHAT;
   cJSON *tools = wire == AIMEE_WIRE_ANTHROPIC   ? build_tools_array_anthropic()
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
   int prepared = empty && history && !aimee_ir_history_admit(history) && tools && system &&
                  (!seed || !aimee_ir_request_admit(seed)) &&
                  !agent_prepare_request_ir(selected, empty, tools, system,
                                            seed && seed->has_max_tokens ? seed->max_tokens : 0,
                                            seed && seed->has_temperature ? seed->temperature : -1,
                                            wire, cfg->role != NULL, &execution->profile);
   free(system);
   cJSON_Delete(empty);
   cJSON_Delete(tools);
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
   if (prepared && cfg->reasoning_effort &&
       aimee_ir_effort_parse(cfg->reasoning_effort, &execution->profile.reasoning_effort))
      prepared = 0;
   if (!prepared || native_registry_open(execution, owner))
      goto refused;
   const char *sdk = execution->selected.cli_kind;
   if (!native_ambient_config_absent(sdk) ||
       cli_session_native_home(sdk, cwd, server_session_capture_session(execution->capture),
                               server_native_runtime_binding(execution->runtime), execution->home,
                               sizeof execution->home, &execution->home_cleanup) ||
       (execution->executable_fd =
            server_native_launch_open(sdk, cfg->agent->cli_cmd[0] ? cfg->agent->cli_cmd : sdk,
                                      execution->build, error)) < 0 ||
       (!strcmp(sdk, "codex")
            ? native_write_config(execution, selected)
            : native_prepare_directory(execution) || native_environment(execution)))
      goto refused;
   execution->launch =
       (cli_codex_host_launch_t){.executable_fd = execution->executable_fd,
                                 .cwd_fd = execution->scheduler_fd,
                                 .home = execution->home,
                                 .cwd = execution->cwd,
                                 .build_digest = execution->build,
                                 .gateway_url = server_native_runtime_url(execution->runtime),
                                 .gateway_token = server_native_runtime_token(execution->runtime),
                                 .environment = execution->environment,
                                 .expected_config = execution->config,
                                 .ready = native_ready,
                                 .context = execution};
   return execution;
refused:
   if (!error[0])
      snprintf(error, 512, "could not attest canonical native SDK resources");
   native_execution_close(execution);
   return NULL;
}
static int native_execution_result(native_execution_t *execution, agent_result_t *out,
                                   aimee_response_t *canonical)
{
   server_native_runtime_quiesce(execution->runtime);
   aimee_response_t original = {0};
   cJSON *history = NULL;
   int valid = execution->cache && execution->native_id[0] &&
               !server_session_capture_history(execution->capture, &history) &&
               !aimee_ir_history_admit(history) &&
               !server_native_runtime_result(execution->runtime, &original) &&
               original.stop_reason != AIMEE_STOP_TOOL_USE &&
               original.stop_reason != AIMEE_STOP_ERROR && !original.error_message;
   cJSON_Delete(history);
   char *text = valid ? aimee_ir_response_text_alloc(&original) : NULL;
   native_terminal_cache_t *resource = text ? calloc(1, sizeof *resource) : NULL;
   if (!text || !resource)
   {
      free(text);
      free(resource);
      aimee_response_free(&original);
      return -1;
   }
   resource->cache = execution->cache;
   snprintf(resource->native_id, sizeof resource->native_id, "%s", execution->native_id);
   if (server_session_capture_after_terminal(execution->capture, native_cache_terminal, resource))
   {
      free(text);
      free(resource);
      aimee_response_free(&original);
      return -1;
   }
   execution->cache = NULL;
   free(out->response);
   out->response = text;
   out->prompt_tokens = (int)original.usage_in;
   out->completion_tokens = (int)original.usage_out;
   out->cache_read_tokens = (int)original.usage_cache_read;
   out->cache_write_tokens = (int)original.usage_cache_write;
   snprintf(out->stop_reason, sizeof out->stop_reason, "%s",
            original.raw_stop_reason ? original.raw_stop_reason
                                     : aimee_stop_reason_name(original.stop_reason));
   if (canonical)
   {
      *canonical = original;
      original = (aimee_response_t){0};
   }
   aimee_response_free(&original);
   return 0;
}
static int native_claude_init(const cJSON *frame, void *context)
{
   native_execution_t *execution = context;
   const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "session_id"));
   const char *resume = server_native_cache_resume(execution->cache);
   char proof[65];
   if (!id || !id[0] || strlen(id) >= sizeof execution->native_id ||
       (resume && resume[0] && strcmp(resume, id)) ||
       cli_claude_native_launch_digest(&execution->launching, proof) ||
       strcmp(proof, execution->claude_digest))
      return -1;
   return server_native_runtime_attest_launch(execution->runtime, proof);
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
   execution->claude =
       (cli_claude_host_launch_t){.executable_fd = execution->executable_fd,
                                  .cwd_fd = execution->scheduler_fd,
                                  .home = execution->home,
                                  .cwd = execution->cwd,
                                  .mcp_config = execution->mcp_config,
                                  .gateway_url = server_native_runtime_url(execution->runtime),
                                  .gateway_token = server_native_runtime_token(execution->runtime),
                                  .environment = execution->environment,
                                  .catalog = &execution->profile,
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
   return claude_headless_run(owned, native_bind, execution, out);
}

int provider_cli_adapter_host_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   if (!out)
      return -1;
   if (!cfg || !cfg->agent ||
       (strcmp(cfg->agent->cli_kind, "codex") && strcmp(cfg->agent->cli_kind, "claude")))
      return 1;
   memset(out, 0, sizeof *out);
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
   if (!rc && out->success)
      rc = native_execution_result(execution, out, cfg->canonical_response);
   native_execution_close(execution);
   if (!rc && out->success)
      return 0;
refused:
   out->success = 0;
   aimee_ir_session_fail_capture();
   snprintf(out->stop_reason, sizeof out->stop_reason, "capture_failed");
   if (!out->error[0])
      snprintf(out->error, sizeof out->error, "canonical native execution was refused");
   return AGENT_RC_CAPTURE_FAILED;
}
int server_native_codex_chat_stream(const agent_t *selected,
                                    const cli_codex_chat_request_t *request,
                                    cli_codex_chat_event_cb callback, void *context,
                                    cli_codex_chat_result_t *out)
{
   if (!selected || !request || !out)
      return -1;
   memset(out, 0, sizeof *out);
   provider_cli_cfg_t cfg = {.agent = selected,
                             .network = request->network,
                             .reasoning_effort = request->reasoning_effort,
                             .cwd = run_cmd_get_cwd(),
                             .system_prompt = request->system_prompt,
                             .user_prompt = request->user_prompt};
   char error[512] = "";
   native_execution_t *execution = native_execution_open(&cfg, error);
   if (!execution)
      goto refused;
   cli_codex_chat_request_t owned = *request;
   owned.home = NULL;
   owned.cwd = execution->cwd;
   owned.user_prompt = "Continue the server-owned canonical turn.";
   owned.system_prompt = NULL;
   owned.host_launch = &execution->launch;
   owned.thread_id = NULL;
   owned.history = NULL;
   owned.tool_catalog = &execution->profile;
   owned.tool_call = server_native_runtime_execute_tool;
   owned.tool_context = execution->runtime;
   int rc = cli_codex_chat_stream(&owned, callback, context, out);
   if (!rc)
   {
      rc = native_bind(out->thread_id, execution);
      agent_result_t result = {0};
      if (!rc)
         rc = native_execution_result(execution, &result, NULL);
      free(result.response);
      if (!rc)
      {
         out->prompt_tokens = result.prompt_tokens;
         out->completion_tokens = result.completion_tokens;
      }
   }
   native_execution_close(execution);
   if (!rc)
      return 0;
refused:
   aimee_ir_session_fail_capture();
   if (out && !out->error[0])
      snprintf(out->error, sizeof out->error, "%s",
               error[0] ? error : "canonical native execution was refused");
   return -1;
}
