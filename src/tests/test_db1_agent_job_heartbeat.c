/* test_db1_agent_job_heartbeat.c: tests for the per-tool-call heartbeat
 * extension on agent_jobs.
 *   1. db1_agent_job_heartbeat_ext writes current_tool + api_call_count
 *      and bumps heartbeat_at.
 *   2. db1_agent_job_classify_stale returns "fresh" for a row that just
 *      got a heartbeat.
 *   3. db1_agent_job_classify_stale returns "in_tool" when a row has a
 *      non-empty current_tool and heartbeat_at older than the in_tool
 *      threshold.
 *   4. db1_agent_job_classify_stale returns "idle" when current_tool is
 *      empty and heartbeat_at older than the idle threshold.
 *   5. The per-state thresholds are independent: an in-tool row with
 *      heartbeat older than idle_threshold but newer than in_tool_threshold
 *      is still "fresh" (a long bash should not idle-time-out). */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>

#include "agent_jobs.h"
#include "agent_log.h"

int db1_init(const char *path);
void db1_shutdown(void);
sqlite3 *db1_conn(void);

static char tmp_db_path[256];

static void setup_db(void)
{
   /* Use a real on-disk DB so db1_init's WAL setup paths exercise.
    * Using /tmp keeps cleanup simple. */
   snprintf(tmp_db_path, sizeof(tmp_db_path), "/tmp/test_db1_aj_hb_%d.sqlite", (int)getpid());
   unlink(tmp_db_path);
   /* Also remove WAL/SHM siblings if they survived a previous run. */
   char p2[300];
   snprintf(p2, sizeof(p2), "%s-wal", tmp_db_path);
   unlink(p2);
   snprintf(p2, sizeof(p2), "%s-shm", tmp_db_path);
   unlink(p2);
   assert(db1_init(tmp_db_path) == 0);
}

static void teardown_db(void)
{
   db1_shutdown();
   unlink(tmp_db_path);
   char p2[300];
   snprintf(p2, sizeof(p2), "%s-wal", tmp_db_path);
   unlink(p2);
   snprintf(p2, sizeof(p2), "%s-shm", tmp_db_path);
   unlink(p2);
}

static void age_heartbeat(int job, int seconds)
{
   char sql[192];
   snprintf(sql, sizeof(sql),
            "UPDATE agent_jobs SET heartbeat_at = datetime('now', '-%d seconds') WHERE id = %d",
            seconds, job);
   assert(sqlite3_exec(db1_conn(), sql, NULL, NULL, NULL) == SQLITE_OK);
}

static void test_heartbeat_ext_writes_fields(void)
{
   setup_db();
   int job = db1_agent_job_create("code", "test prompt", "test_agent", "test_owner");
   assert(job > 0);

   db1_agent_job_t row;
   assert(db1_agent_job_get(job, &row) == 0);
   assert(strcmp(row.status, "pending") == 0);
   assert(row.lease_owner[0] == '\0');
   assert(row.heartbeat_at[0] == '\0');
   db1_agent_job_set_agent(job, "routed_agent");
   assert(db1_agent_job_get(job, &row) == 0);
   assert(strcmp(row.agent_name, "routed_agent") == 0);
   assert(db1_agent_job_take_lease(job, "worker") == 0);
   assert(db1_agent_job_get(job, &row) == 0);
   assert(strcmp(row.status, "running") == 0);
   assert(strcmp(row.lease_owner, "worker") == 0);
   assert(row.heartbeat_at[0] != '\0');

   db1_agent_job_heartbeat_ext(job, "Bash", 7);

   assert(db1_agent_job_get(job, &row) == 0);
   assert(strcmp(row.current_tool, "Bash") == 0);
   assert(row.api_call_count == 7);
   assert(row.heartbeat_at[0] != '\0');

   /* Empty current_tool clears the field. */
   db1_agent_job_heartbeat_ext(job, "", 8);
   assert(db1_agent_job_get(job, &row) == 0);
   assert(row.current_tool[0] == '\0');
   assert(row.api_call_count == 8);

   /* NULL current_tool is treated as empty (defensive). */
   db1_agent_job_heartbeat_ext(job, NULL, 9);
   assert(db1_agent_job_get(job, &row) == 0);
   assert(row.current_tool[0] == '\0');
   assert(row.api_call_count == 9);

   db1_agent_job_free(&row);
   teardown_db();
   printf("  PASS: test_heartbeat_ext_writes_fields\n");
}

/* WP-A: prompt/result are heap-backed (was a fixed char[4096]); a prompt or
 * result larger than the old 4096 cap must round-trip intact, and a result
 * over the storage ceiling must be truncated-with-marker, not the full blob. */
static void test_uncapped_prompt_result_round_trip(void)
{
   setup_db();

   const size_t big = 50000; /* >> the old 4096 cap */
   char *big_prompt = malloc(big + 1);
   assert(big_prompt);
   memset(big_prompt, 'P', big);
   big_prompt[big] = '\0';

   int job = db1_agent_job_create("code", big_prompt, "agent", "owner");
   assert(job > 0);

   db1_agent_job_t row;
   assert(db1_agent_job_get(job, &row) == 0);
   assert(strlen(row.prompt) == big); /* not clipped at 4096 */
   assert(row.prompt[big - 1] == 'P');
   db1_agent_job_free(&row);

   const size_t big_res = 60000; /* > old cap, < storage ceiling */
   char *big_result = malloc(big_res + 1);
   assert(big_result);
   memset(big_result, 'R', big_res);
   big_result[big_res] = '\0';
   db1_agent_job_update(job, "done", 1, big_result);
   assert(db1_agent_job_get(job, &row) == 0);
   assert(strlen(row.result) == big_res);
   db1_agent_job_free(&row);

   /* Over the storage ceiling -> truncated with marker, never the full blob. */
   const size_t over = (size_t)DB1_AJ_RESULT_STORE_MAX + 100000;
   char *huge = malloc(over + 1);
   assert(huge);
   memset(huge, 'H', over);
   huge[over] = '\0';
   db1_agent_job_update(job, "done", 1, huge);
   assert(db1_agent_job_get(job, &row) == 0);
   assert(strlen(row.result) <= DB1_AJ_RESULT_STORE_MAX);
   assert(strstr(row.result, "[truncated") != NULL);
   db1_agent_job_free(&row);

   free(big_prompt);
   free(big_result);
   free(huge);
   teardown_db();
   printf("  PASS: test_uncapped_prompt_result_round_trip\n");
}

static void test_classify_stale_fresh(void)
{
   setup_db();
   int job = db1_agent_job_create("code", "test", "agent", "owner");
   assert(job > 0);
   db1_agent_job_heartbeat_ext(job, "Bash", 1);

   char state[16] = {0};
   int stale =
       db1_agent_job_classify_stale(job, /*idle*/ 60, /*in_tool*/ 600, state, sizeof(state));
   assert(stale == 0);
   assert(strcmp(state, "fresh") == 0);

   teardown_db();
   printf("  PASS: test_classify_stale_fresh\n");
}

static void test_classify_stale_in_tool(void)
{
   setup_db();
   int job = db1_agent_job_create("code", "test", "agent", "owner");
   assert(job > 0);
   db1_agent_job_heartbeat_ext(job, "Bash", 1);

   age_heartbeat(job, 2);

   char state[16] = {0};
   int stale =
       db1_agent_job_classify_stale(job, /*idle*/ 9999, /*in_tool*/ 0, state, sizeof(state));
   assert(stale == 1);
   assert(strcmp(state, "in_tool") == 0);

   teardown_db();
   printf("  PASS: test_classify_stale_in_tool\n");
}

static void test_classify_stale_idle(void)
{
   setup_db();
   int job = db1_agent_job_create("code", "test", "agent", "owner");
   assert(job > 0);
   /* current_tool="" — between calls, awaiting model. */
   db1_agent_job_heartbeat_ext(job, "", 1);

   age_heartbeat(job, 2);

   char state[16] = {0};
   int stale =
       db1_agent_job_classify_stale(job, /*idle*/ 0, /*in_tool*/ 9999, state, sizeof(state));
   assert(stale == 1);
   assert(strcmp(state, "idle") == 0);

   teardown_db();
   printf("  PASS: test_classify_stale_idle\n");
}

static void test_classify_thresholds_are_independent(void)
{
   /* In-tool row with heartbeat already old; idle threshold 0 s, in_tool
    * threshold 9999 s. The row has current_tool != "" so it must use the
    * in_tool threshold and stay "fresh" — a legitimate long bash should not
    * idle-time-out. */
   setup_db();
   int job = db1_agent_job_create("code", "test", "agent", "owner");
   assert(job > 0);
   db1_agent_job_heartbeat_ext(job, "Bash", 1);
   age_heartbeat(job, 2);

   char state[16] = {0};
   int stale =
       db1_agent_job_classify_stale(job, /*idle*/ 0, /*in_tool*/ 9999, state, sizeof(state));
   assert(stale == 0);
   assert(strcmp(state, "fresh") == 0);

   teardown_db();
   printf("  PASS: test_classify_thresholds_are_independent\n");
}

static void test_review_in_tool_uses_short_stale_cap(void)
{
   setup_db();
   int job = db1_agent_job_create("review", "test", "agent", "owner");
   assert(job > 0);
   db1_agent_job_heartbeat_ext(job, "bash", 1);
   age_heartbeat(job, 241);

   char state[16] = {0};
   int stale =
       db1_agent_job_classify_stale(job, /*idle*/ 9999, /*in_tool*/ 1200, state, sizeof(state));
   assert(stale == 1);
   assert(strcmp(state, "in_tool") == 0);

   teardown_db();
   printf("  PASS: test_review_in_tool_uses_short_stale_cap\n");
}

static void test_code_in_tool_keeps_long_stale_threshold(void)
{
   setup_db();
   int job = db1_agent_job_create("code", "test", "agent", "owner");
   assert(job > 0);
   db1_agent_job_heartbeat_ext(job, "bash", 1);
   age_heartbeat(job, 241);

   char state[16] = {0};
   int stale =
       db1_agent_job_classify_stale(job, /*idle*/ 9999, /*in_tool*/ 1200, state, sizeof(state));
   assert(stale == 0);
   assert(strcmp(state, "fresh") == 0);

   teardown_db();
   printf("  PASS: test_code_in_tool_keeps_long_stale_threshold\n");
}

static void test_classify_model_wait_uses_idle_threshold(void)
{
   setup_db();
   int job = db1_agent_job_create("review", "test", "agent", "owner");
   assert(job > 0);
   db1_agent_job_heartbeat_ext(job, "model", 3);
   age_heartbeat(job, 2);

   char state[16] = {0};
   int stale =
       db1_agent_job_classify_stale(job, /*idle*/ 0, /*in_tool*/ 9999, state, sizeof(state));
   assert(stale == 1);
   assert(strcmp(state, "model") == 0);

   teardown_db();
   printf("  PASS: test_classify_model_wait_uses_idle_threshold\n");
}

static void test_classify_final_response_uses_idle_threshold(void)
{
   setup_db();
   int job = db1_agent_job_create("review", "test", "agent", "owner");
   assert(job > 0);
   db1_agent_job_heartbeat_ext(job, "final_response", 3);
   age_heartbeat(job, 121);

   char state[16] = {0};
   int stale =
       db1_agent_job_classify_stale(job, /*idle*/ 9999, /*in_tool*/ 9999, state, sizeof(state));
   assert(stale == 0);
   assert(strcmp(state, "fresh") == 0);

   stale = db1_agent_job_classify_stale(job, /*idle*/ 60, /*in_tool*/ 9999, state, sizeof(state));
   assert(stale == 0);
   assert(strcmp(state, "fresh") == 0);

   age_heartbeat(job, 10000);
   stale = db1_agent_job_classify_stale(job, /*idle*/ 9999, /*in_tool*/ 9999, state, sizeof(state));
   assert(stale == 1);
   assert(strcmp(state, "final_response") == 0);

   teardown_db();
   printf("  PASS: test_classify_final_response_uses_idle_threshold\n");
}

static void test_classify_model_wait_does_not_use_tool_threshold(void)
{
   setup_db();
   int job = db1_agent_job_create("review", "test", "agent", "owner");
   assert(job > 0);
   db1_agent_job_heartbeat_ext(job, "model", 3);
   age_heartbeat(job, 2);

   char state[16] = {0};
   int stale =
       db1_agent_job_classify_stale(job, /*idle*/ 9999, /*in_tool*/ 0, state, sizeof(state));
   assert(stale == 0);
   assert(strcmp(state, "fresh") == 0);

   teardown_db();
   printf("  PASS: test_classify_model_wait_does_not_use_tool_threshold\n");
}

static void test_is_cancelled_round_trip(void)
{
   setup_db();
   int job = db1_agent_job_create("code", "test", "agent", "owner");
   assert(job > 0);

   /* Fresh job is not cancelled. */
   assert(db1_agent_job_is_cancelled(job) == 0);

   assert(db1_agent_job_cancel_by_id(job, "operator cancel") > 0);
   assert(db1_agent_job_is_cancelled(job) == 1);

   /* Unknown id returns 0 rather than erroring. */
   assert(db1_agent_job_is_cancelled(999999) == 0);
   assert(db1_agent_job_is_cancelled(-1) == 0);

   teardown_db();
   printf("  PASS: test_is_cancelled_round_trip\n");
}

static void test_update_does_not_overwrite_cancelled(void)
{
   /* The agent loop's per-turn db1_agent_job_update("running", ...) must
    * not stomp a 'cancelled' status written out-of-band by cmd_cancel —
    * that is what makes cooperative cancellation actually halt the loop. */
   setup_db();
   int job = db1_agent_job_create("code", "test", "agent", "owner");
   assert(job > 0);
   assert(db1_agent_job_cancel_by_id(job, "operator cancel") > 0);

   /* Simulate the agent loop's per-turn update arriving after cancel. */
   db1_agent_job_update(job, "running", 5, NULL);

   db1_agent_job_t row;
   assert(db1_agent_job_get(job, &row) == 0);
   assert(strcmp(row.status, "cancelled") == 0);

   teardown_db();
   printf("  PASS: test_update_does_not_overwrite_cancelled\n");
}

static void test_done_update_wins_cancel_complete_race(void)
{
   setup_db();
   int job = db1_agent_job_create("review", "test", "agent", "owner");
   assert(job > 0);
   assert(db1_agent_job_cancel_by_id(job, "operator cancel") > 0);

   db1_agent_job_update(job, "done", 8, "review result");

   db1_agent_job_t row;
   assert(db1_agent_job_get(job, &row) == 0);
   assert(strcmp(row.status, "done") == 0);
   assert(row.cursor_turn == 8);
   assert(strcmp(row.result, "review result") == 0);

   teardown_db();
   printf("  PASS: test_done_update_wins_cancel_complete_race\n");
}

static void test_failed_update_does_not_overwrite_cancelled(void)
{
   setup_db();
   int job = db1_agent_job_create("review", "test", "agent", "owner");
   assert(job > 0);
   assert(db1_agent_job_cancel_by_id(job, "operator cancel") > 0);

   db1_agent_job_update(job, "failed", 8, "provider error");

   db1_agent_job_t row;
   assert(db1_agent_job_get(job, &row) == 0);
   assert(strcmp(row.status, "cancelled") == 0);
   assert(strcmp(row.result, "cancelled: operator cancel") == 0);

   teardown_db();
   printf("  PASS: test_failed_update_does_not_overwrite_cancelled\n");
}

static void test_backfill_agent_name_from_agent_log(void)
{
   setup_db();
   int job = db1_agent_job_create("review", "test", "", "owner");
   assert(job > 0);
   db1_agent_job_update(job, "done", 4, "ok");

   db1_agent_log_insert_row_t log_row = {
       .agent_name = "llama-eval",
       .role = "review",
       .success = 1,
       .turns = 4,
       .tool_calls = 4,
       .confidence = 80,
       .session_id = "test-session",
   };
   assert(db1_agent_log_insert(&log_row) > 0);

   assert(db1_agent_job_backfill_agent_names_from_log() >= 1);
   db1_agent_job_t row;
   assert(db1_agent_job_get(job, &row) == 0);
   assert(strcmp(row.agent_name, "llama-eval") == 0);

   teardown_db();
   printf("  PASS: test_backfill_agent_name_from_agent_log\n");
}

int main(void)
{
   printf("db1_agent_job_heartbeat:\n");
   test_heartbeat_ext_writes_fields();
   test_uncapped_prompt_result_round_trip();
   test_classify_stale_fresh();
   test_classify_stale_in_tool();
   test_classify_stale_idle();
   test_classify_thresholds_are_independent();
   test_review_in_tool_uses_short_stale_cap();
   test_code_in_tool_keeps_long_stale_threshold();
   test_classify_model_wait_uses_idle_threshold();
   test_classify_final_response_uses_idle_threshold();
   test_classify_model_wait_does_not_use_tool_threshold();
   test_is_cancelled_round_trip();
   test_update_does_not_overwrite_cancelled();
   test_done_update_wins_cancel_complete_race();
   test_failed_update_does_not_overwrite_cancelled();
   test_backfill_agent_name_from_agent_log();
   printf("ok\n");
   return 0;
}
