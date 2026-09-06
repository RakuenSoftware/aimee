/* Native tests use the Go implementation as their fixture peer. No provider
 * policy is reimplemented here. The unit runner supplies the fixture executable. */
#include "providers_client.h"
#include "aimee_home.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
static pthread_mutex_t fixture_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *to_fixture, *from_fixture;
static pid_t fixture_pid;
static void close_fixture(void)
{
   if (to_fixture)
      fclose(to_fixture);
   if (from_fixture)
      fclose(from_fixture);
   if (fixture_pid > 0)
      waitpid(fixture_pid, NULL, 0);
}
static int start_fixture(void)
{
   if (to_fixture)
      return 0;
   const char *exe = getenv("AIMEE_PROVIDERS_TEST_MODULE");
   if (!exe || !*exe)
      return -1;
   int in[2], out[2];
   if (pipe(in) || pipe(out))
      return -1;
   pid_t pid = fork();
   if (pid == 0)
   {
      dup2(in[0], STDIN_FILENO);
      dup2(out[1], STDOUT_FILENO);
      close(in[0]);
      close(in[1]);
      close(out[0]);
      close(out[1]);
      execl(exe, exe, NULL);
      _exit(127);
   }
   close(in[0]);
   close(out[1]);
   if (pid < 0)
   {
      close(in[1]);
      close(out[0]);
      return -1;
   }
   fixture_pid = pid;
   to_fixture = fdopen(in[1], "w");
   from_fixture = fdopen(out[0], "r");
   atexit(close_fixture);
   return to_fixture && from_fixture ? 0 : -1;
}
__attribute__((weak)) cJSON *providers_module_request(const char *operation, cJSON *arguments,
                                                      const char *actor, int allowed)
{
   pthread_mutex_lock(&fixture_lock);
   if (start_fixture() != 0)
   {
      pthread_mutex_unlock(&fixture_lock);
      return NULL;
   }
   cJSON *wire = cJSON_CreateObject();
   const char *home = aimee_home();
   cJSON_AddStringToObject(wire, "home", home ? home : "");
   cJSON *env = cJSON_AddObjectToObject(wire, "env");
   const char *keys[] = {"HOME", "XDG_CACHE_HOME", "AIMEE_MODELS_DEV_SNAPSHOT",
                         "AIMEE_MODEL_CAPABILITY_OVERRIDES", NULL};
   for (int i = 0; keys[i]; i++)
   {
      const char *v = getenv(keys[i]);
      cJSON_AddStringToObject(env, keys[i], v ? v : "");
   }
   cJSON *req = cJSON_AddObjectToObject(wire, "request");
   cJSON_AddStringToObject(req, "operation", operation);
   cJSON_AddStringToObject(req, "actor", actor ? actor : "");
   cJSON_AddBoolToObject(req, "secret_write_allowed", allowed);
   cJSON_AddItemToObject(req, "arguments",
                         arguments ? cJSON_Duplicate(arguments, 1) : cJSON_CreateObject());
   char *body = cJSON_PrintUnformatted(wire);
   cJSON_Delete(wire);
   cJSON *reply = NULL;
   if (body && fputs(body, to_fixture) >= 0 && fputc('\n', to_fixture) != EOF &&
       fflush(to_fixture) == 0)
   {
      char *line = NULL;
      size_t cap = 0;
      if (getline(&line, &cap, from_fixture) > 0)
         reply = cJSON_Parse(line);
      free(line);
   }
   free(body);
   pthread_mutex_unlock(&fixture_lock);
   return reply;
}
