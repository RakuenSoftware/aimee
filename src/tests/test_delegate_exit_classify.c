/* test_delegate_exit_classify.c: delegate outcomes purchase reusable knowledge. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "db1_client/delegate_learning.h"

static void test_no_progress_is_specific_and_round_trips(void)
{
   dl_exit_metrics_t metrics = {
       .role = "execute",
       .turns = 18,
       .tool_calls = 28,
       .success = 0,
       .had_writes = 0,
       .write_enforce_fired = 0,
       .max_turns_limit = 30,
       .error = "no-progress circuit breaker tripped after 28 successful calls without an edit",
   };
   dl_classification_t result;

   classify_delegate_exit(&metrics, &result);

   assert(result.failure_mode == DL_MODE_NO_PROGRESS);
   assert(result.confidence >= 0.9);
   assert(strstr(result.lesson, "concrete defect hypothesis") != NULL);
   assert(strstr(result.lesson, "smallest justified edit or decisive test") != NULL);
   assert(strstr(result.evidence, "no-progress circuit breaker") != NULL);

   const char *stored = dl_failure_mode_to_string(result.failure_mode);
   assert(strcmp(stored, "no-progress/retrieval-loop") == 0);
   assert(dl_string_to_failure_mode(stored) == DL_MODE_NO_PROGRESS);
}

static void test_other_failures_keep_existing_classification(void)
{
   dl_exit_metrics_t metrics = {
       .role = "execute",
       .turns = 30,
       .tool_calls = 40,
       .success = 0,
       .had_writes = 0,
       .write_enforce_fired = 0,
       .max_turns_limit = 30,
       .error = "turn budget exhausted",
   };
   dl_classification_t result;

   classify_delegate_exit(&metrics, &result);
   assert(result.failure_mode == DL_MODE_MAX_TURNS);
}

int main(void)
{
   printf("delegate_exit_classify: ");
   test_no_progress_is_specific_and_round_trips();
   test_other_failures_keep_existing_classification();
   printf("ok\n");
   return 0;
}
