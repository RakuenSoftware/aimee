/* Test-only JSON pipe to a Go runtime fixture; contains no module policy. */
#include "module_runtime_fixture.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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
   int status;
   assert(waitpid(fixture_pid, &status, 0) == fixture_pid);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void start_fixture(void)
{
   if (to_fixture)
      return;
   const char *exe = getenv("AIMEE_TEST_RUNTIME_FIXTURE");
   assert(exe && *exe);
   int in[2], out[2];
   assert(pipe(in) == 0 && pipe(out) == 0);
   fixture_pid = fork();
   assert(fixture_pid >= 0);
   if (fixture_pid == 0)
   {
      if (dup2(in[0], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0)
         _exit(126);
      close(in[0]);
      close(in[1]);
      close(out[0]);
      close(out[1]);
      execl(exe, exe, NULL);
      _exit(127);
   }
   close(in[0]);
   close(out[1]);
   to_fixture = fdopen(in[1], "w");
   from_fixture = fdopen(out[0], "r");
   assert(to_fixture && from_fixture);
   atexit(close_fixture);
}

int module_runtime_fixture_call(const cJSON *request, cJSON **reply)
{
   pthread_mutex_lock(&fixture_lock);
   start_fixture();
   char *body = cJSON_PrintUnformatted(request);
   assert(body && fputs(body, to_fixture) >= 0 && fputc('\n', to_fixture) != EOF &&
          fflush(to_fixture) == 0);
   free(body);
   char *line = NULL;
   size_t cap = 0;
   assert(getline(&line, &cap, from_fixture) > 0);
   *reply = cJSON_Parse(line);
   free(line);
   assert(*reply);
   pthread_mutex_unlock(&fixture_lock);
   return 1;
}
