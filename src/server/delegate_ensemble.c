/* delegate_ensemble.c: Mixture-of-Agents ensemble implementation.
 *
 * Fans the same prompt to N diverse reference models in parallel, then runs
 * a synthesis pass that produces one reconciled answer. Different from
 * agent_vote() (same model N times, majority) -- MoA uses diverse models
 * and synthesizes via an aggregator.
 */
#include "aimee.h"
#include "delegate_ensemble.h"
#include "agent_exec.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Rough cost estimate: $15/MTok all-in (covers GPT-4o, Claude Sonnet range). */
#define ENSEMBLE_COST_PER_TOKEN 0.000015

static void shuffle_indices(int *indices, int count)
{
   if (!indices || count <= 1)
      return;
   /* Seed a thread-local rand_r state from the monotonic clock rather than
    * srand(time(NULL)): the per-call srand clobbered process-global RNG and
    * produced identical shuffles for two calls in the same second (which would
    * silently disable position-bias control when shuffling per round). */
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   unsigned int seed = (unsigned int)ts.tv_nsec ^ (unsigned int)ts.tv_sec;
   for (int i = count - 1; i > 0; i--)
   {
      int j = (int)(rand_r(&seed) % (unsigned int)(i + 1));
      int tmp = indices[i];
      indices[i] = indices[j];
      indices[j] = tmp;
   }
}

static int count_successful(const agent_result_t *results, int count)
{
   int n = 0;
   for (int i = 0; i < count; i++)
   {
      if (results[i].response && results[i].response[0])
         n++;
   }
   return n;
}

static int best_candidate(const agent_result_t *results, int count)
{
   int best = -1;
   int best_len = -1;
   for (int i = 0; i < count; i++)
   {
      if (results[i].response)
      {
         int len = (int)strlen(results[i].response);
         if (len > best_len)
         {
            best_len = len;
            best = i;
         }
      }
   }
   return best;
}

static double estimate_cost(const agent_result_t *results, int count)
{
   double total = 0.0;
   for (int i = 0; i < count; i++)
      total += (results[i].prompt_tokens + results[i].completion_tokens) * ENSEMBLE_COST_PER_TOKEN;
   return total;
}

static int build_synthesis_prompt(char *buf, size_t cap, const char *original_prompt,
                                  const agent_result_t *results, const int *order,
                                  const char (*names)[128], int count)
{
   size_t pos = 0;
   int n = snprintf(buf + pos, cap - pos,
                    "You are a synthesis aggregator for a Mixture-of-Agents ensemble.\n\n"
                    "ORIGINAL PROMPT:\n%s\n\n"
                    "CANDIDATE ANSWERS FROM REFERENCE MODELS:\n\n",
                    original_prompt);
   if (n < 0 || (size_t)n >= cap - pos)
      return -1;
   pos += (size_t)n;

   for (int i = 0; i < count; i++)
   {
      int idx = order[i];
      if (!results[idx].response || !results[idx].response[0])
         continue;
      const char *name = (names && names[idx][0]) ? names[idx] : "model";
      n = snprintf(buf + pos, cap - pos, "--- %s [%d] ---\n%s\n\n", name, i + 1,
                   results[idx].response);
      if (n < 0 || (size_t)n >= cap - pos)
         return -1;
      pos += (size_t)n;
   }

   n = snprintf(buf + pos, cap - pos,
                "TASK: Produce a single reconciled synthesis incorporating the best elements from "
                "each candidate. Do not mention the ensemble process.\n\nSYNTHESIZED ANSWER:\n");
   if (n < 0 || (size_t)n >= cap - pos)
      return -1;
   return 0;
}

static int run_aggregator(agent_config_t *acfg, const config_t *cfg, const char *synthesis_prompt,
                          agent_result_t *out)
{
   memset(out, 0, sizeof(*out));

   agent_config_t agg_cfg = *acfg;
   const char *role = "review"; /* default aggregator role */

   if (cfg->ensemble_aggregator[0])
   {
      const char *agg = cfg->ensemble_aggregator;
      const char *at = strchr(agg, '@');
      if (at)
      {
         size_t role_len = (size_t)(at - agg);
         if (role_len >= sizeof(agg_cfg.default_agent))
            role_len = sizeof(agg_cfg.default_agent) - 1;
         memcpy(agg_cfg.default_agent, agg, role_len);
         agg_cfg.default_agent[role_len] = '\0';
         role = agg_cfg.default_agent;
      }
      else
      {
         snprintf(agg_cfg.default_agent, sizeof(agg_cfg.default_agent), "%s", agg);
         role = agg_cfg.default_agent;
      }
   }

   return agent_run_with_tools_write_enforce(&agg_cfg, role, NULL, synthesis_prompt, 4096, 0, out);
}

int delegate_ensemble_run(agent_config_t *acfg, const config_t *cfg, const char *prompt,
                          delegate_ensemble_result_t *out)
{
   if (!acfg || !cfg || !prompt || !out)
      return -1;

   memset(out, 0, sizeof(*out));

   if (!cfg->ensemble_enabled)
      return -1;

   int ref_count = cfg->ensemble_reference_count;
   if (ref_count <= 0 || ref_count > ENSEMBLE_MAX_REFS)
      return -1;

   /* Build parallel tasks for each reference model */
   agent_task_t tasks[ENSEMBLE_MAX_REFS];
   memset(tasks, 0, sizeof(tasks));
   for (int i = 0; i < ref_count; i++)
   {
      /* Route each fan-out task to its distinct configured reference agent
       * (resolved by name in parallel_worker), not the single default agent.
       * This is what makes the ensemble an ensemble of *diverse* models. */
      tasks[i].role = NULL;
      tasks[i].agent = cfg->ensemble_reference_models[i];
      tasks[i].system_prompt = NULL;
      tasks[i].user_prompt = prompt;
      tasks[i].max_tokens = 0;
   }

   agent_result_t results[ENSEMBLE_MAX_REFS];
   memset(results, 0, sizeof(results));

   agent_run_parallel(acfg, tasks, ref_count, results);

   double cost = estimate_cost(results, ref_count);
   out->cost_usd = cost;

   /* Check cost cap before doing more work */
   if (cfg->ensemble_max_cost_usd > 0.0 && cost > cfg->ensemble_max_cost_usd)
   {
      aimee_log(LOG_INFO, "delegate_ensemble", "cost cap exceeded: $%.4f > $%.4f", cost,
                cfg->ensemble_max_cost_usd);
      out->cost_capped = 1;
      int best = best_candidate(results, ref_count);
      if (best >= 0 && results[best].response)
      {
         snprintf(out->response, sizeof(out->response), "%s", results[best].response);
         out->success = 1;
      }
      for (int i = 0; i < ref_count; i++)
         free(results[i].response);
      return 0;
   }

   int successful = count_successful(results, ref_count);
   int min_ok = cfg->ensemble_min_successful > 0 ? cfg->ensemble_min_successful : 2;

   if (successful < min_ok)
   {
      aimee_log(LOG_INFO, "delegate_ensemble", "insufficient refs: %d < %d, degrading", successful,
                min_ok);
      out->degraded = 1;
      int best = best_candidate(results, ref_count);
      if (best >= 0 && results[best].response)
      {
         snprintf(out->response, sizeof(out->response), "%s", results[best].response);
         out->success = 1;
      }
      for (int i = 0; i < ref_count; i++)
         free(results[i].response);
      return 0;
   }

   /* Shuffle candidate order to avoid position bias */
   int order[ENSEMBLE_MAX_REFS];
   for (int i = 0; i < ref_count; i++)
      order[i] = i;
   shuffle_indices(order, ref_count);

   char synthesis_buf[16384];
   if (build_synthesis_prompt(synthesis_buf, sizeof(synthesis_buf), prompt, results, order,
                              cfg->ensemble_reference_models, ref_count) != 0)
   {
      aimee_log(LOG_ERROR, "delegate_ensemble", "failed to build synthesis prompt");
      for (int i = 0; i < ref_count; i++)
         free(results[i].response);
      return -1;
   }

   for (int i = 0; i < ref_count; i++)
      free(results[i].response);

   agent_result_t agg_result;
   int agg_rc = run_aggregator(acfg, cfg, synthesis_buf, &agg_result);

   if (agg_rc == 0 && agg_result.response && agg_result.response[0])
   {
      snprintf(out->response, sizeof(out->response), "%s", agg_result.response);
      out->cost_usd +=
          (agg_result.prompt_tokens + agg_result.completion_tokens) * ENSEMBLE_COST_PER_TOKEN;
      out->success = 1;
   }
   else
   {
      aimee_log(LOG_ERROR, "delegate_ensemble", "aggregator failed: %s", agg_result.error);
      out->degraded = 1;
   }

   free(agg_result.response);
   return 0;
}

double delegate_ensemble_cost_usd(const delegate_ensemble_result_t *r)
{
   if (!r)
      return 0.0;
   return r->cost_usd;
}