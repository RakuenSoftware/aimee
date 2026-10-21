/* Delegate exit capture retains observations without inventing task evidence. */

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
   cJSON *observed = cJSON_Parse(evidence);
   assert(cJSON_IsObject(observed));
   assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(observed, "had_writes")));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(observed, "verification")),
                  "ungraded"));
   cJSON_Delete(observed);
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

static void test_completion_does_not_fabricate_correctness_or_effects(void)
{
   dl_exit_metrics_t metrics = {
       .role = "execute", .turns = 2, .tool_calls = 1, .success = 1, .had_writes = -1};
   dl_classification_t result;
   classify_delegate_exit(&metrics, &result);
   assert(result.failure_mode == DL_MODE_SUCCESS);
   assert(strstr(result.lesson, "ungraded") && !strstr(result.lesson, "reuse"));
   cJSON *evidence = cJSON_Parse(result.evidence);
   assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(evidence, "had_writes")));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(evidence, "verification")),
                  "ungraded"));
   cJSON_Delete(evidence);
   metrics.success = 0;
   metrics.error = "opaque native execution failed";
   classify_delegate_exit(&metrics, &result);
   assert(result.failure_mode == DL_MODE_UNKNOWN);
   assert(dl_string_to_failure_mode(NULL) == DL_MODE_UNKNOWN);
   assert(dl_string_to_failure_mode("unrecognized") == DL_MODE_UNKNOWN);
   assert(!strcmp(dl_failure_mode_to_string(DL_MODE_UNKNOWN), "unknown"));
}

static void test_exit_metrics_cannot_duplicate_canonical_learning(void)
{
   agent_result_t result = {.turns = 2, .tool_calls = 1};
   agent_config_t cfg = {0};
   agent_t target = {0};
   snprintf(target.provider, sizeof target.provider, "test-provider");
   snprintf(target.model, sizeof target.model, "fixture-model");
   for (int failed = 0; failed < 2; failed++)
   {
      learning_rc = approach_rc = -1;
      exit_rc = failed ? -1 : 42;
      learning_calls = exit_calls = approach_calls = 0;
      int rc = delegate_record_exit_metrics("receipt-session", "execute", &result, 0, 30, &cfg,
                                            &target, "receipt-delegate");
      assert((rc == 0) == !failed);
      assert(learning_calls == 0 && exit_calls == 1 && approach_calls == 0);
      cJSON *payload = cJSON_Parse(stored_payload);
      assert(!cJSON_GetObjectItemCaseSensitive(payload, "learning_recorded"));
      assert(
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "learning_owner")),
                  "canonical_session_log"));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "provider")),
                     "test-provider"));
      assert(cJSON_GetObjectItemCaseSensitive(payload, "turns")->valueint == 2);
      assert(cJSON_GetObjectItemCaseSensitive(payload, "tool_calls")->valueint == 1);
      cJSON_Delete(payload);
   }
   exit_rc = 42;
   snprintf(result.error, sizeof result.error,
            "no-progress circuit breaker tripped after 28 successful calls without an edit");
   learning_calls = exit_calls = approach_calls = 0;
   assert(!delegate_record_exit_metrics("receipt-session", "execute", &result, -1, 30, &cfg,
                                        &target, "receipt-delegate"));
   assert(learning_calls == 0 && exit_calls == 1 && approach_calls == 0);
   cJSON *payload = cJSON_Parse(stored_payload);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "failure_mode")),
                  "no-progress/retrieval-loop"));
   assert(!cJSON_GetObjectItemCaseSensitive(payload, "approach_failure_recorded"));
   cJSON_Delete(payload);
}

int main(void)
{
   printf("delegate_exit_classify: ");
   test_no_progress_is_specific_and_round_trips();
   test_other_failures_keep_existing_classification();
   test_failure_evidence_preserves_escaped_errors();
   test_completion_does_not_fabricate_correctness_or_effects();
   test_exit_metrics_cannot_duplicate_canonical_learning();
   printf("ok\n");
   return 0;
}
