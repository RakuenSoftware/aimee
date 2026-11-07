/* CLI configuration and process lifetime for the shared model listener. */
#include "cli_proxy.h"
#include "aimee_client.h"
#include "platform_path.h"
#include "platform_process.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef AIMEE_WINDOWS
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

static int remote_forward(const char *method, const char *path, const char *headers,
                          const void *body, unsigned long body_len, const char *session_id,
                          const cli_proxy_io_t *io, void *context)
{
   (void)context;
   return aimee_client_proxy_request(method, path, headers, body, body_len, session_id,
                                     io->write_response, io->on_upstream, io->context);
}

cli_proxy_t *cli_proxy_start(unsigned port, const char *local_token, const char *session_id,
                             unsigned *bound_port)
{
   if (!aimee_client_has_remote())
      return NULL;
   return cli_proxy_start_bound(port, local_token, session_id, bound_port, remote_forward, NULL);
}

static volatile sig_atomic_t proxy_interrupted;
static volatile sig_atomic_t proxy_child_pid;
static void proxy_signal(int signum)
{
   proxy_interrupted = 1;
   if (proxy_child_pid > 0)
#ifdef AIMEE_WINDOWS
      platform_signal_send_term(proxy_child_pid);
#else
      kill(proxy_child_pid, signum);
#endif
}

int cli_proxy_cmd(int argc, char **argv)
{
   unsigned port = 8911;
   if (argc == 2 && strcmp(argv[0], "--port") == 0 && *argv[1])
   {
      port = 0;
      for (const char *p = argv[1]; *p; p++)
      {
         if (*p < '0' || *p > '9' || (port = port * 10 + (unsigned)(*p - '0')) > 65535)
            goto usage;
      }
   }
   else if (argc)
      goto usage;
   const char *token = getenv("AIMEE_PROXY_TOKEN");
   if (!token || !*token)
   {
      fprintf(stderr, "aimee: set AIMEE_PROXY_TOKEN to a local-only credential, or use "
                      "`aimee launch --gateway -- codex` for automatic setup\n");
      return 1;
   }
   cli_proxy_t *proxy = cli_proxy_start(port, token, NULL, &port);
   if (!proxy)
   {
      fprintf(stderr, "aimee: could not start local proxy; check remote configuration and port\n");
      return 1;
   }
   platform_signal_term(proxy_signal);
   platform_signal_int(proxy_signal);
   fprintf(stderr, "aimee: model proxy listening at http://127.0.0.1:%u/v1\n", port);
   while (!proxy_interrupted)
#ifdef AIMEE_WINDOWS
      Sleep(200);
#else
      usleep(200000);
#endif
   cli_proxy_stop(proxy);
   return 0;
usage:
   fprintf(stderr, "usage: aimee proxy [--port 0..65535]\n");
   return 2;
}

/* Hook discovery in Codex 0.153.4 reads plugin enablement from file layers,
 * not -c overrides. A private profile disables only Aimee's plugin without
 * changing the user's config, hook trust, credentials, or other plugins. */
static int codex_proxy_profile(char *profile, size_t profile_size, char *path, size_t path_size)
{
   unsigned char random[16];
   char hex[33], default_home[4096];
   const char *config_home = getenv("CODEX_HOME");
   if (!config_home || !*config_home)
   {
      const char *home = platform_home_dir();
      if (!home || snprintf(default_home, sizeof(default_home), "%s/.codex", home) >=
                       (int)sizeof(default_home))
         return -1;
      config_home = default_home;
   }
   if (platform_random_bytes(random, sizeof(random)) != 0 ||
       platform_mkdir_p(config_home, 0700) != 0)
      return -1;
   for (size_t i = 0; i < sizeof(random); i++)
      snprintf(hex + 2 * i, 3, "%02x", random[i]);
   if (snprintf(profile, profile_size, "aimee-proxy-%s", hex) >= (int)profile_size ||
       snprintf(path, path_size, "%s/%s.config.toml", config_home, profile) >= (int)path_size)
   {
      path[0] = '\0';
      return -1;
   }
   FILE *file = fopen(path, "wx");
   if (!file)
   {
      path[0] = '\0'; /* Never remove a file we did not create. */
      return -1;
   }
   int ok = platform_set_permissions(path, 0600) == 0 &&
            fputs("[plugins.\"aimee@local\"]\nenabled = false\n", file) >= 0;
   if (fclose(file) != 0)
      ok = 0;
   return ok ? 0 : -1;
}

int cli_proxy_launch(char *const argv[], const char *session_id)
{
   unsigned char random[32];
   char token[65], origin[80], base[sizeof(origin) + sizeof("/v1") - 1];
   if (platform_random_bytes(random, sizeof(random)) != 0)
      return 1;
   for (size_t i = 0; i < sizeof(random); i++)
      snprintf(token + 2 * i, 3, "%02x", random[i]);
   unsigned port;
   cli_proxy_t *proxy = cli_proxy_start(0, token, session_id, &port);
   if (!proxy)
   {
      fprintf(stderr, "aimee: could not start the local model proxy; configure an Aimee remote\n");
      return 1;
   }
   snprintf(origin, sizeof(origin), "http://127.0.0.1:%u", port);
   snprintf(base, sizeof(base), "%s/v1", origin);
   int status = 1;
   char profile[64], profile_path[4096] = "";
   if (platform_setenv("AIMEE_CONVERSATION_GATEWAY", base) != 0 ||
       platform_setenv("OPENAI_BASE_URL", base) != 0 ||
       platform_setenv("OPENAI_API_KEY", token) != 0 ||
       platform_setenv("ANTHROPIC_BASE_URL", origin) != 0 ||
       platform_setenv("ANTHROPIC_AUTH_TOKEN", token) != 0 ||
       platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "1") != 0)
      goto done;
   /* Codex's ChatGPT login does not select an API-key provider just because
    * OPENAI_BASE_URL is set. Explicitly select Responses + the local credential.
    * Disable Aimee's CLI/MCP plugin for this invocation. */
   const char *name = strrchr(argv[0], '/');
   name = name ? name + 1 : argv[0];
   const char *backslash = strrchr(name, '\\');
   name = backslash ? backslash + 1 : name;
   int codex = strcmp(name, "codex") == 0 || strcmp(name, "codex.exe") == 0;
   int custom_profile = 0;
   if (codex)
   {
      for (size_t i = 1; argv[i] && strcmp(argv[i], "--") != 0; i++)
         if (strcmp(argv[i], "--profile") == 0 || strcmp(argv[i], "-p") == 0 ||
             strncmp(argv[i], "--profile=", 10) == 0 ||
             (strncmp(argv[i], "-p", 2) == 0 && argv[i][2]))
            custom_profile = 1;
      if (custom_profile)
         fprintf(stderr, "aimee: using your Codex profile; disable plugins.\"aimee@local\" "
                         "in that profile to keep its CLI hooks off\n");
      else if (codex_proxy_profile(profile, sizeof(profile), profile_path, sizeof(profile_path)) !=
               0)
      {
         fprintf(stderr, "aimee: could not create the temporary Codex proxy profile\n");
         goto done;
      }
   }
   char base_config[128];
   snprintf(base_config, sizeof(base_config), "model_providers.aimee.base_url=\"%s\"", base);
   /* The server's model IDs are agent bindings, not upstream provider IDs.
    * Use its primary binding by default; the user's later -m/-c can select a
    * named binding without changing their global Codex model preference. */
   char *options[] = {"-c", "model_provider=\"aimee\"",
                      "-c", "model=\"aimee\"",
                      "-c", "model_providers.aimee.name=\"Aimee local proxy\"",
                      "-c", base_config,
                      "-c", "model_providers.aimee.wire_api=\"responses\"",
                      "-c", "model_providers.aimee.env_key=\"OPENAI_API_KEY\"",
                      "-c", "model_providers.aimee.requires_openai_auth=false",
                      "-c", "model_providers.aimee.supports_websockets=false",
                      "-c", "plugins.\"aimee@local\".enabled=false",
                      "-c", "mcp_servers.aimee={command=\"aimee\",enabled=false}"};
   size_t argc = 0, option_count = codex ? sizeof(options) / sizeof(options[0]) : 0;
   while (argv[argc])
      argc++;
   size_t profile_args = codex && !custom_profile ? 2 : 0;
   char **command = calloc(argc + option_count + profile_args + 1, sizeof(*command));
   if (!command)
      goto done;
   command[0] = argv[0];
   for (size_t i = 0; i < option_count; i++)
      command[i + 1] = options[i];
   if (profile_args)
   {
      command[option_count + 1] = "--profile";
      command[option_count + 2] = profile;
   }
   for (size_t i = 1; i < argc; i++)
      command[i + option_count + profile_args] = argv[i];
   /* The provider child receives only the local proxy credential, even if the
    * parent selected its upstream bearer through the environment. */
#ifdef AIMEE_WINDOWS
   char **parent_env = _environ;
#else
   char **parent_env = environ;
#endif
   size_t env_count = 0, copied = 0;
   while (parent_env[env_count])
      env_count++;
   char **child_env = calloc(env_count + 1, sizeof(*child_env));
   if (!child_env)
   {
      free(command);
      goto done;
   }
   for (size_t i = 0; i < env_count; i++)
      if (strncmp(parent_env[i], "AIMEE_SERVER_TOKEN=", 19) != 0 &&
          strncmp(parent_env[i], "AIMEE_TLS_CLIENT_P12_PASS=", 26) != 0)
         child_env[copied++] = parent_env[i];
   platform_signal_term(proxy_signal);
   platform_signal_int(proxy_signal);
#ifdef AIMEE_WINDOWS
   intptr_t child = _spawnvpe(_P_NOWAIT, command[0], (const char *const *)command,
                              (const char *const *)child_env);
   if (child == -1)
      status = 127;
   else
   {
      proxy_child_pid = (sig_atomic_t)GetProcessId((HANDLE)child);
      if (proxy_interrupted)
         platform_signal_send_term(proxy_child_pid);
      int result;
      status = _cwait(&result, child, 0) == -1 ? 1 : result;
   }
#else
   pid_t pid;
   if (posix_spawnp(&pid, command[0], NULL, NULL, command, child_env) != 0)
      status = 127;
   else
   {
      proxy_child_pid = pid;
      if (proxy_interrupted)
         kill(pid, SIGTERM);
      int result;
      pid_t waited;
      do
      {
         waited = waitpid(pid, &result, 0);
      } while (waited < 0 && errno == EINTR);
      status = waited < 0 ? 1 : WIFEXITED(result) ? WEXITSTATUS(result) : 128 + WTERMSIG(result);
   }
#endif
   proxy_child_pid = 0;
   free(child_env);
   free(command);
done:
   cli_proxy_stop(proxy);
   if (profile_path[0] && platform_unlink(profile_path) != 0)
      fprintf(stderr, "aimee: could not remove temporary Codex profile: %s\n", profile_path);
   return status;
}
