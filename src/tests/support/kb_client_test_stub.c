/* tests/support/kb_client_test_stub.c: empty stubs for the kb_client
 * helpers that tasks.c (and other DATA-layer modules) call.  Unit
 * tests run without aimee-kb, so the wrappers would otherwise fail
 * to link.  Tests assert on shape, not content, of the output.
 *
 * For tests that exercise work_queue functionality directly via the
 * sqlite shim (e.g. unit-test-cmd-work), the stubs forward to the
 * underlying db2_* helpers when DB2 is initialized so the tests still
 * pass.  Tests that don't init DB2 get the empty-stub behaviour. */
#include "aimee.h"
#include "kb_client.h"
#include "lifecycle.h"
#include "cJSON.h"
#include "dstr.h"
#include <stdlib.h>
#include <string.h>

int kb_client_task_list(const char *state, const char *session_id, int limit, aimee_task_t *out,
                        int max)
{
   (void)state;
   (void)session_id;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int kb_client_decision_log_list(const char *outcome, int limit, db2_decision_log_row_t *out,
                                int max)
{
   (void)outcome;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int kb_client_memory_check_drift(int64_t task_id, const char *file_path, const char *command,
                                 drift_result_t *out)
{
   (void)task_id;
   (void)file_path;
   (void)command;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int kb_client_index_list(project_info_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

int kb_client_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out)
{
   (void)project;
   (void)file_path;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

/* tool_registry wrappers live in kb_client_tool_registry.c — that file
 * already short-circuits to in-process db2_* when DB2 is initialized via
 * the sqlite shim, so unit tests pick up the right behaviour without a
 * stub.  Tests that don't link kb_client_tool_registry.o also don't
 * reference these symbols, so omitting them here is safe. */

int kb_client_memory_list(const char *tier, const char *kind, int limit, memory_t *out, int max)
{
   (void)tier;
   (void)kind;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

char *kb_client_memory_lint_json(void)
{
   return NULL;
}

/* The endogeneity gate is answered by the knowledge service. A test that does
 * not link one gets NULL, which the caller reads as "no reachable ledger" —
 * the same path a real deployment takes when the KB is down. */
char *kb_client_learning_endogeneity_json(int window_days)
{
   (void)window_days;
   return NULL;
}
