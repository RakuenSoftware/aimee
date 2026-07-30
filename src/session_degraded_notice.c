/* session_degraded_notice.c: the SessionStart degraded-dependency notice.
 * See session_degraded_notice.h. */

#include "session_degraded_notice.h"

#include <stdio.h>
#include <string.h>

/* Render the degraded-dependency notice for the agent's additionalContext, or
 * return 0 when everything the agent depends on is up.
 *
 * The agent must be told when the knowledge service is unreachable. Retrieval,
 * memory recall and code search all silently return nothing in that state, and
 * an agent that cannot tell "no results" from "no service" will confidently
 * report that something does not exist in the codebase when it simply could not
 * look. Pure so the wording and the trigger are testable without a server.
 *
 * `kb` / `retrieval` follow /v1/ready's dependency encoding: "ok", "fail", or
 * "unknown". Only an outright "fail" is worth interrupting the agent over --
 * "unknown" means the sampler has not run yet, which is normal at boot. */
int ss_degraded_notice(const char *kb, const char *retrieval, char *out, size_t cap)
{
   if (!out || cap == 0)
      return 0;
   out[0] = '\0';
   int kb_down = (kb && strcmp(kb, "fail") == 0);
   int retrieval_down = (retrieval && strcmp(retrieval, "fail") == 0);
   if (!kb_down && !retrieval_down)
      return 0;

   const char *what = kb_down ? "The knowledge service is unreachable" : "Retrieval is unavailable";
   int n = snprintf(out, cap,
                    "\n# aimee is running in a degraded state\n\n"
                    "%s. Until it recovers:\n"
                    "- KB search, code search and memory recall return NOTHING. Absence of a\n"
                    "  result does NOT mean the thing does not exist -- say so rather than\n"
                    "  concluding a symbol, file or fact is missing.\n"
                    "- Repositories cloned now are indexed once the service returns; they are\n"
                    "  not lost, but they are not searchable yet.\n"
                    "- Tell the user aimee is not fully functional instead of working around it\n"
                    "  silently.\n",
                    what);
   if (n < 0 || (size_t)n >= cap)
   {
      out[0] = '\0';
      return 0;
   }
   return 1;
}
