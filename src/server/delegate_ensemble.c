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

static long monotonic_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static char *xstrdup0(const char *s)
{
   if (!s)
      s = "";
   size_t n = strlen(s) + 1;
   char *out = malloc(n);
   if (out)
      memcpy(out, s, n);
   return out;
}

static char *xasprintf3(const char *a, const char *b, const char *c)
{
   if (!a)
      a = "";
   if (!b)
      b = "";
   if (!c)
      c = "";
   size_t n = strlen(a) + strlen(b) + strlen(c) + 1;
   char *out = malloc(n);
   if (!out)
      return NULL;
   snprintf(out, n, "%s%s%s", a, b, c);
   return out;
}

static int change_ratio_0_100(const char *prev, const char *next)
{
   if (!prev || !prev[0])
      return (next && next[0]) ? 100 : 0;
   if (!next || !next[0])
      return 100;
   size_t lp = strlen(prev);
   size_t ln = strlen(next);
   size_t common = lp < ln ? lp : ln;
   size_t diff = lp > ln ? lp - ln : ln - lp;
   for (size_t i = 0; i < common; i++)
      if (prev[i] != next[i])
         diff++;
   size_t denom = lp > ln ? lp : ln;
   if (denom == 0)
      return 0;
   int ratio = (int)((diff * 100) / denom);
   if (ratio < 0)
      ratio = 0;
   if (ratio > 100)
      ratio = 100;
   return ratio;
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

static char *build_round_prompt(const char *task, const char *artifact, const char *peer_notes,
                                roundtable_mode_t mode, int round, int *truncated)
{
   const size_t budget = 24000;
   const char *role_hint = mode == ROUNDTABLE_REVIEW ? "review and critique" : "draft and revise";
   const char *mode_task =
       mode == ROUNDTABLE_REVIEW
           ? "Return a concise structured review with blocking issues first, then nonblocking notes."
           : "Return the next complete draft. Incorporate useful peer input and do not describe the process.";
   if (!artifact)
      artifact = "";
   if (!peer_notes)
      peer_notes = "";

   size_t needed = strlen(task ? task : "") + strlen(artifact) + strlen(peer_notes) + 1200;
   char *prompt = malloc(needed);
   if (!prompt)
      return NULL;
   snprintf(prompt, needed,
            "You are one participant in an agent roundtable. Your role is to %s.\n\n"
            "TASK:\n%s\n\nCURRENT SHARED ARTIFACT:\n%s\n\nPEER NOTES FROM PREVIOUS TURN:\n%s\n\n"
            "ROUND %d INSTRUCTION:\n%s\n",
            role_hint, task ? task : "", artifact[0] ? artifact : "(none yet)",
            peer_notes[0] ? peer_notes : "(none yet)", round, mode_task);

   if (strlen(prompt) <= budget)
      return prompt;

   if (truncated)
      *truncated = 1;
   free(prompt);
   needed = strlen(task ? task : "") + strlen(artifact) + 1600;
   prompt = malloc(needed);
   if (!prompt)
      return NULL;
   snprintf(prompt, needed,
            "You are one participant in an agent roundtable. Your role is to %s.\n\n"
            "TASK:\n%s\n\nCURRENT SHARED ARTIFACT:\n%.18000s\n\n"
            "PEER NOTES FROM PREVIOUS TURN:\n(omitted from this participant prompt because the "
            "round context exceeded the prompt budget; the aggregator keeps the shared summary.)\n\n"
            "ROUND %d INSTRUCTION:\n%s\n",
            role_hint, task ? task : "", artifact[0] ? artifact : "(none yet)", round, mode_task);
   return prompt;
}

static char *build_round_synthesis_prompt(const char *task, const char *prior_artifact,
                                          const agent_result_t *results, const int *order,
                                          const char (*names)[128], int count,
                                          roundtable_mode_t mode)
{
   size_t cap = 8192 + strlen(task ? task : "") + strlen(prior_artifact ? prior_artifact : "");
   for (int i = 0; i < count; i++)
      if (results[i].response)
         cap += strlen(results[i].response) + 512;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t pos = 0;
   const char *frame =
       mode == ROUNDTABLE_REVIEW
           ? "You are consolidating peer reviews from an agent roundtable."
           : "You are consolidating draft revisions from an agent roundtable.";
   int n = snprintf(buf + pos, cap - pos,
                    "%s\n\nTASK:\n%s\n\nPRIOR SHARED ARTIFACT:\n%s\n\nPARTICIPANT OUTPUTS:\n\n",
                    frame, task ? task : "", prior_artifact && prior_artifact[0] ? prior_artifact : "(none)");
   if (n < 0 || (size_t)n >= cap - pos)
   {
      free(buf);
      return NULL;
   }
   pos += (size_t)n;
   for (int i = 0; i < count; i++)
   {
      int idx = order ? order[i] : i;
      if (!results[idx].response || !results[idx].response[0])
         continue;
      const char *name = (names && names[idx][0]) ? names[idx] : "participant";
      n = snprintf(buf + pos, cap - pos, "--- %s [%d] ---\n%s\n\n", name, i + 1,
                   results[idx].response);
      if (n < 0 || (size_t)n >= cap - pos)
      {
         free(buf);
         return NULL;
      }
      pos += (size_t)n;
   }
   n = snprintf(buf + pos, cap - pos,
                "TASK: Produce the next shared %s. Do not mention the roundtable process.\n",
                mode == ROUNDTABLE_REVIEW ? "consolidated review" : "artifact draft");
   if (n < 0 || (size_t)n >= cap - pos)
   {
      free(buf);
      return NULL;
   }
   return buf;
}

static int run_quality_scorer(agent_config_t *acfg, const char *task, const char *artifact)
{
   char *prompt = xasprintf3(
       "Score this roundtable artifact from 0 to 100 for how well it satisfies the task. "
       "Reply with only the integer score.\n\nTASK:\n",
       task ? task : "", "\n\nARTIFACT:\n");
   if (!prompt)
      return artifact ? (int)strlen(artifact) : 0;
   char *full = xasprintf3(prompt, artifact ? artifact : "", "\n");
   free(prompt);
   if (!full)
      return artifact ? (int)strlen(artifact) : 0;

   agent_result_t score_result;
   memset(&score_result, 0, sizeof(score_result));
   int score = artifact ? (int)strlen(artifact) : 0;
   if (agent_run_with_tools_write_enforce(acfg, "reason", NULL, full, 512, 0, &score_result) == 0 &&
       score_result.response && score_result.response[0])
   {
      char *end = NULL;
      long parsed = strtol(score_result.response, &end, 10);
      if (end && end != score_result.response && parsed >= 0 && parsed <= 100)
         score = (int)parsed;
   }
   free(score_result.response);
   free(full);
   return score;
}

static int run_round_parallel(agent_config_t *acfg, const config_t *cfg, const char *task,
                              const char *artifact, const char *peer_notes, roundtable_mode_t mode,
                              int round, agent_result_t *results, int *truncated)
{
   int ref_count = cfg->ensemble_reference_count;
   agent_task_t tasks[ENSEMBLE_MAX_REFS];
   char *prompts[ENSEMBLE_MAX_REFS];
   memset(tasks, 0, sizeof(tasks));
   memset(prompts, 0, sizeof(prompts));
   for (int i = 0; i < ref_count; i++)
   {
      prompts[i] = build_round_prompt(task, artifact, peer_notes, mode, round, truncated);
      if (!prompts[i])
         goto fail;
      tasks[i].role = mode == ROUNDTABLE_REVIEW ? "review" : "draft";
      tasks[i].agent = cfg->ensemble_reference_models[i];
      tasks[i].user_prompt = prompts[i];
      tasks[i].temperature = 0.3 + (0.05 * i);
      tasks[i].max_tokens = 0;
   }
   agent_run_parallel(acfg, tasks, ref_count, results);
   for (int i = 0; i < ref_count; i++)
      free(prompts[i]);
   return 0;
fail:
   for (int i = 0; i < ref_count; i++)
      free(prompts[i]);
   return -1;
}

static int run_round_sequential(agent_config_t *acfg, const config_t *cfg, const char *task,
                                const char *artifact, char **peer_notes, roundtable_mode_t mode,
                                int round, agent_result_t *results, int *truncated)
{
   int ref_count = cfg->ensemble_reference_count;
   int order[ENSEMBLE_MAX_REFS];
   for (int i = 0; i < ref_count; i++)
      order[i] = i;
   shuffle_indices(order, ref_count);

   for (int oi = 0; oi < ref_count; oi++)
   {
      int i = order[oi];
      char *prompt = build_round_prompt(task, artifact, *peer_notes, mode, round, truncated);
      if (!prompt)
         return -1;
      memset(&results[i], 0, sizeof(results[i]));
      agent_run_named(acfg, cfg->ensemble_reference_models[i], mode == ROUNDTABLE_REVIEW ? "review" : "draft",
                      NULL, prompt, 0, 0.3 + (0.05 * i), &results[i]);
      if (results[i].response && results[i].response[0])
      {
         char label[256];
         snprintf(label, sizeof(label), "\n--- %s ---\n", cfg->ensemble_reference_models[i]);
         char *tmp = xasprintf3(*peer_notes ? *peer_notes : "", label, results[i].response);
         if (tmp)
         {
            free(*peer_notes);
            *peer_notes = tmp;
         }
      }
      free(prompt);
   }
   return 0;
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

int delegate_roundtable_run(agent_config_t *acfg, const config_t *cfg, const char *task,
                            const roundtable_opts_t *opts, roundtable_result_t *out)
{
   if (!acfg || !cfg || !task || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   if (!cfg->ensemble_enabled)
      return -1;
   int ref_count = cfg->ensemble_reference_count;
   if (ref_count <= 0 || ref_count > ENSEMBLE_MAX_REFS)
      return -1;

   roundtable_opts_t local;
   if (opts)
      local = *opts;
   else
   {
      memset(&local, 0, sizeof(local));
      local.mode = ROUNDTABLE_DRAFT;
      local.turns = strcmp(cfg->roundtable_turns, "sequential") == 0 ? ROUNDTABLE_SEQUENTIAL
                                                                      : ROUNDTABLE_PARALLEL;
      local.max_rounds = cfg->roundtable_max_rounds > 0 ? cfg->roundtable_max_rounds : 3;
      local.converge_threshold = cfg->roundtable_converge_threshold;
      local.deadline_ms = cfg->roundtable_deadline_ms;
   }
   if (local.max_rounds <= 0)
      local.max_rounds = 3;
   if (local.converge_threshold < 0)
      local.converge_threshold = 10;

   long start_ms = monotonic_ms();
   char *artifact = xstrdup0("");
   char *peer_notes = xstrdup0("");
   char *best_artifact = NULL;
   int best_score = -1;
   int min_ok = cfg->ensemble_min_successful > 0 ? cfg->ensemble_min_successful : 2;
   if (!artifact || !peer_notes)
      goto fail;

   for (int round = 1; round <= local.max_rounds; round++)
   {
      if (local.deadline_ms > 0 && monotonic_ms() - start_ms >= local.deadline_ms)
      {
         out->deadline_hit = 1;
         break;
      }

      agent_result_t results[ENSEMBLE_MAX_REFS];
      memset(results, 0, sizeof(results));
      int truncated_this_round = 0;
      int rc = local.turns == ROUNDTABLE_SEQUENTIAL
                   ? run_round_sequential(acfg, cfg, task, artifact, &peer_notes, local.mode, round,
                                          results, &truncated_this_round)
                   : run_round_parallel(acfg, cfg, task, artifact, peer_notes, local.mode, round,
                                        results, &truncated_this_round);
      if (rc != 0)
      {
         for (int i = 0; i < ref_count; i++)
            free(results[i].response);
         goto fail;
      }
      if (truncated_this_round)
      {
         out->truncated = 1;
         out->degraded = 1;
      }

      double round_cost = estimate_cost(results, ref_count);
      out->cost_usd += round_cost;
      out->rounds_run = round;
      if (cfg->ensemble_max_cost_usd > 0.0 && out->cost_usd > cfg->ensemble_max_cost_usd)
      {
         out->cost_capped = 1;
         int best = best_candidate(results, ref_count);
         if (best >= 0 && results[best].response)
         {
            free(best_artifact);
            best_artifact = xstrdup0(results[best].response);
            out->best_round = round;
         }
         for (int i = 0; i < ref_count; i++)
            free(results[i].response);
         break;
      }

      int successful = count_successful(results, ref_count);
      if (successful < min_ok)
      {
         out->degraded = 1;
         int best = best_candidate(results, ref_count);
         if (best >= 0 && results[best].response)
         {
            int score = run_quality_scorer(acfg, task, results[best].response);
            if (score > best_score)
            {
               free(best_artifact);
               best_artifact = xstrdup0(results[best].response);
               best_score = score;
               out->best_round = round;
            }
         }
         for (int i = 0; i < ref_count; i++)
            free(results[i].response);
         continue;
      }

      int order[ENSEMBLE_MAX_REFS];
      for (int i = 0; i < ref_count; i++)
         order[i] = i;
      shuffle_indices(order, ref_count);
      char *synthesis_prompt =
          build_round_synthesis_prompt(task, artifact, results, order, cfg->ensemble_reference_models,
                                       ref_count, local.mode);
      for (int i = 0; i < ref_count; i++)
         free(results[i].response);
      if (!synthesis_prompt)
         goto fail;

      agent_result_t agg_result;
      int agg_rc = run_aggregator(acfg, cfg, synthesis_prompt, &agg_result);
      free(synthesis_prompt);
      if (agg_rc != 0 || !agg_result.response || !agg_result.response[0])
      {
         out->degraded = 1;
         free(agg_result.response);
         continue;
      }
      out->cost_usd +=
          (agg_result.prompt_tokens + agg_result.completion_tokens) * ENSEMBLE_COST_PER_TOKEN;

      int ratio = change_ratio_0_100(artifact, agg_result.response);
      free(artifact);
      artifact = xstrdup0(agg_result.response);
      if (!artifact)
      {
         free(agg_result.response);
         goto fail;
      }
      free(peer_notes);
      peer_notes = xstrdup0(agg_result.response);
      if (!peer_notes)
      {
         free(agg_result.response);
         goto fail;
      }

      int score = run_quality_scorer(acfg, task, agg_result.response);
      if (score > best_score)
      {
         free(best_artifact);
         best_artifact = xstrdup0(agg_result.response);
         best_score = score;
         out->best_round = round;
      }
      free(agg_result.response);

      if (cfg->ensemble_max_cost_usd > 0.0 && out->cost_usd > cfg->ensemble_max_cost_usd)
      {
         out->cost_capped = 1;
         break;
      }
      if (round > 1 && ratio <= local.converge_threshold)
      {
         out->converged = 1;
         break;
      }
   }

   if (local.mode == ROUNDTABLE_REVIEW && local.apply_review && best_artifact && best_artifact[0])
   {
      char *apply_prompt =
          xasprintf3("Apply this consolidated review to produce the final draft.\n\nTASK:\n",
                     task ? task : "", "\n\nREVIEW:\n");
      char *full = apply_prompt ? xasprintf3(apply_prompt, best_artifact, "\n") : NULL;
      free(apply_prompt);
      if (full)
      {
         agent_result_t apply_result;
         memset(&apply_result, 0, sizeof(apply_result));
         if (agent_run_with_tools_write_enforce(acfg, "draft", NULL, full, 4096, 0,
                                                &apply_result) == 0 &&
             apply_result.response && apply_result.response[0])
         {
            free(best_artifact);
            best_artifact = xstrdup0(apply_result.response);
            out->cost_usd += (apply_result.prompt_tokens + apply_result.completion_tokens) *
                             ENSEMBLE_COST_PER_TOKEN;
         }
         free(apply_result.response);
         free(full);
      }
   }

   out->artifact = best_artifact ? best_artifact : xstrdup0(artifact);
   free(artifact);
   free(peer_notes);
   return out->artifact ? 0 : -1;

fail:
   free(artifact);
   free(peer_notes);
   free(best_artifact);
   memset(out, 0, sizeof(*out));
   return -1;
}

void delegate_roundtable_result_free(roundtable_result_t *r)
{
   if (!r)
      return;
   free(r->artifact);
   r->artifact = NULL;
}

double delegate_ensemble_cost_usd(const delegate_ensemble_result_t *r)
{
   if (!r)
      return 0.0;
   return r->cost_usd;
}
