/* git_module_fixture.c -- see git_module_fixture.h. */
#include "git_module_fixture.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../platform_test_util.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/git/module_api.h>

static pid_t g_module = -1;
static char g_root[256];

static void die(const char *what)
{
   fprintf(stderr, "git_module_fixture: %s (errno=%s)\n", what, strerror(errno));
   if (g_module > 0)
      kill(g_module, SIGKILL);
   abort();
}

static void run(const char *format, ...)
{
   char command[1024];
   va_list args;
   va_start(args, format);
   vsnprintf(command, sizeof(command), format, args);
   va_end(args);
   if (system(command) != 0)
      die(command);
}

void git_module_fixture_stop(void)
{
   if (g_module > 0)
   {
      kill(g_module, SIGTERM);
      waitpid(g_module, NULL, 0);
      g_module = -1;
   }
   if (g_root[0])
   {
      char command[512];
      snprintf(command, sizeof(command), "rm -rf '%s'", g_root);
      (void)system(command);
      g_root[0] = '\0';
   }
}

void git_module_fixture_start(void)
{
   if (g_module > 0)
      return;

   const char *source = getenv("AIMEE_TEST_MODULE_BIN");
   if (!source || !source[0])
      die("AIMEE_TEST_MODULE_BIN is unset; the make rule must build and name the module binary");

   snprintf(g_root, sizeof(g_root), "%s/aimee-git-module-fixture-%d", platform_tmpdir(),
            (int)getpid());
   char policy[320], socket_path[256], executable[320];
   snprintf(policy, sizeof(policy), "%s/policy", g_root);
   snprintf(socket_path, sizeof(socket_path), "%s/bus.sock", g_root);
   snprintf(executable, sizeof(executable), "%s/aimee-module-git", g_root);
   run("mkdir -p '%s'", policy);

   /* A real file at this exact name, not a symlink: the module derives its
    * identity from argv[0]'s basename, and the runtime pins the peer's
    * resolved executable path against the grant. */
   run("cp '%s' '%s' && chmod 0755 '%s'", source, executable, executable);

   char grant_path[384];
   snprintf(grant_path, sizeof(grant_path), "%s/git.grant", policy);
   FILE *grant = fopen(grant_path, "w");
   if (!grant)
      die("open the grant manifest");
   fprintf(grant,
           "version=1\nprincipal_class=1\nprincipal_ref=13\nuid=self\n"
           "executable=%s\nserve=%u,%u,%u,%u\n",
           executable, AIMEE_GIT_EVENT_OPERATION, AIMEE_GIT_EVENT_REF_VALIDATE,
           AIMEE_GIT_EVENT_CI_GRADE, AIMEE_GIT_EVENT_VERIFY_RUN);
   if (fclose(grant) != 0)
      die("write the grant manifest");

   if (obs_bus_configure_module_runtime(socket_path, policy) != 0)
      die("configure the module endpoint");
   if (obs_bus_start() != 0)
      die("start the bus");

   pid_t parent = getpid();
   pid_t child = fork();
   if (child < 0)
      die("fork the module");
   if (child == 0)
   {
      /* atexit() below does not run when the test dies by a signal -- an
       * assert(), a segfault, or the runner killing a hung job -- and the
       * module then outlives it as an orphan, holding a socket in a directory
       * that has already been deleted. Measured on the development box: four
       * of these had been running for up to 3.9 DAYS. Ask the kernel to kill
       * this child when its parent goes away, whatever the reason. */
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() != parent)
         _exit(0); /* parent died in the window before the prctl call */
      execl(executable, executable, socket_path, (char *)NULL);
      _exit(127);
   }
   g_module = child;
   atexit(git_module_fixture_stop);

   /* Attachment is asynchronous; poll for the registration rather than
    * sleeping, so a module that died is noticed rather than waited out. */
   for (int tick = 0; tick < 200; tick++)
   {
      if (obs_bus_module_available(AIMEE_GIT_EVENT_VERIFY_RUN))
         return;
      int status = 0;
      if (waitpid(child, &status, WNOHANG) == child)
      {
         g_module = -1;
         die("module exited before it attached");
      }
      struct timespec pause = {0, 50 * 1000 * 1000};
      nanosleep(&pause, NULL);
   }
   die("module never registered the verify stage");
}
