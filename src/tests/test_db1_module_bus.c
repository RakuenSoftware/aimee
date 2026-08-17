/* test_db1_module_bus.c: the DB1 module, proven to actually serve.
 *
 * Every other DB1 test proves one half. The client suites stub the bus and
 * check the bytes; the stage suite calls the handler in-process and checks the
 * reply. Both passed while the real module process could not answer a single
 * request -- its generated main went straight to the runtime without ever
 * calling db1_init, so every domain function ran against a NULL connection and
 * returned "no database", which a read reports as MISSING and a caller reads as
 * "there is no such row".
 *
 * That is the gap a halves-only proof leaves: the composition was never
 * exercised. This fixture closes it. It stands up the daemon's module endpoint,
 * execs the REAL module binary against it, and drives the ordinary generated
 * clients -- the same functions the daemon links -- then asserts the data came
 * back rather than an absence.
 *
 * A failure to start the module is a failure of this test, never a skip: a skip
 * would retire the only coverage the composition has.
 */
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <aimee/audit/obs_bus.h>

#include "cognify_jobs.h"
#include "db1_module_api.h"
#include "git_ownership.h"
#include "platform_test_util.h"
#include "wm.h"

static char g_tmp[512];
static pid_t g_module = -1;

static void must(int condition, const char *what)
{
   if (condition)
      return;
   fprintf(stderr, "db1-module-bus: FAILED to %s (errno=%d)\n", what, errno);
   if (g_module > 0)
      kill(g_module, SIGKILL);
   exit(1);
}

static void write_grant(const char *policy_dir, const char *executable)
{
   char path[640];
   snprintf(path, sizeof(path), "%s/db1.grant", policy_dir);
   FILE *file = fopen(path, "w");
   must(file != NULL, "open the grant manifest");
   /* Every declared stage, not a convenient subset: an ungranted stage and an
    * unserved one are indistinguishable to a caller, so granting only what this
    * test happens to call would hide a stage that never registered. */
   fprintf(file,
           "version=1\nprincipal_class=1\nprincipal_ref=30\nuid=self\n"
           "executable=%s\nserve=%u,%u,%u,%u\n",
           executable, AIMEE_DB1_EVENT_ECONOMIZER_STATE, AIMEE_DB1_EVENT_GIT_OWNERSHIP,
           AIMEE_DB1_EVENT_CONVERSATION, AIMEE_DB1_EVENT_AGENT_WORK);
   must(fclose(file) == 0, "write the grant manifest");
}

static void start_module(const char *executable, const char *socket_path, const char *database)
{
   pid_t parent = getpid();
   pid_t child = fork();
   must(child >= 0, "fork the module");
   if (child == 0)
   {
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() != parent)
         _exit(0);
      /* The module is told which database to open. It cannot read the daemon's
         configuration, and deriving a default would let it serve a different
         store than the one the caller means. */
      setenv("AIMEE_DB1_PATH", database, 1);
      execl(executable, executable, socket_path, (char *)NULL);
      _exit(127);
   }
   g_module = child;

   for (int tick = 0; tick < 200; tick++)
   {
      if (obs_bus_module_available(AIMEE_DB1_EVENT_AGENT_WORK))
         return;
      int status = 0;
      if (waitpid(child, &status, WNOHANG) == child)
      {
         g_module = -1;
         must(0, "module exited before it attached");
      }
      struct timespec pause = {0, 50 * 1000 * 1000};
      nanosleep(&pause, NULL);
   }
   must(0, "module never registered its stages");
}

static void stop_module(void)
{
   if (g_module <= 0)
      return;
   kill(g_module, SIGTERM);
   waitpid(g_module, NULL, 0);
   g_module = -1;
}

/* A write followed by a read, both across the bus. This is the assertion the
   halves could not make: that what one call stored, another call finds. With no
   database open both calls "succeed" in the sense of returning, and the read
   reports nothing -- which is why the failure was invisible. */
static void test_a_write_is_visible_to_a_later_read(void)
{
   must(db1_wm_set("sess-bus", "alpha", "stored over the bus", "notes", 0) == 0,
        "store a working-memory entry through the module");

   wm_entry_t entry;
   memset(&entry, 0, sizeof entry);
   must(db1_wm_get("sess-bus", "alpha", &entry) == 0, "read the entry back");
   must(strcmp(entry.value, "stored over the bus") == 0, "read the value that was written");
   printf("  PASS: a write is visible to a later read\n");
}

/* Rows cross whole. A list that came back empty would be indistinguishable from
   a store that was never opened, so the count and the contents are both checked. */
static void test_rows_cross_the_bus(void)
{
   must(db1_wm_set("sess-rows", "one", "first", "notes", 0) == 0, "store the first row");
   must(db1_wm_set("sess-rows", "two", "second", "notes", 0) == 0, "store the second row");

   wm_entry_t rows[8];
   int found = db1_wm_list("sess-rows", "", rows, 8);
   must(found == 2, "list both rows");
   must(rows[0].key[0] != '\0' && rows[1].key[0] != '\0', "rows carry their members");
   printf("  PASS: rows cross the bus\n");
}

/* The queue's status takes no arguments at all, and a claim distinguishes an
   empty queue from a broken one. Against an unopened store the claim returns
   -1, so "empty" and "broken" would be the same answer here too. */
static void test_the_queue_answers_across_the_bus(void)
{
   db1_cognify_job_t job;
   must(db1_cognify_job_claim_next(&job) == 0, "an empty queue is nothing to claim, not an error");

   must(db1_cognify_job_enqueue(4294967297LL) == 0, "enqueue a job");

   db1_cognify_job_stats_t stats;
   memset(&stats, 0, sizeof stats);
   must(db1_cognify_job_status(&stats) == 0, "read the queue status with no arguments");
   must(stats.pending == 1, "the enqueued job is pending");

   must(db1_cognify_job_claim_next(&job) == 1, "claim the job that is there");
   must(job.memory_id == 4294967297LL, "the 64-bit id survived the crossing");
   printf("  PASS: the queue answers across the bus\n");
}

/* Branch ownership was the first domain migrated and has been reached over the
   bus for the longest. It is checked here because it is the one whose silent
   failure is a safety property: a caller told "nobody owns this branch" will
   take a branch somebody else holds. */
static void test_branch_ownership_round_trips(void)
{
   must(db1_git_ownership_upsert("/repo/bus", "feature", "sess-owner") == 0,
        "record a branch owner");
   char owner[128] = {0};
   must(db1_git_ownership_get_owner("/repo/bus", "feature", owner, sizeof owner) == 1,
        "find the owner that was just recorded");
   must(strcmp(owner, "sess-owner") == 0, "the owner is the session that took it");
   printf("  PASS: branch ownership round trips\n");
}

int main(int argc, char **argv)
{
   /* The suite runs its binaries with no arguments, so default to where the
      Makefile builds the module rather than needing a bespoke invocation --
      a test only reachable by a hand-written command is a test CI does not
      run, which is how the composition went unchecked in the first place. */
   const char *module = (argc >= 2) ? argv[1] : "build/obj/aimee-module-db1";
   must(access(module, X_OK) == 0, "find the module binary (pass its path, or build it)");

   snprintf(g_tmp, sizeof(g_tmp), "%s/aimee-db1-bus-%d", platform_tmpdir(), (int)getpid());
   char policy[640], socket_path[600], executable[640], database[640];
   snprintf(policy, sizeof(policy), "%s/policy", g_tmp);
   snprintf(socket_path, sizeof(socket_path), "%s/bus.sock", g_tmp);
   snprintf(executable, sizeof(executable), "%s/aimee-module-db1", g_tmp);
   snprintf(database, sizeof(database), "%s/aimee.db", g_tmp);

   char command[1400];
   snprintf(command, sizeof(command), "mkdir -p '%s' '%s'", g_tmp, policy);
   must(system(command) == 0, "create the fixture directories");
   snprintf(command, sizeof(command), "cp '%s' '%s' && chmod 0755 '%s'", module, executable,
            executable);
   must(system(command) == 0, "install the module binary");

   write_grant(policy, executable);
   must(obs_bus_configure_module_runtime(socket_path, policy) == 0,
        "configure the module endpoint");
   must(obs_bus_start() == 0, "start the bus");

   start_module(executable, socket_path, database);

   test_a_write_is_visible_to_a_later_read();
   test_rows_cross_the_bus();
   test_the_queue_answers_across_the_bus();
   test_branch_ownership_round_trips();

   stop_module();
   obs_bus_stop();
   snprintf(command, sizeof(command), "rm -rf '%s'", g_tmp);
   (void)system(command);
   printf("db1-module-bus: ok (the module serves what the daemon asks it for)\n");
   return 0;
}
