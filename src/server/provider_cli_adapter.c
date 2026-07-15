/* provider_cli_adapter.c: shared provider-CLI delegate adapter registry. */
#include "provider_cli_adapter.h"

#include "aimee.h"
#include "cJSON.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Defined in server_compute.c; called post-fork to seed the child's delegate
 * context env from the forking thread's TLS (race-free). */
void delegate_child_export_context_env(void);

#define PROVIDER_CLI_MAX_ARGS 48
#define PROVIDER_CLI_POLL_MS  250

typedef struct
{
   char *text;
   size_t len;
   size_t cap;
} provider_cli_buf_t;

static int provider_cli_buf_append(provider_cli_buf_t *buf, const char *text)
{
   if (!buf || !text || !text[0])
      return 0;
   size_t n = strlen(text);
   if (buf->len + n + 1 > buf->cap)
   {
      size_t next = buf->cap ? buf->cap : 4096;
      while (next < buf->len + n + 1)
         next *= 2;
      char *grown = realloc(buf->text, next);
      if (!grown)
         return -1;
      buf->text = grown;
      buf->cap = next;
   }
   memcpy(buf->text + buf->len, text, n);
   buf->len += n;
   buf->text[buf->len] = '\0';
   return 0;
}

void provider_cli_free_tokens(char **tokens, int count)
{
   for (int i = 0; i < count; i++)
      free(tokens[i]);
}

static int provider_cli_read_vibe_dotenv(const char *name, char *dst, size_t dst_len)
{
   const char *home = getenv("HOME");
   if (!name || !name[0] || !home || !home[0] || !dst || dst_len == 0)
      return 0;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/.vibe/.env", home);

   FILE *f = fopen(path, "r");
   if (!f)
      return 0;

   char line[MAX_API_KEY_LEN + 128];
   size_t name_len = strlen(name);
   int found = 0;
   while (fgets(line, sizeof(line), f))
   {
      char *p = line;
      if (strncmp(p, "export ", 7) == 0)
         p += 7;
      if (strncmp(p, name, name_len) != 0 || p[name_len] != '=')
         continue;

      char *val = p + name_len + 1;
      size_t len = strlen(val);
      while (len > 0 && (val[len - 1] == '\n' || val[len - 1] == '\r'))
         val[--len] = '\0';
      if (len >= 2 &&
          ((val[0] == '"' && val[len - 1] == '"') || (val[0] == '\'' && val[len - 1] == '\'')))
      {
         val[len - 1] = '\0';
         val++;
      }
      snprintf(dst, dst_len, "%s", val);
      found = 1;
      break;
   }

   fclose(f);
   return found;
}

static int provider_cli_resolve_native_env_key(const char *name, char *dst, size_t dst_len)
{
   if (!name || !name[0] || !dst || dst_len == 0)
      return 0;

   const char *val = getenv(name);
   if (val && val[0])
   {
      snprintf(dst, dst_len, "%s", val);
      return 1;
   }

   return provider_cli_read_vibe_dotenv(name, dst, dst_len);
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
      if (native_agent->api_key[0])
      {
         snprintf(key, sizeof(key), "%s", native_agent->api_key);
         native_agent->api_key[0] = '\0';
      }
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
         }
      }
      else if (!strstr(native_agent->extra_headers, "x-goog-api-key"))
      {
         if (errbuf && errbuf_len > 0)
            snprintf(errbuf, errbuf_len,
                     "%s is not set; native %s adapter also checked GOOGLE_API_KEY and "
                     "~/.vibe/.env",
                     adapter->native_api_key_env ? adapter->native_api_key_env : "GEMINI_API_KEY",
                     adapter->cli_kind);
         return -1;
      }
   }
   else if (!native_agent->api_key[0] && !native_agent->auth_cmd[0] && adapter->native_api_key_env)
   {
      (void)provider_cli_resolve_native_env_key(adapter->native_api_key_env, native_agent->api_key,
                                                sizeof(native_agent->api_key));
      if (!native_agent->api_key[0])
      {
         if (errbuf && errbuf_len > 0)
            snprintf(errbuf, errbuf_len,
                     "%s is not set; native %s adapter also checked ~/.vibe/.env",
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

static void provider_cli_close_fd(int *fd)
{
   if (fd && *fd >= 0)
   {
      close(*fd);
      *fd = -1;
   }
}

static int provider_cli_set_nonblock(int fd)
{
   int flags = fcntl(fd, F_GETFL, 0);
   if (flags < 0)
      return -1;
   return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void provider_cli_terminate_child(pid_t pid)
{
   if (pid <= 0)
      return;
   kill(-pid, SIGTERM);
   kill(pid, SIGTERM);
   for (int i = 0; i < 20; i++)
   {
      int status = 0;
      pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid || (r < 0 && errno == ECHILD))
         return;
      struct timespec ts = {0, 50 * 1000000L};
      nanosleep(&ts, NULL);
   }
   kill(-pid, SIGKILL);
   kill(pid, SIGKILL);
   (void)waitpid(pid, NULL, 0);
}

/* Remove the git/forge credentials a delegate child would otherwise inherit, so the
 * no-git rule does not rest on the classifier alone: wfe_shell_invokes_git() is a
 * string match over a shell line and a determined agent can evade it, whereas a
 * credential that is not in the environment cannot be talked around. git/forge work
 * belongs to aimee's git_* tools, which run in aimee-server where the token is
 * resolved per call and lives only in process memory.
 *
 * This is defence in depth, NOT a credential boundary. It removes what we know how
 * to name; it cannot prove the child has no other path to a token (a key on disk, a
 * helper we do not enumerate, a credential minted later). Treat it as raising the
 * cost, not as a guarantee.
 *
 * Called post-fork. `flag` is resolved by the PARENT because config_load reads files
 * and allocates, and running that between fork and exec in a threaded process risks
 * a malloc-lock deadlock. setenv/unsetenv may themselves allocate, so this is not
 * strictly async-signal-safe either -- it matches the risk the adjacent
 * unsetenv()/platform_setenv() calls already take here rather than adding a new one.
 * The clean fix, if this ever bites, is to build the sanitized envp in the parent
 * and execve() it. */
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

int provider_cli_spawn_argv(const provider_cli_cfg_t *cfg, char *const argv[], int *stdin_fd,
                            int *stdout_fd, pid_t *pid_out)
{
   if (!argv || !argv[0] || !stdin_fd || !stdout_fd || !pid_out)
      return -1;

   /* Resolve the dial BEFORE forking (see delegate_child_strip_forge_creds).
    * Unreadable config reads as enforcing: a guard that fails open is not a guard. */
   config_t spawn_cfg;
   int strip_forge_creds = (config_load(&spawn_cfg) != 0) || spawn_cfg.require_aimee_git;

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

int provider_cli_default_build_prompt(const provider_cli_cfg_t *cfg, const char *system_prompt,
                                      const char *task, char *buf, size_t buf_sz)
{
   (void)cfg;
   if (!task || !task[0] || !buf || buf_sz == 0)
      return -1;
   if (system_prompt && system_prompt[0])
      snprintf(buf, buf_sz, "%s\n\n%s", system_prompt, task);
   else
      snprintf(buf, buf_sz, "%s", task);
   return 0;
}

int provider_cli_format_json_tool_result(const char *tool_name, const char *result_json, char *buf,
                                         size_t buf_sz)
{
   if (!tool_name || !tool_name[0] || !result_json || !buf || buf_sz == 0)
      return -1;
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return -1;
   cJSON_AddStringToObject(obj, "type", "tool_result");
   cJSON_AddStringToObject(obj, "name", tool_name);
   cJSON *result = cJSON_Parse(result_json);
   if (result)
      cJSON_AddItemToObject(obj, "result", result);
   else
      cJSON_AddStringToObject(obj, "result", result_json);
   char *s = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   if (!s)
      return -1;
   int needed = snprintf(buf, buf_sz, "%s\n", s);
   free(s);
   return (needed > 0 && (size_t)needed < buf_sz) ? 0 : -1;
}

static int provider_cli_name_is_write(const char *name)
{
   if (!name || !name[0])
      return 0;
   return strcmp(name, "Write") == 0 || strcmp(name, "Edit") == 0 ||
          strcmp(name, "MultiEdit") == 0 || strcmp(name, "NotebookEdit") == 0 ||
          strcmp(name, "write_file") == 0 || strcmp(name, "edit_file") == 0 ||
          strcmp(name, "replace") == 0 || strcmp(name, "patch") == 0;
}

int provider_cli_event_is_write(const cli_event_t *event)
{
   if (!event)
      return 0;
   return event->write_event || provider_cli_name_is_write(event->tool_name);
}

static const char *json_string(cJSON *obj, const char *name)
{
   cJSON *v = obj ? cJSON_GetObjectItem(obj, name) : NULL;
   return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

static int provider_cli_event_set_text(cli_event_t *event, const char *text)
{
   if (!event || !text || !text[0])
      return 0;
   event->type = CLI_EVENT_TEXT_DELTA;
   snprintf(event->text, sizeof(event->text), "%s", text);
   return 1;
}

static int provider_cli_parse_content_text(cJSON *content, cli_event_t *event)
{
   if (!content)
      return 0;
   if (cJSON_IsString(content))
      return provider_cli_event_set_text(event, content->valuestring);
   if (!cJSON_IsArray(content))
      return 0;

   cJSON *item;
   cJSON_ArrayForEach(item, content)
   {
      if (cJSON_IsString(item) && item->valuestring[0])
         return provider_cli_event_set_text(event, item->valuestring);
      if (!cJSON_IsObject(item))
         continue;
      const char *type = json_string(item, "type");
      if (type && strcmp(type, "text") == 0)
      {
         const char *text = json_string(item, "text");
         if (text && text[0])
            return provider_cli_event_set_text(event, text);
      }
   }
   return 0;
}

static int provider_cli_parse_tool_object(cJSON *tool, cli_event_t *event)
{
   if (!tool || !cJSON_IsObject(tool) || !event)
      return 0;
   const char *name = json_string(tool, "name");
   if (!name)
      name = json_string(tool, "tool_name");
   if (!name)
      name = json_string(tool, "function");
   cJSON *function = cJSON_GetObjectItem(tool, "function");
   if (!name && function && cJSON_IsObject(function))
      name = json_string(function, "name");
   if (!name || !name[0])
      return 0;

   event->type = CLI_EVENT_TOOL_START;
   snprintf(event->tool_name, sizeof(event->tool_name), "%s", name);
   event->write_event = provider_cli_name_is_write(name);
   return 1;
}

int provider_cli_parse_json_line_common(const char *line, cli_event_t *event_out)
{
   if (!line || !event_out)
      return 0;
   memset(event_out, 0, sizeof(*event_out));

   cJSON *obj = cJSON_Parse(line);
   if (!obj || !cJSON_IsObject(obj))
   {
      if (obj)
         cJSON_Delete(obj);
      return 0;
   }

   int matched = 0;
   const char *type = json_string(obj, "type");
   const char *role = json_string(obj, "role");

   if (type && strcmp(type, "error") == 0)
   {
      const char *msg = json_string(obj, "message");
      if (!msg)
      {
         cJSON *err = cJSON_GetObjectItem(obj, "error");
         msg = cJSON_IsString(err) ? err->valuestring : json_string(err, "message");
      }
      event_out->type = CLI_EVENT_ERROR;
      snprintf(event_out->text, sizeof(event_out->text), "%s", msg ? msg : "provider CLI error");
      matched = 1;
   }
   else if (type && (strcmp(type, "done") == 0 || strcmp(type, "turn_complete") == 0 ||
                     strcmp(type, "message_stop") == 0))
   {
      event_out->type = CLI_EVENT_TURN_COMPLETE;
      matched = 1;
   }
   else if ((type && (strcmp(type, "tool_call") == 0 || strcmp(type, "function_call") == 0 ||
                      strcmp(type, "tool_use") == 0)) ||
            cJSON_GetObjectItem(obj, "tool_call") || cJSON_GetObjectItem(obj, "function_call") ||
            cJSON_GetObjectItem(obj, "tool_use"))
   {
      cJSON *tool = cJSON_GetObjectItem(obj, "tool_call");
      if (!tool)
         tool = cJSON_GetObjectItem(obj, "function_call");
      if (!tool)
         tool = cJSON_GetObjectItem(obj, "tool_use");
      if (!tool)
         tool = obj;
      matched = provider_cli_parse_tool_object(tool, event_out);
   }
   else if ((role && (strcmp(role, "assistant") == 0 || strcmp(role, "model") == 0)) ||
            (type && (strcmp(type, "message") == 0 || strcmp(type, "assistant") == 0 ||
                      strcmp(type, "response") == 0 || strcmp(type, "content") == 0)))
   {
      cJSON *content = cJSON_GetObjectItem(obj, "content");
      matched = provider_cli_parse_content_text(content, event_out);
      if (!matched)
         matched = provider_cli_event_set_text(event_out, json_string(obj, "text"));
      if (!matched)
         matched = provider_cli_event_set_text(event_out, json_string(obj, "delta"));
   }
   else
   {
      cJSON *event = cJSON_GetObjectItem(obj, "event");
      if (event && cJSON_IsObject(event))
      {
         const char *etype = json_string(event, "type");
         if (etype && strcmp(etype, "content_block_start") == 0)
            matched = provider_cli_parse_tool_object(cJSON_GetObjectItem(event, "content_block"),
                                                     event_out);
         else if (etype && strcmp(etype, "content_block_delta") == 0)
         {
            cJSON *delta = cJSON_GetObjectItem(event, "delta");
            matched = provider_cli_event_set_text(event_out, json_string(delta, "text"));
         }
      }
   }

   cJSON_Delete(obj);
   return matched;
}

static int provider_cli_collect_child(const provider_cli_adapter_t *adapter, int stdin_fd,
                                      int stdout_fd, pid_t pid, const char *prompt, int timeout_ms,
                                      provider_cli_buf_t *output, agent_result_t *out)
{
   if (provider_cli_set_nonblock(stdin_fd) != 0 || provider_cli_set_nonblock(stdout_fd) != 0)
   {
      provider_cli_close_fd(&stdin_fd);
      provider_cli_close_fd(&stdout_fd);
      provider_cli_terminate_child(pid);
      snprintf(out->error, sizeof(out->error), "failed to configure pipes for %s",
               adapter->display_name);
      return -1;
   }

   struct sigaction old_pipe;
   struct sigaction ignore_pipe;
   memset(&ignore_pipe, 0, sizeof(ignore_pipe));
   ignore_pipe.sa_handler = SIG_IGN;
   sigemptyset(&ignore_pipe.sa_mask);
   int restore_pipe = sigaction(SIGPIPE, &ignore_pipe, &old_pipe) == 0;

   size_t prompt_len = strlen(prompt);
   size_t prompt_off = 0;
   int input_closed = 0;
   int output_closed = 0;
   int child_reaped = 0;
   int write_rc = 0;
   int status = 0;
   long long deadline = timeout_ms > 0 ? util_now_ms() + timeout_ms : 0;

   for (;;)
   {
      while (!output_closed)
      {
         char chunk[4096 + 1];
         ssize_t r = read(stdout_fd, chunk, sizeof(chunk) - 1);
         if (r > 0)
         {
            chunk[r] = '\0';
            if (provider_cli_buf_append(output, chunk) != 0)
            {
               snprintf(out->error, sizeof(out->error), "out of memory collecting %s output",
                        adapter->display_name);
               output_closed = 1;
               break;
            }
            continue;
         }
         if (r == 0)
         {
            output_closed = 1;
            provider_cli_close_fd(&stdout_fd);
            break;
         }
         if (r < 0 && errno == EINTR)
            continue;
         if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
         snprintf(out->error, sizeof(out->error), "failed to read output from %s",
                  adapter->display_name);
         output_closed = 1;
         provider_cli_close_fd(&stdout_fd);
         break;
      }

      while (!input_closed && write_rc == 0 && prompt_off < prompt_len)
      {
         ssize_t n = write(stdin_fd, prompt + prompt_off, prompt_len - prompt_off);
         if (n > 0)
         {
            prompt_off += (size_t)n;
            continue;
         }
         if (n < 0 && errno == EINTR)
            continue;
         if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
         write_rc = -1;
         break;
      }
      if (!input_closed && (prompt_off >= prompt_len || write_rc != 0))
      {
         provider_cli_close_fd(&stdin_fd);
         input_closed = 1;
      }

      if (!child_reaped)
      {
         pid_t wr = waitpid(pid, &status, WNOHANG);
         if (wr == pid)
            child_reaped = 1;
         else if (wr < 0 && errno == ECHILD)
            child_reaped = 1;
      }
      if (output_closed && child_reaped)
         break;

      int wait_ms = PROVIDER_CLI_POLL_MS;
      if (deadline > 0)
      {
         long long now = util_now_ms();
         if (now >= deadline)
         {
            provider_cli_close_fd(&stdin_fd);
            provider_cli_close_fd(&stdout_fd);
            provider_cli_terminate_child(pid);
            if (restore_pipe)
               sigaction(SIGPIPE, &old_pipe, NULL);
            snprintf(out->error, sizeof(out->error), "%s timed out after %d ms",
                     adapter->display_name, timeout_ms);
            return -1;
         }
         long long remaining = deadline - now;
         if (remaining < PROVIDER_CLI_POLL_MS)
            wait_ms = (int)remaining;
      }

      struct pollfd pfds[2];
      nfds_t nfds = 0;
      if (!output_closed)
      {
         pfds[nfds].fd = stdout_fd;
         pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
         pfds[nfds].revents = 0;
         nfds++;
      }
      if (!input_closed && write_rc == 0)
      {
         pfds[nfds].fd = stdin_fd;
         pfds[nfds].events = POLLOUT | POLLHUP | POLLERR;
         pfds[nfds].revents = 0;
         nfds++;
      }
      if (nfds == 0)
      {
         struct timespec ts = {0, (long)wait_ms * 1000000L};
         nanosleep(&ts, NULL);
         continue;
      }
      while (poll(pfds, nfds, wait_ms) < 0 && errno == EINTR)
         ;
   }

   if (restore_pipe)
      sigaction(SIGPIPE, &old_pipe, NULL);
   provider_cli_close_fd(&stdin_fd);
   provider_cli_close_fd(&stdout_fd);

   if (out->error[0])
      return -1;
   if (write_rc != 0 || prompt_off < prompt_len)
   {
      snprintf(out->error, sizeof(out->error), "failed to send prompt to %s",
               adapter->display_name);
      return -1;
   }
   if (WIFSIGNALED(status))
   {
      snprintf(out->error, sizeof(out->error), "%s terminated by signal %d", adapter->display_name,
               WTERMSIG(status));
      return -1;
   }
   if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
   {
      if (output->text && output->text[0])
         snprintf(out->error, sizeof(out->error), "%s exited with status %d: %.320s",
                  adapter->display_name, WEXITSTATUS(status), output->text);
      else
         snprintf(out->error, sizeof(out->error), "%s exited with status %d", adapter->display_name,
                  WEXITSTATUS(status));
      return -1;
   }
   return 0;
}

static int provider_cli_parse_output(const provider_cli_adapter_t *adapter, char *output,
                                     agent_result_t *out)
{
   provider_cli_buf_t parsed = {0};
   provider_cli_buf_t raw = {0};
   char event_error[sizeof(out->error)] = {0};
   int parsed_events = 0;
   int turns = 0;

   char *cursor = output;
   while (cursor && *cursor)
   {
      char *line = cursor;
      char *nl = strchr(cursor, '\n');
      if (nl)
      {
         *nl = '\0';
         cursor = nl + 1;
      }
      else
         cursor = NULL;

      if (!line[0])
         continue;

      cli_event_t event;
      memset(&event, 0, sizeof(event));
      int matched = adapter->parse_line ? adapter->parse_line(line, &event) : 0;
      if (!matched)
      {
         if (provider_cli_buf_append(&raw, line) != 0 ||
             provider_cli_buf_append(&raw, nl ? "\n" : "") != 0)
         {
            free(parsed.text);
            free(raw.text);
            snprintf(out->error, sizeof(out->error), "out of memory parsing %s output",
                     adapter->display_name);
            return -1;
         }
         continue;
      }

      parsed_events++;
      if (event.prompt_tokens > 0)
         out->prompt_tokens = event.prompt_tokens;
      if (event.completion_tokens > 0)
         out->completion_tokens = event.completion_tokens;
      if (event.cache_write_tokens > 0)
         out->cache_write_tokens = event.cache_write_tokens;
      if (event.cache_read_tokens > 0)
         out->cache_read_tokens = event.cache_read_tokens;
      if (event.latency_ms > 0)
         out->latency_ms = event.latency_ms;

      if (event.type == CLI_EVENT_TEXT_DELTA)
      {
         if (provider_cli_buf_append(&parsed, event.text) != 0)
         {
            free(parsed.text);
            free(raw.text);
            snprintf(out->error, sizeof(out->error), "out of memory parsing %s output",
                     adapter->display_name);
            return -1;
         }
      }
      else if (event.type == CLI_EVENT_TOOL_START)
      {
         out->tool_calls++;
         if (adapter->is_write_event && adapter->is_write_event(&event))
            event.write_event = 1;
      }
      else if (event.type == CLI_EVENT_TURN_COMPLETE)
      {
         if (parsed.len == 0 && event.text[0] && provider_cli_buf_append(&parsed, event.text) != 0)
         {
            free(parsed.text);
            free(raw.text);
            snprintf(out->error, sizeof(out->error), "out of memory parsing %s output",
                     adapter->display_name);
            return -1;
         }
         turns++;
      }
      else if (event.type == CLI_EVENT_ERROR && !event_error[0])
         snprintf(event_error, sizeof(event_error), "%s", event.text);
   }

   if (parsed.len > 0)
   {
      out->response = parsed.text;
      if (raw.text)
         free(raw.text);
      out->success = 1;
      out->turns = turns > 0 ? turns : 1;
      return 0;
   }
   free(parsed.text);

   if (parsed_events == 0 && raw.len > 0)
   {
      out->response = raw.text;
      out->success = 1;
      out->turns = 1;
      return 0;
   }
   free(raw.text);

   if (event_error[0])
      snprintf(out->error, sizeof(out->error), "%s", event_error);
   else
      snprintf(out->error, sizeof(out->error), "%s returned no assistant text",
               adapter->display_name);
   return -1;
}

extern const provider_cli_adapter_t acp_provider_cli_adapter;

const provider_cli_adapter_t *provider_cli_adapter_get(const char *cli_kind)
{
   static const provider_cli_adapter_t *adapters[] = {
       &codex_provider_cli_adapter,   &claude_provider_cli_adapter, &gemini_provider_cli_adapter,
       &mistral_provider_cli_adapter, &acp_provider_cli_adapter,    NULL,
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

   provider_cli_cfg_t cfg = {
       .agent = agent,
       .cwd = cwd,
       .system_prompt = system_prompt,
       .user_prompt = user_prompt,
   };

   /* NB: a thin-client (detached workspace) `claude` agent runs the standard
    * `claude` CLI over tmux on the client — that is the tmux-cli backend routed
    * through cli_session over the reverse channel, NOT this provider-cli path
    * (which would use `claude -p`). So there is no detached special-case here. */

   if (adapter->execute)
      return adapter->execute(&cfg, out);

   size_t prompt_cap = (system_prompt ? strlen(system_prompt) : 0) + strlen(user_prompt) + 256;
   char *prompt = malloc(prompt_cap);
   if (!prompt)
   {
      snprintf(out->error, sizeof(out->error), "out of memory");
      return -1;
   }
   int prompt_rc = adapter->build_prompt
                       ? adapter->build_prompt(&cfg, system_prompt, user_prompt, prompt, prompt_cap)
                       : provider_cli_default_build_prompt(&cfg, system_prompt, user_prompt, prompt,
                                                           prompt_cap);
   if (prompt_rc != 0)
   {
      free(prompt);
      snprintf(out->error, sizeof(out->error), "failed to build prompt for %s",
               adapter->display_name);
      return -1;
   }

   int stdin_fd = -1;
   int stdout_fd = -1;
   pid_t pid = -1;
   if (!adapter->spawn || adapter->spawn(&cfg, prompt, &stdin_fd, &stdout_fd, &pid) != 0)
   {
      free(prompt);
      snprintf(out->error, sizeof(out->error), "failed to start %s", adapter->display_name);
      return -1;
   }

   struct timespec start, end;
   clock_gettime(CLOCK_MONOTONIC, &start);

   provider_cli_buf_t output = {0};
   int timeout_ms = (agent && agent->cli_idle_timeout_ms > 0)
                        ? agent->cli_idle_timeout_ms
                        : ((agent && agent->timeout_ms > 0) ? agent->timeout_ms : -1);
   int rc = provider_cli_collect_child(adapter, stdin_fd, stdout_fd, pid, prompt, timeout_ms,
                                       &output, out);
   free(prompt);

   clock_gettime(CLOCK_MONOTONIC, &end);
   out->latency_ms =
       (int)((end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000);

   if (rc != 0)
   {
      free(output.text);
      return -1;
   }
   if (!output.text || !output.text[0])
   {
      free(output.text);
      snprintf(out->error, sizeof(out->error), "empty response from %s", adapter->display_name);
      return -1;
   }

   rc = provider_cli_parse_output(adapter, output.text, out);
   free(output.text);
   return rc;
}
