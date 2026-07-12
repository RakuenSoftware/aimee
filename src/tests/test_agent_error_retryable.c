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

   /* Saturation is NOT a retryable provider error. agent_dispatch_one signals it
    * out-of-band via AGENT_RC_AT_LIMIT and callers key off that rc (never this
    * string), so the "at concurrency limit" message must stay non-retryable — else
    * it would wrongly record provider health and be misclassified as a fault. */
   assert(agent_error_is_retryable("agent 'codex' at concurrency limit (max_parallel=2)") == 0);

   printf("  classify: ok\n");
   printf("All agent_error_retryable tests passed.\n");
   return 0;
}
