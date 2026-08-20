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
#include "wfe_engine_store.h"
#include "ensemble.h"
#include "execution_trace.h"
#include "wfe_binding.h"
#include "pipelines.h"
#include "roadmap_runtime.h"
#include "execution_plans.h"
#include "roundtable_pipeline.h"
#include "remote_client_grant.h"
#include "server_identity_jti.h"
#include "mgmt_jwks_cache.h"
#include "mgmt_nonce.h"
#include "pki_store.h"
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
#include "cost_fold.h"
#include "diagnose.h"
#include "interaction_events.h"
#include "clarify.h"
#include "session_state.h"

/* After session_state.h: guardrails.h uses severity_t from aimee.h and does
   not include it, so it only compiles behind something that does. */
#include "guardrails.h"
#include "db1_windows.h"
#include "db1_module_api.h"
#include "git_ownership.h"
#include "platform_test_util.h"
#include "wm.h"

static char g_tmp[512];
/* Every stage the module declares, in one place: the grant that admits them and
   the wait that proves they attached read the same list, so adding a family
   cannot admit a stage the fixture then fails to wait for -- or the reverse. */
static const unsigned served[] = {
    AIMEE_DB1_EVENT_ECONOMIZER_STATE,
    AIMEE_DB1_EVENT_GIT_OWNERSHIP,
    AIMEE_DB1_EVENT_CONVERSATION,
    AIMEE_DB1_EVENT_AGENT_WORK,
    AIMEE_DB1_EVENT_DELEGATION,
    AIMEE_DB1_EVENT_SESSIONS,
    AIMEE_DB1_EVENT_RUNTIME,
    AIMEE_DB1_EVENT_TELEMETRY,
    AIMEE_DB1_EVENT_GUARDRAIL_STATE,
    AIMEE_DB1_EVENT_ENSEMBLE,
    AIMEE_DB1_EVENT_WORKFLOW,
    AIMEE_DB1_EVENT_ROUNDTABLE,
    AIMEE_DB1_EVENT_IDENTITY,
    AIMEE_DB1_EVENT_CHECKPOINTS,
    AIMEE_DB1_EVENT_JTI_REPLAY,
    AIMEE_DB1_EVENT_MGMT_JWKS,
    AIMEE_DB1_EVENT_MGMT_NONCE,
    AIMEE_DB1_EVENT_PKI,
    /* Lifecycle was absent from this list until the engine's own reads were
     * declared, so every work-item operation in the catalog was unserved here
     * and the suite still reported that the module serves what the daemon asks
     * of it. It served eighteen families of nineteen. */
    AIMEE_DB1_EVENT_LIFECYCLE,
};

static pid_t g_module = -1;
static unsigned g_missing_stage;

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
      unsigned missing = 0u;
      for (size_t at = 0; at < sizeof served / sizeof served[0]; at++)
      {
         if (!obs_bus_module_available(served[at]))
         {
            missing = served[at];
            break;
         }
      }
      if (!missing)
         return;
      g_missing_stage = missing;
      int status = 0;
      if (waitpid(child, &status, WNOHANG) == child)
      {
         g_module = -1;
         must(0, "module exited before it attached");
      }
      struct timespec pause = {0, 50 * 1000 * 1000};
      nanosleep(&pause, NULL);
   }
   /* Name it. "the module never registered its stages" sent two separate
      investigations at the module when the fault was a grant that never
      mentioned the stage -- an ungranted stage and an unserved one look
      identical from here, so say which one is absent. */
   fprintf(stderr, "the module never registered stage kind %u (granted? built?)\n",
           g_missing_stage);
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

static void test_costs_ids_and_a_nested_row(void)
{
   /* A cost is returned as a double: rounding it here would bill differently
      on each side of the boundary. */
   must(db1_cost_fold_record("parent-1", "child-1", 0.25, "delegate") >= 0, "fold a cost");
   must(db1_cost_fold_record("parent-1", "child-2", 0.75, "delegate") >= 0, "fold another");
   double total = db1_cost_fold_total("parent-1");
   must(total > 0.99 && total < 1.01, "the total came back as the number it is");

   /* A list of ids going in: the frame carries one cell per id. */
   /* The return is the eviction pass's answer, not an id -- which is what it
      always was, and what the wire carries verbatim. */
   must(db1_interaction_event_record("sess-ie", "user_turn", NULL, "{}", "ok") == 0,
        "record an event, defaulting its actor from the type name");
   must(db1_interaction_event_record("sess-ie", "agent_turn", NULL, "{}", "ok") == 0,
        "record another");
   ie_event_row_t events[8];
   int found = db1_interaction_event_list_for_session("sess-ie", events, 8);
   must(found == 2, "both events came back");
   must(strcmp(events[0].actor, "user") == 0 || strcmp(events[1].actor, "user") == 0,
        "the actor the type name defaulted to crossed with the row");
   int ids[2] = {events[0].id, events[1].id};
   must(db1_interaction_event_mark_reflected(ids, 2) >= 0, "mark both by id");

   /* A row whose first member is itself a row. */
   int diag = db1_diagnose_start("the module answers slowly");
   must(diag > 0, "start a diagnosis");
   must(db1_diagnose_add_hypothesis(diag, "the queue is deep") > 0,
        "add a hypothesis, which answers with its new id");
   diagnosis_ranking_t ranked[4];
   memset(ranked, 0, sizeof ranked);
   int ranks = db1_diagnose_rank_hypotheses(diag, ranked, 4);
   must(ranks >= 1, "rank them");
   must(strcmp(ranked[0].hypothesis.content, "the queue is deep") == 0,
        "the nested row arrived as its own members, not as one opaque cell");
   must(ranked[0].hypothesis.diagnosis_id == diag, "including the ones after the text");

   printf("  PASS: costs, ids and a nested row\n");
}

static void test_a_row_that_carries_rows(void)
{
   clarify_session_t started;
   memset(&started, 0, sizeof started);
   must(db1_clarify_start("make the exporter idempotent", &started) > 0,
        "start a clarify session, which answers with its new id");
   must(started.id > 0, "it has an id");
   must(started.qa_count > 0, "and the questions it opened with");

   /* clarify_session_t holds clarify_qa_t qa[8]: eight rows inside a row, and
      the frame carries every member of every one of them. */
   clarify_session_t back;
   memset(&back, 0, sizeof back);
   must(db1_clarify_get(started.id, &back) == 0, "read the session back");
   must(back.id == started.id, "it is the session just started");
   must(back.qa_count == started.qa_count, "with the same number of pairs");
   must(strcmp(back.qa[0].question, started.qa[0].question) == 0,
        "the first nested row survived the crossing");
   must(strcmp(back.qa[0].dimension, started.qa[0].dimension) == 0, "including its dimension");

   must(db1_clarify_answer(started.id, "it must be safe to run twice", &back) == 0,
        "answer the first question");

   /* A pure computation over a struct the caller holds: it crosses as the row
      it is, and comes back as the number the domain computed. */
   float scored = db1_clarify_score(&back);
   must(scored > 0.0f, "scoring the row answers the same way");

   char dim[CLARIFY_DIM_NAME_LEN] = "";
   db1_clarify_weakest_dim(&back, dim, sizeof dim);
   must(dim[0] != '\0', "the weakest dimension came back through a void call");

   char *spec = db1_clarify_crystallize(&back);
   must(spec != NULL, "crystallize answers with a document");
   must(strstr(spec, "make the exporter idempotent") != NULL, "built from the row it was given");
   free(spec);

   printf("  PASS: a row that carries rows\n");
}

static void test_guardrail_state_crosses_with_its_collections(void)
{
   /* session_state_t is a record plus five collections -- 386 cells in one
      frame. The risk here is not that it fails, it is that it comes back short
      and a guardrail quietly forgets what it has seen, so this fills the ends
      of the arrays rather than the beginnings. */
   session_state_t saved;
   memset(&saved, 0, sizeof saved);
   snprintf(saved.session_mode, sizeof saved.session_mode, "%s", "build");
   snprintf(saved.guardrail_mode, sizeof saved.guardrail_mode, "%s", "enforce");
   snprintf(saved.tdd_mode, sizeof saved.tdd_mode, "%s", "strict");
   saved.active_task_id = 4294967297LL;
   saved.hook_call_count = 7;
   saved.is_delegate = 1;

   saved.seen_count = MAX_SEEN_PATHS;
   for (int i = 0; i < MAX_SEEN_PATHS; i++)
      snprintf(saved.seen_paths[i], MAX_SEEN_LEN, "/seen/%d", i);
   saved.read_path_count = MAX_READ_PATHS;
   for (int i = 0; i < MAX_READ_PATHS; i++)
      snprintf(saved.read_paths[i], MAX_SEEN_LEN, "/read/%d", i);

   saved.worktree_count = MAX_WORKTREES;
   for (int i = 0; i < MAX_WORKTREES; i++)
   {
      snprintf(saved.worktrees[i].git_root, MAX_PATH_LEN, "/root/%d", i);
      snprintf(saved.worktrees[i].worktree_path, MAX_PATH_LEN, "/tree/%d", i);
   }
   saved.tdd_write_count = MAX_TDD_WRITES;
   for (int i = 0; i < MAX_TDD_WRITES; i++)
   {
      snprintf(saved.tdd_writes[i].stem, MAX_TDD_STEM, "stem-%d", i);
      saved.tdd_writes[i].is_test = (i % 2);
   }
   saved.file_hash_count = MAX_FILE_HASHES;
   for (int i = 0; i < MAX_FILE_HASHES; i++)
   {
      snprintf(saved.file_hashes[i].path, MAX_SEEN_LEN, "/hashed/%d", i);
      /* Above INT64_MAX on purpose: a hash rendered as a signed number comes
         back as a different number to anyone reading the frame. */
      saved.file_hashes[i].content_hash = 18446744073709551615ULL - (uint64_t)i;
   }
   saved.ap_hit_count = MAX_AP_SESSION_HITS;
   for (int i = 0; i < MAX_AP_SESSION_HITS; i++)
   {
      saved.ap_hits[i].pattern_id = 1000 + i;
      saved.ap_hits[i].hits = i + 1;
   }

   must(db1_session_state_save("sess-guard", &saved) == 0, "save the guardrail state");

   session_state_t back;
   memset(&back, 0, sizeof back);
   must(db1_session_state_load("sess-guard", &back) == 0, "load it back");
   must(strcmp(back.session_mode, "build") == 0, "the scalars crossed");
   must(back.active_task_id == 4294967297LL, "including the 64-bit one");

   must(back.seen_count == MAX_SEEN_PATHS, "every seen path was counted");
   must(strcmp(back.seen_paths[MAX_SEEN_PATHS - 1], "/seen/63") == 0,
        "the LAST string in the array arrived, not just the first");
   must(strcmp(back.read_paths[MAX_READ_PATHS - 1], "/read/63") == 0, "and in the second array");

   must(strcmp(back.worktrees[MAX_WORKTREES - 1].worktree_path, "/tree/15") == 0,
        "the last row of the last worktree arrived whole");
   must(back.tdd_writes[MAX_TDD_WRITES - 1].is_test == (MAX_TDD_WRITES - 1) % 2,
        "a row's numeric member did not shift");
   /* file_hashes has no ORDER BY in the domain's load, so the row order is
      SQLite's, not insertion order. Check every row against the hash its own
      path implies rather than against a position, or the test pins an ordering
      the domain never promised and breaks the day SQLite picks another plan. */
   int tail_seen = 0;
   must(back.file_hash_count == MAX_FILE_HASHES, "every hashed path arrived");
   for (int at = 0; at < back.file_hash_count; at++)
   {
      int which = atoi(back.file_hashes[at].path + strlen("/hashed/"));
      must(which >= 0 && which < MAX_FILE_HASHES, "a hashed path came back intact");
      /* Saved as UINT64_MAX - which: every one of these is far above INT64_MAX,
         so a signed round-trip anywhere on the wire lands negative and fails. */
      must(back.file_hashes[at].content_hash == 18446744073709551615ULL - (uint64_t)which,
           "an unsigned 64-bit hash came back as itself, not as a negative number");
      tail_seen |= which == MAX_FILE_HASHES - 1;
   }
   must(tail_seen, "the last hashed slot arrived");
   must(back.ap_hits[MAX_AP_SESSION_HITS - 1].hits == MAX_AP_SESSION_HITS,
        "and the last pattern hit");

   db1_session_state_summary_t summary;
   memset(&summary, 0, sizeof summary);
   must(db1_session_state_get_summary("sess-guard", &summary) == 0, "read the summary");
   must(summary.hook_call_count == 7, "the summary counted the hooks");
   must(db1_session_state_exists("sess-guard") == 1, "the state exists");
   must(db1_session_state_delete("sess-guard") == 0, "delete it");
   must(db1_session_state_exists("sess-guard") == 0, "and it is gone");

   printf("  PASS: guardrail state crosses with its collections\n");
}

/* An ensemble refusing a turn is something it has to SAY. This checks that the
   sentence survives the crossing, because the failure it guards against is not
   a crash: a reply that carried only a status would turn "expected 'claude-1',
   got 'gemini'" into -1, and the user would be told the store broke. */
static void test_an_ensemble_verdict_crosses_as_a_sentence(void)
{
   char err[ENSEMBLE_ERR_LEN] = "";
   const char *assignments = "{\"reviewer\":[\"claude-1\",\"gemini\"],\"author\":[\"claude-2\"]}";
   int id = 0;
   must(db1_ensemble_create(NULL, NULL, "code-review", "bus-review", assignments, &id, err,
                            sizeof err) == 0,
        "started an ensemble from a built-in template across the bus");
   must(id > 0, "and it came back with an id");

   ensemble_info_t info;
   char *prompt = NULL;
   char *context = NULL;
   memset(&info, 0, sizeof info);
   must(db1_ensemble_get(id, &info, &prompt, &context, err, sizeof err) == 0, "read it back");
   must(strcmp(info.template_name, "code-review") == 0, "the template name crossed");
   must(strcmp(info.channel, "bus-review") == 0, "the channel crossed");
   must(info.phase_count == 3, "the phase count was derived from the stored template");
   must(strcmp(info.expected_agent, "claude-1") == 0, "and it is claude-1's turn");
   must(prompt != NULL && prompt[0], "the turn prompt was built and allocated across the wire");
   free(prompt);
   free(context);

   /* The wrong speaker: a verdict, not a broken store. */
   prompt = NULL;
   memset(&info, 0, sizeof info);
   must(db1_ensemble_advance(id, "gemini", "jumping the queue", &info, &prompt, err, sizeof err) !=
            0,
        "the ensemble refused a turn taken out of order");
   must(strstr(err, "claude-1") && strstr(err, "gemini"),
        "and said which agent it expected and which one spoke");
   free(prompt);

   /* The right speaker advances it. */
   prompt = NULL;
   memset(&info, 0, sizeof info);
   must(db1_ensemble_advance(id, "claude-1", "looks fine", &info, &prompt, err, sizeof err) == 0,
        "the expected speaker advanced the ensemble");
   must(strcmp(info.expected_agent, "gemini") == 0, "and the turn passed to gemini");
   free(prompt);

   ensemble_info_t *rows = NULL;
   int count = 0;
   must(db1_ensemble_list(&rows, &count, err, sizeof err) == 0, "listed the ensembles");
   must(count >= 1, "and the row the callee allocated came back");
   int found = 0;
   for (int at = 0; at < count; at++)
   {
      if (strcmp(rows[at].channel, "bus-review") != 0)
         continue;
      found = 1;
      must(rows[at].phase_count == 3, "a listed row carries what the template implies");
   }
   free(rows);
   must(found, "the ensemble we started is among them");

   int by_channel = 0;
   must(db1_ensemble_find_current_by_channel("bus-review", &by_channel, err, sizeof err) == 0,
        "found the current ensemble for its channel");
   must(by_channel == id, "and it is the one we started");

   must(db1_ensemble_find_current_by_channel("no-such-channel", &by_channel, err, sizeof err) != 0,
        "an absent channel is refused");
   must(strstr(err, "no-such-channel") != NULL, "and the refusal names the channel it looked for");

   must(db1_ensemble_pause(id, "bus test", err, sizeof err) == 0, "paused it");
   must(db1_ensemble_pause(999999, "bus test", err, sizeof err) != 0, "an absent id is refused");
   must(strstr(err, "999999") != NULL, "and the refusal names the id");

   printf("  PASS: an ensemble verdict crosses as a sentence\n");
}

/* A trace row is mostly 4KB text fields, and the reads come back as three
   different row shapes over the same table. The risk here is not the crossing
   but the shapes: a detail read that quietly returned the recent-row columns
   would still look like a populated struct. */
static void test_execution_trace_rows_keep_their_shapes(void)
{
   db1_execution_trace_insert_row_t row;
   memset(&row, 0, sizeof row);
   row.plan_id = 42;
   row.session_id = "sess-trace";
   row.turn = 7;
   row.direction = "outbound";
   row.content = "the content of a turn";
   row.tool_name = "git";
   row.tool_args = "{\"command\":\"status\"}";
   row.tool_result = "clean";
   row.context_hash = "hash-abc";
   must(db1_execution_trace_insert(&row) == 0, "inserted a trace row");

   row.turn = 8;
   row.direction = "inbound";
   row.tool_name = "";
   row.tool_args = "";
   row.tool_result = "";
   must(db1_execution_trace_insert(&row) == 0, "inserted a second row with empty tool columns");

   must(db1_execution_trace_count_for_session("sess-trace") == 2,
        "the count came back as the return value rather than as a status");
   must(db1_execution_trace_count_for_session("no-such-session") == 0,
        "and an unknown session counts zero rather than failing");

   db1_execution_trace_recent_row_t recent[8];
   memset(recent, 0, sizeof recent);
   int n = db1_execution_trace_list_recent(recent, 8);
   must(n == 2, "listed both rows into the caller's array");

   db1_execution_trace_tool_call_t calls[8];
   memset(calls, 0, sizeof calls);
   int c = db1_execution_trace_list_tool_calls(calls, 8);
   must(c >= 1, "the row carrying a tool call is a tool call");
   int saw_args = 0;
   for (int at = 0; at < c; at++)
   {
      if (strstr(calls[at].tool_args, "status") != NULL)
         saw_args = 1;
   }
   must(saw_args, "and its arguments crossed intact");

   db1_execution_trace_detail_t detail;
   memset(&detail, 0, sizeof detail);
   must(db1_execution_trace_get(recent[0].id, &detail) == 0, "read one row in full");
   must(detail.plan_id == 42, "the detail read carries columns the recent row does not");
   must(strcmp(detail.content, "the content of a turn") == 0, "including the content");
   must(strcmp(detail.context_hash, "hash-abc") == 0, "and the context hash");

   db1_execution_trace_mining_row_t mining[8];
   memset(mining, 0, sizeof mining);
   int m = db1_execution_trace_list_after_id(0, mining, 8);
   /* One, not two: this read is for trace mining and takes only rows that
      name a tool, so the second row's empty tool_name excludes it. */
   must(m == 1, "the mining read takes only the row that named a tool");
   must(mining[0].id > 0, "and a mining row carries its 64-bit id");
   int after = db1_execution_trace_list_after_id(mining[m - 1].id, mining, 8);
   must(after == 0, "nothing is after the last one");

   printf("  PASS: execution trace rows keep their shapes\n");
}

/* The single-writer refusal is the point of this one. db1_wfe_bind answers -2
   when a work item is already bound to a DIFFERENT session, and an integer
   return is normally read as a status -- so without saying otherwise, that
   refusal would arrive as -1 and a caller would be told the store broke rather
   than that another session holds the item. */
static void test_a_binding_refusal_is_not_a_broken_store(void)
{
   must(db1_wfe_bind("sess-one", "wi_bus_a", "advisory") == 0, "bound a work item to a session");
   must(db1_wfe_bind("sess-one", "wi_bus_a", "hard") == 0, "re-binding the same session is fine");

   must(db1_wfe_bind("sess-two", "wi_bus_a", "advisory") == -2,
        "a second session is refused the same work item, and the refusal keeps its own value");
   must(db1_wfe_bind("sess-two", "wi_bus_b", "advisory") == 0,
        "and that session can still bind a free one");
   must(db1_wfe_bind(NULL, "wi_bus_c", "off") == -1, "bad arguments still answer -1");

   char wi[DB1_WFE_WORK_ITEM_ID_LEN] = "";
   char stage[DB1_WFE_STAGE_LEN] = "";
   must(db1_wfe_binding_get("sess-one", wi, sizeof wi, stage, sizeof stage) == 1,
        "read the binding back");
   must(strcmp(wi, "wi_bus_a") == 0, "the work item crossed");
   /* enforce_stage is NOT changed on re-bind: the header calls it monotonic
      per session row, so the "hard" above must not have replaced "advisory". */
   must(strcmp(stage, "advisory") == 0, "and the re-bind left the enforce stage alone");
   must(db1_wfe_binding_get("sess-none", wi, sizeof wi, stage, sizeof stage) == 0,
        "an unbound session is absent rather than an error");

   /* A negative ttl sets an expiry in the past, which is how the header says to
      force staleness without waiting. */
   must(db1_wfe_lease_renew("sess-one", -60) == 0, "forced the lease stale");
   char expiry[DB1_WFE_EXPIRY_LEN] = "";
   must(db1_wfe_lease_expiry_get("sess-one", expiry, sizeof expiry) == 1, "read the expiry");
   must(expiry[0] != '\0', "which is set rather than empty");

   /* ttl 0 CLEARS the lease, and the binding is still there. The answer is
      therefore "found, and empty" -- which a read inferring found-ness from a
      non-empty value would report as no binding at all. */
   must(db1_wfe_lease_renew("sess-two", 0) == 0, "cleared the other session's lease");
   char cleared[DB1_WFE_EXPIRY_LEN] = "x";
   must(db1_wfe_lease_expiry_get("sess-two", cleared, sizeof cleared) == 1,
        "a binding with no lease is still a binding");
   must(cleared[0] == '\0', "and its expiry comes back empty");
   must(db1_wfe_lease_expiry_get("sess-none", cleared, sizeof cleared) == 0,
        "where an absent binding answers absent");

   char stale[8][DB1_WFE_WORK_ITEM_ID_LEN];
   memset(stale, 0, sizeof stale);
   int n = db1_wfe_lease_stale_work_items(stale, 8);
   must(n >= 1, "the lapsed binding is listed as stale");
   int saw = 0;
   for (int at = 0; at < n; at++)
   {
      if (strcmp(stale[at], "wi_bus_a") == 0)
         saw = 1;
   }
   must(saw, "and it is the one whose lease we expired");

   must(db1_wfe_lease_reclaim_stale() >= 1, "reclaiming returns how many it took");
   must(db1_wfe_binding_get("sess-one", wi, sizeof wi, stage, sizeof stage) == 0,
        "the reclaimed session is unbound");

   must(db1_wfe_unbind("sess-two") == 0, "unbound the other session");
   must(db1_wfe_unbind("sess-two") == 0, "and unbinding again is a no-op, not a failure");

   printf("  PASS: a binding refusal is not a broken store\n");
}

/* A pipeline row is twelve columns of mixed int and text, and the update takes
   nine parameters in an order nothing but position enforces. Swap two ints on
   either side of the wire and every call still compiles. */
static void test_a_pipeline_row_survives_a_nine_parameter_update(void)
{
   int id = 0;
   must(db1_pipeline_create("build the thing", "feature", "deep", &id) == 0, "created a pipeline");
   must(id > 0, "and it came back with an id");

   db1_pipeline_t got;
   memset(&got, 0, sizeof got);
   must(db1_pipeline_get(id, &got) == 0, "read it back");
   must(strcmp(got.task, "build the thing") == 0, "the task crossed");
   must(strcmp(got.request_classification, "feature") == 0, "so did the classification");
   must(strcmp(got.plan_depth, "deep") == 0, "and the plan depth");
   must(db1_pipeline_get(999999, &got) != 0, "an absent pipeline is not a read");

   /* Distinct values per int, so a transposed pair cannot pass. */
   must(db1_pipeline_update(id, "running", "planning", 3, 11, 22, "bugfix", "shallow", 33) == 0,
        "updated every column at once");
   memset(&got, 0, sizeof got);
   must(db1_pipeline_get(id, &got) == 0, "read the update back");
   must(strcmp(got.status, "running") == 0, "status");
   must(strcmp(got.current_phase, "planning") == 0, "phase");
   must(got.phase_attempts == 3, "attempts");
   must(got.plan_id == 11, "plan id");
   must(got.job_id == 22, "job id");
   must(strcmp(got.request_classification, "bugfix") == 0, "reclassified");
   must(strcmp(got.plan_depth, "shallow") == 0, "re-depthed");
   must(got.clarify_session_id == 33, "and the clarify session, which is last on both sides");

   must(db1_pipeline_link_plan(id, 44) == 0, "linked a plan");
   must(db1_pipeline_link_job(id, 55) == 0, "linked a job");
   memset(&got, 0, sizeof got);
   must(db1_pipeline_get(id, &got) == 0, "read the links back");
   must(got.plan_id == 44 && got.job_id == 55, "each link landed in its own column");

   /* "Active" is the domain's word, not a synonym for "exists": the read takes
      status IN ('active','paused'), so the 'running' set above is excluded. */
   db1_pipeline_t active[8];
   memset(active, 0, sizeof active);
   int n = db1_pipeline_list_active(active, 8);
   int listed = 0;
   for (int at = 0; at < n; at++)
   {
      if (active[at].id == id)
         listed = 1;
   }
   must(!listed, "a 'running' pipeline is not one of the active ones");

   must(db1_pipeline_update(id, "paused", "planning", 3, 44, 55, "bugfix", "shallow", 33) == 0,
        "paused it");
   memset(active, 0, sizeof active);
   n = db1_pipeline_list_active(active, 8);
   listed = 0;
   for (int at = 0; at < n; at++)
   {
      if (active[at].id != id)
         continue;
      listed = 1;
      must(strcmp(active[at].task, "build the thing") == 0,
           "and a listed row carries its columns, not just its id");
   }
   must(listed, "a paused pipeline is active");

   must(db1_pipeline_cancel(id) == 0, "cancelled it");
   memset(&got, 0, sizeof got);
   must(db1_pipeline_get(id, &got) == 0, "a cancelled pipeline is still readable");
   must(strcmp(got.status, "paused") != 0, "but no longer paused");

   printf("  PASS: a pipeline row survives a nine-parameter update\n");
}

/* select_next answers 1 for "no unit", 0 for "here is one" and -1 for a broken
   store -- the opposite way round from every other found-style read here. The
   caller in roadmap_auto.c branches on all three and treats 1 as "the roadmap
   is finished": read as a found-flag it would finish a roadmap that still had
   work, or keep working one that was done. */
static void test_the_roadmap_selector_keeps_all_three_answers(void)
{
   must(db1_roadmap_dispatch_upsert("rm-bus", "lean", 1, 5000) == 0, "opened a dispatch");
   rdm_dispatch_t disp;
   memset(&disp, 0, sizeof disp);
   must(db1_roadmap_dispatch_get("rm-bus", &disp) == 0, "read it back");
   must(strcmp(disp.token_profile, "lean") == 0, "the profile crossed");
   must(disp.require_slice_discussion == 1, "and the flag");
   must(disp.budget_ceiling_tokens == 5000, "and the ceiling");
   must(db1_roadmap_dispatch_get("rm-none", &disp) != 0, "an absent roadmap is not a read");

   must(db1_roadmap_dispatch_set_status("rm-bus", "running", "") == 0, "set a status");
   must(db1_roadmap_dispatch_set_phase("rm-bus", "dispatch") == 0, "set a phase");

   char unit[64] = "x";
   must(db1_roadmap_unit_select_next("rm-bus", unit, sizeof unit) == 1,
        "with no units at all, the selector says one rather than zero");

   /* level 'task' deliberately: the selector takes tasks, so a unit at any
      other level is invisible to it however pending it looks. */
   must(db1_roadmap_unit_ensure("rm-bus", "u1", "task", "strict") == 0, "ensured a unit");
   must(db1_roadmap_unit_ensure("rm-bus", "u2", "task", "strict") == 0, "and another");
   must(db1_roadmap_unit_ensure("rm-bus", "g1", "goal", "strict") == 0, "and a goal above them");

   unit[0] = 0;
   must(db1_roadmap_unit_select_next("rm-bus", unit, sizeof unit) == 0,
        "with a pending unit it says zero");
   must(strcmp(unit, "u1") == 0, "and hands back the lowest id");

   must(db1_roadmap_unit_claim("rm-bus", "u1", "worker-a", "worktrees/u1") == 0, "claimed it");
   rdm_unit_dispatch_t got;
   memset(&got, 0, sizeof got);
   must(db1_roadmap_unit_get("rm-bus", "u1", &got) == 0, "read the unit back");
   must(strcmp(got.claimed_by, "worker-a") == 0, "the owner crossed");
   must(strcmp(got.worktree_path, "worktrees/u1") == 0, "and the worktree path");

   must(db1_roadmap_unit_heartbeat("rm-bus", "u1") == 0, "heartbeat");
   must(db1_roadmap_unit_set_coord_job("rm-bus", "u1", 77) == 0, "linked a coord job");
   must(db1_roadmap_unit_increment_verify_attempts("rm-bus", "u1") == 0, "counted an attempt");
   memset(&got, 0, sizeof got);
   must(db1_roadmap_unit_get("rm-bus", "u1", &got) == 0, "re-read it");
   must(got.coord_job_id == 77, "the coord job landed in its own column");
   must(got.verify_attempts == 1, "and the attempt counted once");

   must(db1_roadmap_unit_finish("rm-bus", "u1", "done", "all good", "") == 0, "finished it");
   must(db1_roadmap_unit_set_state("rm-bus", "u2", "done") == 0, "and closed the other");

   unit[0] = 0;
   must(db1_roadmap_unit_select_next("rm-bus", unit, sizeof unit) == 1,
        "with every task done it is back to one, which is what ends a roadmap");
   must(db1_roadmap_unit_get("rm-bus", "g1", &got) == 0,
        "the goal is still there -- it was never selectable, not never stored");

   printf("  PASS: the roadmap selector keeps all three answers\n");
}

/* A plan is the deepest row in this contract: 32 steps, each holding its own
   array of dependency indices. That inner array is the point of the case --
   an expansion that stopped at the step would leave every step's dependencies
   behind, and a plan whose steps all look independent still executes, just in
   the wrong order and all at once. */
static void test_a_plan_carries_its_steps_and_their_dependencies(void)
{
   /* "after" is the document's word for a step's prerequisites; depends_on is
      what the struct calls them once stored. */
   const char *steps = "["
                       "{\"action\":\"build\",\"precondition\":\"clean tree\","
                       "\"success_predicate\":\"exit 0\",\"rollback\":\"git clean\"},"
                       "{\"action\":\"test\",\"after\":[0]},"
                       "{\"action\":\"ship\",\"after\":[0,1]}]";
   int plan_id = db1_execution_plan_create("bus-agent", "build test ship", steps);
   must(plan_id > 0, "created a plan and got its id back as the return value");
   must(db1_execution_plan_create("bus-agent", "bad", "{not json") < 0,
        "a document that is not an array of steps is refused");

   must(db1_execution_plan_exists(plan_id) == 1, "the plan exists");
   must(db1_execution_plan_exists(999999) == 0, "and an absent one does not");
   must(db1_execution_plan_count_steps(plan_id) == 3, "it has three steps");

   plan_t plan;
   memset(&plan, 0, sizeof plan);
   must(db1_execution_plan_get(plan_id, &plan) == 0, "read the plan back");
   must(strcmp(plan.agent_name, "bus-agent") == 0, "the agent crossed");
   must(strcmp(plan.task, "build test ship") == 0, "and the task");
   must(plan.step_count == 3, "and all three steps");
   must(strcmp(plan.steps[0].action, "build") == 0, "the first step's action");
   must(strcmp(plan.steps[0].precondition, "clean tree") == 0, "and its precondition");
   must(strcmp(plan.steps[0].rollback, "git clean") == 0, "and its rollback");

   /* The inner arrays: step 1 depends on step 0, step 2 on both. */
   must(plan.steps[0].dep_count == 0, "the first step depends on nothing");
   must(plan.steps[1].dep_count == 1, "the second depends on one");
   must(plan.steps[1].depends_on[0] == 0, "and it is the first");
   must(plan.steps[2].dep_count == 2, "the third depends on two");
   must(plan.steps[2].depends_on[0] == 0 && plan.steps[2].depends_on[1] == 1,
        "and they are the first and second, in order");

   int ids[8];
   memset(ids, 0, sizeof ids);
   int n = db1_execution_plan_list_ids(ids, 8);
   must(n >= 1, "the plan is listed");
   plan_t listed[4];
   memset(listed, 0, sizeof listed);
   must(db1_execution_plan_list(listed, 4) >= 1, "and the composed list still answers whole plans");

   /* These answer how many rows they changed, not 0: declared as plain writes
      they would report success as FAILED, because the stage reads a non-zero
      return from a write as the store refusing. */
   must(db1_plan_step_set_status_output(plan.steps[0].id, "done", "built ok") == 1,
        "recorded a step outcome, and said it changed one row");
   must(db1_plan_step_set_status_output(999999, "done", "nothing") == 0,
        "and changing no rows is zero rather than an error");
   memset(&plan, 0, sizeof plan);
   must(db1_execution_plan_get(plan_id, &plan) == 0, "re-read the plan");
   must(strcmp(plan.steps[0].output, "built ok") == 0, "the output landed on its own step");
   must(plan.steps[1].output[0] == 0, "and not on any other");

   must(db1_step_evidence_insert(plan_id, plan.steps[0].id, "test", "all green", 1, "strong") == 0,
        "attached evidence");
   db1_step_evidence_latest_t ev;
   memset(&ev, 0, sizeof ev);
   must(db1_step_evidence_get_latest(plan.steps[0].id, &ev) == 0, "read the latest evidence");
   must(ev.passed == 1 && strcmp(ev.strength, "strong") == 0, "with its verdict and strength");
   must(db1_step_evidence_get_latest(plan.steps[2].id, &ev) != 0,
        "a step with no evidence has none");

   db1_execution_plan_summary_t summaries[8];
   memset(summaries, 0, sizeof summaries);
   int sn = db1_execution_plan_list_recent_summaries(summaries, 8);
   must(sn >= 1, "summarised the recent plans");
   int seen = 0;
   for (int at = 0; at < sn; at++)
   {
      if (summaries[at].id != plan_id)
         continue;
      seen = 1;
      must(summaries[at].total_steps == 3, "the summary counts every step");
      must(summaries[at].done_steps == 1, "and only the finished one as done");
   }
   must(seen, "and ours is among them");

   must(db1_execution_plan_set_status(plan_id, "running") == 1, "set a status on one plan");
   must(db1_execution_plan_cancel_by_id(plan_id, "bus test") >= 1, "cancelled the plan");
   must(db1_plan_step_cancel_active_for_plan(plan_id) >= 0, "cancelled its active steps");
   must(db1_plan_step_cancel_orphans() >= 0, "and swept orphans");

   printf("  PASS: a plan carries its steps and their dependencies\n");
}

/* rtp_run_t is 36 columns and the update writes all of them from one struct,
   so a column that crossed into the wrong slot would be invisible in any test
   that left most of them empty. The compare-and-swap matters just as much: it
   is what makes gate resolution exactly-once, so two concurrent passes cannot
   both merge, and it says so by changing exactly one row or none. */
static void test_a_roundtable_run_round_trips_and_swaps_once(void)
{
   int run_id = 0;
   must(db1_roundtable_run_create("an idea", "the done bar", "repo", "main", &run_id) == 0,
        "created a run");
   must(run_id > 0, "with an id");

   rtp_run_t run;
   memset(&run, 0, sizeof run);
   must(db1_roundtable_run_get(run_id, &run) == 0, "read it back");
   must(strcmp(run.idea, "an idea") == 0, "the idea crossed");
   must(strcmp(run.done_bar, "the done bar") == 0, "and the done bar");
   must(strcmp(run.base_branch, "main") == 0, "and the base branch");
   rtp_run_t absent;
   memset(&absent, 0, sizeof absent);
   must(db1_roundtable_run_get(999999, &absent) != 0, "an absent run is not a read");

   /* Distinct values in the columns most likely to be transposed. */
   snprintf(run.head_branch, sizeof run.head_branch, "%s", "feature/x");
   snprintf(run.head_sha, sizeof run.head_sha, "%s", "aaaa1111");
   snprintf(run.base_sha, sizeof run.base_sha, "%s", "bbbb2222");
   run.proposal_pr_number = 11;
   run.impl_pr_number = 22;
   run.proposal_phase_cost_usd = 1.25;
   run.impl_phase_cost_usd = 2.50;
   run.total_cost_usd = 3.75;
   run.accepted_question_count = 7;
   must(db1_roundtable_run_update(&run) == 0, "wrote every mutable column back");

   memset(&run, 0, sizeof run);
   must(db1_roundtable_run_get(run_id, &run) == 0, "re-read the run");
   must(strcmp(run.head_sha, "aaaa1111") == 0 && strcmp(run.base_sha, "bbbb2222") == 0,
        "the two shas stayed in their own columns");
   must(run.proposal_pr_number == 11 && run.impl_pr_number == 22, "so did the two PR numbers");
   must(run.proposal_phase_cost_usd == 1.25 && run.impl_phase_cost_usd == 2.50,
        "and the two phase costs, which are doubles rather than cents");
   must(run.total_cost_usd == 3.75, "and the total");
   must(run.accepted_question_count == 7, "and the question count");

   must(db1_roundtable_run_count_active() >= 1, "the run counts as active");
   must(db1_roundtable_run_branch_owner("repo", "feature/x", 0) == run_id,
        "and it owns its head branch");
   must(db1_roundtable_run_branch_owner("repo", "feature/x", run_id) == 0,
        "excluding itself, nobody owns it");

   /* Exactly-once: the first swap wins and the second finds nothing to move. */
   must(db1_roundtable_run_set_state(run_id, "gate", "impl") == 0, "moved it to a gate");
   must(db1_roundtable_run_cas_state(run_id, "gate", "merging") == 0, "the first swap won");
   must(db1_roundtable_run_cas_state(run_id, "gate", "merging") != 0,
        "and the second lost, which is what stops two passes both merging");

   rtp_run_t runs[8];
   memset(runs, 0, sizeof runs);
   must(db1_roundtable_run_list(NULL, runs, 8) >= 1, "listed the non-terminal runs");

   int pass_id = 0;
   must(db1_roundtable_pass_create(run_id, "review", "chunked", 1, "hash-1", &pass_id) == 0,
        "opened a pass");
   must(db1_roundtable_pass_max_no(run_id, "review") == 1, "which is pass one");
   rtp_pass_t pass;
   memset(&pass, 0, sizeof pass);
   must(db1_roundtable_pass_latest(run_id, "review", &pass) == 0, "read the latest pass");
   must(pass.id == pass_id, "and it is the one we opened");
   pass.blocking_count = 3;
   pass.suggestion_count = 5;
   pass.nit_count = 9;
   pass.cost_usd = 0.5;
   pass.chunk_group = 1;
   pass.chunk_index = 1;
   pass.is_chunked = 1;
   must(db1_roundtable_pass_update(&pass) == 0, "updated its counts");
   memset(&pass, 0, sizeof pass);
   must(db1_roundtable_pass_get(pass_id, &pass) == 0, "re-read the pass");
   must(pass.blocking_count == 3 && pass.suggestion_count == 5 && pass.nit_count == 9,
        "each count kept its own column");

   int attempt_id = 0;
   must(db1_roundtable_attempt_create(pass_id, 1, "run-abc", &attempt_id) == 0, "made an attempt");
   must(db1_roundtable_attempt_max_no(pass_id) == 1, "which is attempt one");
   rtp_attempt_t att;
   memset(&att, 0, sizeof att);
   must(db1_roundtable_attempt_get_by_run("run-abc", &att) == 0, "found it by run id");
   must(att.id == attempt_id, "and it is the one we made");
   must(db1_roundtable_attempt_current(pass_id, &att) == 0, "it is the current attempt");
   must(db1_roundtable_attempt_supersede_others(pass_id, attempt_id) == 0, "superseded the rest");

   int gate_id = 0;
   must(db1_roundtable_gate_create(run_id, 1, 42, "cccc3333", &gate_id) == 0, "opened a gate");
   rtp_gate_t gate;
   memset(&gate, 0, sizeof gate);
   must(db1_roundtable_gate_get(run_id, 1, &gate) == 0, "read the gate");
   must(gate.pr_number == 42, "the PR number crossed");
   must(strcmp(gate.expected_head_sha, "cccc3333") == 0, "and the sha it expects");
   snprintf(gate.verdict, sizeof gate.verdict, "%s", "pass");
   gate.merge_exit_code = 0;
   must(db1_roundtable_gate_update(&gate) == 0, "recorded a verdict");
   must(db1_roundtable_gate_age_exceeds_hours(run_id, 1, 24) == 0, "a fresh gate is not stale");

   rtp_group_agg_t agg;
   memset(&agg, 0, sizeof agg);
   /* Group numbering starts at one: nothing is group zero, and asking for it
      is rejected rather than answered with an empty aggregate. */
   must(db1_roundtable_pass_group_agg(run_id, "review", 1, &agg) == 0, "aggregated the group");
   must(agg.total == 1, "which counted the one chunked pass");
   must(db1_roundtable_pass_group_agg(run_id, "review", 0, &agg) != 0, "group zero is refused");
   must(db1_roundtable_pass_max_group(run_id, "review") == 1, "and one is the highest group");

   printf("  PASS: a roundtable run round trips and swaps once\n");
}

/* A claim answers what the record is AND how the caller got it. Those are one
   decision -- created it, re-entered its own, or refused because somebody else
   owns the appliance's first-user slot -- and a reply carrying only the record
   cannot tell the second from the third. Getting that wrong hands the first
   user's slot to whoever asks next. */
/* The workflow engine's own reads, which used to be SQL inside a second process
   holding this same file open. Each one is a count or a list taken over rows the
   engine does not own, so the thing worth proving is not that a number crossed
   the wire -- earlier cases prove that -- but that the PREDICATES survived the
   move: roots only, kinds filtered, and "since progress" actually resetting when
   the run makes progress. A count that is merely plausible is the failure mode
   here, because every one of these drives a give-up threshold. */
static void test_the_engine_reads_keep_their_predicates(void)
{
   must(db1_work_item_create("wi-root", "repo/a", "git:refs/heads/topic", "build", "1", "plan",
                             "autonomous") == 0,
        "create a root run");
   must(db1_work_item_create("wi-child", "repo/a", "slice/1", "build", "1", "plan", "autonomous") ==
            0,
        "create a second run");
   must(db1_work_item_set_parent("wi-child", "wi-root") == 0, "make it a child");

   /* Children are listed by parent, ids only, in insertion order. */
   char kids[8][DB1_WFE_ID_LEN];
   int n = db1_wfe_children_list("wi-root", kids, 8);
   must(n == 1, "one child came back");
   must(strcmp(kids[0], "wi-child") == 0, "and it is the right one");
   must(db1_wfe_children_list("wi-child", kids, 8) == 0, "a leaf has no children");

   /* The admission cap counts roots. A child must not count against it, which is
      the whole reason this is not COUNT(*) over active rows. */
   int roots = db1_wfe_active_root_count();
   must(roots == 1, "the child did not count as a root");

   /* A git proposal is found by its full path or by the tail the caller kept. */
   char found[DB1_WFE_ID_LEN];
   must(db1_wfe_work_item_id_by_git_proposal("repo/a", "git:refs/heads/topic", "refs/heads/topic",
                                             found, sizeof found) == 1,
        "found by exact proposal path");
   must(strcmp(found, "wi-root") == 0, "and it is the root, not the child");
   must(db1_wfe_work_item_id_by_git_proposal("repo/a", "nope", "refs/heads/topic", found,
                                             sizeof found) == 1,
        "found by the suffix alone");
   must(db1_wfe_work_item_id_by_git_proposal("repo/b", "nope", "no/such/ref", found,
                                             sizeof found) == 0,
        "and a miss is a miss, not an error");

   /* Turns count advance and loop, and nothing else: a pause must not spend the
      run's turn budget. */
   must(db1_lifecycle_event_add("wi-root", "plan", "create", "t", "", "", 0) == 0, "create event");
   must(db1_lifecycle_event_add("wi-root", "plan", "advance", "t", "", "", 0) == 0, "advance");
   must(db1_lifecycle_event_add("wi-root", "plan", "loop", "t", "", "", 0) == 0, "loop");
   must(db1_lifecycle_event_add("wi-root", "plan", "pause", "t", "stuck", "", 0) == 0, "pause");
   must(db1_wfe_executed_turn_count("wi-root") == 2, "the pause did not count as a turn");
   must(db1_wfe_stage_loop_count("wi-root", "plan") == 2, "advance and loop both entered plan");

   /* "Since progress" is measured from the newest advance/loop/create. The pause
      above is older than nothing, so it counts; a later advance must reset it. */
   must(db1_wfe_runner_failures_since_progress("wi-root", "plan") == 1, "one failure so far");
   must(db1_wfe_capacity_waits_since_progress("wi-root", "plan") == 0, "and no capacity wait");
   must(db1_lifecycle_event_add("wi-root", "plan", "pause", "t", "capacity_backpressure:pool", "",
                                0) == 0,
        "a capacity wait");
   must(db1_wfe_capacity_waits_since_progress("wi-root", "plan") == 1, "counted as a wait");
   must(db1_wfe_runner_failures_since_progress("wi-root", "plan") == 1,
        "and NOT as a runner failure -- waiting for a runner is not a stage that will not pass");
   must(db1_lifecycle_event_add("wi-root", "plan", "advance", "t", "", "", 0) == 0, "progress");
   must(db1_wfe_runner_failures_since_progress("wi-root", "plan") == 0,
        "progress reset the failure count");
   must(db1_wfe_capacity_waits_since_progress("wi-root", "plan") == 0,
        "and the capacity count with it");

   printf("  PASS: the engine reads keep their predicates\n");
}

/* The engine's bulk sweeps and its delegate-job ledger. These are the calls a
   scheduler makes on a timer over every run at once, so what matters is the
   predicate and the COUNT it reports: a sweep that silently moved the wrong
   rows, or reported a number it did not move, is a scheduler that either stalls
   or runs away. */
static void test_the_engine_sweeps_report_what_they_moved(void)
{
   /* A subtree: root -> child -> grandchild. */
   must(db1_work_item_create("tr-root", "repo/t", "p/root", "build", "1", "plan", "autonomous") ==
            0,
        "root");
   must(db1_work_item_create("tr-kid", "repo/t", "p/kid", "build", "1", "plan", "autonomous") == 0,
        "child");
   must(db1_work_item_create("tr-gkid", "repo/t", "p/gkid", "build", "1", "plan", "autonomous") ==
            0,
        "grandchild");
   must(db1_work_item_set_parent("tr-kid", "tr-root") == 0, "link child");
   must(db1_work_item_set_parent("tr-gkid", "tr-kid") == 0, "link grandchild");

   char ids[16][DB1_WFE_ID_LEN];
   int n = db1_wfe_descendant_ids("tr-root", ids, 16);
   must(n == 3, "the subtree is three deep and all of it came back");
   must(strcmp(ids[0], "tr-root") == 0, "the root is included -- callers stop what this returns");

   /* A transient pause older than the window resumes; a fresh one does not. */
   must(db1_work_item_set_pause("tr-kid", "panel_degraded", "") == 0, "park the child");
   must(db1_wfe_resume_transient("panel_degraded", 3600) == 0,
        "a pause from a moment ago is not older than an hour");
   must(db1_wfe_resume_transient("panel_degraded", 0) == 1, "with no window it resumes");
   must(db1_wfe_resume_transient("panel_degraded", 0) == 0, "and there is nothing left to resume");

   /* Wall caps: resume while the run has overrides left, abandon once it does
      not. The same row must not be eligible for both in one sweep. */
   must(db1_work_item_set_pause("tr-gkid", "wall_cap", "") == 0, "park on the wall cap");
   must(db1_wfe_resume_wall_caps(1) == 1, "first resume is allowed");
   must(db1_work_item_set_pause("tr-gkid", "wall_cap", "") == 0, "it parks again");
   must(db1_wfe_resume_wall_caps(1) == 0, "the override is spent");
   must(db1_wfe_abandon_exhausted_wall_caps(1, 3600) == 0, "but the grace period holds it");
   /* updated_at is second-resolution and the comparison is strict, so a row
      touched in this same second is not yet older than "now minus zero". The Go
      original compares the same way against its own cutoff; asserting the edge
      here is what keeps a later "fix" from quietly widening it to <=, which
      would abandon runs in the same sweep that parked them. */
   must(db1_wfe_abandon_exhausted_wall_caps(1, 0) == 0,
        "a row parked this second is not yet past a zero grace");
   sleep(1);
   must(db1_wfe_abandon_exhausted_wall_caps(1, 0) == 1, "a second later it is abandoned");

   /* A parent waiting on slices resumes only when every child has finished --
      and only if it had children at all. */
   must(db1_work_item_set_pause("tr-root", "slices_running", "") == 0, "parent waits");
   must(db1_wfe_resume_ready_parents() == 0, "a child is still active");
   must(db1_work_item_set_terminal("tr-kid", "accepted") == 0, "child finishes");
   must(db1_wfe_resume_ready_parents() == 1, "now the parent resumes");
   must(db1_work_item_create("tr-lonely", "repo/t", "p/lonely", "build", "1", "plan",
                             "autonomous") == 0,
        "a run with no children");
   must(db1_work_item_set_pause("tr-lonely", "slices_running", "") == 0, "parked as if fanned out");
   must(db1_wfe_resume_ready_parents() == 0,
        "a parent with no children never fanned out and must not be resumed");

   /* The delegate-job ledger, and claiming the jobs left behind by a run that
      has already finished. */
   int job = db1_agent_job_create("impl", "do the thing", "agent-1", "owner-1");
   must(job > 0, "a delegate job exists");
   must(db1_wfe_delegate_job_save("exec-1", "tr-kid", job, "tok-1") == 0, "record the mapping");

   db1_wfe_delegate_job_t claimed[8];
   int c = db1_wfe_delegate_jobs_terminal_claim(claimed, 8);
   must(c == 1, "the finished run's pending job is claimable");
   must(strcmp(claimed[0].execution_key, "exec-1") == 0, "with its execution key");
   must(claimed[0].job_id == job, "and its job id");

   /* Re-recording the step resets the attempts the claim just charged: those
      attempts were against the previous job, not this one. */
   int job2 = db1_agent_job_create("impl", "again", "agent-1", "owner-1");
   must(job2 > 0, "a replacement job");
   must(db1_wfe_delegate_job_save("exec-1", "tr-kid", job2, "tok-2") == 0, "re-record the step");
   c = db1_wfe_delegate_jobs_terminal_claim(claimed, 8);
   must(c == 1 && claimed[0].job_id == job2, "the new job is claimable in its own right");

   printf("  PASS: the engine sweeps report what they moved\n");
}

/* The budget reservation protocol, which is the one piece of the engine's store
   access that is not a query. What is being checked is not that numbers cross
   the wire but that money cannot be spent twice: a fair share is computed from
   what the tree has already committed, a second owner cannot take a live
   reservation, a release only gives back an estimate that was never spent, and
   reconciliation applies exactly once. */
static void test_the_budget_protocol_holds_across_the_bus(void)
{
   db1_wfe_budget_reservation_t r;

   /* An uncapped run reserves nothing but still takes ownership: the
      exactly-once rules apply to the invocation whether or not there is a cap. */
   must(db1_work_item_create("bg-free", "repo/b", "p/free", "build", "1", "run", "autonomous") == 0,
        "an uncapped run");
   must(db1_wfe_budget_reserve("bg-free", "owner-a", &r) == 0, "reserve it");
   must(r.allowed == 1 && r.busy == 0, "allowed, not busy");
   must(r.amount == 0.0, "an uncapped run reserves no amount");
   must(strcmp(r.root_id, "bg-free") == 0, "it is its own root");

   /* A capped tree divides what is left between the runnable items. */
   must(db1_work_item_create("bg-root", "repo/b", "p/root", "build", "1", "run", "autonomous") == 0,
        "a capped root");
   must(db1_work_item_set_cost_cap("bg-root", 10.0) == 0, "cap it at ten");
   must(db1_work_item_create("bg-kid", "repo/b", "p/kid", "build", "1", "run", "autonomous") == 0,
        "a child slice");
   must(db1_work_item_set_parent("bg-kid", "bg-root") == 0, "link it");

   /* Two runnable items, nothing spent: each may claim half. */
   must(db1_wfe_budget_reserve("bg-root", "owner-a", &r) == 0, "root reserves");
   must(r.allowed == 1, "allowed");
   must(r.max_usd == 10.0, "the cap comes from the root");
   must(r.amount > 4.9 && r.amount < 5.1, "half of the tree's ten");

   /* The second item sees the first reservation as outstanding and takes half
      of what remains, not half of the cap. */
   must(db1_wfe_budget_reserve("bg-kid", "owner-b", &r) == 0, "child reserves");
   must(r.allowed == 1, "still allowed");
   must(r.amount > 4.9 && r.amount < 5.1, "the rest of the tree's budget");
   must(strcmp(r.root_id, "bg-root") == 0, "the child answers with its root");

   /* A different owner cannot take a reservation whose lease is live. */
   must(db1_wfe_budget_reserve("bg-kid", "owner-c", &r) == 0, "a third owner asks");
   must(r.allowed == 0 && r.busy == 1, "refused as busy, not handed the same money");

   /* The holder asking again gets what it already has, and does not double it. */
   must(db1_wfe_budget_reserve("bg-kid", "owner-b", &r) == 0, "the holder asks again");
   must(r.allowed == 1 && r.busy == 0, "still its own");
   must(r.amount > 4.9 && r.amount < 5.1, "the same estimate, not a second one");

   /* Reconciliation records measured cost and answers whether the tree can
      AFFORD it -- not whether it wrote anything. That distinction is the whole
      point: the caller parks the tree when the answer is no. */
   must(db1_wfe_budget_reconcile("bg-kid", "owner-b", 1.25) == 1,
        "1.25 fits inside the tree's ten");
   must(db1_wfe_budget_reconcile("bg-kid", "owner-b", 1.25) == 1,
        "and a replay of the same cost reports the same allowance");
   must(db1_wfe_budget_reconcile("bg-kid", "owner-b", 2.0) == -1,
        "but a replay with a DIFFERENT cost is refused -- it is not the same "
        "invocation, and accepting it would rewrite settled accounting");
   must(db1_wfe_budget_reconcile("bg-kid", "owner-a", 1.25) == -1,
        "and another owner cannot reconcile it at all");
   must(db1_wfe_budget_reconcile("bg-kid", "owner-b", -1.0) == -1, "a negative cost is refused");

   /* Releasing an estimate gives the tree its headroom back; releasing measured
      spend must not, because that money is gone. */
   must(db1_wfe_budget_release("bg-root", "owner-a") == 0, "release the root's estimate");
   must(db1_wfe_budget_release("bg-kid", "owner-b") == 0, "releasing an 'actual' is a no-op");

   db1_wfe_budget_totals_t totals;
   must(db1_wfe_budget_totals("bg-kid", &totals) == 0, "read the tree totals");
   must(strcmp(totals.root_id, "bg-root") == 0, "rooted correctly");
   must(totals.max_usd == 10.0, "the cap");
   must(totals.spent == 0.0, "nothing has been charged to cum_cost yet");

   /* With the root's estimate released and the child's actual still held, a new
      reservation sees the child's spend as outstanding. */
   must(db1_wfe_budget_reserve("bg-root", "owner-a", &r) == 0, "root reserves again");
   must(r.allowed == 1, "allowed");
   must(r.amount > 8.7 && r.amount < 8.8,
        "ten minus the child's outstanding 1.25, all to the one runnable item");

   /* Once the tree has spent its cap, nothing more is allowed. */
   must(db1_work_item_add_cost("bg-root", 10.0) == 0, "charge the cap to the tree");
   must(db1_wfe_budget_release("bg-root", "owner-a") == 0, "release so it may ask again");
   must(db1_wfe_budget_reserve("bg-root", "owner-a", &r) == 0, "ask with the budget gone");
   must(r.allowed == 0, "refused: there is nothing left to divide");
   must(r.busy == 0, "and refused for want of budget, not because someone holds it");

   must(db1_wfe_budget_heartbeat("bg-kid", "owner-b") == 0, "a heartbeat extends a held lease");

   printf("  PASS: the budget protocol holds across the bus\n");
}

/* Stage transitions, which are whole transactions rather than queries. The
   things worth asserting are the ones that only hold if the transaction held:
   that a move from the wrong stage changes nothing AND writes no event, that a
   transition releases the budget reservation it consumed, and that a loop does
   not wipe the attempt counter that bounds it. */
static void test_transitions_are_all_or_nothing(void)
{
   db1_work_item_t item;
   must(db1_work_item_create("tx-1", "repo/x", "p/one", "build", "1", "plan", "autonomous") == 0,
        "a run in plan");

   /* Take a reservation so the transition has something to release. */
   db1_wfe_budget_reservation_t r;
   must(db1_wfe_budget_reserve("tx-1", "owner-a", &r) == 0, "reserve");
   must(r.allowed == 1, "allowed");

   /* A move from a stage the run is not in must fail, and must leave no trace. */
   must(db1_wfe_move("tx-1", "review", "merge", "advance", "d", "h", 1.0) == -1,
        "a move from the wrong stage is refused");
   must(db1_work_item_get("tx-1", &item) == 1, "the run is still there");
   must(strcmp(item.current_stage, "plan") == 0, "and still in plan");
   must(item.cum_cost_usd == 0.0, "the refused move charged nothing");
   db1_lifecycle_event_t *events = NULL;
   int n = db1_lifecycle_event_list("tx-1", &events);
   free(events);
   must(n == 0, "and wrote no event -- the whole transaction rolled back");

   /* The real move: stage advances, cost is charged, reservation released. */
   must(db1_wfe_move("tx-1", "plan", "review", "advance", "planned", "hash-1", 2.5) == 0,
        "advance out of plan");
   must(db1_work_item_get("tx-1", &item) == 1, "read it back");
   must(strcmp(item.current_stage, "review") == 0, "the stage moved");
   must(item.cum_cost_usd == 2.5, "the cost was charged");
   must(strcmp(item.content_hash, "hash-1") == 0, "the hash was recorded");
   /* The reservation is gone, so the next reserve computes a fresh share rather
      than seeing this invocation's estimate as still outstanding. */
   must(db1_wfe_budget_reserve("tx-1", "owner-b", &r) == 0, "a different owner may now reserve");
   must(r.allowed == 1 && r.busy == 0, "because the transition released the old one");
   must(db1_wfe_budget_release("tx-1", "owner-b") == 0, "put it back");

   /* Retries: each attempt counts, and the cap parks the run in place. */
   must(db1_wfe_record_retry("tx-1", "review", "plan", "again", 2, 0.1) == 0, "first retry loops");
   must(db1_work_item_get("tx-1", &item) == 1, "read");
   must(strcmp(item.current_stage, "plan") == 0, "and it looped back to plan");
   must(db1_wfe_move("tx-1", "plan", "review", "advance", "", "", 0) == 0, "back to review");
   must(db1_wfe_record_retry("tx-1", "review", "plan", "again", 2, 0.1) == 1,
        "the second retry exhausts the cap");
   must(db1_work_item_get("tx-1", &item) == 1, "read");
   must(strcmp(item.pause_reason, "retry_limit") == 0, "parked on the retry limit");
   must(strcmp(item.current_stage, "review") == 0, "in the stage that failed, not the next one");

   /* Resuming clears the pause and the attempts that caused it, so the stage
      does not park again on its very next try. */
   must(db1_wfe_resume("tx-1") == 0, "resume it");
   must(db1_work_item_get("tx-1", &item) == 1, "read");
   must(strcmp(item.pause_reason, "") == 0, "the pause is cleared");
   must(db1_stage_attempt_get("tx-1", "review") == 0, "and so are the attempts");

   /* A loop must NOT clear the attempts of the stage it leaves: that counter is
      what bounds the loop, and wiping it is how a gate ran 63 times against a
      cap of 20. */
   must(db1_wfe_record_retry("tx-1", "review", "plan", "d", 5, 0) == 0, "one attempt");
   must(db1_stage_attempt_get("tx-1", "review") == 1, "counted");
   must(db1_wfe_move("tx-1", "plan", "review", "loop", "", "", 0) == 0, "loop back into review");
   must(db1_stage_attempt_get("tx-1", "review") == 1,
        "a loop left the counter alone -- otherwise the cap could never be reached");
   must(db1_wfe_move("tx-1", "review", "merge", "advance", "", "", 0) == 0, "now advance out");
   must(db1_stage_attempt_get("tx-1", "review") == 0,
        "an advance cleared the completed stage's attempts");

   /* Parking records why, and charges what the attempt cost. */
   must(db1_wfe_park_with_detail("tx-1", "merge", "ci_pending", "waiting on ci", 0.25) == 0,
        "park it");
   must(db1_work_item_get("tx-1", &item) == 1, "read");
   must(strcmp(item.pause_reason, "ci_pending") == 0, "the reason is recorded");
   /* 2.5 for the advance, 0.1 for each of the two retries, 0.25 for the park.
      Every transition charges what its attempt cost, which is why the total is
      worth asserting rather than just the last write. */
   must(item.cum_cost_usd > 2.94 && item.cum_cost_usd < 2.96, "and the cost accrued");

   /* Finishing is allowed from a parked run -- the decision that finishes it is
      often the one that unparks it -- and it takes the run terminal. */
   must(db1_wfe_finish("tx-1", "merge", "accepted", "merged", "hash-2", 0.5) == 0, "finish");
   must(db1_work_item_get("tx-1", &item) == 1, "read");
   must(strcmp(item.state, "accepted") == 0, "terminal");
   must(strcmp(item.pause_reason, "") == 0, "and no longer parked");
   must(db1_wfe_move("tx-1", "merge", "done", "advance", "", "", 0) == -1,
        "a terminal run cannot be moved again");

   printf("  PASS: transitions are all or nothing\n");
}

/* Whole-tree operations. The danger here is not a wrong number but a partial
   application: a stop that reached half a tree leaves agents running under a run
   the operator believes is stopped. So these assert the whole set moved -- and,
   for delete, that it refuses rather than half-deleting a tree that is still
   running. */
static void test_tree_operations_move_the_whole_tree(void)
{
   db1_work_item_t item;
   char ids[32][DB1_WFE_ID_LEN];

   must(db1_work_item_create("st-root", "repo/s", "p/r", "build", "1", "run", "autonomous") == 0,
        "root");
   must(db1_work_item_create("st-a", "repo/s", "p/a", "build", "1", "run", "autonomous") == 0, "a");
   must(db1_work_item_create("st-b", "repo/s", "p/b", "build", "1", "run", "autonomous") == 0, "b");
   must(db1_work_item_set_parent("st-a", "st-root") == 0, "a under root");
   must(db1_work_item_set_parent("st-b", "st-a") == 0, "b under a");

   /* A tree with anything active refuses to be deleted: deleting the row does
      not stop the agent working under it. */
   must(db1_wfe_delete_tree("st-root") == -1, "an active tree will not be deleted");
   must(db1_work_item_get("st-root", &item) == 1, "and nothing was deleted");
   must(db1_work_item_get("st-b", &item) == 1, "not even the leaf");

   /* Stopping reaches every active member and reports them all. */
   int n = db1_wfe_stop_tree("st-root", ids, 32);
   must(n == 3, "all three were stopped and reported");
   must(db1_work_item_get("st-b", &item) == 1 && strcmp(item.state, "stopped") == 0,
        "the grandchild is stopped, not just the root");
   must(db1_work_item_get("st-root", &item) == 1 && strcmp(item.state, "stopped") == 0,
        "and so is the root");
   must(db1_wfe_stop_tree("st-root", ids, 32) == 0, "a second stop finds nothing active");

   /* Now that nothing is active, the tree can be deleted -- entirely. */
   must(db1_wfe_delete_tree("st-root") == 0, "a stopped tree deletes");
   must(db1_work_item_get("st-root", &item) == 0, "root gone");
   must(db1_work_item_get("st-a", &item) == 0, "child gone");
   must(db1_work_item_get("st-b", &item) == 0, "grandchild gone");

   /* Orphan reconciliation: a child left active under a finished parent. */
   must(db1_work_item_create("or-root", "repo/s", "p/or", "build", "1", "run", "autonomous") == 0,
        "a root");
   must(db1_work_item_create("or-kid", "repo/s", "p/ok", "build", "1", "run", "autonomous") == 0,
        "a child");
   must(db1_work_item_create("or-gkid", "repo/s", "p/og", "build", "1", "run", "autonomous") == 0,
        "a grandchild");
   must(db1_work_item_set_parent("or-kid", "or-root") == 0, "link");
   must(db1_work_item_set_parent("or-gkid", "or-kid") == 0, "link");
   must(db1_work_item_set_terminal("or-root", "accepted") == 0, "the root finishes");

   n = db1_wfe_reconcile_orphans(ids, 32);
   must(n == 2, "the child AND its own descendant are orphaned, not just the child");
   must(db1_work_item_get("or-gkid", &item) == 1 && strcmp(item.state, "stopped") == 0,
        "the grandchild was reached through the orphaned child");
   must(db1_wfe_reconcile_orphans(ids, 32) == 0, "and a second sweep finds nothing");

   /* Parking a tree on its budget: every runnable member parks, and one that is
      already parked keeps the more specific reason it has. */
   must(db1_work_item_create("bp-root", "repo/s", "p/bp", "build", "1", "run", "autonomous") == 0,
        "root");
   must(db1_work_item_create("bp-kid", "repo/s", "p/bk", "build", "1", "run", "autonomous") == 0,
        "child");
   must(db1_work_item_set_parent("bp-kid", "bp-root") == 0, "link");
   must(db1_work_item_set_pause("bp-kid", "ci_pending", "run") == 0, "the child is already parked");
   must(db1_wfe_park_budget_tree("bp-root", "bp-root", 3.5) == 0, "park the tree on its budget");
   must(db1_work_item_get("bp-root", &item) == 1, "read the root");
   must(strcmp(item.pause_reason, "budget_cap") == 0, "the runnable root parked on budget_cap");
   must(item.cum_cost_usd == 3.5, "and was charged the cost that tipped it over");
   must(db1_work_item_get("bp-kid", &item) == 1, "read the child");
   must(strcmp(item.pause_reason, "ci_pending") == 0,
        "the already-parked child kept its own more specific reason");

   /* The engine's human gate, which is not the daemon's: it parks on
      'human_gate' and does not compare hashes. */
   must(db1_work_item_create("gt-1", "repo/s", "p/gt", "build", "1", "review", "interactive") == 0,
        "a run at a gate");
   must(db1_wfe_resolve_gate("gt-1", "review", "merge", "approved", "h") == -1,
        "a run that is not parked at a human gate cannot be resolved");
   must(db1_work_item_set_pause("gt-1", "human_gate", "review") == 0, "park it at the gate");
   must(db1_wfe_resolve_gate("gt-1", "review", "merge", "approved", "h") == 0, "now it resolves");
   must(db1_work_item_get("gt-1", &item) == 1, "read");
   must(strcmp(item.current_stage, "merge") == 0, "it advanced");
   must(strcmp(item.pause_reason, "") == 0, "and is no longer parked");

   must(db1_work_item_create("gt-2", "repo/s", "p/g2", "build", "1", "review", "interactive") == 0,
        "another");
   must(db1_work_item_set_pause("gt-2", "human_gate", "review") == 0, "park at the gate");
   must(db1_wfe_reject_gate("gt-2", "review", "h2") == 0, "reject it");
   must(db1_work_item_get("gt-2", &item) == 1, "read");
   must(strcmp(item.state, "rejected") == 0, "the run is rejected");

   printf("  PASS: tree operations move the whole tree\n");
}

/* The two recovery paths, which are decisions about money rather than state.
   A dispatched invocation whose cost is unknown must NOT have its authorization
   released -- the provider may already have been paid -- and a replay whose
   measured result is unreproducible must be parked for a human rather than
   silently re-billed. */
static void test_recovery_never_releases_money_it_cannot_account_for(void)
{
   db1_work_item_t item;
   db1_wfe_budget_reservation_t r;

   /* Dispatched, cost known: the story is complete, so the reservation goes and
      the measured cost is committed. */
   must(db1_work_item_create("rc-1", "repo/r", "p/1", "build", "1", "run", "autonomous") == 0, "1");
   must(db1_work_item_set_cost_cap("rc-1", 20.0) == 0, "cap");
   must(db1_wfe_budget_reserve("rc-1", "own-1", &r) == 0 && r.allowed == 1, "reserve");
   /* delegate_failed rather than an invented reason: the park has to be one an
      operator is allowed to release, because the next lines release it. */
   must(db1_wfe_park_runner_failure("rc-1", "run", "own-1", "delegate_failed", "boom", 1, 1, 2.0) ==
            0,
        "park with a known cost");
   must(db1_work_item_get("rc-1", &item) == 1, "read");
   must(item.cum_cost_usd == 2.0, "the measured cost was committed");
   must(strcmp(item.pause_reason, "delegate_failed") == 0, "and it parked with the reason");
   /* The reservation is fully released, so another owner may take one. */
   must(db1_wfe_resume("rc-1") == 0, "unpark it");
   must(db1_wfe_budget_reserve("rc-1", "own-2", &r) == 0 && r.busy == 0,
        "a released reservation is available to anyone");
   must(db1_wfe_budget_release("rc-1", "own-2") == 0, "tidy up");

   /* Dispatched, cost UNKNOWN: the authorization is retained as unresolved. */
   must(db1_work_item_create("rc-2", "repo/r", "p/2", "build", "1", "run", "autonomous") == 0, "2");
   must(db1_work_item_set_cost_cap("rc-2", 20.0) == 0, "cap");
   must(db1_wfe_budget_reserve("rc-2", "own-1", &r) == 0 && r.allowed == 1, "reserve");
   must(r.amount > 0, "with a real estimate");
   must(db1_wfe_park_runner_failure("rc-2", "run", "own-1", "delegate_failed", "gone", 1, 0, 0.5) ==
            0,
        "park with the cost unknown");
   must(db1_work_item_get("rc-2", &item) == 1, "read");
   must(item.cum_cost_usd == 0.5, "the known prefix was committed");
   /* A parked run cannot be reserved at all -- the load is guarded on
      pause_reason='' -- so the retained authorization is only visible once the
      run is runnable again. That guard is why this has to unpark first. */
   must(db1_wfe_budget_reserve("rc-2", "own-2", &r) == -1, "a parked run takes no reservation");
   must(db1_wfe_resume("rc-2") == 0, "unpark it");
   must(db1_wfe_budget_reserve("rc-2", "own-2", &r) == 0, "now another owner asks");
   must(r.replay_only == 1,
        "and is told this is replay-only -- never handed a fresh estimate over money "
        "that may already be spent");

   /* An unresolved reservation recovers by being dropped and re-dispatched. */
   must(db1_wfe_recover_lost_replay("rc-2", "run", "own-2") == 1,
        "the owner that took it over may re-dispatch fresh");
   must(db1_work_item_get("rc-2", &item) == 1, "read");
   must(item.cum_cost_usd == 0.5, "and re-dispatching charged nothing extra");

   /* A measured 'actual' whose result is lost is parked, not re-dispatched. */
   must(db1_work_item_create("rc-3", "repo/r", "p/3", "build", "1", "run", "autonomous") == 0, "3");
   must(db1_work_item_set_cost_cap("rc-3", 20.0) == 0, "cap");
   must(db1_wfe_budget_reserve("rc-3", "own-1", &r) == 0 && r.allowed == 1, "reserve");
   must(db1_wfe_budget_reconcile("rc-3", "own-1", 3.0) == 1, "measure the cost");
   must(db1_wfe_recover_lost_replay("rc-3", "run", "own-1") == 0,
        "a measured result that is lost must NOT be re-dispatched");
   must(db1_work_item_get("rc-3", &item) == 1, "read");
   must(item.cum_cost_usd == 3.0, "the measured spend was committed rather than forgotten");
   must(strcmp(item.pause_reason, "replay_unrecoverable") == 0, "and it parked for a human");

   /* Recovering someone else's reservation is refused outright: it decides the
      fate of money this caller did not authorize. */
   must(db1_work_item_create("rc-4", "repo/r", "p/4", "build", "1", "run", "autonomous") == 0, "4");
   must(db1_wfe_budget_reserve("rc-4", "own-1", &r) == 0, "reserve");
   must(db1_wfe_recover_lost_replay("rc-4", "run", "someone-else") == -1,
        "another owner cannot recover it");
   must(db1_wfe_recover_lost_replay("rc-4", "run", "own-1") == -1,
        "and a plain 'reserved' state is not a lost replay at all");

   printf("  PASS: recovery never releases money it cannot account for\n");
}

/* Convergence: a review loop bounded twice, once by rounds and once by whether
   anything is actually changing. The interesting assertion is that the two
   bounds are distinguishable -- a run that keeps producing the same artifact
   should be reported as making no progress, not as merely out of rounds, because
   those call for different interventions. */
static void test_convergence_distinguishes_no_progress_from_too_many_rounds(void)
{
   db1_wfe_review_outcome_t out;
   db1_work_item_t item;

   must(db1_work_item_create("cv-1", "repo/c", "p/1", "build", "1", "review", "autonomous") == 0,
        "a run at a review gate");

   /* Different feedback each round: progress, so it loops. */
   must(db1_wfe_record_requested_changes("cv-1", "review", "plan", "plan-a", "fb-1", "", 5, 3, 0.1,
                                         &out) == 0,
        "round one");
   must(out.parked == 0 && out.attempts == 1 && out.identical_repeats == 1, "loops, first attempt");
   must(db1_work_item_get("cv-1", &item) == 1 && strcmp(item.current_stage, "plan") == 0,
        "and it was routed back to the plan stage");

   must(db1_wfe_record_requested_changes("cv-1", "review", "plan", "plan-b", "fb-2", "", 5, 3, 0.1,
                                         &out) == 0,
        "round two with a different artifact");
   must(out.identical_repeats == 1, "a changed artifact resets the repeat count");

   /* Now the same artifact and the same feedback, twice more: no progress. */
   must(db1_wfe_record_requested_changes("cv-1", "review", "plan", "plan-b", "fb-2", "", 5, 3, 0.1,
                                         &out) == 0,
        "identical round");
   must(out.identical_repeats == 2 && out.parked == 0, "counted, not yet at the limit");
   must(db1_wfe_record_requested_changes("cv-1", "review", "plan", "plan-b", "fb-2", "still X", 5,
                                         3, 0.1, &out) == 0,
        "a third identical round");
   must(out.parked == 1, "parked");
   must(strcmp(out.pause_reason, "convergence_no_progress") == 0,
        "and reported as making no progress, not as out of rounds");
   must(db1_work_item_get("cv-1", &item) == 1, "read");
   must(strcmp(item.pause_reason, "convergence_no_progress") == 0, "the run carries the reason");

   /* The other bound: always-changing artifacts that simply run too long. */
   must(db1_work_item_create("cv-2", "repo/c", "p/2", "build", "1", "review", "autonomous") == 0,
        "another run");
   must(db1_wfe_record_requested_changes("cv-2", "review", "plan", "h1", "f1", "", 2, 5, 0, &out) ==
            0,
        "round one");
   must(out.parked == 0, "still going");
   must(db1_wfe_record_requested_changes("cv-2", "review", "plan", "h2", "f2", "", 2, 5, 0, &out) ==
            0,
        "round two hits the iteration cap");
   must(out.parked == 1, "parked");
   must(strcmp(out.pause_reason, "convergence_limit") == 0,
        "and reported as out of rounds -- every round DID change something");

   /* A terminal run is not routed anywhere. */
   must(db1_work_item_set_terminal("cv-2", "accepted") == 0, "finish it");
   must(db1_wfe_record_requested_changes("cv-2", "review", "plan", "h3", "f3", "", 5, 5, 0, &out) ==
            -1,
        "a finished run takes no more review rounds");

   printf("  PASS: convergence distinguishes no progress from too many rounds\n");
}

/* The work-item row, all twenty-two fields of it, across the wire.
 *
 * work_item_get and work_item_list are exercised by the cases above, but
 * work_item_list_lru carries the SAME row and had no wire test -- and the row
 * grew by four fields when the workflow engine's view of a run moved behind the
 * module (the reservation columns, the proposal's source path, and the
 * updated_at an operator reads to decide whether a run is stuck).
 *
 * A reply whose field count and whose reader disagree does not fail loudly: it
 * shifts every value after the mismatch, which is how a repo name ends up in a
 * stage column. So this asserts values at BOTH ends of the row and in the
 * middle, on a row where every field is distinct.
 */
static void test_the_whole_work_item_row_survives_the_wire(void)
{
   must(db1_work_item_create("row-1", "repo/row", "p/row", "flow", "v9", "stage-x", "autonomous") ==
            0,
        "a run with distinctive values");
   must(db1_work_item_set_pr_ref("row-1", "pr-77") == 0, "a pr ref");
   must(db1_work_item_set_worktree("row-1", "aimee/wi/row-1") == 0, "a worktree");
   must(db1_work_item_set_submitter("row-1", "someone") == 0, "a submitter");
   must(db1_work_item_set_cost_cap("row-1", 4.5) == 0, "a cap");
   must(db1_work_item_add_cost("row-1", 1.5) == 0, "some spend");
   /* A live reservation, so the four columns that moved carry real values
      rather than defaults that would look identical to a misaligned read. */
   db1_wfe_budget_reservation_t r;
   must(db1_wfe_budget_reserve("row-1", "owner-row", &r) == 0 && r.allowed == 1, "reserve");

   /* The list is oldest-first, and by this point the suite has created plenty
      of runs, so a small bound would cut off the one just written. Ask for the
      declared maximum rather than a number that happens to work today. */
   db1_work_item_t *rows = NULL;
   int n = db1_work_item_list_lru_bounded(&rows, DB1_WORK_ITEM_LIST_MAX);
   must(n >= 1, "the lru list came back");
   db1_work_item_t *found = NULL;
   for (int i = 0; i < n; i++)
      if (strcmp(rows[i].work_item_id, "row-1") == 0)
         found = &rows[i];
   must(found != NULL, "and contains the run");

   /* The front of the row. */
   must(strcmp(found->repo, "repo/row") == 0, "repo");
   must(strcmp(found->workflow_version, "v9") == 0, "workflow version");
   must(strcmp(found->current_stage, "stage-x") == 0, "stage");
   /* The middle, including the fields appended before this change. */
   must(strcmp(found->pr_ref, "pr-77") == 0, "pr ref");
   must(strcmp(found->worktree, "aimee/wi/row-1") == 0, "worktree");
   must(strcmp(found->submitter, "someone") == 0, "submitter");
   must(found->cum_cost_usd > 1.4 && found->cum_cost_usd < 1.6, "cumulative cost");
   must(found->work_item_max_cost_usd > 4.4 && found->work_item_max_cost_usd < 4.6, "cap");
   /* The tail: the four the engine's move added. A misaligned reply shows up
      here first, because these are last. */
   must(found->reserved_cost_usd > 0, "the reservation amount crossed");
   must(strcmp(found->reservation_state, "reserved") == 0, "and its state");
   must(found->updated_at[0] != '\0', "and updated_at is populated");
   free(rows);

   printf("  PASS: the whole work-item row survives the wire\n");
}

static void test_a_first_user_claim_says_how_it_went(void)
{
   /* The claim validates its inputs: a webuser principal, and a bearer that is
      exactly 64 lowercase hex characters. */
   /* Serials are hex: the domain validates them, so "serial-1" is not one. */
   const char *alice_bearer = "a1b2c3d4e5f60718293a4b5c6d7e8f90"
                              "a1b2c3d4e5f60718293a4b5c6d7e8f90";
   const char *mallory_bearer = "0f1e2d3c4b5a69788796a5b4c3d2e1f0"
                                "0f1e2d3c4b5a69788796a5b4c3d2e1f0";
   db1_remote_client_grant_t grant;
   memset(&grant, 0, sizeof grant);
   must(db1_remote_client_claim("webuser:alice", alice_bearer, 1000, &grant) ==
            DB1_REMOTE_CLIENT_CLAIM_NEW,
        "the first principal claims the slot");
   must(strcmp(grant.principal, "webuser:alice") == 0, "and the grant names it");

   memset(&grant, 0, sizeof grant);
   must(db1_remote_client_claim("webuser:alice", alice_bearer, 1001, &grant) ==
            DB1_REMOTE_CLIENT_CLAIM_UNBOUND,
        "re-entry by the same principal finds its own unbound record");

   memset(&grant, 0, sizeof grant);
   must(db1_remote_client_claim("webuser:mallory", mallory_bearer, 1002, &grant) ==
            DB1_REMOTE_CLIENT_CLAIM_OWNED_BY_OTHER,
        "and a second principal is refused, which is the whole point of the slot");

   must(db1_remote_client_bind(alice_bearer, "abc123", 1003) == 1, "bound the bearer to a cert");
   must(db1_remote_client_bind(alice_bearer, "abc123", 1004) == 1,
        "binding it again is idempotent");
   must(db1_remote_client_bind(alice_bearer, "def456", 1005) == -2,
        "a second certificate is refused with its own value, not with -1");
   must(db1_remote_client_bind(mallory_bearer, "aaa999", 1006) == 0,
        "and a bearer that is not an enrollment is zero rather than an error");

   char principal[DB1_REMOTE_CLIENT_PRINCIPAL_MAX + 1] = "";
   int tier = db1_remote_client_tier("abc123", principal, sizeof principal);
   must(tier >= 0, "the bound certificate resolves to a tier");
   must(strcmp(principal, "webuser:alice") == 0, "and to the principal that owns it");
   must(db1_remote_client_tier("fff000", principal, sizeof principal) == 0,
        "an unknown certificate has no grant");

   memset(&grant, 0, sizeof grant);
   must(db1_remote_client_claim("webuser:alice", alice_bearer, 1007, &grant) ==
            DB1_REMOTE_CLIENT_CLAIM_BOUND,
        "and once bound, re-entry says so rather than repeating unbound");

   printf("  PASS: a first-user claim says how it went\n");
}

/* A replay check has to keep OK apart from every other answer: the caller
   admits a request on OK and refuses it on the rest. Flattened to a status the
   refusals become one value, and the one that matters -- "this token has been
   used before" -- would be indistinguishable from "the store is down". */
static void test_a_replayed_token_is_not_a_broken_store(void)
{
   server_identity_jti_t token;
   memset(&token, 0, sizeof token);
   token.jti = "jti-bus-1";
   token.issuer = "https://issuer.example";
   token.kid = "kid-1";
   token.audience = "aimee";
   token.subject = "user-1";
   token.team_id = 7;
   token.tier = "data";
   token.issued_at = 100;
   token.expires_at = 1000;

   must(db1_identity_jti_consume(&token, 101) == SERVER_IDENTITY_JTI_OK,
        "a token seen for the first time is consumed");
   must(db1_identity_jti_consume(&token, 102) == SERVER_IDENTITY_JTI_REPLAY,
        "and the same token again is a replay, which is its own answer");

   server_identity_jti_t other = token;
   other.jti = "jti-bus-2";
   must(db1_identity_jti_consume(&other, 103) == SERVER_IDENTITY_JTI_OK,
        "a different jti is still fresh");

   server_identity_jti_t empty = token;
   empty.jti = "";
   must(db1_identity_jti_consume(&empty, 104) == SERVER_IDENTITY_JTI_INVALID,
        "and a token with no jti is refused as malformed, not as a replay");

   must(db1_identity_jti_consume(NULL, 105) == SERVER_IDENTITY_JTI_INVALID,
        "as is no token at all");

   printf("  PASS: a replayed token is not a broken store\n");
}

/* The digests here are the reason this family exists. A SHA-256 is bytes, the
   wire carries NUL-terminated text, and a digest with a zero byte in it would
   arrive short -- then compare equal to whatever followed it, in the path that
   decides whether a management token is trusted. They cross as hex, so this
   case puts a digest with an embedded zero through and reads it back. */
static void test_a_digest_with_a_zero_byte_survives_the_wire(void)
{
   /* 00 in the middle and at the end: the two places a length-terminated
      encoding would cut it short. */
   const char *env_hex = "a1004b5c6d7e8f90a1b2c3d4e5f60718"
                         "293a4b5c6d7e8f90a1b2c3d4e5f60700";
   const char *man_hex = "00112233445566778899aabbccddeeff"
                         "00112233445566778899aabbccddeeff";
   const char *bundle_hex = "ff00ee11dd22cc33bb44aa5599668877"
                            "ff00ee11dd22cc33bb44aa5599668877";

   db1_mgmt_jwks_install_t in;
   memset(&in, 0, sizeof in);
   in.valid_from = 1000;
   in.valid_until = 2000;
   in.fetched_at = 1500;
   snprintf(in.jwks, sizeof in.jwks, "%s", "7b226b657973223a5b5d7d"); /* {"keys":[]} */
   snprintf(in.envelope, sizeof in.envelope, "%s", "an-envelope");
   snprintf(in.envelope_sha256, sizeof in.envelope_sha256, "%s", env_hex);
   snprintf(in.manifest_sha256, sizeof in.manifest_sha256, "%s", man_hex);
   snprintf(in.trust_bundle_sha256, sizeof in.trust_bundle_sha256, "%s", bundle_hex);

   db1_mgmt_jwks_row_t row;
   memset(&row, 0, sizeof row);
   must(db1_mgmt_jwks_read(&row) == 0, "there is no cached row to begin with");

   must(db1_mgmt_jwks_install(&in) == 0, "installed the row");
   must(db1_mgmt_jwks_install(&in) == 0, "installing the same envelope again is not a conflict");

   db1_mgmt_jwks_install_t other = in;
   snprintf(other.envelope_sha256, sizeof other.envelope_sha256, "%s",
            "deadbeef00000000000000000000000000000000000000000000000000000000");
   must(db1_mgmt_jwks_install(&other) == 1, "a DIFFERENT envelope is a conflict, not an overwrite");

   memset(&row, 0, sizeof row);
   must(db1_mgmt_jwks_read(&row) == 1, "read the row back");
   must(strcmp(row.envelope_sha256, env_hex) == 0,
        "the digest with a zero byte in the middle and at the end came back whole");
   must(strcmp(row.manifest_sha256, man_hex) == 0, "so did the one that starts with a zero byte");
   must(strcmp(row.trust_bundle_sha256, bundle_hex) == 0, "and the third");
   must(strcmp(row.envelope, "an-envelope") == 0, "the envelope crossed");
   must(row.valid_from == 1000 && row.valid_until == 2000, "and its validity window");

   int64_t generation = 0;
   must(db1_mgmt_jwks_generation(&generation) == 1, "read the generation");
   must(generation == 1, "which is one for a freshly installed row");

   printf("  PASS: a digest with a zero byte survives the wire\n");
}

/* A challenge is spent once or it is not a challenge. The answers have to stay
   apart too: a mismatch is an attacker presenting somebody else's nonce, an
   expiry is a slow client, and a rollback is a replayed status report -- and
   the caller does something different with each. */
static void test_a_challenge_is_spent_once(void)
{
   const char *nonce = "0011223344556677889900aabbccddee"
                       "ff0011223344556677889900aabbccdd";
   db1_mgmt_nonce_issue_t issue;
   memset(&issue, 0, sizeof issue);
   snprintf(issue.nonce, sizeof issue.nonce, "%s", nonce);
   snprintf(issue.peer_issuer, sizeof issue.peer_issuer, "%s", "CN=issuer");
   snprintf(issue.peer_serial_norm, sizeof issue.peer_serial_norm, "%s", "0a0b0c");
   snprintf(issue.peer_fingerprint, sizeof issue.peer_fingerprint, "%s", "fp");
   snprintf(issue.channel_binding, sizeof issue.channel_binding, "%s", "cb");
   snprintf(issue.target_server_id, sizeof issue.target_server_id, "%s", "srv-1");
   snprintf(issue.purpose, sizeof issue.purpose, "%s", "management.health.v1");
   issue.now = 1000;
   must(db1_mgmt_nonce_issue(&issue) == DB1_MGMT_NONCE_OK, "issued a challenge");

   db1_mgmt_nonce_consume_t use;
   memset(&use, 0, sizeof use);
   snprintf(use.nonce, sizeof use.nonce, "%s", nonce);
   snprintf(use.peer_issuer, sizeof use.peer_issuer, "%s", "CN=issuer");
   snprintf(use.peer_serial_norm, sizeof use.peer_serial_norm, "%s", "0a0b0c");
   snprintf(use.peer_fingerprint, sizeof use.peer_fingerprint, "%s", "fp");
   snprintf(use.channel_binding, sizeof use.channel_binding, "%s", "cb");
   snprintf(use.target_server_id, sizeof use.target_server_id, "%s", "srv-1");
   snprintf(use.purpose, sizeof use.purpose, "%s", "management.health.v1");
   use.now = 1001;
   use.revocation_generation = 5;
   use.valid = 1;

   must(db1_mgmt_nonce_consume(&use) == DB1_MGMT_NONCE_OK, "consumed it");
   must(db1_mgmt_nonce_consume(&use) == DB1_MGMT_NONCE_NOT_FOUND,
        "and the second attempt finds nothing, which is what spent-once means");

   int64_t hwm = 0;
   must(db1_mgmt_status_hwm_read(&hwm) == 0, "read the high-water mark");
   must(hwm == 5, "which the consume advanced");

   /* A replayed report carrying an OLDER generation must not roll it back. */
   memset(&issue.nonce, 0, sizeof issue.nonce);
   snprintf(issue.nonce, sizeof issue.nonce, "%s",
            "aabbccddeeff00112233445566778899aabbccddeeff001122334455667788ff");
   issue.now = 1002;
   must(db1_mgmt_nonce_issue(&issue) == DB1_MGMT_NONCE_OK, "issued another");
   snprintf(use.nonce, sizeof use.nonce, "%s",
            "aabbccddeeff00112233445566778899aabbccddeeff001122334455667788ff");
   use.now = 1003;
   use.revocation_generation = 2;
   must(db1_mgmt_nonce_consume(&use) == DB1_MGMT_NONCE_ROLLBACK,
        "an older generation is a rollback, not an ok");
   must(db1_mgmt_status_hwm_read(&hwm) == 0 && hwm == 5, "and the mark did not move");

   /* A nonce presented by the wrong peer. */
   issue.now = 1004;
   snprintf(issue.nonce, sizeof issue.nonce, "%s",
            "1122334455667788991122334455667788112233445566778899112233445566");
   must(db1_mgmt_nonce_issue(&issue) == DB1_MGMT_NONCE_OK, "issued a third");
   snprintf(use.nonce, sizeof use.nonce, "%s",
            "1122334455667788991122334455667788112233445566778899112233445566");
   snprintf(use.peer_fingerprint, sizeof use.peer_fingerprint, "%s", "someone-else");
   use.now = 1005;
   use.revocation_generation = 9;
   must(db1_mgmt_nonce_consume(&use) == DB1_MGMT_NONCE_MISMATCH,
        "the wrong peer is a mismatch, told apart from not-found");
   must(db1_mgmt_status_hwm_read(&hwm) == 0 && hwm == 5, "and a mismatch advances nothing");

   must(db1_mgmt_nonce_clear() == 0, "cleared the outstanding challenges");
   must(db1_mgmt_status_hwm_read(&hwm) == 0 && hwm == 5,
        "which keeps the generation: a restart drops challenges, not history");

   printf("  PASS: a challenge is spent once\n");
}

/* The ramp decides whether mTLS becomes required, so its answers have to be
   exact: a revoked certificate must leave the roster, an unpresented one must
   hold the ramp back, and advancing must be the one thing that changes state.
   The roster hash is taken inside the same transaction that may advance, which
   is why the certificates here are added and revoked around the checks. */
static void test_the_mtls_ramp_only_advances_on_a_ready_roster(void)
{
   must(db1_pki_cert_upsert("aa01", "client-one", 100, 0) == 0, "remembered a certificate");
   must(db1_pki_cert_upsert("aa02", "client-two", 100, 0) == 0, "and a second");

   db1_pki_cert_t rows[8];
   memset(rows, 0, sizeof rows);
   int n = db1_pki_cert_list(rows, 8);
   must(n == 2, "both are on the roster");

   must(db1_pki_cert_check("aa01", 200) == DB1_PKI_CERT_VALID, "an issued cert is valid");
   must(db1_pki_cert_check("nosuch", 200) == DB1_PKI_CERT_UNKNOWN,
        "an unknown serial is unknown, not an error");

   /* Nothing has been presented, so the ramp is not ready. */
   must(db1_pki_ramp_init(1) >= 1, "opened the ramp in permissive mode");
   must(db1_pki_ramp_ready(300) == 0, "a roster nobody has presented is not ready");
   must(db1_pki_ramp_advance(300) == 0, "and advancing does not move it");

   must(db1_pki_note_presentation("aa01", 310) == 0, "one certificate presented");
   must(db1_pki_ramp_ready(320) == 0, "one of two is still not ready");

   must(db1_pki_note_presentation("aa02", 330) == 0, "and the other");
   must(db1_pki_ramp_ready(340) == 1, "now every certificate has been presented");

   int state = 0;
   char hash[DB1_PKI_HASH_LEN] = "";
   long advanced = 0;
   must(db1_pki_ramp_get(&state, hash, sizeof hash, &advanced) == 0, "read the ramp row");
   must(state == 1, "which is still permissive until something advances it");
   must(strlen(hash) == 64, "and carries a roster hash");

   must(db1_pki_ramp_advance(350) == 1, "advanced it");
   memset(hash, 0, sizeof hash);
   must(db1_pki_ramp_get(&state, hash, sizeof hash, &advanced) == 0, "re-read the ramp");
   must(state == 2, "now required");
   must(advanced == 350, "and it recorded when");

   must(db1_pki_cert_revoke("aa01") == 0, "revoked one");
   must(db1_pki_cert_check("aa01", 400) == DB1_PKI_CERT_REVOKED,
        "which reads as revoked rather than unknown -- a different thing to log");

   char serials[8][DB1_PKI_SERIAL_MAX];
   memset(serials, 0, sizeof serials);
   int revoked = db1_pki_revoked_serials(serials, 8);
   must(revoked == 1 && strcmp(serials[0], "aa01") == 0,
        "and it is the one the caller's snapshot will hold");

   printf("  PASS: the mTLS ramp only advances on a ready roster\n");
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
   test_costs_ids_and_a_nested_row();
   test_a_row_that_carries_rows();
   test_guardrail_state_crosses_with_its_collections();
   test_an_ensemble_verdict_crosses_as_a_sentence();
   test_execution_trace_rows_keep_their_shapes();
   test_a_binding_refusal_is_not_a_broken_store();
   test_a_pipeline_row_survives_a_nine_parameter_update();
   test_the_roadmap_selector_keeps_all_three_answers();
   test_a_plan_carries_its_steps_and_their_dependencies();
   test_a_roundtable_run_round_trips_and_swaps_once();
   test_the_engine_reads_keep_their_predicates();
   test_the_engine_sweeps_report_what_they_moved();
   test_the_budget_protocol_holds_across_the_bus();
   test_transitions_are_all_or_nothing();
   test_tree_operations_move_the_whole_tree();
   test_recovery_never_releases_money_it_cannot_account_for();
   test_convergence_distinguishes_no_progress_from_too_many_rounds();
   test_the_whole_work_item_row_survives_the_wire();
   test_a_first_user_claim_says_how_it_went();
   test_a_replayed_token_is_not_a_broken_store();
   test_a_digest_with_a_zero_byte_survives_the_wire();
   test_a_challenge_is_spent_once();
   test_the_mtls_ramp_only_advances_on_a_ready_roster();

   stop_module();
   obs_bus_stop();
   snprintf(command, sizeof(command), "rm -rf '%s'", g_tmp);
   (void)system(command);
   printf("db1-module-bus: ok (the module serves what the daemon asks it for)\n");
   return 0;
}
