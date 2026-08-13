/* test_pipeline.c: unit tests for agent_pipeline.c
 *
 * Tests: pipeline creation, phase transitions, circuit breakers,
 * graceful degradation (plan/job not present), cancel/resume.
 */
#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "../headers/aimee.h"
#include "../db1/db.h"
#include "../db1/db1.h"
#include "../headers/agent_tasks.h"
#include "../headers/agent_pipeline.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static int insert_plan(const char *task, const char *action)
{
   cJSON *steps = cJSON_CreateArray();
   cJSON *step = cJSON_CreateObject();
   cJSON_AddStringToObject(step, "action", action ? action : "");
   cJSON_AddStringToObject(step, "precondition", "");
   cJSON_AddStringToObject(step, "success_predicate", "");
   cJSON_AddStringToObject(step, "rollback", "");
   cJSON_AddItemToArray(steps, step);
   int plan_id = db1_execution_plan_create("test-agent", task, steps);
   cJSON_Delete(steps);
   return plan_id;
}

static int create_pipeline(const char *task, const char *plan_depth, int *out_id)
{
   char classification[16] = "";
   pipeline_classify_task(task, plan_depth, classification, sizeof(classification));
   return db1_pipeline_create(task, classification, classification, out_id);
}

/* ---- helpers ---- */

static char g_tmpdir[256] = {0};

extern sqlite3 *db1_conn(void);

static void setup_db(void)
{
   snprintf(g_tmpdir, sizeof(g_tmpdir), "%s/aimee-pipeline-test-XXXXXX", platform_tmpdir());
   assert(mkdtemp(g_tmpdir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/aimee.db", g_tmpdir);

   assert(db1_init(path) == 0);
}

static void teardown_db(void)
{
   db1_shutdown();
   if (g_tmpdir[0])
   {
      char cmd[512];
      snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmpdir);
      (void)system(cmd);
      g_tmpdir[0] = '\0';
   }
}

/* ---- test: create pipeline ---- */

static void test_pipeline_create(void)
{
   setup_db();

   int id = 0;
   int rc = create_pipeline(
       "Refactor the autopilot pipeline to validate plan references and coordinate clarify state",
       NULL, &id);
   assert(rc == 0);
   assert(id > 0);

   char buf[1024];
   int sr = pipeline_status_report(id, buf, sizeof(buf));
   assert(sr > 0);
   assert(strstr(buf, "active") != NULL);
   assert(strstr(buf, "classify") != NULL);
   assert(strstr(buf, "plan_depth:complex") != NULL);

   teardown_db();
   printf("  PASS: test_pipeline_create\n");
}

/* ---- test: initial advance (classify -> plan for concrete task) ---- */

static void test_pipeline_advance_plan_missing(void)
{
   setup_db();

   int id = 0;
   assert(create_pipeline(
              "Refactor src/agent_pipeline.c and related tests to improve autopilot status output",
              NULL, &id) == 0);

   char msg[1024];
   int rc = pipeline_advance(id, msg, sizeof(msg));

   /* Should move through classify into plan phase. */
   assert(rc == PIPELINE_ADV_PROGRESSED);
   assert(strstr(msg, "classified") != NULL);

   /* Phase should now be 'plan' */
   char report[1024];
   pipeline_status_report(id, report, sizeof(report));
   assert(strstr(report, "plan") != NULL);
   assert(strstr(report, "class:complex") != NULL);

   teardown_db();
   printf("  PASS: test_pipeline_advance_plan_missing\n");
}

static void test_pipeline_starts_clarify_for_vague_task(void)
{
   setup_db();

   int id = 0;
   assert(create_pipeline("fix it", NULL, &id) == 0);

   char msg[1024];
   int rc = pipeline_advance(id, msg, sizeof(msg));
   assert(rc == PIPELINE_ADV_PROGRESSED);
   assert(strstr(msg, "Clarification session") != NULL);

   char report[1024];
   pipeline_status_report(id, report, sizeof(report));
   assert(strstr(report, "clarify") != NULL);
   assert(strstr(report, "clarify_session_id:") != NULL);

   teardown_db();
   printf("  PASS: test_pipeline_starts_clarify_for_vague_task\n");
}

/* ---- test: circuit breaker on plan phase ---- */

static void test_circuit_breaker_plan(void)
{
   setup_db();

   int id = 0;
   assert(create_pipeline("Update src/agent_pipeline.c without linking any plan", NULL, &id) == 0);

   /* Advance PIPELINE_MAX_ATTEMPTS_PLAN times without linking a plan */
   int last_rc = 0;
   assert(pipeline_advance(id, NULL, 0) == PIPELINE_ADV_PROGRESSED);
   for (int i = 0; i < PIPELINE_MAX_ATTEMPTS_PLAN; i++)
      last_rc = pipeline_advance(id, NULL, 0);

   assert(last_rc == PIPELINE_ADV_PAUSED);

   /* Status should be paused */
   char report[1024];
   pipeline_status_report(id, report, sizeof(report));
   assert(strstr(report, "paused") != NULL);

   /* Further advance on a paused pipeline returns PAUSED */
   int rc2 = pipeline_advance(id, NULL, 0);
   assert(rc2 == PIPELINE_ADV_PAUSED);

   teardown_db();
   printf("  PASS: test_circuit_breaker_plan\n");
}

/* ---- test: resume after pause ---- */

static void test_resume(void)
{
   setup_db();

   int id = 0;
   assert(create_pipeline("Edit src/agent_pipeline.c in a resumable pipeline test", NULL, &id) ==
          0);

   /* Exhaust attempts → paused */
   assert(pipeline_advance(id, NULL, 0) == PIPELINE_ADV_PROGRESSED);
   for (int i = 0; i < PIPELINE_MAX_ATTEMPTS_PLAN; i++)
      pipeline_advance(id, NULL, 0);

   char report[1024];
   pipeline_status_report(id, report, sizeof(report));
   assert(strstr(report, "paused") != NULL);

   /* Resume resets phase_attempts */
   db1_pipeline_t p;
   assert(db1_pipeline_get(id, &p) == 0);
   assert(strcmp(p.status, PIPE_STATUS_PAUSED) == 0);
   assert(db1_pipeline_update(id, PIPE_STATUS_ACTIVE, p.current_phase, 0, p.plan_id, p.job_id,
                              p.request_classification, p.plan_depth, p.clarify_session_id) == 0);
   pipeline_status_report(id, report, sizeof(report));
   assert(strstr(report, "active") != NULL);

   /* Can advance again */
   int rc = pipeline_advance(id, NULL, 0);
   assert(rc == PIPELINE_ADV_PROGRESSED || rc == PIPELINE_ADV_PAUSED);

   teardown_db();
   printf("  PASS: test_resume\n");
}

/* ---- test: cancel ---- */

static void test_cancel(void)
{
   setup_db();

   int id = 0;
   assert(create_pipeline("cancel me", NULL, &id) == 0);

   db1_pipeline_t p;
   assert(db1_pipeline_get(id, &p) == 0);
   assert(db1_pipeline_cancel(id) == 0);

   char report[1024];
   pipeline_status_report(id, report, sizeof(report));
   assert(strstr(report, "cancelled") != NULL);

   /* Advance on cancelled returns error */
   int rc = pipeline_advance(id, NULL, 0);
   assert(rc == PIPELINE_ADV_ERROR);

   teardown_db();
   printf("  PASS: test_cancel\n");
}

/* ---- test: graceful degradation — plan linked, no steps (empty plan) ---- */

static void test_execute_no_steps(void)
{
   setup_db();

   /* Insert a plan with no steps */
   char ts[32];
   now_utc(ts, sizeof(ts));
   sqlite3_stmt *ins =
       db1_prepare(db1_conn(), "INSERT INTO execution_plans (agent_name, task, status, created_at) "
                               "VALUES ('test', 'empty task', 'active', ?)");
   assert(ins != NULL);
   sqlite3_bind_text(ins, 1, ts, -1, SQLITE_TRANSIENT);
   assert(sqlite3_step(ins) == SQLITE_DONE);
   int plan_id = (int)sqlite3_last_insert_rowid(db1_conn());
   sqlite3_reset(ins);

   int id = 0;
   assert(create_pipeline("task with empty plan", NULL, &id) == 0);

   assert(db1_pipeline_link_plan(id, plan_id) == 0);

   /* Advance classify phase */
   char msg[1024];
   int rc = pipeline_advance(id, msg, sizeof(msg));
   assert(rc == PIPELINE_ADV_PROGRESSED);

   /* Advance plan phase → should complete (plan exists) */
   rc = pipeline_advance(id, msg, sizeof(msg));
   assert(rc == PIPELINE_ADV_PROGRESSED); /* plan phase complete → moved to execute */

   /* Advance execute phase → 0 steps → should skip to qa */
   rc = pipeline_advance(id, msg, sizeof(msg));
   assert(rc == PIPELINE_ADV_PROGRESSED); /* execute skipped */

   /* Now in qa phase */
   char report[1024];
   pipeline_status_report(id, report, sizeof(report));
   assert(strstr(report, "qa") != NULL || strstr(report, "validate") != NULL);

   teardown_db();
   printf("  PASS: test_execute_no_steps\n");
}

static void test_plan_validation_blocks_missing_file_reference(void)
{
   setup_db();

   int id = 0;
   assert(create_pipeline("Add a focused edit to src/agent_pipeline.c", "simple", &id) == 0);
   assert(pipeline_advance(id, NULL, 0) == PIPELINE_ADV_PROGRESSED);

   int plan_id = insert_plan("validate refs", "Edit file src/does_not_exist.c");
   assert(plan_id > 0);

   assert(db1_pipeline_link_plan(id, plan_id) == 0);

   char msg[2048];
   int rc = pipeline_advance(id, msg, sizeof(msg));
   assert(rc == PIPELINE_ADV_PROGRESSED);
   assert(strstr(msg, "failed mechanical validation") != NULL);
   assert(strstr(msg, "missing file reference") != NULL);

   char report[1024];
   pipeline_status_report(id, report, sizeof(report));
   assert(strstr(report, "phase:plan") != NULL);

   teardown_db();
   printf("  PASS: test_plan_validation_blocks_missing_file_reference\n");
}

static void test_plan_validation_accepts_existing_file_reference(void)
{
   setup_db();

   int id = 0;
   assert(create_pipeline("Update pipeline code in src/agent_pipeline.c", "simple", &id) == 0);
   assert(pipeline_advance(id, NULL, 0) == PIPELINE_ADV_PROGRESSED);

   int plan_id = insert_plan("validate refs", "Edit file src/server/agent_pipeline.c");
   assert(plan_id > 0);

   assert(db1_pipeline_link_plan(id, plan_id) == 0);

   char msg[2048];
   int rc = pipeline_advance(id, msg, sizeof(msg));
   assert(rc == PIPELINE_ADV_PROGRESSED);
   assert(strstr(msg, "validated") != NULL);

   char report[1024];
   pipeline_status_report(id, report, sizeof(report));
   assert(strstr(report, "phase:execute") != NULL);

   teardown_db();
   printf("  PASS: test_plan_validation_accepts_existing_file_reference\n");
}

/* ---- test: list pipelines ---- */

static void test_pipeline_list(void)
{
   setup_db();

   int id1 = 0, id2 = 0;
   assert(create_pipeline("task alpha", NULL, &id1) == 0);
   assert(create_pipeline("task beta", NULL, &id2) == 0);

   char buf[4096];
   int n = pipeline_list(buf, sizeof(buf), 10);
   assert(n == 2);
   assert(strstr(buf, "task alpha") != NULL);
   assert(strstr(buf, "task beta") != NULL);

   teardown_db();
   printf("  PASS: test_pipeline_list\n");
}

/* ---- test: not-found returns error ---- */

static void test_pipeline_not_found(void)
{
   setup_db();

   int rc = pipeline_advance(99999, NULL, 0);
   assert(rc == PIPELINE_ADV_ERROR);

   char buf[256];
   int sr = pipeline_status_report(99999, buf, sizeof(buf));
   assert(sr == -1);

   teardown_db();
   printf("  PASS: test_pipeline_not_found\n");
}

static void test_handle_autopilot_db1_only_actions_without_shared_db(void)
{
   setup_db();

   int id = 0;
   assert(create_pipeline("task gamma", NULL, &id) == 0);

   /* Pipelines live entirely in DB1 — every autopilot action must run
    * against a DB1-only setup (daemon / CLI fork) without the obsolete
    * "no database connection" gate firing. */

   cJSON *status_args = cJSON_CreateObject();
   cJSON_AddStringToObject(status_args, "action", "status");
   cJSON_AddNumberToObject(status_args, "pipeline_id", id);
   char *status_json = handle_autopilot(status_args);
   assert(status_json != NULL);
   assert(strstr(status_json, "\"error\":\"no database connection\"") == NULL);
   assert(strstr(status_json, "pipeline #") != NULL);
   free(status_json);
   cJSON_Delete(status_args);

   cJSON *list_args = cJSON_CreateObject();
   cJSON_AddStringToObject(list_args, "action", "list");
   char *list_json = handle_autopilot(list_args);
   assert(list_json != NULL);
   assert(strstr(list_json, "\"error\":\"no database connection\"") == NULL);
   assert(strstr(list_json, "task gamma") != NULL);
   free(list_json);
   cJSON_Delete(list_args);

   cJSON *cancel_args = cJSON_CreateObject();
   cJSON_AddStringToObject(cancel_args, "action", "cancel");
   cJSON_AddNumberToObject(cancel_args, "pipeline_id", id);
   char *cancel_json = handle_autopilot(cancel_args);
   assert(cancel_json != NULL);
   assert(strstr(cancel_json, "\"error\":\"no database connection\"") == NULL);
   assert(strstr(cancel_json, "cancelled") != NULL);
   free(cancel_json);
   cJSON_Delete(cancel_args);

   cJSON *start_args = cJSON_CreateObject();
   cJSON_AddStringToObject(start_args, "action", "start");
   cJSON_AddStringToObject(start_args, "task", "fresh pipeline without db2");
   char *start_json = handle_autopilot(start_args);
   assert(start_json != NULL);
   assert(strstr(start_json, "\"error\":\"no database connection\"") == NULL);
   assert(strstr(start_json, "pipeline #") != NULL);
   free(start_json);
   cJSON_Delete(start_args);

   cJSON *advance_args = cJSON_CreateObject();
   cJSON_AddStringToObject(advance_args, "action", "advance");
   cJSON_AddNumberToObject(advance_args, "pipeline_id", id);
   char *advance_json = handle_autopilot(advance_args);
   assert(advance_json != NULL);
   assert(strstr(advance_json, "\"error\":\"no database connection\"") == NULL);
   free(advance_json);
   cJSON_Delete(advance_args);

   teardown_db();
   printf("  PASS: test_handle_autopilot_db1_only_actions_without_shared_db\n");
}

/* ---- main ---- */

int main(void)
{
   printf("test_pipeline\n");
   test_pipeline_create();
   test_pipeline_advance_plan_missing();
   test_pipeline_starts_clarify_for_vague_task();
   test_circuit_breaker_plan();
   test_resume();
   test_cancel();
   test_execute_no_steps();
   test_plan_validation_blocks_missing_file_reference();
   test_plan_validation_accepts_existing_file_reference();
   test_pipeline_list();
   test_pipeline_not_found();
   test_handle_autopilot_db1_only_actions_without_shared_db();
   printf("All tests passed.\n");
   return 0;
}
