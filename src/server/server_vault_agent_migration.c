/* Pre-bus ABI transport to the Go providers parser; no roster policy in C. */
#include "server.h"
#include "config.h"
#include "vault_store.h"
#include "cJSON.h"
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

static int server_bootstrap_resolve_agent(const char *name, char *canon, size_t cap)
{
   int input[2], output[2];
   if (pipe(input) != 0)
      return 0;
   if (pipe(output) != 0)
   {
      close(input[0]);
      close(input[1]);
      return 0;
   }
   posix_spawn_file_actions_t actions;
   posix_spawn_file_actions_init(&actions);
   posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
   posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
   posix_spawn_file_actions_addclose(&actions, input[1]);
   posix_spawn_file_actions_addclose(&actions, output[0]);
   char *args[] = {"/usr/local/libexec/aimee-modules/aimee-module-providers",
                   "__aimee_provider_bootstrap_lookup", NULL};
   pid_t child;
   int started = posix_spawn(&child, args[0], &actions, NULL, args, environ);
   posix_spawn_file_actions_destroy(&actions);
   close(input[0]);
   close(output[1]);
   if (started != 0)
   {
      close(input[1]);
      close(output[0]);
      return 0;
   }
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "name", name);
   cJSON_AddStringToObject(request, "home", config_default_dir());
   char *body = cJSON_PrintUnformatted(request);
   cJSON_Delete(request);
   FILE *stream = fdopen(input[1], "w");
   if (stream)
   {
      if (body)
         fputs(body, stream);
      fclose(stream);
   }
   else
      close(input[1]);
   free(body);
   char buffer[256] = {0};
   struct pollfd fd = {.fd = output[0], .events = POLLIN};
   ssize_t n = poll(&fd, 1, 5000) > 0 ? read(output[0], buffer, sizeof(buffer) - 1) : -1;
   close(output[0]);
   if (n <= 0)
      kill(child, SIGKILL);
   int status = 0;
   waitpid(child, &status, 0);
   if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
      return 0;
   cJSON *reply = cJSON_Parse(buffer);
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(reply, "name"));
   int ok = value && value[0] && strlen(value) < cap;
   if (ok)
      snprintf(canon, cap, "%s", value);
   cJSON_Delete(reply);
   return ok;
}

int server_vault_bootstrap_prepare(void)
{
   server_vault_bootstrap_set_resolver(server_bootstrap_resolve_agent);
   return server_vault_bootstrap();
}
