/* test_plan_waves.c: unit tests for plan wave assignment (plan_assign_waves)
 * and coord_job-per-wave primitives.
 */
#include <assert.h>
#include <sqlite3.h>
#include "platform_test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aimee.h"
#include "agent_tasks.h"
#include "db1.h"
#include "cJSON.h"

static char g_db_path[256];

extern sqlite3 *db1_conn(void);

static void setup(void)
{
   snprintf(g_db_path, sizeof(g_db_path), "/tmp/aimee-plan-waves-XXXXXX");
   int fd = mkstemp(g_db_path);
   assert(fd >= 0);
   close(fd);
   assert(db1_init(g_db_path) == 0);
}

static void teardown(void)
{
   db1_shutdown();
   if (g_db_path[0])
   {
      platform_test_remove_sqlite(g_db_path);
      g_db_path[0] = '\0';
   }
}

static int count_step_evidence(int step_id, const char *kind, const char *strength, int passed)
{
   sqlite3_stmt *stmt = NULL;
   int count = 0;
   int rc = sqlite3_prepare_v2(
       db1_conn(),
       "SELECT COUNT(*) FROM step_evidence WHERE step_id = ? AND kind = ? AND strength = ? AND "
       "passed = ?",
       -1, &stmt, NULL);
   assert(rc == SQLITE_OK);
   sqlite3_bind_int(stmt, 1, step_id);
   sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, strength, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 4, passed);
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

/* --- Test: no dependencies → all wave 0 --- */

static void test_assign_waves_no_deps(void)
{
   plan_t p;
   memset(&p, 0, sizeof(p));
   p.id = 1;
   p.step_count = 3;
   snprintf(p.steps[0].action, sizeof(p.steps[0].action), "Step A");
   snprintf(p.steps[1].action, sizeof(p.steps[1].action), "Step B");
   snprintf(p.steps[2].action, sizeof(p.steps[2].action), "Step C");
   for (int i = 0; i < 3; i++)
      p.steps[i].wave = -1;

   int rc = plan_assign_waves(&p);
   assert(rc == 0);

   assert(p.steps[0].wave == 0);
   assert(p.steps[1].wave == 0);
   assert(p.steps[2].wave == 0);

   printf("  PASS: test_assign_waves_no_deps\n");
}

/* --- Test: linear chain A→B→C → waves 0,1,2 --- */

static void test_assign_waves_linear_chain(void)
{
   plan_t p;
   memset(&p, 0, sizeof(p));
   p.id = 1;
   p.step_count = 3;
   snprintf(p.steps[0].action, sizeof(p.steps[0].action), "Step A");
   snprintf(p.steps[1].action, sizeof(p.steps[1].action), "Step B");
   snprintf(p.steps[2].action, sizeof(p.steps[2].action), "Step C");
   for (int i = 0; i < 3; i++)
      p.steps[i].wave = -1;
   /* B depends on A */
   p.steps[1].depends_on[0] = 0;
   p.steps[1].dep_count = 1;
   /* C depends on B */
   p.steps[2].depends_on[0] = 1;
   p.steps[2].dep_count = 1;

   int rc = plan_assign_waves(&p);
   assert(rc == 0);

   assert(p.steps[0].wave == 0);
   assert(p.steps[1].wave == 1);
   assert(p.steps[2].wave == 2);

   printf("  PASS: test_assign_waves_linear_chain\n");
}

/* --- Test: diamond A→{B,C}→D → waves 0,1,1,2 --- */

static void test_assign_waves_diamond(void)
{
   plan_t p;
   memset(&p, 0, sizeof(p));
   p.id = 1;
   p.step_count = 4;

   /* A = step 0, B = step 1, C = step 2, D = step 3 */
   snprintf(p.steps[0].action, sizeof(p.steps[0].action), "A");
   snprintf(p.steps[1].action, sizeof(p.steps[1].action), "B");
   snprintf(p.steps[2].action, sizeof(p.steps[2].action), "C");
   snprintf(p.steps[3].action, sizeof(p.steps[3].action), "D");

   for (int i = 0; i < 4; i++)
      p.steps[i].wave = -1;

   /* B depends on A */
   p.steps[1].depends_on[0] = 0;
   p.steps[1].dep_count = 1;
   /* C depends on A */
   p.steps[2].depends_on[0] = 0;
   p.steps[2].dep_count = 1;
   /* D depends on both B and C */
   p.steps[3].depends_on[0] = 1;
   p.steps[3].depends_on[1] = 2;
   p.steps[3].dep_count = 2;

   int rc = plan_assign_waves(&p);
   assert(rc == 0);

   assert(p.steps[0].wave == 0);
   assert(p.steps[1].wave == 1);
   assert(p.steps[2].wave == 1); /* B and C are in the same wave */
   assert(p.steps[3].wave == 2);

   printf("  PASS: test_assign_waves_diamond\n");
}

/* --- Test: cycle detection returns -1 --- */

static void test_assign_waves_cycle(void)
{
   plan_t p;
   memset(&p, 0, sizeof(p));
   p.id = 1;
   p.step_count = 2;

   snprintf(p.steps[0].action, sizeof(p.steps[0].action), "A");
   snprintf(p.steps[1].action, sizeof(p.steps[1].action), "B");
   p.steps[0].wave = -1;
   p.steps[1].wave = -1;

   /* A depends on B and B depends on A → cycle */
   p.steps[0].depends_on[0] = 1;
   p.steps[0].dep_count = 1;
   p.steps[1].depends_on[0] = 0;
   p.steps[1].dep_count = 1;

   int rc = plan_assign_waves(&p);
   assert(rc == -1);

   printf("  PASS: test_assign_waves_cycle\n");
}

/* --- Test: empty plan returns 0 --- */

static void test_assign_waves_empty(void)
{
   plan_t p;
   memset(&p, 0, sizeof(p));
   p.step_count = 0;

   int rc = plan_assign_waves(&p);
   assert(rc == 0);

   printf("  PASS: test_assign_waves_empty\n");
}

/* --- Test: single step → wave 0 --- */

static void test_assign_waves_single_step(void)
{
   plan_t p;
   memset(&p, 0, sizeof(p));
   p.step_count = 1;
   snprintf(p.steps[0].action, sizeof(p.steps[0].action), "Only step");
   p.steps[0].wave = -1;

   int rc = plan_assign_waves(&p);
   assert(rc == 0);
   assert(p.steps[0].wave == 0);

   printf("  PASS: test_assign_waves_single_step\n");
}

/* --- Test: plan_assign_waves round-trips through DB ---
 * Create a plan with `after` deps, load it, then assign waves. */

static void test_assign_waves_db_roundtrip(void)
{
   setup();

   cJSON *steps = cJSON_CreateArray();

   /* Step 0: no deps */
   cJSON *s0 = cJSON_CreateObject();
   cJSON_AddStringToObject(s0, "action", "Step 0 (no deps)");
   cJSON_AddItemToArray(steps, s0);

   /* Step 1: depends on step 0 */
   cJSON *s1 = cJSON_CreateObject();
   cJSON_AddStringToObject(s1, "action", "Step 1 (after 0)");
   cJSON *after1 = cJSON_CreateArray();
   cJSON_AddItemToArray(after1, cJSON_CreateNumber(0));
   cJSON_AddItemToObject(s1, "after", after1);
   cJSON_AddItemToArray(steps, s1);

   /* Step 2: depends on step 0 (same wave as step 1) */
   cJSON *s2 = cJSON_CreateObject();
   cJSON_AddStringToObject(s2, "action", "Step 2 (after 0)");
   cJSON *after2 = cJSON_CreateArray();
   cJSON_AddItemToArray(after2, cJSON_CreateNumber(0));
   cJSON_AddItemToObject(s2, "after", after2);
   cJSON_AddItemToArray(steps, s2);

   /* Step 3: depends on step 1 and step 2 */
   cJSON *s3 = cJSON_CreateObject();
   cJSON_AddStringToObject(s3, "action", "Step 3 (after 1 and 2)");
   cJSON *after3 = cJSON_CreateArray();
   cJSON_AddItemToArray(after3, cJSON_CreateNumber(1));
   cJSON_AddItemToArray(after3, cJSON_CreateNumber(2));
   cJSON_AddItemToObject(s3, "after", after3);
   cJSON_AddItemToArray(steps, s3);

   int plan_id = db1_execution_plan_create("test-agent", "roundtrip task", steps);
   cJSON_Delete(steps);
   assert(plan_id > 0);

   plan_t p;
   int rc = db1_execution_plan_get(plan_id, &p);
   assert(rc == 0);
   assert(p.step_count == 4);

   /* Verify deps were loaded */
   assert(p.steps[0].dep_count == 0);
   assert(p.steps[1].dep_count == 1 && p.steps[1].depends_on[0] == 0);
   assert(p.steps[2].dep_count == 1 && p.steps[2].depends_on[0] == 0);
   assert(p.steps[3].dep_count == 2);

   rc = plan_assign_waves(&p);
   assert(rc == 0);

   /* Expected: step0=0, step1=1, step2=1, step3=2 */
   assert(p.steps[0].wave == 0);
   assert(p.steps[1].wave == 1);
   assert(p.steps[2].wave == 1);
   assert(p.steps[3].wave == 2);

   teardown();
   printf("  PASS: test_assign_waves_db_roundtrip\n");
}

/* --- Test: wave assignments can be grouped into one coord_job per wave --- */

static void test_execute_waves_creates_coord_jobs(void)
{
   setup();

   /* Three steps: A (wave 0), B (wave 1, depends A), C (wave 1, depends A) */
   cJSON *steps = cJSON_CreateArray();

   cJSON *sA = cJSON_CreateObject();
   cJSON_AddStringToObject(sA, "action", "echo A");
   cJSON_AddItemToArray(steps, sA);

   cJSON *sB = cJSON_CreateObject();
   cJSON_AddStringToObject(sB, "action", "echo B");
   cJSON *afterB = cJSON_CreateArray();
   cJSON_AddItemToArray(afterB, cJSON_CreateNumber(0));
   cJSON_AddItemToObject(sB, "after", afterB);
   cJSON_AddItemToArray(steps, sB);

   cJSON *sC = cJSON_CreateObject();
   cJSON_AddStringToObject(sC, "action", "echo C");
   cJSON *afterC = cJSON_CreateArray();
   cJSON_AddItemToArray(afterC, cJSON_CreateNumber(0));
   cJSON_AddItemToObject(sC, "after", afterC);
   cJSON_AddItemToArray(steps, sC);

   int plan_id = db1_execution_plan_create("test-agent", "wave job test", steps);
   cJSON_Delete(steps);
   assert(plan_id > 0);

   plan_t p;
   assert(db1_execution_plan_get(plan_id, &p) == 0);
   assert(plan_assign_waves(&p) == 0);

   /* Confirm expected waves */
   assert(p.steps[0].wave == 0);
   assert(p.steps[1].wave == 1);
   assert(p.steps[2].wave == 1);

   /* Create coord jobs manually for each wave and verify the correct
    * number of tasks land in each job. */

   /* Wave 0: 1 task */
   int job0 = db1_coord_job_create(plan_id, 3);
   assert(job0 > 0);
   for (int i = 0; i < p.step_count; i++)
      if (p.steps[i].wave == 0)
         assert(db1_coord_job_add_task(job0, p.steps[i].id, "[]", "", "", "", "engineer") > 0);

   db1_coord_job_t status0;
   db1_coord_job_get(job0, &status0);
   assert(status0.total_tasks == 1);

   /* Wave 1: 2 tasks */
   int job1 = db1_coord_job_create(plan_id, 3);
   assert(job1 > 0);
   for (int i = 0; i < p.step_count; i++)
      if (p.steps[i].wave == 1)
         assert(db1_coord_job_add_task(job1, p.steps[i].id, "[]", "", "", "", "engineer") > 0);

   db1_coord_job_t status1;
   db1_coord_job_get(job1, &status1);
   assert(status1.total_tasks == 2);

   teardown();
   printf("  PASS: test_execute_waves_creates_coord_jobs\n");
}

/* --- Test: wave gating — wave 1 tasks are not present in wave 0 job --- */

static void test_execute_waves_gating(void)
{
   setup();

   cJSON *steps = cJSON_CreateArray();

   /* A: wave 0 */
   cJSON *sA = cJSON_CreateObject();
   cJSON_AddStringToObject(sA, "action", "wave0-step");
   cJSON_AddItemToArray(steps, sA);

   /* B: wave 1 (depends on A) */
   cJSON *sB = cJSON_CreateObject();
   cJSON_AddStringToObject(sB, "action", "wave1-step");
   cJSON *afterB = cJSON_CreateArray();
   cJSON_AddItemToArray(afterB, cJSON_CreateNumber(0));
   cJSON_AddItemToObject(sB, "after", afterB);
   cJSON_AddItemToArray(steps, sB);

   int plan_id = db1_execution_plan_create("test-agent", "gating test", steps);
   cJSON_Delete(steps);
   assert(plan_id > 0);

   plan_t p;
   assert(db1_execution_plan_get(plan_id, &p) == 0);
   assert(plan_assign_waves(&p) == 0);

   /* Wave 0 job contains only step A */
   int job0 = db1_coord_job_create(plan_id, 2);
   assert(job0 > 0);
   for (int i = 0; i < p.step_count; i++)
      if (p.steps[i].wave == 0)
         db1_coord_job_add_task(job0, p.steps[i].id, "[]", "", "", "", "engineer");

   db1_coord_task_t tasks[8];
   int count = db1_coord_job_list_tasks(job0, tasks, 8);
   assert(count == 1);

   /* Claim and complete the single wave-0 task */
   db1_coord_task_t claimed;
   int tid = db1_coord_job_claim_next(job0, "worker", &claimed);
   assert(tid > 0);
   db1_coord_job_complete_task(tid, "ok");

   /* Job 0 should now be complete */
   db1_coord_job_t st;
   db1_coord_job_get(job0, &st);
   assert(strcmp(st.status, "complete") == 0);

   /* Only now create wave 1 job — gating enforced by the caller */
   int job1 = db1_coord_job_create(plan_id, 2);
   assert(job1 > 0);
   for (int i = 0; i < p.step_count; i++)
      if (p.steps[i].wave == 1)
         db1_coord_job_add_task(job1, p.steps[i].id, "[]", "", "", "", "engineer");

   int count1 = db1_coord_job_list_tasks(job1, tasks, 8);
   assert(count1 == 1);

   teardown();
   printf("  PASS: test_execute_waves_gating\n");
}

/* --- Test: file-conflict detection across wave tasks --- */

static void test_wave_file_conflict_detection(void)
{
   setup();

   cJSON *steps = cJSON_CreateArray();
   cJSON *s0 = cJSON_CreateObject();
   cJSON_AddStringToObject(s0, "action", "edit src/foo.c");
   cJSON_AddItemToArray(steps, s0);
   cJSON *s1 = cJSON_CreateObject();
   cJSON_AddStringToObject(s1, "action", "also edit src/foo.c");
   cJSON_AddItemToArray(steps, s1);

   int plan_id = db1_execution_plan_create("test-agent", "conflict test", steps);
   cJSON_Delete(steps);
   assert(plan_id > 0);

   /* Both steps in wave 0 */
   plan_t p;
   assert(db1_execution_plan_get(plan_id, &p) == 0);
   assert(plan_assign_waves(&p) == 0);
   assert(p.steps[0].wave == 0 && p.steps[1].wave == 0);

   int job = db1_coord_job_create(plan_id, 2);
   assert(job > 0);
   db1_coord_job_add_task(job, p.steps[0].id, "[\"src/foo.c\"]", "", "", "", "engineer");
   db1_coord_job_add_task(job, p.steps[1].id, "[\"src/foo.c\"]", "", "", "", "engineer");

   /* Claim the first task */
   db1_coord_task_t t1;
   int tid1 = db1_coord_job_claim_next(job, "worker-A", &t1);
   assert(tid1 > 0);

   /* Second task conflicts on src/foo.c → claim_next returns -1 */
   db1_coord_task_t t2;
   int tid2 = db1_coord_job_claim_next(job, "worker-B", &t2);
   assert(tid2 < 0);

   /* After completing the first, the second becomes claimable */
   db1_coord_job_complete_task(tid1, "done");

   db1_coord_task_t t3;
   int tid3 = db1_coord_job_claim_next(job, "worker-B", &t3);
   assert(tid3 > 0);

   teardown();
   printf("  PASS: test_wave_file_conflict_detection\n");
}

static void test_plan_verify_records_strong_and_weak_evidence(void)
{
   setup();
   cJSON *steps = cJSON_CreateArray();
   char sql[256];

   cJSON *s0 = cJSON_CreateObject();
   cJSON_AddStringToObject(s0, "action", "step with predicate");
   cJSON_AddStringToObject(s0, "success_predicate", "needle");
   cJSON_AddItemToArray(steps, s0);

   cJSON *s1 = cJSON_CreateObject();
   cJSON_AddStringToObject(s1, "action", "step without predicate");
   cJSON_AddItemToArray(steps, s1);

   int plan_id = db1_execution_plan_create("test-agent", "verify evidence", steps);
   cJSON_Delete(steps);
   assert(plan_id > 0);

   snprintf(sql, sizeof(sql),
            "UPDATE plan_steps SET status = 'done', output = 'contains needle' WHERE plan_id = %d "
            "AND seq = 0",
            plan_id);
   assert(sqlite3_exec(db1_conn(), sql, NULL, NULL, NULL) == SQLITE_OK);
   snprintf(sql, sizeof(sql),
            "UPDATE plan_steps SET status = 'done', output = 'plain completion' WHERE plan_id = %d "
            "AND seq = 1",
            plan_id);
   assert(sqlite3_exec(db1_conn(), sql, NULL, NULL, NULL) == SQLITE_OK);

   plan_t plan;
   plan_verify_summary_t summary;
   assert(db1_execution_plan_get(plan_id, &plan) == 0);
   assert(agent_plan_verify(&plan, &summary) == 0);
   assert(summary.total_steps == 2);
   assert(summary.strong_passed == 1);
   assert(summary.weak_passed == 1);
   assert(summary.failed == 0);

   assert(count_step_evidence(plan.steps[0].id, "plan_verify", "strong", 1) == 1);
   assert(count_step_evidence(plan.steps[1].id, "plan_verify", "weak", 1) == 1);

   teardown();
   printf("  PASS: test_plan_verify_records_strong_and_weak_evidence\n");
}

static void test_plan_verify_marks_missing_predicate_as_failed(void)
{
   setup();
   cJSON *steps = cJSON_CreateArray();
   char sql[256];

   cJSON *s0 = cJSON_CreateObject();
   cJSON_AddStringToObject(s0, "action", "predicate miss");
   cJSON_AddStringToObject(s0, "success_predicate", "needle");
   cJSON_AddItemToArray(steps, s0);

   int plan_id = db1_execution_plan_create("test-agent", "verify failure", steps);
   cJSON_Delete(steps);
   assert(plan_id > 0);

   snprintf(sql, sizeof(sql),
            "UPDATE plan_steps SET status = 'done', output = 'does not match' WHERE plan_id = %d "
            "AND seq = 0",
            plan_id);
   assert(sqlite3_exec(db1_conn(), sql, NULL, NULL, NULL) == SQLITE_OK);

   plan_t plan;
   plan_verify_summary_t summary;
   assert(db1_execution_plan_get(plan_id, &plan) == 0);
   assert(agent_plan_verify(&plan, &summary) != 0);
   assert(summary.total_steps == 1);
   assert(summary.strong_passed == 0);
   assert(summary.weak_passed == 0);
   assert(summary.failed == 1);
   assert(plan.steps[0].status == 3);
   assert(strcmp(plan.status, "failed") == 0);
   assert(count_step_evidence(plan.steps[0].id, "plan_verify", "strong", 0) == 1);

   sqlite3_stmt *stmt = NULL;
   assert(sqlite3_prepare_v2(db1_conn(), "SELECT status FROM plan_steps WHERE id = ?", -1, &stmt,
                             NULL) == SQLITE_OK);
   sqlite3_bind_int(stmt, 1, plan.steps[0].id);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "failed") == 0);
   sqlite3_finalize(stmt);

   teardown();
   printf("  PASS: test_plan_verify_marks_missing_predicate_as_failed\n");
}

int main(void)
{
   printf("test_plan_waves\n");

   test_assign_waves_no_deps();
   test_assign_waves_linear_chain();
   test_assign_waves_diamond();
   test_assign_waves_cycle();
   test_assign_waves_empty();
   test_assign_waves_single_step();
   test_assign_waves_db_roundtrip();
   test_execute_waves_creates_coord_jobs();
   test_execute_waves_gating();
   test_wave_file_conflict_detection();
   test_plan_verify_records_strong_and_weak_evidence();
   test_plan_verify_marks_missing_predicate_as_failed();

   printf("All plan_waves tests passed.\n");
   return 0;
}
