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
#include "module_json_call.h" /* aimee_module_call_deadline_ns */

/* The store's principal reference, and the kinds derived from it:
 * kind = 4096 + ref*256 + stage. Stages 1..19 are the served families, so the
 * block runs 11777..11795 inclusive. Kept as first/last rather than a list
 * because it IS a contiguous block by construction -- a gap would mean a family
 * lost its stage, which the Go contract test catches before this ever runs. */
#define STORE_PRINCIPAL_REF 30u
/* The store's OUTBOUND principal, and the postgres module it calls. */
#define STORE_CLIENT_REF 69u
#define PG_PRINCIPAL_REF 28u
#define PG_KIND_HEALTH   11265u
#define PG_KIND_SQL      11266u
#define STORE_KIND_FIRST 11777u
#define STORE_KIND_LAST  11795u
/* The runtime family, whose availability means the module is serving. Any of
 * the block would do; this one is polled because every suite that needs the
 * fixture touches runtime state somewhere. */
#define STORE_KIND_RUNTIME 11783u

static pid_t g_module = -1;
/* The postgres module, which the store calls to reach the database. Two
 * processes, because they are two principals: the store serves aimee's
 * nineteen kinds at ref 30 and asks for SQL as ref 69, and postgres serves
 * health and SQL at ref 28. Running them as one process would collapse a
 * boundary the grants exist to hold. */
static pid_t g_postgres = -1;
static char g_root[256];

static void die(const char *what)
{
   fprintf(stderr, "store_module_fixture: %s (errno=%s)\n", what, strerror(errno));
   if (g_module > 0)
      kill(g_module, SIGKILL);
   if (g_postgres > 0)
      kill(g_postgres, SIGKILL);
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

   return 1;
}

void store_module_fixture_stop(void)
{
   if (g_module > 0)
   {
      kill(g_module, SIGTERM);
      waitpid(g_module, NULL, 0);
      g_module = -1;
   }
   /* The store first, then what it calls: stopping postgres underneath a live
    * store would have the store log a lost backend on its way out, which reads
    * like a fault in the thing being torn down. */
   if (g_postgres > 0)
   {
      kill(g_postgres, SIGTERM);
      waitpid(g_postgres, NULL, 0);
      g_postgres = -1;
   }
   if (g_root[0])
   {
      char command[512];
      snprintf(command, sizeof(command), "rm -rf '%s'", g_root);
      (void)system(command);
      g_root[0] = '\0';
   }
}

/* The SQL stage's request frame: op, a statement id, a transaction handle (0
 * for "on the pool"), the statement, and an argument count. Little-endian, the
 * same encoding server-go/modules/postgres/sql.go reads. */
#define PG_STAGE_SQL 2u
#define PG_OP_EXEC   1u

static void put_u32(unsigned char *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (unsigned char)((v >> (i * 8u)) & 0xffu);
}

static void store_module_fixture_reset_schema(void)
{
   static const char *const id = "fixture-reset";
   /* CASCADE because the store's tables reference each other; recreating the
    * schema immediately afterwards leaves the database as initdb left it. */
   static const char *const sql = "DROP SCHEMA public CASCADE; CREATE SCHEMA public";

   unsigned char frame[256];
   uint32_t at = 0;
   put_u32(frame + at, PG_OP_EXEC);
   at += 4u;
   put_u32(frame + at, (uint32_t)strlen(id));
   at += 4u;
   memcpy(frame + at, id, strlen(id));
   at += (uint32_t)strlen(id);
   memset(frame + at, 0, 8); /* handle 0: not inside a transaction */
   at += 8u;
   put_u32(frame + at, (uint32_t)strlen(sql));
   at += 4u;
   memcpy(frame + at, sql, strlen(sql));
   at += (uint32_t)strlen(sql);
   put_u32(frame + at, 0u); /* no bound arguments */
   at += 4u;

   unsigned char reply[512];
   uint32_t reply_len = 0;
   aimee_module_call_result_t rc =
       obs_bus_module_call(PG_KIND_SQL, PG_STAGE_SQL, 0, aimee_module_call_deadline_ns(30000),
                           frame, at, reply, (uint32_t)sizeof reply, &reply_len, NULL, NULL);
   if (rc != AIMEE_MODULE_CALL_OK)
   {
      char why[128];
      snprintf(why, sizeof why, "the SQL stage refused the schema reset (result %d)", (int)rc);
      die(why);
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
   char pg_executable[320];
   snprintf(pg_executable, sizeof(pg_executable), "%s/aimee-module-postgres", g_root);
   run("mkdir -p '%s'", policy);

   /* Real files at these exact names, not symlinks: the module is a multicall
    * binary that derives its identity from argv[0]'s basename, and the runtime
    * pins the peer's resolved executable path against the grant. */
   run("cp '%s' '%s' && chmod 0755 '%s'", source, executable, executable);
   run("cp '%s' '%s' && chmod 0755 '%s'", source, pg_executable, pg_executable);

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

   /* THE STORE'S OUTBOUND GRANT, separate from the one above and easy to
    * forget. The store serves at ref 30 and CALLS at ref 69, and the bus judges
    * each attachment on its own: without this the module serves its nineteen
    * kinds and is refused the moment it asks postgres for SQL, which reads as
    * the database being unreachable rather than as a missing grant.
    *
    * request=, not serve=. A client that served anything would be a second
    * server on kinds it does not own. */
   snprintf(grant_path, sizeof(grant_path), "%s/store-client.grant", policy);
   grant = fopen(grant_path, "w");
   if (!grant)
      die("open the store client grant");
   fprintf(grant,
           "version=1\nprincipal_class=1\nprincipal_ref=%u\nuid=self\nexecutable=%s\n"
           "request=%u\n",
           STORE_CLIENT_REF, executable, PG_KIND_SQL);
   if (fclose(grant) != 0)
      die("write the store client grant");

   /* And the postgres module itself, which owns the connection and the DSN. */
   snprintf(grant_path, sizeof(grant_path), "%s/postgres.grant", policy);
   grant = fopen(grant_path, "w");
   if (!grant)
      die("open the postgres grant");
   fprintf(grant,
           "version=1\nprincipal_class=1\nprincipal_ref=%u\nuid=self\nexecutable=%s\n"
           "serve=%u,%u\n",
           PG_PRINCIPAL_REF, pg_executable, PG_KIND_HEALTH, PG_KIND_SQL);
   if (fclose(grant) != 0)
      die("write the postgres grant");

   if (obs_bus_configure_module_runtime(socket_path, policy) != 0)
      die("configure the module endpoint");
   if (obs_bus_start() != 0)
      die("start the bus");

   pid_t parent = getpid();

   /* POSTGRES FIRST, and waited for. The store exits when it cannot reach the
    * SQL stage -- that is its correct behaviour, not a race to be slept
    * through -- so starting it against a module that is not yet serving would
    * fail intermittently and look like flakiness in whatever suite ran it. */
   pid_t pg = fork();
   if (pg < 0)
      die("fork the postgres module");
   if (pg == 0)
   {
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() != parent)
         _exit(0);
      execl(pg_executable, pg_executable, socket_path, (char *)NULL);
      _exit(127);
   }
   g_postgres = pg;
   atexit(store_module_fixture_stop);
   for (int tick = 0;; tick++)
   {
      if (obs_bus_module_available(PG_KIND_SQL))
         break;
      int status = 0;
      if (waitpid(pg, &status, WNOHANG) == pg)
      {
         g_postgres = -1;
         die("the postgres module exited before it served SQL "
             "(check AIMEE_STORE_URL reaches the database)");
      }
      if (tick >= 400)
         die("the postgres module never served the SQL stage");
      struct timespec pause = {0, 50 * 1000 * 1000};
      nanosleep(&pause, NULL);
   }

   /* Empty the database before the store reads it, so every run starts where
      db1_init(":memory:") used to leave these suites. */
   store_module_fixture_reset_schema();

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
