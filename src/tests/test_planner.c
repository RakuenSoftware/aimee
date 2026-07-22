/* test_planner.c — unit tests for the deliberate-planning substrate.
 *
 * Tests:
 *   1. config_planner_defaults: parser defaults are sane (commands empty, budget=32).
 *   2. config_planner_overrides: parser picks up command paths and planner.* knobs.
 *   3. planner_search_disabled: returns -1 when planner_search_command empty.
 *   4. planner_validate_disabled: returns -1 when constraint_solver_command empty.
 *   5. planner_artifact_write: writes plan_candidate / plan_template artifacts.
 *   6. planner_artifact_write_rejects_bad_kind: rejects unknown kinds and invalid JSON.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "db2_test_shim.h"
#include "../kb_planner.h"
#include "../db2/artifacts.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "config.h"
#include "config_learning.h"

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

/* ---- 1. config_planner_defaults ---- */
static void test_config_planner_defaults(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_apply_planner_settings(&cfg, NULL);

   assert(cfg.planner_search_command[0] == '\0');
   assert(cfg.constraint_solver_command[0] == '\0');
   assert(cfg.planner_budget_default == 32);
   assert(cfg.planner_exploration_constant > 1.3 && cfg.planner_exploration_constant < 1.5);

   printf("  config_planner_defaults: ok\n");
}

/* ---- 2. config_planner_overrides ---- */
static void test_config_planner_overrides(void)
{
   const char *json = "{\"intelligence\":{"
                      "\"planner_search_command\":\"python3 scripts/mcts-planner.py\","
                      "\"constraint_solver_command\":\"python3 scripts/z3-solver.py\","
                      "\"planner\":{\"budget_default\":64,\"exploration_constant\":2.0}"
                      "}}";

   cJSON *root = cJSON_Parse(json);
   assert(root);

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_apply_planner_settings(&cfg, root);
   cJSON_Delete(root);

   assert(strcmp(cfg.planner_search_command, "python3 scripts/mcts-planner.py") == 0);
   assert(strcmp(cfg.constraint_solver_command, "python3 scripts/z3-solver.py") == 0);
   assert(cfg.planner_budget_default == 64);
   assert(cfg.planner_exploration_constant > 1.9 && cfg.planner_exploration_constant < 2.1);

   printf("  config_planner_overrides: ok\n");
}

/* ---- 3. planner_search_disabled ---- */
static void test_planner_search_disabled(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   /* planner_search_command intentionally empty */

   char *out = NULL;
   size_t out_len = 0;
   int rc = kb_planner_search(&cfg, "{\"version\":1}", &out, &out_len);
   assert(rc == -1);
   assert(out == NULL);
   assert(out_len == 0);

   printf("  planner_search_disabled: ok\n");
}

/* ---- 4. planner_validate_disabled ---- */
static void test_planner_validate_disabled(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   /* constraint_solver_command intentionally empty */

   char *out = NULL;
   size_t out_len = 0;
   int rc = kb_planner_validate(&cfg, "{\"version\":1}", &out, &out_len);
   assert(rc == -1);
   assert(out == NULL);
   assert(out_len == 0);

   printf("  planner_validate_disabled: ok\n");
}

/* ---- 5. planner_artifact_write ---- */
static void test_planner_artifact_write(void)
{
   open_db();

   const char *plan_payload =
       "{\"goal\":\"migrate coord_jobs to typed db1 API\","
       "\"subgoals\":[{\"id\":\"s1\",\"desc\":\"read shape\",\"depends_on\":[]}],"
       "\"risk_classes\":[\"db-migration\"],"
       "\"required_verifications\":[\"aimee git verify\"],"
       "\"rollback_plan\":{\"available\":true,\"kind\":\"git_reset --soft\"}}";

   char id_candidate[64] = "";
   int rc = kb_planner_artifact_write("plan_candidate", "coord_jobs_migration", plan_payload,
                                      id_candidate, sizeof(id_candidate));
   assert(rc == 0);
   assert(id_candidate[0] != '\0');

   char id_template[64] = "";
   rc = kb_planner_artifact_write("plan_template", "db_migration", plan_payload, id_template,
                                  sizeof(id_template));
   assert(rc == 0);
   assert(id_template[0] != '\0');

   /* Verify both rows landed with the right kind and scope_kind. */
   void *conn = db2_conn();
   assert(conn);
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT kind, scope_kind, scope_id, state FROM artifacts WHERE id = ?1", err,
       sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", id_candidate);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   assert(strcmp(aimee_pg_column_text(st, 0), "plan_candidate") == 0);
   assert(strcmp(aimee_pg_column_text(st, 1), "plan") == 0);
   assert(strcmp(aimee_pg_column_text(st, 2), "coord_jobs_migration") == 0);
   assert(strcmp(aimee_pg_column_text(st, 3), "proposed") == 0);
   aimee_pg_finalize(st);

   st = aimee_pg_prepare(conn, "SELECT kind FROM artifacts WHERE id = ?1", err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", id_template);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   assert(strcmp(aimee_pg_column_text(st, 0), "plan_template") == 0);
   aimee_pg_finalize(st);

   close_db();
   printf("  planner_artifact_write: ok\n");
}

/* ---- 6. planner_artifact_write_rejects_bad_kind ---- */
static void test_planner_artifact_write_rejects_bad_kind(void)
{
   open_db();

   const char *valid_payload = "{\"goal\":\"x\"}";

   /* Bad kind */
   int rc = kb_planner_artifact_write("plan_typo", "scope", valid_payload, NULL, 0);
   assert(rc == -1);

   /* Bad JSON payload */
   rc = kb_planner_artifact_write("plan_candidate", "scope", "not-json{", NULL, 0);
   assert(rc == -1);

   /* NULL args */
   rc = kb_planner_artifact_write(NULL, "scope", valid_payload, NULL, 0);
   assert(rc == -1);
   rc = kb_planner_artifact_write("plan_candidate", NULL, valid_payload, NULL, 0);
   assert(rc == -1);
   rc = kb_planner_artifact_write("plan_candidate", "scope", NULL, NULL, 0);
   assert(rc == -1);

   close_db();
   printf("  planner_artifact_write_rejects_bad_kind: ok\n");
}

/* ---- main ---- */
int main(void)
{
   printf("planner:\n");

   test_config_planner_defaults();
   test_config_planner_overrides();
   test_planner_search_disabled();
   test_planner_validate_disabled();
   test_planner_artifact_write();
   test_planner_artifact_write_rejects_bad_kind();

   printf("All planner tests passed.\n");
   return 0;
}
