/* test_delegate_exit_classify.c: delegate outcomes purchase reusable knowledge. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "db1_client/delegate_learning.h"
#include "db1_client/interaction_events.h"
#include <aimee/delegates/delegate_run_phases.h>
#include "agent_config.h"
#include "cJSON.h"

static int learning_rc, exit_rc, approach_rc;
static int learning_calls, exit_calls, approach_calls;
static char stored_payload[4096];

agent_t *agent_find(agent_config_t *cfg, const char *name)
{
   (void)cfg;
   (void)name;
   return NULL;
}

int db1_delegate_learning_record(const char *sid, const char *role, const char *mode,
                                 const char *lesson, const char *evidence, double confidence)
{
   assert(!strcmp(sid, "receipt-session") && !strcmp(role, "execute"));
   assert(mode && lesson && evidence && confidence >= 0);
   learning_calls++;
   return learning_rc;
}

int db1_interaction_event_record(const char *sid, const char *type, const char *actor,
                                 const char *payload, const char *outcome)
{
   assert(!strcmp(sid, "receipt-session") && !strcmp(type, "delegate_exit"));
   assert(!strcmp(actor, "system") && outcome);
   snprintf(stored_payload, sizeof stored_payload, "%s", payload);
   exit_calls++;
   return exit_rc;
}

int approach_store_record_no_progress(const char *goal, const char *failure, const char *ref)
{
   assert(!strcmp(goal, "actual goal") && failure && !strcmp(ref, "receipt-delegate"));
   approach_calls++;
   return approach_rc;
}

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

static void test_failure_evidence_preserves_escaped_errors(void)
{
   char worst[512];
   memset(worst, 1, sizeof worst - 1);
   worst[sizeof worst - 1] = 0;
   const char *errors[] = {
       "provider rejected \"quoted\" value; path C:\\repo\nnext line 界",
       "no-progress circuit breaker: \"quoted\" path \\repo\nnext line 界",
       "drift pre-flight: \"quoted\" path \\repo\nnext line 界",
       "cancel: \"quoted\" path \\repo\nnext line 界",
       worst,
   };
   for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++)
   {
      dl_exit_metrics_t metrics = {
          .role = "execute", .turns = 2, .tool_calls = 1, .error = errors[i]};
      dl_classification_t result;
      classify_delegate_exit(&metrics, &result);
      cJSON *evidence = cJSON_Parse(result.evidence);
      assert(cJSON_IsObject(evidence));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(evidence, "error")),
                     errors[i]));
      cJSON_Delete(evidence);
   }
}

static void test_capture_reports_each_failed_durable_write(void)
{
   agent_result_t result = {.turns = 2, .tool_calls = 1};
   agent_config_t cfg = {0};
   agent_t target = {0};
   snprintf(target.provider, sizeof target.provider, "test-provider");
   snprintf(target.model, sizeof target.model, "fixture-model");
   for (int failed = 0; failed < 3; failed++)
   {
      learning_rc = failed == 1 ? -1 : 0;
      exit_rc = failed == 2 ? -1 : 42;
      approach_rc = 0;
      learning_calls = exit_calls = approach_calls = 0;
      int rc = delegate_record_exit_learning("receipt-session", "execute", &result, 0, 30, &cfg,
                                             &target, "actual goal", "receipt-delegate");
      assert((rc == 0) == (failed == 0));
      assert(learning_calls == 1 && exit_calls == 1 && approach_calls == 0);
      cJSON *payload = cJSON_Parse(stored_payload);
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "learning_recorded")) ==
             (learning_rc == 0));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "provider")),
                     "test-provider"));
      cJSON_Delete(payload);
   }
   learning_rc = 0;
   exit_rc = 42;
   snprintf(result.error, sizeof result.error,
            "no-progress circuit breaker tripped after 28 successful calls without an edit");
   for (int failed = 0; failed < 2; failed++)
   {
      approach_rc = failed ? -1 : 0;
      learning_calls = exit_calls = approach_calls = 0;
      int rc = delegate_record_exit_learning("receipt-session", "execute", &result, -1, 30, &cfg,
                                             &target, "actual goal", "receipt-delegate");
      assert((rc == 0) == !failed);
      assert(learning_calls == 1 && exit_calls == 1 && approach_calls == 1);
   }
}

int main(void)
{
   printf("delegate_exit_classify: ");
   test_no_progress_is_specific_and_round_trips();
   test_other_failures_keep_existing_classification();
   test_failure_evidence_preserves_escaped_errors();
   test_capture_reports_each_failed_durable_write();
   printf("ok\n");
   return 0;
}
