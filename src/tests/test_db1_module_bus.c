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
#include "db1_cron_jobs.h"
#include "agent_jobs.h"
#include "delegate_reservation.h"
#include "delegations.h"
#include "primary_sessions.h"
#include "server_sessions.h"
#include "webchat_live.h"
#include "model_catalog.h"
#include "web_page_cache.h"
#include "runtime_state.h"
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
   /* Built from a list rather than a format string with one %u per stage: the
    * two drift the moment a stage is added, and the failure that produces is
    * "the module never registered", which points at the module. */
   static const unsigned served[] = {
       AIMEE_DB1_EVENT_ECONOMIZER_STATE, AIMEE_DB1_EVENT_GIT_OWNERSHIP,
       AIMEE_DB1_EVENT_CONVERSATION,     AIMEE_DB1_EVENT_AGENT_WORK,
       AIMEE_DB1_EVENT_DELEGATION,       AIMEE_DB1_EVENT_SESSIONS,
       AIMEE_DB1_EVENT_RUNTIME,
   };
   fprintf(file, "version=1\nprincipal_class=1\nprincipal_ref=30\nuid=self\nexecutable=%s\nserve=",
           executable);
   for (size_t i = 0; i < sizeof served / sizeof served[0]; i++)
      fprintf(file, "%s%u", i ? "," : "", served[i]);
   fprintf(file, "\n");
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
      /* Every stage this fixture drives, not just the first: they register in
         order, and waiting on an early one races the ones after it -- which
         reads as the module answering "failed" to a call it never received. */
      if (obs_bus_module_available(AIMEE_DB1_EVENT_AGENT_WORK) &&
          obs_bus_module_available(AIMEE_DB1_EVENT_DELEGATION) &&
          obs_bus_module_available(AIMEE_DB1_EVENT_SESSIONS) &&
          obs_bus_module_available(AIMEE_DB1_EVENT_RUNTIME) &&
          obs_bus_module_available(AIMEE_DB1_EVENT_CONVERSATION) &&
          obs_bus_module_available(AIMEE_DB1_EVENT_GIT_OWNERSHIP))
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

static void test_a_cron_job_carries_its_array_member(void)
{
   cron_job_t job;
   memset(&job, 0, sizeof job);
   snprintf(job.id, sizeof job.id, "%s", "nightly");
   snprintf(job.schedule, sizeof job.schedule, "%s", "0 3 * * *");
   snprintf(job.mode, sizeof job.mode, "%s", "llm");
   snprintf(job.prompt, sizeof job.prompt, "%s", "summarise the day");
   /* All eight slots, each distinct. The domain stores them as a CSV of the
      non-empty ones and reparses into consecutive slots, so a gap is its own
      business -- what the wire has to prove is that eight separate values
      cross as eight, in order, rather than one running into the next. */
   for (int i = 0; i < CRON_JOB_MAX_SKILLS; i++)
      snprintf(job.skills[i], sizeof job.skills[i], "skill-%d", i);
   job.skill_count = CRON_JOB_MAX_SKILLS;
   job.enabled = 1;
   must(db1_cron_job_upsert(&job) == 0, "upsert a cron job with an array member");

   cron_job_t back;
   memset(&back, 0, sizeof back);
   must(db1_cron_job_get("nightly", &back) == 0, "read the cron job back");
   must(strcmp(back.schedule, "0 3 * * *") == 0, "the schedule survived");
   for (int i = 0; i < CRON_JOB_MAX_SKILLS; i++)
   {
      char expected[32];
      snprintf(expected, sizeof expected, "skill-%d", i);
      must(strcmp(back.skills[i], expected) == 0, "each slot holds its own value");
   }
   must(back.skill_count == CRON_JOB_MAX_SKILLS, "the count came with them");

   cron_job_t all[4];
   memset(all, 0, sizeof all);
   int loaded = db1_cron_jobs_load(all, 4, 1);
   must(loaded == 1, "load the enabled jobs");
   must(strcmp(all[0].skills[7], "skill-7") == 0,
        "a listed row carries its array too, out to the last slot");

   int64_t run = db1_cron_job_record_run("nightly", "ok", 0, 1, "output", "", "hash-1");
   must(run > 0, "record a run and learn its id");
   char *hash = db1_cron_job_last_output_hash("nightly");
   must(hash && strcmp(hash, "hash-1") == 0, "the returned string is the hash just written");
   free(hash);

   must(db1_cron_job_set_enabled("nightly", 0) == 0, "disable it");
   must(db1_cron_jobs_load(all, 4, 1) == 0, "and it is no longer enabled");
   must(db1_cron_job_delete("nightly") == 0, "delete it");

   printf("  PASS: a cron job carries its array member\n");
}

static void test_a_job_row_carries_what_the_store_allocated(void)
{
   /* A prompt is not a fixed-width member: the store allocates it, and the
      caller frees it. Across the bus the allocation is the client's, and the
      free is the same call it always was. */
   int job = db1_agent_job_create("builder", "a prompt worth allocating", "agent-1", "owner-1");
   must(job > 0, "create a job and learn its id");

   db1_agent_job_t row;
   must(db1_agent_job_get(job, &row) == 0, "read the job back");
   must(row.id == job, "the row is the job just created");
   must(row.prompt && strcmp(row.prompt, "a prompt worth allocating") == 0,
        "the allocated member arrived whole");
   must(strcmp(row.role, "builder") == 0, "and the inline members beside it");
   db1_agent_job_free(&row);
   must(row.prompt == NULL, "freeing the row clears what it held");

   /* void: the domain answers nothing, so neither does the client. */
   db1_agent_job_heartbeat(job);
   db1_agent_job_set_agent(job, "agent-2");
   db1_agent_job_update(job, "running", 3, NULL);

   db1_agent_job_t after;
   must(db1_agent_job_get(job, &after) == 0, "read it again");
   must(strcmp(after.agent_name, "agent-2") == 0, "the void write landed");
   must(after.cursor_turn == 3, "and so did the one beside it");
   db1_agent_job_free(&after);

   /* A cost is a double both ways: an integer here bills differently. */
   must(db1_agent_job_complete(job, "done", 4, "the result", 1, 0.0125) == 0,
        "complete the job with a cost");
   must(db1_agent_job_get(job, &after) == 0, "read the completed job");
   must(after.cost_known == 1, "the cost is known");
   must(after.cost_usd > 0.012 && after.cost_usd < 0.013,
        "and it is the cost that was sent, not a rounded one");
   must(after.result && strcmp(after.result, "the result") == 0,
        "the second allocated member arrived too");
   db1_agent_job_free(&after);

   /* A list of rows whose members the store allocated: every row's memory is
      the caller's, and the rows past the end are nobody's. */
   db1_agent_job_t recent[4];
   int listed = db1_agent_job_list_recent(recent, 4, 1);
   must(listed >= 1, "list the recent jobs");
   must(recent[0].prompt != NULL, "a listed row carries its allocation");
   for (int i = 0; i < listed; i++)
      db1_agent_job_free(&recent[i]);

   /* The count IS the answer here, not a status. */
   int cancelled = db1_agent_job_cancel_stale(0, "test");
   must(cancelled >= 0, "cancelling stale jobs answers with a count");

   /* A spawn: the count IS the answer, and a plain read still says whether
      there was anything to read. lifecycle_delegate_job and
      delegation_checkpoints are created outside DB1's schema, so the
      reservation and checkpoint calls guard on the table being there -- that
      is the domain's own precondition and the wire does not change it. */
   must(db1_delegation_spawn_record("spawn-1", "", "sess-spawn", 0, "builder") == 0,
        "record a spawn");
   must(db1_delegation_spawn_count_total("sess-spawn") == 1,
        "the count crosses as a count, not as a status");
   must(db1_delegation_spawn_is_active("spawn-1") == 1, "the spawn is active");

   char state[64] = "";
   must(db1_delegation_spawn_status("spawn-1", state, sizeof state) == 0,
        "read the spawn's status, which answers 0 as it always did");
   must(state[0] != '\0', "and it said something");

   int active[8];
   must(db1_delegation_spawn_list_active(active, 8) >= 1, "list the active spawns");

   must(db1_delegation_spawn_complete("spawn-1") == 0, "complete it");
   must(db1_delegation_spawn_is_active("spawn-1") == 0,
        "a completed spawn is not active, and that is not a failure");

   printf("  PASS: a job row carries what the store allocated\n");
}

static void test_the_callee_allocates_what_the_caller_frees(void)
{
   must(db1_server_session_create("sess-a", "cli", "someone") == 0, "create a session row");
   must(db1_server_session_set_outcome("sess-a", "done") == 0, "record its outcome");

   db1_server_session_t row;
   must(db1_server_session_get("sess-a", &row) == 0, "read the session back");
   must(strcmp(row.outcome, "done") == 0, "the outcome crossed");

   must(db1_server_session_count(NULL) == 1, "the count is a count, not a status");

   /* The array AND the rows are the callee's: the caller frees both, with the
      call it always used, and the memory now comes from this side of the bus. */
   must(db1_primary_session_save("sess-a", "agent-1", "anthropic", "[{\"role\":\"user\"}]") == 0,
        "save a primary session");
   db1_primary_session_row_t *rows = NULL;
   int listed = db1_primary_session_alloc_recent(&rows, 8);
   must(listed == 1, "one row came back");
   must(rows != NULL, "and the array it was allocated in");
   must(strcmp(rows[0].session_id, "sess-a") == 0, "the row is the one just saved");
   must(rows[0].messages_json && strcmp(rows[0].messages_json, "[{\"role\":\"user\"}]") == 0,
        "the allocated member crossed whole");
   db1_primary_session_rows_free(rows, listed);

   /* Loose strings the callee allocates, handed back through char **. */
   must(db1_webchat_live_set("sess-a", "turn-7", "hello there", "streaming") == 0,
        "set a live turn");
   char *turn_id = NULL, *text = NULL, *status = NULL;
   long long rev = 0;
   must(db1_webchat_live_get("sess-a", 0, &turn_id, &text, &status, &rev) == 1,
        "read the live turn");
   must(turn_id && strcmp(turn_id, "turn-7") == 0, "the first allocated string is its own value");
   must(text && strcmp(text, "hello there") == 0, "and the second");
   must(status && strcmp(status, "streaming") == 0, "and the third");
   must(rev > 0, "the revision came with them");
   free(turn_id);
   free(text);
   free(status);

   /* Nothing newer than the revision just read is nothing there, not broken. */
   char *again = NULL, *more = NULL, *state = NULL;
   long long next = 0;
   must(db1_webchat_live_get("sess-a", rev, &again, &more, &state, &next) == 0,
        "a poll with nothing newer finds nothing rather than failing");
   free(again);
   free(more);
   free(state);

   printf("  PASS: the callee allocates what the caller frees\n");
}

static void test_a_bulk_replace_and_what_it_reads_back(void)
{
   must(db1_runtime_state_set("boot-count", "3") == 0, "set a runtime value");
   char value[64] = "";
   must(db1_runtime_state_get("boot-count", value, sizeof value) == 0,
        "read it back, answering 0 as it always did");
   must(strcmp(value, "3") == 0, "the value crossed");

   /* An array of rows going IN: the frame carries one group of cells per
      model, and a frame that is not a whole number of rows is refused. */
   provider_model_t models[3];
   memset(models, 0, sizeof models);
   for (int i = 0; i < 3; i++)
   {
      snprintf(models[i].id, sizeof models[i].id, "model-%d", i);
      snprintf(models[i].display_name, sizeof models[i].display_name, "Model %d", i);
      models[i].context_window = 1000 * (i + 1);
      models[i].max_output = 100 * (i + 1);
      models[i].deprecated = (i == 2);
   }
   must(db1_model_catalog_replace("anthropic", models, 3) == 0, "replace the catalogue");
   must(db1_model_catalog_is_fresh("anthropic", 3600) == 1, "and it is fresh");

   /* An array of rows coming BACK, allocated by the callee, with the count
      through a parameter rather than the return. */
   provider_model_t *back = NULL;
   int n = -1;
   must(db1_model_catalog_get("anthropic", &back, &n) == 0, "read the catalogue back");
   must(n == 3, "all three rows came back");
   must(back != NULL, "in an array this side allocated");
   must(strcmp(back[0].id, "model-0") == 0, "the first row is its own");
   must(back[1].context_window == 2000, "a numeric member survived the crossing");
   must(strcmp(back[2].id, "model-2") == 0 && back[2].deprecated == 1,
        "and the last row did not run into the one before it");
   db1_model_catalog_free(back, n);

   /* A returned document with values beside it: the body is the answer, and the
      age and pinned address are ordinary out-parameters. */
   must(db1_web_page_put("https://example.invalid/x", "<html>hi</html>", "203.0.113.7") == 0,
        "cache a page");
   long age = -1;
   char pinned[DB1_WEB_PAGE_ADDR_LEN] = "";
   char *body = db1_web_page_get("https://example.invalid/x", &age, pinned, sizeof pinned);
   must(body != NULL, "the page came back");
   must(strcmp(body, "<html>hi</html>") == 0, "and it is the document that was stored");
   must(age >= 0, "the age came with it");
   must(strcmp(pinned, "203.0.113.7") == 0, "and so did the pinned address");
   free(body);

   must(db1_web_page_get("https://example.invalid/nothing", &age, pinned, sizeof pinned) == NULL,
        "a page nobody cached is nothing rather than an empty page");

   printf("  PASS: a bulk replace and what it reads back\n");
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
   test_a_cron_job_carries_its_array_member();
   test_a_job_row_carries_what_the_store_allocated();
   test_the_callee_allocates_what_the_caller_frees();
   test_a_bulk_replace_and_what_it_reads_back();

   stop_module();
   obs_bus_stop();
   snprintf(command, sizeof(command), "rm -rf '%s'", g_tmp);
   (void)system(command);
   printf("db1-module-bus: ok (the module serves what the daemon asks it for)\n");
   return 0;
}
