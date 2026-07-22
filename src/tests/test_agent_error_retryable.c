/* test_agent_error_retryable.c: agent_error_is_retryable classification —
 * transient (retryable) vs hard (non-retryable) provider errors. Regression for
 * the empty-HTTP-200 case: a 200 with no content is a transient glitch and must
 * be retryable (else one blank response degrades a provider / fails a pinned
 * roundtable seat). */
#include <assert.h>
#include <stdio.h>

#include "aimee.h"
#include "agent_exec.h"

int main(void)
{
   printf("test_agent_error_retryable:\n");

   /* Empty completion on an otherwise-successful HTTP 200 -> retryable. */
   assert(agent_error_is_retryable("no content in response") == 1);
   assert(agent_error_is_retryable("no content in response stream") == 1);
   assert(agent_error_is_retryable("no content in final response") == 1);

   /* Existing transient classes stay retryable. */
   assert(agent_error_is_retryable("HTTP 429 rate limit") == 1);
   assert(agent_error_is_retryable("upstream overloaded") == 1);
   assert(agent_error_is_retryable("connection failed") == 1);
   assert(agent_error_is_retryable("Connection failed") == 1);
   assert(agent_error_is_retryable("request timed out") == 1);

   /* Hard errors stay non-retryable. */
   assert(agent_error_is_retryable(NULL) == 0);
   assert(agent_error_is_retryable("") == 0);
   assert(agent_error_is_retryable("HTTP 400 invalid request") == 0);
   assert(agent_error_is_retryable("HTTP 401 unauthorized") == 0);
   assert(agent_error_is_retryable("model refused the request") == 0);

   /* A hard credential/subscription failure must remain a hard health signal
    * for that agent while still allowing an unpinned delegation to try a peer. */
   assert(agent_rc_should_try_another(-1, "HTTP 401 invalid_api_key") == 1);
   assert(agent_rc_should_try_another(-1, "HTTP 403 authentication failed") == 1);
   assert(agent_rc_should_try_another(-1, "reached your usage limit for this billing cycle") == 1);
   assert(agent_rc_should_try_another(-1, "provider quota exhausted") == 1);
   assert(agent_rc_should_try_another(-1, NULL) == 0);
   assert(agent_rc_should_try_another(AGENT_RC_AT_LIMIT, NULL) == 1);
   /* Bare infrastructure auth statuses and benign prose must not fan out across
    * the provider fleet. */
   assert(agent_rc_should_try_another(-1, "HTTP 401 unauthorized") == 0);
   assert(agent_rc_should_try_another(-1, "HTTP 403 from ingress policy") == 0);
   assert(agent_rc_should_try_another(-1, "no usage limit configured") == 0);
   assert(agent_rc_should_try_another(-1, "job quota exceeded") == 0);
   assert(agent_rc_should_try_another(-1, "HTTP 400 invalid request") == 0);

   /* Every peer-substitutable credential/subscription diagnostic remains a
    * hard provider-health signal. The two classifiers must stay disjoint. */
   static const char *const hard_peer_failures[] = {
       "HTTP 403 authentication failed",
       "invalid_api_key",
       "invalid API key",
       "incorrect API key",
       "reached your usage limit",
       "usage limit for this billing cycle",
       "insufficient_quota",
       "quota exhausted",
       "exceeded your current quota",
       "subscription has lapsed",
       "payment required",
   };
   for (size_t i = 0; i < sizeof(hard_peer_failures) / sizeof(hard_peer_failures[0]); i++)
   {
      assert(agent_error_is_retryable(hard_peer_failures[i]) == 0);
      assert(agent_rc_should_try_another(-1, hard_peer_failures[i]) == 1);
   }

   /* Saturation is NOT a retryable provider error. agent_dispatch_one signals it
    * out-of-band via AGENT_RC_AT_LIMIT and callers key off that rc (never this
    * string), so the "at concurrency limit" message must stay non-retryable — else
    * it would wrongly record provider health and be misclassified as a fault. */
   assert(agent_error_is_retryable("agent 'codex' at concurrency limit (max_parallel=2)") == 0);
   /* The aimee-error slug tag must not flip that classification: a digit-free slug
    * (vs a numeric code that could contain "502"/"503"/...) keeps it non-retryable. */
   assert(
       agent_error_is_retryable(
           "agent 'codex' at concurrency limit (max_parallel=2) [aimee_err=concurrency_limit]") ==
       0);

   printf("  classify: ok\n");
   printf("All agent_error_retryable tests passed.\n");
   return 0;
}
