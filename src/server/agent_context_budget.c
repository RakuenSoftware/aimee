#include "aimee.h"
#include "agent.h"
#include "agent_config.h" /* agent_catalog_provider */
#include "model_registry.h"

size_t agent_exec_context_budget_chars(const agent_t *agent)
{
   if (!agent || agent->middleware.context_window <= 0)
      return AGENT_CONTEXT_BUDGET;

   /* Reserve room for the reply, then spend the rest on the prompt.
    *
    * An operator-pinned max_tokens is a real commitment and is reserved in full.
    * The model registry ceiling is NOT: it is the model's theoretical maximum
    * (128k on current frontier models), while `context_window` here is often a
    * deliberate POLICY ceiling below the model's true capability — Claude bills
    * a premium above 200k, and the Codex product expects requests to stay within
    * 272k. Reserving a theoretical maximum out of a policy-capped window spends
    * most of the window on a reply that a delegate turn will not produce: with a
    * 200k ceiling and a 128k ceiling-reserve the prompt budget collapses to 72k.
    *
    * So an UNPINNED reserve is capped at a quarter of the window. A caller that
    * genuinely needs a long reply pins max_tokens (or passes an explicit
    * per-request budget, which never reaches this function). */
   int output_tokens = agent->max_tokens;
   if (output_tokens <= 0)
   {
      int model_ceiling = model_max_output(agent_catalog_provider(agent), agent->model);
      int reserve_cap = agent->middleware.context_window / 4;
      output_tokens = (model_ceiling > 0 && model_ceiling < reserve_cap) ? model_ceiling
                                                                        : reserve_cap;
   }
   int prompt_tokens = agent->middleware.context_window - output_tokens;
   if (prompt_tokens <= 0)
      prompt_tokens = agent->middleware.context_window / 2;
   if (prompt_tokens < 1024)
      prompt_tokens = 1024;

   size_t budget = (size_t)prompt_tokens * 4u;
   if (budget < 4096)
      budget = 4096;
   return budget;
}
