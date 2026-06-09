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
#include <ctype.h>

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

static int token_seen(char tokens[][64], int count, const char *tok)
{
   for (int i = 0; i < count; i++)
      if (strcmp(tokens[i], tok) == 0)
         return 1;
   return 0;
}

static int key_seen128(char keys[][128], int count, const char *key)
{
   for (int i = 0; i < count; i++)
      if (strcmp(keys[i], key) == 0)
         return 1;
   return 0;
}

static int tokenize_unique(const char *s, char tokens[][64], int max)
{
   int count = 0;
   char tok[64];
   int pos = 0;
   for (const unsigned char *p = (const unsigned char *)(s ? s : "");; p++)
   {
      if (isalnum(*p))
      {
         if (pos < (int)sizeof(tok) - 1)
            tok[pos++] = (char)tolower(*p);
      }
      else if (pos > 0)
      {
         tok[pos] = '\0';
         if (count < max && !token_seen(tokens, count, tok))
         {
            snprintf(tokens[count], 64, "%s", tok);
            count++;
         }
         pos = 0;
      }
      if (*p == '\0')
         break;
   }
   return count;
}

static int normalized_edit_change_0_100(const char *prev, const char *next);

static int token_jaccard_change_0_100(const char *prev, const char *next)
{
   char(*a)[64] = calloc(512, sizeof(*a));
   char(*b)[64] = calloc(512, sizeof(*b));
   if (!a || !b)
   {
      free(a);
      free(b);
      return normalized_edit_change_0_100(prev, next);
   }
   int na = tokenize_unique(prev, a, 512);
   int nb = tokenize_unique(next, b, 512);
   if (na == 0 && nb == 0)
   {
      free(a);
      free(b);
      return 0;
   }
   int inter = 0;
   for (int i = 0; i < na; i++)
      if (token_seen(b, nb, a[i]))
         inter++;
   int uni = na + nb - inter;
   if (uni <= 0)
   {
      free(a);
      free(b);
      return 0;
   }
   int change = 100 - (inter * 100 / uni);
   free(a);
   free(b);
   return change;
}

static int normalized_edit_change_0_100(const char *prev, const char *next)
{
   const char *ps = prev ? prev : "";
   const char *ns = next ? next : "";
   size_t a = strlen(ps);
   size_t b = strlen(ns);
   size_t denom = a > b ? a : b;
   if (denom == 0)
      return 0;
   if (a > 4096 || b > 4096)
   {
      size_t common = a < b ? a : b;
      size_t diff = a > b ? a - b : b - a;
      for (size_t i = 0; i < common; i++)
         if (ps[i] != ns[i])
            diff++;
      return (int)((diff * 100) / denom);
   }
   int *prev_row = calloc(b + 1, sizeof(int));
   int *cur_row = calloc(b + 1, sizeof(int));
   if (!prev_row || !cur_row)
   {
      free(prev_row);
      free(cur_row);
      return token_jaccard_change_0_100(prev, next);
   }
   for (size_t j = 0; j <= b; j++)
      prev_row[j] = (int)j;
   for (size_t i = 1; i <= a; i++)
   {
      cur_row[0] = (int)i;
      for (size_t j = 1; j <= b; j++)
      {
         int cost = ps[i - 1] == ns[j - 1] ? 0 : 1;
         int del = prev_row[j] + 1;
         int ins = cur_row[j - 1] + 1;
         int sub = prev_row[j - 1] + cost;
         int v = del < ins ? del : ins;
         cur_row[j] = v < sub ? v : sub;
      }
      int *tmp = prev_row;
      prev_row = cur_row;
      cur_row = tmp;
   }
   int dist = prev_row[b];
   free(prev_row);
   free(cur_row);
   return (int)(((size_t)dist * 100) / denom);
}

static int change_ratio_0_100(const char *prev, const char *next)
{
   int tj = token_jaccard_change_0_100(prev, next);
   int ed = normalized_edit_change_0_100(prev, next);
   return (tj + ed) / 2;
}

static double estimated_stage_cost(int calls, int tokens_per_call)
{
   return (double)(calls * tokens_per_call) * ENSEMBLE_COST_PER_TOKEN;
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
   (void)truncated;
   const char *role_hint = mode == ROUNDTABLE_REVIEW ? "review and critique" : "draft and revise";
   const char *mode_task =
       mode == ROUNDTABLE_REVIEW
           ? "Return only JSON: {\"items\":[{\"severity\":\"blocking|suggestion|nit\","
             "\"category\":\"correctness|security|performance|maintainability|style\","
             "\"location\":\"file:line or artifact section\",\"summary\":\"one-sentence issue\","
             "\"recommendation\":\"...\"}],\"overall\":\"...\"}. "
             "Do not invent stable keys; the engine computes identity keys."
           : "Return the next complete draft. Incorporate useful peer input and do not describe "
             "the process.";
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
   const char *frame = mode == ROUNDTABLE_REVIEW
                           ? "You are consolidating peer reviews from an agent roundtable."
                           : "You are consolidating draft revisions from an agent roundtable.";
   int n =
       snprintf(buf + pos, cap - pos,
                "%s\n\nTASK:\n%s\n\nPRIOR SHARED ARTIFACT:\n%s\n\nPARTICIPANT OUTPUTS:\n\n", frame,
                task ? task : "", prior_artifact && prior_artifact[0] ? prior_artifact : "(none)");
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

static int run_quality_scorer(agent_config_t *acfg, const char *task, const char *artifact,
                              double *cost_usd)
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
      if (cost_usd)
         *cost_usd += (score_result.prompt_tokens + score_result.completion_tokens) *
                      ENSEMBLE_COST_PER_TOKEN;
      char *end = NULL;
      long parsed = strtol(score_result.response, &end, 10);
      if (end && end != score_result.response && parsed >= 0 && parsed <= 100)
         score = (int)parsed;
   }
   free(score_result.response);
   free(full);
   return score;
}

static int run_convergence_tiebreak(agent_config_t *acfg, const char *task, const char *prev,
                                    const char *next, double *cost_usd)
{
   char *prompt =
       xasprintf3("Judge whether this roundtable artifact has converged. Reply only as JSON "
                  "{\"completion\":N} where N is 0-100.\n\nTASK:\n",
                  task ? task : "", "\n\nPREVIOUS:\n");
   char *p2 = prompt ? xasprintf3(prompt, prev ? prev : "", "\n\nCURRENT:\n") : NULL;
   free(prompt);
   char *full = p2 ? xasprintf3(p2, next ? next : "", "\n") : NULL;
   free(p2);
   if (!full)
      return 0;
   agent_result_t res;
   memset(&res, 0, sizeof(res));
   int completion = 0;
   if (agent_run_with_tools_write_enforce(acfg, "reason", NULL, full, 512, 0, &res) == 0 &&
       res.response && res.response[0])
   {
      if (cost_usd)
         *cost_usd += (res.prompt_tokens + res.completion_tokens) * ENSEMBLE_COST_PER_TOKEN;
      cJSON *j = cJSON_Parse(res.response);
      cJSON *c = j ? cJSON_GetObjectItemCaseSensitive(j, "completion") : NULL;
      if (cJSON_IsNumber(c))
         completion = (int)c->valuedouble;
      else
         completion = atoi(res.response);
      cJSON_Delete(j);
   }
   free(res.response);
   free(full);
   return completion >= 90;
}

static int summarize_forward(agent_config_t *acfg, const config_t *cfg, const char *task,
                             char **artifact, char **peer_notes, double *cost_usd)
{
   if (!artifact || !*artifact || !peer_notes || !*peer_notes)
      return -1;
   char *prompt = xasprintf3(
       "Summarize this roundtable context forward so the next participants can continue without "
       "losing decisions, blockers, or concrete edits. Preserve the current artifact first, then "
       "compact peer notes.\n\nTASK:\n",
       task ? task : "", "\n\nCURRENT ARTIFACT:\n");
   char *p2 = prompt ? xasprintf3(prompt, *artifact, "\n\nPEER NOTES:\n") : NULL;
   free(prompt);
   char *full = p2 ? xasprintf3(p2, *peer_notes, "\n") : NULL;
   free(p2);
   if (!full)
      return -1;
   agent_result_t res;
   memset(&res, 0, sizeof(res));
   int rc = run_aggregator(acfg, cfg, full, &res);
   free(full);
   if (rc != 0 || !res.response || !res.response[0])
   {
      free(res.response);
      return -1;
   }
   if (cost_usd)
      *cost_usd += (res.prompt_tokens + res.completion_tokens) * ENSEMBLE_COST_PER_TOKEN;
   free(*peer_notes);
   *peer_notes = xstrdup0(res.response);
   if (strlen(*artifact) > 20000)
   {
      free(*artifact);
      *artifact = xstrdup0(res.response);
   }
   free(res.response);
   return (*artifact && *peer_notes) ? 0 : -1;
}

static void normalized_identity_key(const char *category, const char *location, const char *summary,
                                    char *out, size_t out_n)
{
   if (!out || out_n == 0)
      return;
   const char *parts[2] = {(category && category[0]) ? category : "general",
                           (location && location[0]) ? location : (summary ? summary : "")};
   size_t pos = 0;
   for (int part = 0; part < 2; part++)
   {
      const char *s = parts[part];
      while (*s && pos + 1 < out_n)
      {
         if (isalnum((unsigned char)*s))
            out[pos++] = (char)tolower((unsigned char)*s);
         else if (pos > 0 && out[pos - 1] != ':')
            out[pos++] = ':';
         s++;
      }
      if (part == 0 && pos > 0 && pos + 1 < out_n && out[pos - 1] != ':')
         out[pos++] = ':';
   }
   while (pos > 0 && out[pos - 1] == ':')
      pos--;
   out[pos] = '\0';
}

static int parse_review_issue_keys(const char *text, char keys[][128], int *count, int max)
{
   if (!text || !count || max <= 0)
      return -1;
   cJSON *root = cJSON_Parse(text);
   if (!root)
      return -1;
   cJSON *issues = cJSON_GetObjectItemCaseSensitive(root, "issues");
   if (!cJSON_IsArray(issues))
      issues = cJSON_GetObjectItemCaseSensitive(root, "items");
   if (!cJSON_IsArray(issues))
      issues = cJSON_GetObjectItemCaseSensitive(root, "blocking_issues");
   if (!cJSON_IsArray(issues))
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON *it;
   cJSON_ArrayForEach(it, issues)
   {
      cJSON *severity = cJSON_GetObjectItemCaseSensitive(it, "severity");
      cJSON *category = cJSON_GetObjectItemCaseSensitive(it, "category");
      cJSON *location = cJSON_GetObjectItemCaseSensitive(it, "location");
      cJSON *summary = cJSON_GetObjectItemCaseSensitive(it, "summary");
      if (cJSON_IsString(severity) && strcmp(severity->valuestring, "blocking") != 0)
         continue;
      char key[128];
      normalized_identity_key(cJSON_IsString(category) ? category->valuestring : "",
                              cJSON_IsString(location) ? location->valuestring : "",
                              cJSON_IsString(summary) ? summary->valuestring : "", key,
                              sizeof(key));
      if (!key[0] || key_seen128(keys, *count, key))
         continue;
      if (*count < max)
         snprintf(keys[(*count)++], 128, "%s", key);
   }
   cJSON_Delete(root);
   return 0;
}

static int repair_review_json(agent_config_t *acfg, const config_t *cfg, int participant,
                              const char *bad_text, char **fixed, double *cost_usd)
{
   if (!acfg || !cfg || participant < 0 || participant >= cfg->ensemble_reference_count || !fixed)
      return -1;
   char *prompt = xasprintf3(
       "Repair this malformed roundtable review into valid JSON only. Preserve every issue. "
       "Use shape {\"items\":[{\"severity\":\"blocking|suggestion|nit\","
       "\"category\":\"correctness|security|performance|maintainability|style\","
       "\"location\":\"file:line or section\",\"summary\":\"one sentence\"}]}. "
       "Do not add commentary.\n\nMALFORMED REVIEW:\n",
       bad_text ? bad_text : "", "\n");
   if (!prompt)
      return -1;
   agent_result_t res;
   memset(&res, 0, sizeof(res));
   int rc = agent_run_named(acfg, cfg->ensemble_reference_models[participant], "review", NULL,
                            prompt, 512, 0.2, &res);
   free(prompt);
   if (rc != 0 || !res.response || !res.response[0])
   {
      free(res.response);
      return -1;
   }
   if (cost_usd)
      *cost_usd += (res.prompt_tokens + res.completion_tokens) * ENSEMBLE_COST_PER_TOKEN;
   *fixed = res.response;
   return 0;
}

static int review_saturated(char prev[][128], int prev_count, char cur[][128], int cur_count)
{
   if (cur_count <= 0)
      return 1;
   if (prev_count <= 0)
      return 0;
   for (int i = 0; i < cur_count; i++)
      if (!key_seen128(prev, prev_count, cur[i]))
         return 0;
   return 1;
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
      agent_run_named(acfg, cfg->ensemble_reference_models[i],
                      mode == ROUNDTABLE_REVIEW ? "review" : "draft", NULL, prompt, 0,
                      0.3 + (0.05 * i), &results[i]);
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
   char prev_review_keys[64][128];
   int prev_review_key_count = 0;
   if (!artifact || !peer_notes)
      goto fail;

   for (int round = 1; round <= local.max_rounds; round++)
   {
      if (local.cancel_requested && local.cancel_requested(local.cancel_ctx))
      {
         out->cancelled = 1;
         break;
      }
      if (local.deadline_ms > 0 && monotonic_ms() - start_ms >= local.deadline_ms)
      {
         out->deadline_hit = 1;
         break;
      }
      if (cfg->ensemble_max_cost_usd > 0.0)
      {
         double preflight = estimated_stage_cost(ref_count + 2, 768);
         if (out->cost_usd + preflight > cfg->ensemble_max_cost_usd)
            aimee_log(LOG_WARN, "delegate_roundtable",
                      "round cost estimate $%.4f would exceed cap $%.4f; continuing until "
                      "observed cost is available",
                      out->cost_usd + preflight, cfg->ensemble_max_cost_usd);
      }
      if (strlen(artifact) + strlen(peer_notes) + strlen(task) > 22000)
      {
         if (summarize_forward(acfg, cfg, task, &artifact, &peer_notes, &out->cost_usd) == 0)
         {
            out->truncated = 1;
            out->degraded = 1;
         }
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
      if (local.cancel_requested && local.cancel_requested(local.cancel_ctx))
      {
         out->cancelled = 1;
         for (int i = 0; i < ref_count; i++)
            free(results[i].response);
         break;
      }

      char cur_review_keys[64][128];
      int cur_review_key_count = 0;
      if (local.mode == ROUNDTABLE_REVIEW)
      {
         for (int i = 0; i < ref_count; i++)
         {
            if (!results[i].response || !results[i].response[0])
               continue;
            int before = cur_review_key_count;
            if (parse_review_issue_keys(results[i].response, cur_review_keys, &cur_review_key_count,
                                        64) != 0)
            {
               char *fixed = NULL;
               if (local.cancel_requested && local.cancel_requested(local.cancel_ctx))
               {
                  out->cancelled = 1;
                  break;
               }
               if (repair_review_json(acfg, cfg, i, results[i].response, &fixed, &out->cost_usd) ==
                       0 &&
                   parse_review_issue_keys(fixed, cur_review_keys, &cur_review_key_count, 64) == 0)
               {
                  free(results[i].response);
                  results[i].response = fixed;
               }
               else
               {
                  free(fixed);
                  out->degraded = 1;
                  free(results[i].response);
                  results[i].response = NULL;
                  snprintf(results[i].error, sizeof(results[i].error), "%s",
                           "malformed review JSON");
               }
            }
            else if (cur_review_key_count == before)
            {
               /* Valid review JSON with no blocking issues still counts as a
                * successful participant; saturation uses the blocking-key set. */
            }
         }
         if (out->cancelled)
         {
            for (int i = 0; i < ref_count; i++)
               free(results[i].response);
            break;
         }
      }

      double round_cost = estimate_cost(results, ref_count);
      out->cost_usd += round_cost;
      out->rounds_run = round;
      if (cfg->ensemble_max_cost_usd > 0.0 && out->cost_usd > cfg->ensemble_max_cost_usd)
      {
         out->cost_capped = 1;
         int best = best_candidate(results, ref_count);
         if (best_score < 0 && best >= 0 && results[best].response)
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
            if (local.cancel_requested && local.cancel_requested(local.cancel_ctx))
            {
               out->cancelled = 1;
               for (int i = 0; i < ref_count; i++)
                  free(results[i].response);
               break;
            }
            int score = run_quality_scorer(acfg, task, results[best].response, &out->cost_usd);
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
      char *synthesis_prompt = build_round_synthesis_prompt(
          task, artifact, results, order, cfg->ensemble_reference_models, ref_count, local.mode);
      for (int i = 0; i < ref_count; i++)
         free(results[i].response);
      if (!synthesis_prompt)
         goto fail;

      if (local.cancel_requested && local.cancel_requested(local.cancel_ctx))
      {
         out->cancelled = 1;
         free(synthesis_prompt);
         break;
      }
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

      char *prev_artifact_for_judge = xstrdup0(artifact);
      int ratio = change_ratio_0_100(artifact, agg_result.response);
      free(artifact);
      artifact = xstrdup0(agg_result.response);
      if (!artifact)
      {
         free(agg_result.response);
         free(prev_artifact_for_judge);
         goto fail;
      }
      free(peer_notes);
      peer_notes = xstrdup0(agg_result.response);
      if (!peer_notes)
      {
         free(agg_result.response);
         free(prev_artifact_for_judge);
         goto fail;
      }

      if (local.cancel_requested && local.cancel_requested(local.cancel_ctx))
      {
         out->cancelled = 1;
         free(agg_result.response);
         free(prev_artifact_for_judge);
         break;
      }
      int score = run_quality_scorer(acfg, task, agg_result.response, &out->cost_usd);
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
         free(prev_artifact_for_judge);
         break;
      }
      if (local.mode == ROUNDTABLE_REVIEW &&
          review_saturated(prev_review_keys, prev_review_key_count, cur_review_keys,
                           cur_review_key_count))
      {
         out->converged = 1;
         free(prev_artifact_for_judge);
         break;
      }
      if (local.mode == ROUNDTABLE_REVIEW)
      {
         prev_review_key_count = cur_review_key_count;
         for (int i = 0; i < cur_review_key_count; i++)
            snprintf(prev_review_keys[i], sizeof(prev_review_keys[i]), "%s", cur_review_keys[i]);
      }
      if (local.mode == ROUNDTABLE_DRAFT && round > 1 && ratio <= local.converge_threshold)
      {
         out->converged = 1;
         free(prev_artifact_for_judge);
         break;
      }
      if (local.mode == ROUNDTABLE_DRAFT && round > 1 &&
          abs(ratio - local.converge_threshold) <= 5 &&
          !(local.cancel_requested && local.cancel_requested(local.cancel_ctx)) &&
          run_convergence_tiebreak(acfg, task, prev_artifact_for_judge, artifact, &out->cost_usd))
      {
         free(prev_artifact_for_judge);
         out->converged = 1;
         break;
      }
      if (local.cancel_requested && local.cancel_requested(local.cancel_ctx))
      {
         free(prev_artifact_for_judge);
         out->cancelled = 1;
         break;
      }
      free(prev_artifact_for_judge);
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
