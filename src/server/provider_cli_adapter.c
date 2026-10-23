/* provider_cli_adapter.c: shared provider-CLI delegate adapter registry. */
#include "provider_cli_adapter.h"
#include "agent_exec.h"
#include <aimee/ir/session_event.h>

#include "aimee.h"
#include "cJSON.h"
#include "aimee_home.h"                  /* aimee_home() — the delegate's /v1 socket path */
#include "modules/git/git_cred_inject.h" /* git_cred_forge_configured — no aimee route, no strip */
#include "agent_config.h"                /* agent_api_key_secret: resolve at use, not at load */
#include "runtime_secret.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Defined in server_compute.c; called post-fork to seed the child's delegate
 * context env from the forking thread's TLS (race-free). */
void delegate_child_export_context_env(void);

/* Non-server adapters/fixtures have no host capture or Vault authority. */
__attribute__((weak)) int provider_cli_adapter_host_execute(const provider_cli_cfg_t *cfg,
                                                            agent_result_t *out)
{
   (void)cfg;
   (void)out;
   return 1;
}

#define PROVIDER_CLI_MAX_ARGS 48

void provider_cli_free_tokens(char **tokens, int count)
{
   for (int i = 0; i < count; i++)
      free(tokens[i]);
}

static int provider_cli_resolve_native_env_key(const char *name, char *dst, size_t dst_len)
{
   if (!name || !name[0] || !dst || dst_len == 0)
      return 0;
   /* The name remains environment-shaped for provider compatibility, but the
    * value was sealed during first boot and is available only through the
    * process-local Vault cache. A ~/.vibe/.env fallback would reintroduce a
    * plaintext credential file on aimee-server and is intentionally forbidden. */
   return runtime_secret_get(name, dst, dst_len);
}

static void provider_cli_append_header(char *headers, size_t headers_len, const char *header)
{
   if (!headers || headers_len == 0 || !header || !header[0])
      return;
   if (strstr(headers, header))
      return;
   size_t used = strlen(headers);
   if (used > 0 && used + 1 < headers_len)
      snprintf(headers + used, headers_len - used, "\n");
   used = strlen(headers);
   if (used < headers_len)
      snprintf(headers + used, headers_len - used, "%s", header);
}

int provider_cli_adapter_prepare_native_agent(const provider_cli_adapter_t *adapter,
                                              const agent_t *agent, agent_t *native_agent,
                                              char *errbuf, size_t errbuf_len)
{
   if (errbuf && errbuf_len > 0)
      errbuf[0] = '\0';
   if (!adapter || !agent || !native_agent || !adapter->native_provider ||
       !adapter->native_provider[0])
   {
      if (errbuf && errbuf_len > 0)
         snprintf(errbuf, errbuf_len, "provider adapter has no native provider");
      return -1;
   }

   memcpy(native_agent, agent, sizeof(*native_agent));
   snprintf(native_agent->provider, sizeof(native_agent->provider), "%s", adapter->native_provider);
   native_agent->backend[0] = '\0';
   native_agent->cli_kind[0] = '\0';
   native_agent->cli_cmd[0] = '\0';
   native_agent->cli_idle_timeout_ms = 0;
   native_agent->session_reuse = 0;

   if (!native_agent->endpoint[0] && adapter->native_default_endpoint)
      snprintf(native_agent->endpoint, sizeof(native_agent->endpoint), "%s",
               adapter->native_default_endpoint);
   if (!native_agent->model[0] && adapter->native_default_model)
      snprintf(native_agent->model, sizeof(native_agent->model), "%s",
               adapter->native_default_model);
   if (!native_agent->auth_type[0])
      snprintf(native_agent->auth_type, sizeof(native_agent->auth_type), "%s", "bearer");
   if (native_agent->auth_cmd[0] && strcmp(native_agent->auth_type, "bearer") == 0)
      snprintf(native_agent->auth_type, sizeof(native_agent->auth_type), "%s", "oauth");

   if (strcmp(adapter->native_provider, "gemini") == 0 && !native_agent->auth_cmd[0])
   {
      char key[MAX_API_KEY_LEN] = {0};
      /* api_key holds what agents.json holds -- possibly a "$VAR" reference --
       * so resolve it rather than copying the field. */
      if (agent_api_key_secret(native_agent, key, sizeof(key)))
         native_agent->api_key[0] = '\0';
      else if (adapter->native_api_key_env)
         (void)provider_cli_resolve_native_env_key(adapter->native_api_key_env, key, sizeof(key));
      if (!key[0])
         (void)provider_cli_resolve_native_env_key("GOOGLE_API_KEY", key, sizeof(key));
      if (key[0])
      {
         const char *mechanism = getenv("GEMINI_API_KEY_AUTH_MECHANISM");
         if (mechanism && strcmp(mechanism, "bearer") == 0)
         {
            snprintf(native_agent->api_key, sizeof(native_agent->api_key), "%s", key);
            snprintf(native_agent->auth_type, sizeof(native_agent->auth_type), "%s", "bearer");
         }
         else
         {
            char header[MAX_API_KEY_LEN + 32];
            snprintf(header, sizeof(header), "x-goog-api-key: %s", key);
            provider_cli_append_header(native_agent->extra_headers,
                                       sizeof(native_agent->extra_headers), header);
            snprintf(native_agent->auth_type, sizeof(native_agent->auth_type), "%s", "none");
            runtime_secret_wipe(header, sizeof(header));
         }
         runtime_secret_wipe(key, sizeof(key));
      }
      else if (!strstr(native_agent->extra_headers, "x-goog-api-key"))
      {
         if (errbuf && errbuf_len > 0)
            snprintf(errbuf, errbuf_len,
                     "%s is not present in Vault; native %s adapter also checked "
                     "GOOGLE_API_KEY in Vault",
                     adapter->native_api_key_env ? adapter->native_api_key_env : "GEMINI_API_KEY",
                     adapter->cli_kind);
         return -1;
      }
   }
   else if (!native_agent->api_key[0] && !native_agent->auth_cmd[0] && adapter->native_api_key_env)
   {
      /* Nothing on disk: fall back to the provider's vault slot. Writing the
       * resolved secret into api_key here is deliberate and local -- native_agent
       * is this call's mutable copy, not the cached registry. */
      (void)provider_cli_resolve_native_env_key(adapter->native_api_key_env, native_agent->api_key,
                                                sizeof(native_agent->api_key));
      if (!native_agent->api_key[0])
      {
         if (errbuf && errbuf_len > 0)
            snprintf(errbuf, errbuf_len, "%s is not present in Vault for native %s adapter",
                     adapter->native_api_key_env, adapter->cli_kind);
         return -1;
      }
   }

   if (!native_agent->endpoint[0] || !native_agent->model[0])
   {
      if (errbuf && errbuf_len > 0)
         snprintf(errbuf, errbuf_len, "native %s adapter missing endpoint or model",
                  adapter->cli_kind);
      return -1;
   }

   return 0;
}

int provider_cli_split_command(const char *cli_cmd, char **argv, int max_args, char *errbuf,
                               size_t errbuf_len)
{
   int count = shlex_split(cli_cmd, argv, max_args);
   if (count <= 0)
   {
      snprintf(errbuf, errbuf_len, "empty provider CLI command");
      return -1;
   }
   if (count >= max_args)
   {
      provider_cli_free_tokens(argv, count);
      snprintf(errbuf, errbuf_len, "provider CLI command has too many arguments");
      return -1;
   }
   for (int i = 0; i < count; i++)
   {
      if (util_token_is_shell_operator(argv[i]))
      {
         provider_cli_free_tokens(argv, count);
         snprintf(errbuf, errbuf_len,
                  "provider CLI command may not contain shell operators; configure an argv-style "
                  "command instead");
         return -1;
      }
   }
   argv[count] = NULL;
   return count;
}

int provider_cli_command_has_shell_operators(const char *cli_cmd)
{
   char *argv[PROVIDER_CLI_MAX_ARGS + 1] = {0};
   char err[128];
   int count = provider_cli_split_command(cli_cmd, argv, PROVIDER_CLI_MAX_ARGS, err, sizeof(err));
   if (count < 0)
      return 1;
   provider_cli_free_tokens(argv, count);
   return 0;
}

static int provider_cli_executable_on_path(const char *exe)
{
   if (!exe || !exe[0])
      return 0;
   if (strchr(exe, '/'))
      return access(exe, X_OK) == 0;

   const char *path = getenv("PATH");
   if (!path || !path[0])
      path = "/usr/local/bin:/usr/bin:/bin";
   char *copy = safe_strdup(path);
   if (!copy)
      return 0;

   int available = 0;
   char *saveptr = NULL;
   for (char *dir = strtok_r(copy, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr))
   {
      char candidate[MAX_PATH_LEN];
      snprintf(candidate, sizeof(candidate), "%s/%s", dir[0] ? dir : ".", exe);
      if (access(candidate, X_OK) == 0)
      {
         available = 1;
         break;
      }
   }
   free(copy);
   return available;
}

int provider_cli_check_available(const char *cli_cmd, const char *cli_kind)
{
   const char *cmd = (cli_cmd && cli_cmd[0]) ? cli_cmd : cli_kind;
   if (!cmd || !cmd[0])
      return 0;

   char *argv[PROVIDER_CLI_MAX_ARGS + 1] = {0};
   char err[128];
   int count = provider_cli_split_command(cmd, argv, PROVIDER_CLI_MAX_ARGS, err, sizeof(err));
   if (count < 0)
      return 0;

   int available = provider_cli_executable_on_path(argv[0]);
   provider_cli_free_tokens(argv, count);
   return available;
}

static void delegate_child_strip_forge_creds(int flag)
{
   if (!flag)
      return;
   /* Ambient forge tokens gh/git would pick up. */
   unsetenv("GH_TOKEN");
   unsetenv("GITHUB_TOKEN");
   unsetenv("GH_ENTERPRISE_TOKEN");
   unsetenv("GITHUB_ENTERPRISE_TOKEN");
   /* Credential plumbing: the askpass shim, an agent-forwarded SSH key, and any
    * inherited credential helper reachable via the global/system git config.
    * SSH_AUTH_SOCK is broader than git: dropping it means a delegate cannot use
    * agent-backed SSH to ANY host, not just the forge. That is intended (a forwarded
    * agent is a general-purpose credential we do not want in a delegate), but it is
    * the one strip here that can bite non-git work -- see require_aimee_git in
    * config.h. */
   unsetenv("GIT_ASKPASS");
   unsetenv("SSH_ASKPASS");
   unsetenv("SSH_AUTH_SOCK");
   setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
   setenv("GIT_CONFIG_SYSTEM", "/dev/null", 1);
   /* gh reads its stored oauth token from GH_CONFIG_DIR (default ~/.config/gh);
    * point it at a path that holds no hosts.yml rather than leaving the default. */
   setenv("GH_CONFIG_DIR", "/nonexistent/aimee-delegate-no-gh", 1);
   /* Never let a credential prompt block the child waiting on a tty. */
   setenv("GIT_TERMINAL_PROMPT", "0", 1);
}

/* Point the delegate's `aimee` CLI at THIS server, so its MCP proxy
 * (`aimee mcp serve`, wired in cli_claude.c) has somewhere to forward to. The /v1
 * Unix socket is the right transport here: it is always served, needs no TCP port
 * (server_api_http_port defaults to 0/disabled) and no network bearer.
 *
 * Never overwrites: an operator who set AIMEE_API_ENDPOINT explicitly means it.
 * Pre-resolved by the parent — see delegate_child_strip_forge_creds on why the
 * child does no allocation. */
static void delegate_child_export_aimee_endpoint(const char *endpoint)
{
   if (endpoint && endpoint[0])
      setenv("AIMEE_API_ENDPOINT", endpoint, 0);
}

int provider_cli_spawn_argv(const provider_cli_cfg_t *cfg, char *const argv[], int *stdin_fd,
                            int *stdout_fd, pid_t *pid_out)
{
   if (!argv || !argv[0] || !stdin_fd || !stdout_fd || !pid_out)
      return -1;

   /* Resolve the dial BEFORE forking (see delegate_child_strip_forge_creds).
    * Unreadable config reads as enforcing: a guard that fails open is not a guard.
    *
    * ...but ONLY strip when aimee-server actually holds a forge credential
    * (operator ruling 2026-07-15). The point of taking the delegate's credentials
    * away is to push its git through aimee, which runs on the server. If the server
    * has no credential, aimee's git cannot work either, so stripping would remove
    * the delegate's only route and leave it nothing — breakage dressed as policy.
    * No aimee route, no restriction. */
   int strip_forge_creds =
       (!config_present() || config_require_aimee_git()) && git_cred_forge_configured();

   /* Resolve the delegate's aimee endpoint before forking (aimee_home may read/
    * allocate). See delegate_child_export_aimee_endpoint. */
   char aimee_endpoint[MAX_PATH_LEN + 32] = "";
   {
      const char *home = aimee_home();
      if (home && home[0])
         snprintf(aimee_endpoint, sizeof(aimee_endpoint), "unix:%s/aimee-http.sock", home);
   }

   int in_pipe[2] = {-1, -1};
   int out_pipe[2] = {-1, -1};
   if (pipe(in_pipe) < 0)
      return -1;
   if (pipe(out_pipe) < 0)
   {
      close(in_pipe[0]);
      close(in_pipe[1]);
      return -1;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      return -1;
   }

   if (pid == 0)
   {
      setsid();
      close(in_pipe[1]);
      close(out_pipe[0]);
      dup2(in_pipe[0], STDIN_FILENO);
      dup2(out_pipe[1], STDOUT_FILENO);
      dup2(out_pipe[1], STDERR_FILENO);
      close(in_pipe[0]);
      close(out_pipe[1]);
      if (cfg && cfg->cwd && cfg->cwd[0] && chdir(cfg->cwd) != 0)
         _exit(126);
      if (cfg && cfg->home && cfg->home[0])
      {
         setenv("HOME", cfg->home, 1);
         unsetenv("CLAUDE_CONFIG_DIR");
      }
      if (cfg && cfg->session_id && cfg->session_id[0])
         setenv("AIMEE_SESSION_ID", cfg->session_id, 1);
      /* A spawned provider-CLI agent is its own session: clear the surrounding
       * Claude Code session's nesting guards so agents that are themselves
       * Claude Code (e.g. the `claude-code-acp` ACP adapter) don't refuse to
       * launch with "cannot be launched inside another Claude Code session".
       * No-op when aimee-server isn't running inside a Claude Code session. */
      unsetenv("CLAUDECODE");
      unsetenv("CLAUDE_CODE_SSE_PORT");
      unsetenv("CLAUDE_CODE_ENTRYPOINT");
      /* Give a cross-process delegate sub-client this delegate's depth/parent/
       * source context from the forking thread's TLS, immune to the parent-side
       * concurrent env clobber. */
      delegate_child_export_context_env();
      delegate_child_export_aimee_endpoint(aimee_endpoint);
      delegate_child_strip_forge_creds(strip_forge_creds);
      execvp(argv[0], argv);
      _exit(127);
   }

   close(in_pipe[0]);
   close(out_pipe[1]);
   *stdin_fd = in_pipe[1];
   *stdout_fd = out_pipe[0];
   *pid_out = pid;
   return 0;
}

extern const provider_cli_adapter_t acp_provider_cli_adapter;

const provider_cli_adapter_t *provider_cli_adapter_get(const char *cli_kind)
{
   static const provider_cli_adapter_t *adapters[] = {
       &codex_provider_cli_adapter,
       &claude_provider_cli_adapter,
       &mistral_provider_cli_adapter,
       &acp_provider_cli_adapter,
       NULL,
   };

   if (!cli_kind || !cli_kind[0])
      return NULL;
   if (strcmp(cli_kind, "mistral-plan") == 0 || strcmp(cli_kind, "vibe") == 0 ||
       strcmp(cli_kind, "vibe-plan") == 0)
      return &mistral_provider_cli_adapter;
   for (int i = 0; adapters[i]; i++)
   {
      if (strcmp(adapters[i]->cli_kind, cli_kind) == 0)
         return adapters[i];
   }
   return NULL;
}

int provider_cli_adapter_execute(const provider_cli_adapter_t *adapter, const agent_t *agent,
                                 const char *cwd, const char *system_prompt,
                                 const char *user_prompt, agent_result_t *out)
{
   provider_cli_cfg_t cfg = {
       .agent = agent, .cwd = cwd, .system_prompt = system_prompt, .user_prompt = user_prompt};
   return provider_cli_adapter_execute_cfg(adapter, &cfg, out);
}

int provider_cli_adapter_execute_cfg(const provider_cli_adapter_t *adapter,
                                     const provider_cli_cfg_t *config, agent_result_t *out)
{
   if (!config)
      return -1;
   provider_cli_cfg_t cfg = *config;
   const agent_t *agent = cfg.agent;
   const char *user_prompt = cfg.user_prompt;
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (agent)
   {
      snprintf(out->agent_name, MAX_AGENT_NAME, "%s", agent->name);
      snprintf(out->model, MAX_MODEL_LEN, "%s", agent->model);
      snprintf(out->served_model, MAX_MODEL_LEN, "%s", agent->model);
   }

   if (!adapter)
   {
      snprintf(out->error, sizeof(out->error), "provider-cli: no adapter");
      return -1;
   }
   if (!user_prompt || !user_prompt[0])
   {
      snprintf(out->error, sizeof(out->error), "empty prompt");
      return -1;
   }

   if (!adapter->execute)
   {
      snprintf(out->error, sizeof out->error, "provider-cli: %s has no canonical executor",
               adapter->display_name);
      return AGENT_RC_CONTEXT_REFUSED;
   }
   int rc = adapter->execute(&cfg, out);
   /* A host scheduler is only an intermediate stage: its factory validates and
    * installs the original response after joining native callbacks. Every
    * standalone completed executor must already own admitted model output. */
   if (!rc && out->success && !cfg.codex_host && !cfg.claude_host &&
       (!out->canonical_response.n_content || aimee_ir_response_admit(&out->canonical_response)))
   {
      out->success = 0;
      snprintf(out->error, sizeof out->error,
               "provider-cli: canonical output is missing or invalid");
      aimee_ir_session_fail_capture();
      return AGENT_RC_CAPTURE_FAILED;
   }
   if (!rc && out->success && !cfg.codex_host && !cfg.claude_host &&
       aimee_ir_response_complete_valid(&out->canonical_response))
   {
      out->success = 0;
      if (!out->error[0])
         snprintf(out->error, sizeof out->error,
                  "provider-cli: executor did not return a complete canonical answer");
      return AGENT_RC_CONTEXT_REFUSED;
   }
   return rc;
}
