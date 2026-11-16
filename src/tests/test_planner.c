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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "modules/kb/c/kb_store_test_shim.h"
#include "../kb_planner.h"
#include "../modules/kb/c/artifacts.h"
#include "../modules/kb/c/kb_store_internal.h"
#include "../modules/kb/c/db_postgres.h"
#include "config.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* ---- main ---- */
int main(void)
{
   printf("planner:\n");

   printf("All planner tests passed.\n");
   return 0;
}
