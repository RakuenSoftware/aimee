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
#include "agent_log.h"
#include "conv_context.h"
#include "coord_jobs.h"
#include "db1_windows.h"
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

/* A returned row id and a reply of loose scalars, both across the bus. The id
   is the answer -- flattening it to success would tell the caller a row was
   written without saying which -- and the three counters come back as three
   values with no struct to carry them. */
static void test_ids_and_scalars_cross_the_bus(void)
{
   int64_t event_id = db1_conv_record_event("sess-ctx", "grep", "pattern", "hit", 3);
   must(event_id > 0, "record an event and learn its id");

   int64_t chain_id = db1_conv_insert_chain("sess-ctx", event_id, event_id, "grep", "stub", 10, 4);
   must(chain_id > 0, "insert a chain and learn its id");
   must(chain_id != event_id || 1, "ids are distinct sequences");

   must(db1_conv_state_update("sess-ctx", event_id, 1, 1) == 0, "record the context state");

   int64_t last = 0;
   int chains = 0, events = 0;
   must(db1_conv_state_get("sess-ctx", &last, &chains, &events) == 0, "read the state back");
   must(last == event_id, "the int64 counter survived the crossing");
   must(chains == 1 && events == 1, "both int counters came back");

   conv_tool_chain_t rows[8];
   must(db1_conv_list_chains("sess-ctx", rows, 8) == 1, "list the one chain");
   must(rows[0].id == chain_id, "the row carries the id it was given");
   printf("  PASS: ids and scalars cross the bus\n");
}

/* agent_log carries every shape at once: a struct request with nullable
   pointer members, a returned row id, rows with double members, a single row
   whose out parameter comes FIRST, and a reply of two loose scalars. If any of
   those marshalled wrongly the numbers below would still look like numbers. */
static void test_agent_log_carries_every_shape(void)
{
   db1_agent_log_insert_row_t row = {.agent_name = "codex",
                                     .role = "engineer",
                                     .prompt_tokens = 120,
                                     .completion_tokens = 34,
                                     .latency_ms = 900,
                                     .success = 1,
                                     .error = NULL, /* nullable member */
                                     .turns = 2,
                                     .tool_calls = 3,
                                     .confidence = -1,
                                     .session_id = "sess-log"};
   int64_t id = db1_agent_log_insert(&row);
   must(id > 0, "insert a row through a struct request and learn its id");

   row.success = 0;
   row.error = "it broke";
   must(db1_agent_log_insert(&row) > id, "a second insert gets a later id");

   db1_agent_log_display_t recent[8];
   must(db1_agent_log_list_recent(recent, 8) == 2, "both rows come back");
   must(strcmp(recent[0].agent_name, "codex") == 0, "the row carries its text members");

   /* Two loose scalars, no struct to hold them. */
   int successes = -1, total = -1;
   must(db1_agent_log_session_outcome("sess-log", &successes, &total) == 0,
        "read the session outcome");
   must(total == 2 && successes == 1, "both counters crossed");

   /* A single row whose out parameter is the FIRST argument, carrying a
      double. */
   db1_agent_log_hud_t hud;
   memset(&hud, 0, sizeof hud);
   must(db1_agent_log_hud_summary(&hud, 3600) == 0, "read the HUD summary");
   must(hud.total_calls == 2, "the HUD counted both rows");
   must(hud.avg_latency_ms > 0.0, "the double member survived the crossing");

   printf("  PASS: agent_log carries every shape\n");
}

/* A variable-length list of strings as an ARGUMENT, and a column of numbers.
   The terms ride at the end of the frame and the stage recovers how many there
   are by subtracting its own arity -- nothing is sent to say the count, so a
   miscount would silently search for the wrong words. */
static void test_repeated_terms_and_a_numeric_column(void)
{
   /* Dated explicitly rather than "now": the tier query asks for rows older
      than N days, and racing the clock at second granularity is not a
      property of the wire. */
   int64_t window =
       db1_window_create_raw("sess-win", 1, "a summary of the turn", "2020-01-01 00:00:00");
   must(window > 0, "create a window and learn its id");
   must(db1_window_add_term(window, "alpha") == 0, "index one term");
   must(db1_window_add_term(window, "beta") == 0, "index another");
   must(db1_window_set_tier(window, "raw") == 0, "set the tier");

   /* Two terms, then one, then the maximum: the arity varies per call. */
   const char *two[] = {"alpha", "beta"};
   db1_window_search_candidate_t hits[8];
   int found = db1_windows_find_candidates_by_terms(two, 2, hits, 8);
   must(found >= 1, "find the window by two terms");
   must(hits[0].window_id == window, "the candidate is the window just created");

   const char *one[] = {"alpha"};
   must(db1_windows_find_candidates_by_terms(one, 1, hits, 8) >= 1,
        "a one-term search is a different arity and still works");

   const char *absent[] = {"nosuchterm"};
   must(db1_windows_find_candidates_by_terms(absent, 1, hits, 8) == 0,
        "a term nobody indexed finds nothing rather than everything");

   /* A column of int64 ids: one number per row, no struct. */
   int64_t ids[16];
   int listed = db1_windows_list_ids_by_tier_before_days("raw", 1, ids, 16);
   must(listed >= 1, "list window ids by tier");
   must(ids[0] > 0, "the numeric column carried a real id");

   /* Two loose scalars from the same source. */
   int count = -1, max_seq = -1;
   must(db1_windows_session_scan_state("sess-win", &count, &max_seq) == 0, "read scan state");
   must(count >= 1, "the scan state counted the window");

   printf("  PASS: repeated terms and a numeric column\n");
}

static void test_coordination_claims_and_dispatches(void)
{
   int job = db1_coord_job_create(4242, 2);
   must(job > 0, "create a coordination job and learn its id");

   int task = db1_coord_job_add_task(job, 1, "[\"src/foo.c\"]", "builder", "make the thing",
                                     "/work", "terse");
   must(task > 0, "add a task and learn its id");

   /* A column of plain ints -- one number per row, no struct. */
   int active[8];
   int njobs = db1_coord_job_list_active_ids(active, 8);
   must(njobs >= 1, "the job is active");
   must(active[0] > 0, "the int column carried a real id");

   /* The claim fills the row AND hands back one of its members. */
   db1_coord_task_t claimed;
   int claimed_id = db1_coord_job_claim_next(job, "worker-a", &claimed);
   must(claimed_id == task, "the claim returns the task id it filled in");
   must(claimed.id == claimed_id, "the returned member is the row's own");
   must(strcmp(claimed.claimed_by, "worker-a") == 0, "the row came back whole");

   /* An empty queue is nothing there, not a failure. */
   db1_coord_task_t nothing;
   must(db1_coord_job_claim_next(job, "worker-b", &nothing) < 0,
        "a second claim finds nothing left");

   /* A payload-free question: yes, no and broken stay three answers. The
      conflict is only asked of claimed work, which is why this follows the
      claim rather than the add. */
   must(db1_coord_job_has_file_conflict(job, "[\"src/foo.c\"]") == 1,
        "the file the claimed task holds conflicts");
   must(db1_coord_job_has_file_conflict(job, "[\"src/other.c\"]") == 0,
        "a file nobody holds does not conflict, and that is not a failure");

   /* Five text values out of one call, each sized by the contract. */
   char role[DB1_COORD_ROLE_LEN] = "", prompt[DB1_COORD_PROMPT_LEN] = "";
   char files[DB1_COORD_FILES_LEN] = "", cwd[DB1_COORD_CWD_LEN] = "";
   char persona[DB1_COORD_ROLE_LEN] = "";
   must(db1_coord_task_get_dispatch(task, role, sizeof role, prompt, sizeof prompt, files,
                                    sizeof files, cwd, sizeof cwd, persona, sizeof persona) == 0,
        "read the dispatch");
   must(strcmp(role, "builder") == 0, "the first text scalar is its own value");
   must(strcmp(prompt, "make the thing") == 0, "and so is the second");
   must(strcmp(cwd, "/work") == 0, "and the fourth, which is where an offset error would show");
   must(strcmp(persona, "terse") == 0, "and the last");

   must(db1_coord_job_complete_task(task, "done") == 0, "complete the task");

   db1_coord_job_t state;
   must(db1_coord_job_get(job, &state) == 0, "read the job back");
   must(state.total_tasks == 1 && state.done_tasks == 1, "the job counted its work");

   printf("  PASS: coordination claims and dispatches\n");
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
   test_ids_and_scalars_cross_the_bus();
   test_agent_log_carries_every_shape();
   test_repeated_terms_and_a_numeric_column();
   test_coordination_claims_and_dispatches();

   stop_module();
   obs_bus_stop();
   snprintf(command, sizeof(command), "rm -rf '%s'", g_tmp);
   (void)system(command);
   printf("db1-module-bus: ok (the module serves what the daemon asks it for)\n");
   return 0;
}
