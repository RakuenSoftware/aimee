/* store_module_fixture.c -- see store_module_fixture.h. */
#include "store_module_fixture.h"

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

/* The store's principal reference, and the kinds derived from it:
 * kind = 4096 + ref*256 + stage. Stages 1..19 are the served families, so the
 * block runs 11777..11795 inclusive. Kept as first/last rather than a list
 * because it IS a contiguous block by construction -- a gap would mean a family
 * lost its stage, which the Go contract test catches before this ever runs. */
#define STORE_PRINCIPAL_REF 30u
#define STORE_KIND_FIRST    11777u
#define STORE_KIND_LAST     11795u
/* The runtime family, whose availability means the module is serving. Any of
 * the block would do; this one is polled because every suite that needs the
 * fixture touches runtime state somewhere. */
#define STORE_KIND_RUNTIME 11783u

static pid_t g_module = -1;
static char g_root[256];

static void die(const char *what)
{
   fprintf(stderr, "store_module_fixture: %s (errno=%s)\n", what, strerror(errno));
   if (g_module > 0)
      kill(g_module, SIGKILL);
   abort();
}

static void run(const char *format, ...)
{
   char command[2048];
   va_list args;
   va_start(args, format);
   vsnprintf(command, sizeof(command), format, args);
   va_end(args);
   if (system(command) != 0)
      die(command);
}

int store_module_fixture_available(void)
{
   const char *dsn = getenv("AIMEE_STORE_URL");
   if (!dsn || !dsn[0])
   {
      printf("  SKIP: AIMEE_STORE_URL is unset; the store module needs a PostgreSQL\n");
      return 0;
   }

   /* THE DSN IS NOT SUFFICIENT, and answering on it alone was a yes to the wrong
    * question. The store module does not open PostgreSQL itself: storeBackend()
    * in cmd/aimee-module connects as principal 68 and reaches the database
    * through the postgres module's SQL stage, kind 11266. Nothing in this tree
    * serves that stage -- see docs/validation/store-module-on-a-clean-container.md
    * -- so the module exits before it attaches whatever the DSN names.
    *
    * This matters because start() ABORTS on failure, by design: once available()
    * has said yes, a failure there is a real fault. So every suite gated on this
    * died the moment anyone set AIMEE_STORE_URL, reporting "module exited before
    * it attached (check AIMEE_STORE_URL reaches the database)" -- which points at
    * the operator's DSN, and the DSN was fine.
    *
    * Found by setting the variable and running, not by reading. With it unset
    * every one of these suites skips, and a skip that would abort if taken is
    * indistinguishable from a skip that would pass.
    *
    * DELETE THIS BLOCK when the postgres SQL stage lands; the DSN check above is
    * the real one and stays. */
   printf("  SKIP: nothing serves kind 11266 (the postgres SQL stage) in this tree,\n"
          "        so the store module cannot attach whatever AIMEE_STORE_URL names\n");
   return 0;
}

void store_module_fixture_stop(void)
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

void store_module_fixture_start(void)
{
   if (g_module > 0)
      return;

   const char *source = getenv("AIMEE_TEST_MODULE_BIN");
   if (!source || !source[0])
      die("AIMEE_TEST_MODULE_BIN is unset; the make rule must name the module binary");
   const char *dsn = getenv("AIMEE_STORE_URL");
   if (!dsn || !dsn[0])
      die("AIMEE_STORE_URL is unset; call store_module_fixture_available() first");
   snprintf(g_root, sizeof(g_root), "%s/aimee-store-module-fixture-%d", platform_tmpdir(),
            (int)getpid());
   char policy[320], socket_path[256], executable[320];
   snprintf(policy, sizeof(policy), "%s/policy", g_root);
   snprintf(socket_path, sizeof(socket_path), "%s/bus.sock", g_root);
   snprintf(executable, sizeof(executable), "%s/aimee-module-store", g_root);
   run("mkdir -p '%s'", policy);

   /* A real file at this exact name, not a symlink: the module derives its
    * identity from argv[0]'s basename, and the runtime pins the peer's resolved
    * executable path against the grant. */
   run("cp '%s' '%s' && chmod 0755 '%s'", source, executable, executable);

   char grant_path[384];
   snprintf(grant_path, sizeof(grant_path), "%s/store.grant", policy);
   FILE *grant = fopen(grant_path, "w");
   if (!grant)
      die("open the grant manifest");
   fprintf(grant, "version=1\nprincipal_class=1\nprincipal_ref=%u\nuid=self\nexecutable=%s\nserve=",
           STORE_PRINCIPAL_REF, executable);
   for (unsigned kind = STORE_KIND_FIRST; kind <= STORE_KIND_LAST; kind++)
      fprintf(grant, "%s%u", kind == STORE_KIND_FIRST ? "" : ",", kind);
   fprintf(grant, "\n");
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
      /* atexit() below does not run when the test dies by a signal, and the
       * module would then outlive it as an orphan holding a socket in a
       * directory that has already been deleted. Ask the kernel to kill this
       * child when its parent goes away, whatever the reason. */
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() != parent)
         _exit(0); /* parent died in the window before the prctl call */
      execl(executable, executable, socket_path, (char *)NULL);
      _exit(127);
   }
   g_module = child;
   atexit(store_module_fixture_stop);

   /* Attachment is asynchronous; poll for the registration rather than
    * sleeping, so a module that died is noticed rather than waited out. The
    * budget is longer than the git fixture's: this one opens a connection pool
    * against a database that may be on another host. */
   for (int tick = 0; tick < 400; tick++)
   {
      if (obs_bus_module_available(STORE_KIND_RUNTIME))
         return;
      int status = 0;
      if (waitpid(child, &status, WNOHANG) == child)
      {
         g_module = -1;
         die("module exited before it attached (check AIMEE_STORE_URL reaches the database)");
      }
      struct timespec pause = {0, 50 * 1000 * 1000};
      nanosleep(&pause, NULL);
   }
   die("module never registered the runtime stage");
}
