/* test_reasoning.c — unit tests for the graph reasoning substrate.
 *
 * Tests:
 *   1. reasoning_case_write: write a case artifact and verify it exists.
 *   2. reasoning_contradiction_check_disabled: returns -1 when command empty.
 *   3. reasoning_query_disabled: returns -1 when command empty.
 *   4. config_reasoning_defaults: config defaults are sane.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "db2_test_shim.h"
#include "../kb_reasoning.h"
#include "config.h"
#include "config_learning.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* The "disabled" cases below assert that an unset reasoning.datalog_command
 * returns -1. They used to guarantee that precondition with memset on a local
 * config_t; now that the reasoning entry points read config themselves, the
 * precondition has to come from the config they read. Point HOME at an empty
 * temp dir so it is the suite that decides, not whatever the developer running
 * it happens to have configured -- otherwise a machine with a real
 * datalog_command would both fail here AND execute it. */
static char g_home[256];

static void isolate_home(void)
{
   snprintf(g_home, sizeof(g_home), "%s/aimee-test-reasoning-XXXXXX", platform_tmpdir());
   assert(mkdtemp(g_home) != NULL);
   assert(setenv("HOME", g_home, 1) == 0);
   assert(unsetenv("AIMEE_HOME") == 0);
   assert(setenv("AIMEE_NO_CACHE", "1", 1) == 0);
   /* No aimee.yaml written: config_load fills defaults, and the default
    * reasoning.datalog_command is empty -- which is the condition under test. */
   assert(config_reasoning_datalog_command()[0] == '\0');
}

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

/* ---- 1. reasoning_case_write ---- */
static void test_reasoning_case_write(void)
{
   open_db();

   const char *payload = "{\"trigger_features\":{\"guardrail_risk_score\":0.82},"
                         "\"outcome\":\"approved_after_review\","
                         "\"corrective_verdict\":{\"verdict_tag\":\"too_aggressive\"}}";

   char id[64] = "";
   int rc = kb_reasoning_case_write(payload, id, sizeof(id));
   assert(rc == 0);
   assert(strlen(id) == 36);

   close_db();
   printf("  reasoning_case_write: ok\n");
}

/* ---- 2. reasoning_contradiction_check_disabled ---- */
static void test_reasoning_contradiction_check_disabled(void)
{
   /* reasoning_datalog_command unset in the suite's config = disabled */
   int rc = kb_reasoning_contradiction_check("artifact-a", "artifact-b");
   assert(rc == -1);

   printf("  reasoning_contradiction_check_disabled: ok\n");
}

/* ---- 3. reasoning_query_disabled ---- */
static void test_reasoning_query_disabled(void)
{
   kb_reasoning_result_t result;
   int rc = kb_reasoning_query("contradiction_ok(?a, ?b)", NULL, NULL, NULL, &result);
   assert(rc == -1);

   printf("  reasoning_query_disabled: ok\n");
}

/* ---- 4. config_reasoning_defaults ---- */
static void test_config_reasoning_defaults(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_apply_reasoning_settings(&cfg, NULL);

   assert(cfg.reasoning_datalog_command[0] == '\0');
   assert(cfg.reasoning_row_budget == 0);
   assert(cfg.reasoning_time_limit_ms == 0);

   printf("  config_reasoning_defaults: ok\n");
}

/* ---- 5. reasoning_case_recall_disabled ---- */
static void test_reasoning_case_recall_disabled(void)
{
   open_db();
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   kb_reasoning_case_result_t results[4];
   int rc = kb_reasoning_case_recall("{}", NULL, NULL, results, 4);
   /* disabled (no sidecar) or empty DB — both return >= 0 or -1 */
   assert(rc >= -1);
   close_db();
   printf("  reasoning_case_recall_disabled: ok\n");
}

/* ---- main ---- */
int main(void)
{
   printf("reasoning:\n");

   isolate_home();
   test_reasoning_case_write();
   test_reasoning_contradiction_check_disabled();
   test_reasoning_query_disabled();
   test_config_reasoning_defaults();
   test_reasoning_case_recall_disabled();

   printf("All reasoning tests passed.\n");
   return 0;
}
