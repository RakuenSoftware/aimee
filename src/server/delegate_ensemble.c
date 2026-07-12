/* delegate_ensemble.c: Mixture-of-Agents ensemble implementation.
 *
 * Fans the same prompt to N diverse reference models in parallel, then runs
 * a synthesis pass that produces one reconciled answer. Different from
 * agent_vote() (same model N times, majority) -- MoA uses diverse models
 * and synthesizes via an aggregator.
 */
#include "delegate_ensemble_internal.h"
#include "aimee.h"
#include "delegate_ensemble.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "config.h"
#include "delegate_credentials.h"
#include "cost_fold.h"
#include "log.h"
#include "persona.h"
#include "dstr.h"
#include "roundtable_seat_resolve.h"
#include "roundtable_verify.h"
#include "token_tracker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <time.h>
#include <ctype.h>

/* The config arrays the engine fans out over must be sized to ENSEMBLE_MAX_REFS.
 * config.h keeps the dim as a literal (it can't include this header), so catch
 * any future drift at compile time rather than overrunning the stack arrays. */
_Static_assert(sizeof(((config_t *)0)->ensemble_reference_models) /
                       sizeof(((config_t *)0)->ensemble_reference_models[0]) ==
                   ENSEMBLE_MAX_REFS,
               "config_t.ensemble_reference_models first dim must equal ENSEMBLE_MAX_REFS");
_Static_assert(sizeof(((config_t *)0)->ensemble_reference_personas) /
                       sizeof(((config_t *)0)->ensemble_reference_personas[0]) ==
                   ENSEMBLE_MAX_REFS,
               "config_t.ensemble_reference_personas first dim must equal ENSEMBLE_MAX_REFS");

/* Fallback when an ad-hoc model is absent from the capability registry. */
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

/* Balance-match a JSON object starting at text[start]=='{'. Scans forward
 * tracking brace depth, correctly skipping braces that appear INSIDE JSON string
 * literals (respecting \-escapes), and returns the index just past the matching
 * '}', or -1 if unbalanced. This is what lets us extract the real object out of a
 * prose review that also contains `{...}` code snippets. */
static long json_object_extent(const char *text, long start)
{
   int depth = 0;
   int in_str = 0;
   for (long i = start; text[i]; i++)
   {
      char c = text[i];
      if (in_str)
      {
         if (c == '\\' && text[i + 1])
            i++; /* skip the escaped char */
         else if (c == '"')
            in_str = 0;
         continue;
      }
      if (c == '"')
         in_str = 1;
      else if (c == '{')
         depth++;
      else if (c == '}')
      {
         depth--;
         if (depth == 0)
            return i + 1;
      }
   }
   return -1;
}

/* Parse JSON from a model response, tolerating a markdown code fence
 * (```json ... ```) or surrounding prose. Panelists given a persona system prompt
 * — especially on a large task, where the "raw JSON only" instruction is far up
 * the context — frequently return their `{"items":[...]}` wrapped in prose that
 * ALSO contains `{...}` code snippets. A naive first-'{'-to-last-'}' slice then
 * spans unrelated braces and fails to parse, collapsing the review to zero items.
 * Strategy: strict parse first (bare JSON); else scan every '{', balance-match a
 * candidate object (string-aware), parse it, and prefer the first that carries an
 * "items" array (the review contract), falling back to the first that parses at
 * all. Returns NULL if nothing parses; caller owns the result. */
cJSON *parse_model_json_lenient(const char *text)
{
   if (!text || !text[0])
      return NULL;
   cJSON *root = cJSON_Parse(text);
   if (root)
      return root;

   cJSON *first_valid = NULL;
   for (long i = 0; text[i]; i++)
   {
      if (text[i] != '{')
         continue;
      long end = json_object_extent(text, i);
      if (end < 0)
         break; /* no balanced object from here on */
      size_t len = (size_t)(end - i);
      char *buf = (char *)malloc(len + 1);
      if (!buf)
         break;
      memcpy(buf, text + i, len);
      buf[len] = '\0';
      cJSON *cand = cJSON_Parse(buf);
      free(buf);
      if (cand)
      {
         if (cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(cand, "items")))
         {
            cJSON_Delete(first_valid);
            return cand; /* the review contract object — best match */
         }
         if (!first_valid)
            first_valid = cand; /* remember, keep scanning for an items object */
         else
            cJSON_Delete(cand);
         /* skip past this object to avoid rescanning its inner braces */
         i = end - 1;
      }
   }
   return first_valid;
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

static const agent_t *find_agent_cost_model(const agent_config_t *acfg, const char *name)
{
   if (!acfg || !name || !name[0])
      return NULL;
   for (int i = 0; i < acfg->agent_count; i++)
      if (strcmp(acfg->agents[i].name, name) == 0)
         return &acfg->agents[i];
   return NULL;
}

static double fallback_token_cost(int prompt_tokens, int completion_tokens)
{
   return (double)(prompt_tokens + completion_tokens) * ENSEMBLE_COST_PER_TOKEN;
}

double delegate_cost_estimate_usd(const char *provider, const char *model, int prompt_tokens,
                                  int completion_tokens)
{
   (void)provider; /* token_estimate_cost infers the provider from the model id */
   if (model && model[0])
   {
      /* token_estimate_cost is the single cost authority (static cache-aware
       * table + model-registry fallback), the same one agent_log_call bills
       * against, so the ensemble path keeps no divergent price source. */
      token_usage_t usage = {
          .input_tokens = prompt_tokens,
          .output_tokens = completion_tokens,
      };
      int priced = 0;
      double cost = token_estimate_cost_ex(model, &usage, &priced);
      /* Return the real price when the model is KNOWN — including a genuine $0
       * for a free model — so a free model is not mistaken for unknown and
       * flat-rated. Only fall back when no source prices the model. */
      if (priced)
         return cost;
   }
   /* Last resort: a coarse flat rate so ensemble economics stay non-zero for
    * models neither the table nor the registry prices. */
   return fallback_token_cost(prompt_tokens, completion_tokens);
}

static double agent_token_cost(const agent_config_t *acfg, const char *agent_name,
                               int prompt_tokens, int completion_tokens)
{
   const agent_t *ag = find_agent_cost_model(acfg, agent_name);
   if (ag)
      return delegate_cost_estimate_usd(ag->provider, ag->model, prompt_tokens, completion_tokens);
   return fallback_token_cost(prompt_tokens, completion_tokens);
}

static double result_token_cost(const agent_config_t *acfg, const agent_result_t *result,
                                const char *fallback_agent)
{
   if (!result)
      return 0.0;
   /* Prefer the model actually served (recorded on the result, and updated
    * across fallback-model swaps) over re-deriving it from the configured agent
    * by name — so observed cost reflects what really ran. */
   if (result->model[0])
      return delegate_cost_estimate_usd(NULL, result->model, result->prompt_tokens,
                                        result->completion_tokens);
   const char *agent = result->agent_name[0] ? result->agent_name : fallback_agent;
   return agent_token_cost(acfg, agent, result->prompt_tokens, result->completion_tokens);
}

static double estimate_cost(const agent_config_t *acfg, const agent_result_t *results,
                            const char (*fallback_agents)[128], int count)
{
   double total = 0.0;
   for (int i = 0; i < count; i++)
      total += result_token_cost(acfg, &results[i], fallback_agents ? fallback_agents[i] : NULL);
   return total;
}

/* Originating session of the in-flight roundtable, set at delegate_roundtable_run
 * entry. Read by the delegate-run core + panel cost-fold so each model call is
 * accounted onto the caller's session like a delegate. */
static _Thread_local const char *g_rt_parent_sid = NULL;

/* Fold a completed panel/aggregator run's cost onto the originating session, the
 * same accounting a real delegate does. No-op without a parent session or a
 * billable result. */
static void ensemble_fold_cost(const agent_config_t *acfg, const agent_result_t *r,
                               const char *fallback_agent)
{
   if (!g_rt_parent_sid || !g_rt_parent_sid[0] || !r || !r->success)
      return;
   double cost = result_token_cost(acfg, r, fallback_agent);
   if (cost > 0.0)
      (void)db1_cost_fold_record(g_rt_parent_sid, "ensemble", cost, "ensemble");
}

/* Synchronous delegate run: the delegate economics core — a credential lease for
 * agents with a server-side pool, plus cost-fold accounting — wrapped around a
 * no-tools agent run. The ensemble dispatches its aggregator (and any serial
 * participant) through this so the work runs as a delegate rather than a raw
 * in-process agent call. delegate_worker stays the async-job path; this is its
 * synchronous sibling. A non-empty name routes by name; NULL routes by role. */
static int delegate_run_inline(agent_config_t *acfg, const char *agent_name, const char *role,
                               const char *system_prompt, const char *user_prompt, int max_tokens,
                               double temperature, agent_result_t *out)
{
   agent_t *ag = (agent_name && agent_name[0]) ? agent_find(acfg, agent_name) : NULL;
   char leased_cred[MAX_CRED_NAME_LEN] = "";
   int leased = 0;
   /* Client-held-credential agents (the usual panel) have no pool, so this is
    * skipped and keys resolve from the session keyring as before; a pooled agent
    * leases one credential for the call, exactly like delegate_worker. */
   if (ag && ag->credential_count > 0)
   {
      char leased_env[MAX_CRED_ENV_VAR_LEN] = "";
      char state_path[MAX_PATH_LEN];
      const char *dir = config_output_dir();
      snprintf(state_path, sizeof(state_path), "%s/delegate-credential-state.tsv",
               dir ? dir : "/tmp");
      (void)delegate_credentials_load_file(state_path, time(NULL));
      if (delegate_credentials_acquire(NULL, ag->name, ag->credentials, ag->credential_count,
                                       leased_cred, sizeof(leased_cred), leased_env,
                                       sizeof(leased_env)) == 0)
      {
         const char *v = getenv(leased_env);
         if (v && v[0])
            snprintf(ag->api_key, MAX_API_KEY_LEN, "%s", v);
         leased = 1;
      }
   }
   int rc =
       (agent_name && agent_name[0])
           ? agent_run_named(acfg, agent_name, role, system_prompt, user_prompt, max_tokens,
                             temperature, out)
           : agent_run_ex(acfg, role, system_prompt, user_prompt, max_tokens, temperature, out);
   ensemble_fold_cost(acfg, out, agent_name);
   if (leased && ag)
      delegate_credentials_release(NULL, ag->name, leased_cred);
   return rc;
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

int key_seen128(char keys[][128], int count, const char *key)
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

static int positional_change_0_100(const char *prev, const char *next)
{
   const char *ps = prev ? prev : "";
   const char *ns = next ? next : "";
   size_t a = strlen(ps);
   size_t b = strlen(ns);
   size_t denom = a > b ? a : b;
   if (denom == 0)
      return 0;
   size_t common = a < b ? a : b;
   size_t diff = a > b ? a - b : b - a;
   for (size_t i = 0; i < common; i++)
      if (ps[i] != ns[i])
         diff++;
   return (int)((diff * 100) / denom);
}

static int token_jaccard_change_0_100(const char *prev, const char *next)
{
   char(*a)[64] = calloc(512, sizeof(*a));
   char(*b)[64] = calloc(512, sizeof(*b));
   if (!a || !b)
   {
      free(a);
      free(b);
      return positional_change_0_100(prev, next);
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
      return positional_change_0_100(prev, next);
   int *prev_row = calloc(b + 1, sizeof(int));
   int *cur_row = calloc(b + 1, sizeof(int));
   if (!prev_row || !cur_row)
   {
      free(prev_row);
      free(cur_row);
      return positional_change_0_100(prev, next);
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

static double estimated_agent_call_cost(const agent_config_t *acfg, const char *agent_name,
                                        int tokens_per_call)
{
   int prompt_tokens = tokens_per_call / 2;
   int completion_tokens = tokens_per_call - prompt_tokens;
   return agent_token_cost(acfg, agent_name, prompt_tokens, completion_tokens);
}

static double estimated_round_cost(const agent_config_t *acfg, const config_t *cfg, int ref_count,
                                   int tokens_per_call)
{
   double total = 0.0;
   for (int i = 0; i < ref_count; i++)
      total += estimated_agent_call_cost(acfg, cfg->ensemble_reference_models[i], tokens_per_call);
   total += estimated_agent_call_cost(acfg, cfg->ensemble_aggregator, tokens_per_call);
   total += estimated_agent_call_cost(acfg, "reason", tokens_per_call);
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
                          long deadline_abs_ms, agent_result_t *out)
{
   memset(out, 0, sizeof(*out));

   /* deadline_abs_ms (absolute CLOCK_MONOTONIC ms; 0 = UNBOUNDED) caps the FALLBACK
    * FAN-OUT below. Without it a flaky/empty primary aggregator followed by N
    * panelist fallbacks — each up to the provider HTTP timeout x retries (~9 min) —
    * can run ~an hour and wedge the roundtable past its deadline (the panel is
    * deadline-bounded; this path was not).
    *
    * This is a FAN-OUT limit, not a hard per-call bound: a call already in flight
    * can still overrun by up to one provider timeout (delegate_run_inline exposes
    * no per-call timeout hook). Only the roundtable round-loop passes a deadline;
    * other callers pass 0 and keep their original unbounded behavior, so this never
    * silently couples non-roundtable synthesis to the roundtable config. */

   agent_config_t agg_cfg = *acfg;

   /* Synthesis is pure text/JSON merging of the panel's responses — it needs no
    * tools. Running it through the tool-use loop breaks aggregators whose model
    * has no tool-calling support (e.g. MiniMax), which then return empty and
    * collapse the whole round to a degraded, artifact-less result. So all paths
    * below use a no-tools run.
    *
    * The aggregator may be configured as a bare agent NAME (what the default
    * panel picks — its first panellist) or as "<...>@<role>". A bare name must
    * be dispatched by NAME (agent_run_named): agent_run_ex selects by role and
    * an agent's name is not one of its roles, so a name routed as a role finds
    * nothing and returns empty. A role form (or no aggregator) routes by role. */
   /* Primary: the configured (or default) aggregator. max_tokens 0 = derive from
    * the aggregator model's own output ceiling, so a reasoning aggregator isn't
    * truncated mid-synthesis (was a hard 4096). */
   const char *primary_name = NULL; /* bare-name aggregator we tried, to not retry it */
   if (cfg->ensemble_aggregator[0])
   {
      const char *agg = cfg->ensemble_aggregator;
      const char *at = strchr(agg, '@');
      if (!at)
      {
         primary_name = agg;
         if (delegate_run_inline(&agg_cfg, agg, "review", NULL, synthesis_prompt, 0, 0.3, out) ==
                 0 &&
             out->response && out->response[0])
            return 0;
      }
      else
      {
         const char *role = (at[1]) ? at + 1 : "review";
         if (delegate_run_inline(&agg_cfg, NULL, role, NULL, synthesis_prompt, 0, 0.3, out) == 0 &&
             out->response && out->response[0])
            return 0;
      }
   }
   else if (delegate_run_inline(&agg_cfg, NULL, "review", NULL, synthesis_prompt, 0, 0.3, out) ==
                0 &&
            out->response && out->response[0])
      return 0;

   /* Fallback: the configured aggregator failed or returned empty. Try each
    * panelist by name (no-tools) until one synthesizes — a single flaky
    * aggregator model must not collapse the whole round to an artifact-less
    * degrade when other capable panelists are available. */
   for (int i = 0; i < cfg->ensemble_reference_count; i++)
   {
      if (deadline_abs_ms > 0 && monotonic_ms() >= deadline_abs_ms)
      {
         aimee_log(LOG_WARN, "delegate_roundtable",
                   "aggregator: deadline reached at fallback candidate %d/%d; abandoning the "
                   "remaining %d (a flaky synthesis model must not wedge the roundtable)",
                   i, cfg->ensemble_reference_count, cfg->ensemble_reference_count - i);
         break;
      }
      const char *cand = cfg->ensemble_reference_models[i];
      if (!cand[0] || (primary_name && strcmp(cand, primary_name) == 0))
         continue;
      free(out->response);
      memset(out, 0, sizeof(*out));
      if (delegate_run_inline(&agg_cfg, cand, "review", NULL, synthesis_prompt, 0, 0.3, out) == 0 &&
          out->response && out->response[0])
         return 0;
   }
   return -1;
}

static char *build_round_prompt(const char *task, const char *artifact, const char *peer_notes,
                                roundtable_mode_t mode, int round, const char *brief,
                                const char *context)
{
   const char *role_hint = mode == ROUNDTABLE_REVIEW ? "review and critique" : "draft and revise";
   const char *mode_task =
       mode == ROUNDTABLE_REVIEW
           ? "You have NO filesystem, git, shell, or tool access in this review: the SHARED "
             "ARTIFACT above is the complete and authoritative material under review. Judge it on "
             "its own contents only. Never claim to have read files, run git, or grepped — and "
             "never report a finding like \"the change is not applied / not in the tree\" (you "
             "cannot observe the tree; the artifact is the change). "
             "Return only JSON: {\"items\":[{\"severity\":\"blocking|suggestion|nit\","
             "\"category\":\"correctness|security|performance|maintainability|style\","
             "\"location\":\"file:line or artifact section\",\"summary\":\"one-sentence issue\","
             "\"recommendation\":\"...\","
             "\"evidence\":{\"kind\":\"refs|symbol|search|none\",\"target\":\"symbol or search "
             "term\",\"project\":\"\",\"count\":0,\"factual\":true}}],\"overall\":\"...\"}. "
             "evidence lets the engine VERIFY a factual structural claim against the code index "
             "(you do NOT run it — just name it): kind=refs target=<fn> count=<claimed call "
             "sites>; kind=symbol target=<symbol> (does it exist); kind=search target=<term>. Set "
             "factual=true ONLY for a checkable code fact; for a judgment/opinion use kind=none "
             "and factual=false (it will be capped below blocking). A blocking severity REQUIRES "
             "reproducible factual evidence. "
             "Output raw JSON only — no markdown, no ``` code fences, no prose. "
             "Do not invent stable keys; the engine computes identity keys."
           : "Return the next complete draft. Incorporate useful peer input and do not describe "
             "the process.";
   if (!artifact)
      artifact = "";
   if (!peer_notes)
      peer_notes = "";
   /* A one-line format reminder at the TOP as well as the bottom. On a large task
    * the bottom "return only JSON" instruction is tens of KB down the context and
    * long-context models drift to prose — which then parses to zero items. Leading
    * with the contract keeps it salient. */
   const char *head_note =
       mode == ROUNDTABLE_REVIEW
           ? "OUTPUT CONTRACT: reply with ONE raw JSON object "
             "{\"items\":[...],\"overall\":\"...\"} "
             "and nothing else — no prose, no markdown fences. Full schema is in the ROUND "
             "INSTRUCTION below.\n\n"
           : "";
   const char *brief_block = "";
   char *owned_brief_block = NULL;
   if (mode == ROUNDTABLE_REVIEW && brief && brief[0])
   {
      owned_brief_block = xasprintf3(
          "CALLER BRIEF:\n", brief,
          "\nPrioritize this brief and answer its questions, but report any blocking issue you "
          "find even if it is outside the brief.\n\n");
      if (!owned_brief_block)
         return NULL;
      brief_block = owned_brief_block;
   }

   /* Read-only aimee context (memory recall + code-graph snippets), assembled by
    * the server-side caller. Panelists have no tools, so this block is the only
    * way they see aimee memory and the code graph; it is authoritative reference,
    * not something they can re-query. */
   const char *context_block = "";
   char *owned_context_block = NULL;
   if (context && context[0])
   {
      owned_context_block =
          xasprintf3("AIMEE CONTEXT (read-only reference — aimee memory + code graph):\n", context,
                     "\nUse this context as grounding; it is authoritative reference you cannot "
                     "re-query.\n\n");
      if (!owned_context_block)
      {
         free(owned_brief_block);
         return NULL;
      }
      context_block = owned_context_block;
   }

   size_t needed = strlen(task ? task : "") + strlen(artifact) + strlen(peer_notes) +
                   strlen(brief_block) + strlen(context_block) + strlen(mode_task) +
                   strlen(role_hint) + strlen(head_note) + 512;
   char *prompt = malloc(needed);
   if (!prompt)
   {
      free(owned_brief_block);
      free(owned_context_block);
      return NULL;
   }
   snprintf(
       prompt, needed,
       "You are one participant in an agent roundtable. Your role is to %s.\n\n%s"
       "TASK:\n%s\n\n%sCURRENT SHARED ARTIFACT:\n%s\n\nPEER NOTES FROM PREVIOUS TURN:\n%s\n\n%s"
       "ROUND %d INSTRUCTION:\n%s\n",
       role_hint, head_note, task ? task : "", context_block, artifact[0] ? artifact : "(none yet)",
       peer_notes[0] ? peer_notes : "(none yet)", brief_block, round, mode_task);

   free(owned_brief_block);
   free(owned_context_block);
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
   if (mode == ROUNDTABLE_REVIEW)
      n = snprintf(buf + pos, cap - pos,
                   "TASK: Produce the consolidated review as a SINGLE JSON object "
                   "{\"items\":[{\"severity\":...,\"category\":...,\"location\":...,"
                   "\"summary\":...,\"recommendation\":...}],\"overall\":...}. "
                   "Output ONLY that JSON object — no analysis, no reasoning, no step-by-step "
                   "commentary, no markdown fences, no text before or after. Emitting the final "
                   "JSON directly (not your deliberation) is required so the verdict is never "
                   "truncated. Do not mention the roundtable process.\n");
   else
      n = snprintf(buf + pos, cap - pos,
                   "TASK: Produce the next shared artifact draft. Do not mention the roundtable "
                   "process.\n");
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
         *cost_usd += result_token_cost(acfg, &score_result, "reason");
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
         *cost_usd += result_token_cost(acfg, &res, "reason");
      cJSON *j = parse_model_json_lenient(res.response);
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
   /* Nothing to compress if there is no carryover. The `!*x` checks reject a NULL
    * string pointer; the `!(*x)[0]` checks also reject an allocated-but-EMPTY
    * string (round 1, before any artifact/peer notes exist) so a large task can
    * never provoke a pointless summarize call. */
   if (!artifact || !*artifact || !(*artifact)[0])
      return -1;
   if (!peer_notes || !*peer_notes || !(*peer_notes)[0])
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
   int rc = run_aggregator(acfg, cfg, full, 0, &res);
   free(full);
   if (rc != 0 || !res.response || !res.response[0])
   {
      free(res.response);
      return -1;
   }
   if (cost_usd)
      *cost_usd += result_token_cost(acfg, &res, cfg->ensemble_aggregator);
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

static void mark_question_gaps(const roundtable_opts_t *opts, roundtable_result_t *out)
{
   if (!opts || !out)
      return;
   int n = opts->question_count;
   if (n > ROUNDTABLE_MAX_QUESTIONS)
      n = ROUNDTABLE_MAX_QUESTIONS;
   for (int i = 0; i < n; i++)
   {
      const char *q = opts->questions && opts->questions[i] ? opts->questions[i] : "";
      snprintf(out->answered_questions[i].question, sizeof(out->answered_questions[i].question),
               "%s", q);
      snprintf(out->answered_questions[i].answer, sizeof(out->answered_questions[i].answer), "%s",
               "");
      snprintf(out->answered_questions[i].evidence, sizeof(out->answered_questions[i].evidence),
               "%s", "");
      out->answered_questions[i].answered = 0;
      snprintf(out->coverage_gaps[i], sizeof(out->coverage_gaps[i]), "%s", q);
   }
   out->answered_question_count = n;
   out->coverage_gap_count = n;
}

static void parse_question_answers(const char *text, const roundtable_opts_t *opts,
                                   roundtable_result_t *out)
{
   /* Seed one entry per asked question (answered=0), then fill answers in by
    * matching the model's entries back to the asked questions by text. This
    * guarantees exactly one result entry per asked question regardless of how
    * many answers the model returns or in what order, so a partial or reordered
    * answer set never silently drops a question (§5). */
   mark_question_gaps(opts, out);
   int n = out->answered_question_count;
   cJSON *root = text ? parse_model_json_lenient(text) : NULL;
   if (!root)
      return; /* keep the gap-seeded result */
   cJSON *answers = cJSON_GetObjectItemCaseSensitive(root, "answered_questions");
   if (!cJSON_IsArray(answers))
      answers = cJSON_GetObjectItemCaseSensitive(root, "answeredQuestions");
   cJSON *it;
   cJSON_ArrayForEach(it, answers)
   {
      cJSON *q = cJSON_GetObjectItemCaseSensitive(it, "question");
      cJSON *a = cJSON_GetObjectItemCaseSensitive(it, "answer");
      cJSON *e = cJSON_GetObjectItemCaseSensitive(it, "evidence");
      cJSON *answered = cJSON_GetObjectItemCaseSensitive(it, "answered");
      const char *qtext = cJSON_IsString(q) ? q->valuestring : "";
      int idx = -1;
      for (int i = 0; i < n; i++)
         if (strcmp(out->answered_questions[i].question, qtext) == 0)
         {
            idx = i;
            break;
         }
      if (idx < 0)
         continue; /* model answered something that was not asked; ignore it */
      snprintf(out->answered_questions[idx].answer, sizeof(out->answered_questions[idx].answer),
               "%s", cJSON_IsString(a) ? a->valuestring : "");
      snprintf(out->answered_questions[idx].evidence, sizeof(out->answered_questions[idx].evidence),
               "%s", cJSON_IsString(e) ? e->valuestring : "");
      out->answered_questions[idx].answered = cJSON_IsBool(answered)
                                                  ? cJSON_IsTrue(answered)
                                                  : (cJSON_IsString(a) && a->valuestring[0]);
   }
   cJSON_Delete(root);
   /* Coverage gaps are exactly the asked questions still unanswered. */
   int gi = 0;
   for (int i = 0; i < n; i++)
      if (!out->answered_questions[i].answered)
         snprintf(out->coverage_gaps[gi++], sizeof(out->coverage_gaps[0]), "%s",
                  out->answered_questions[i].question);
   out->coverage_gap_count = gi;
}

static void answer_roundtable_questions(agent_config_t *acfg, const char *task,
                                        const roundtable_opts_t *opts, roundtable_result_t *out)
{
   if (!acfg || !opts || !out || !out->artifact || opts->question_count <= 0)
      return;
   int qn = opts->question_count;
   if (qn > ROUNDTABLE_MAX_QUESTIONS)
      qn = ROUNDTABLE_MAX_QUESTIONS;
   cJSON *qs = cJSON_CreateArray();
   if (!qs)
   {
      mark_question_gaps(opts, out);
      return;
   }
   for (int i = 0; i < qn; i++)
      cJSON_AddItemToArray(qs, cJSON_CreateString(opts->questions[i] ? opts->questions[i] : ""));
   char *qjson = cJSON_PrintUnformatted(qs);
   cJSON_Delete(qs);
   if (!qjson)
   {
      mark_question_gaps(opts, out);
      return;
   }
   const char *prefix =
       "Answer the caller's roundtable review questions from the consolidated review. Return only "
       "JSON shaped {\"answered_questions\":[{\"question\":\"...\",\"answer\":\"...\","
       "\"evidence\":\"...\",\"answered\":true}],\"coverage_gaps\":[\"...\"]}.\n\nTASK:\n";
   size_t n =
       strlen(prefix) + strlen(task ? task : "") + strlen(out->artifact) + strlen(qjson) + 80;
   char *prompt = (char *)malloc(n);
   if (!prompt)
   {
      free(qjson);
      mark_question_gaps(opts, out);
      return;
   }
   snprintf(prompt, n, "%s%s\n\nQUESTIONS:\n%s\n\nCONSOLIDATED REVIEW:\n%s\n", prefix,
            task ? task : "", qjson, out->artifact);
   free(qjson);
   agent_result_t res;
   memset(&res, 0, sizeof(res));
   if (agent_run_with_tools_write_enforce(acfg, "reason", NULL, prompt, 1024, 0, &res) == 0 &&
       res.response && res.response[0])
   {
      parse_question_answers(res.response, opts, out);
      out->cost_usd += result_token_cost(acfg, &res, "reason");
   }
   else
      mark_question_gaps(opts, out);
   free(res.response);
   free(prompt);
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
      *cost_usd += result_token_cost(acfg, &res, cfg->ensemble_reference_models[participant]);
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

/* Diverse default review lineup, round-robined across the panel by the
 * participant's stable position in reference_models (not the shuffled slot), so
 * persona<->model pairing is reproducible run to run. Pairs the contrarian
 * `reviewer` with the constructive `reviewer-constructive`: one tries to break
 * the change, one assesses it as written. */
static const char *const PANEL_DEFAULT_PERSONAS[] = {"security", "architect", "qa", "reviewer",
                                                     "reviewer-constructive"};
#define PANEL_DEFAULT_PERSONA_COUNT                                                                \
   ((int)(sizeof(PANEL_DEFAULT_PERSONAS) / sizeof(PANEL_DEFAULT_PERSONAS[0])))

/* Persona name for panelist `model_index`. An explicit per-slot override
 * (ensemble.reference_personas[model_index]) binds first in ANY mode. Otherwise
 * the default depends on mode: DRAFT authors every panelist as the configured
 * default persona (config.default_persona, itself defaulting to `engineer`),
 * while REVIEW round-robins the diverse critique lineup keyed on the stable model
 * index. The returned pointer is a borrowed string literal or config field; do
 * not free it. */
const char *panel_persona_name(const config_t *cfg, roundtable_mode_t mode, int model_index)
{
   if (!cfg || model_index < 0)
      return NULL;
   if (model_index < cfg->ensemble_reference_persona_count &&
       cfg->ensemble_reference_personas[model_index][0])
      return cfg->ensemble_reference_personas[model_index];
   if (mode != ROUNDTABLE_REVIEW)
      return cfg->default_persona[0] ? cfg->default_persona : "engineer";
   return PANEL_DEFAULT_PERSONAS[model_index % PANEL_DEFAULT_PERSONA_COUNT];
}

/* Compose the system prompt for panelist `model_index` (draft: engineer; review:
 * the diverse lineup — see panel_persona_name). Returns a heap string the caller
 * must free, or NULL (no persona name, or compose failed — both fall back to a
 * NULL-system-prompt run, never dropping a panelist). persona_compose_delegate_prompt
 * resolves project->user->built-in and itself falls back to a usable persona, so
 * an unknown custom name is non-fatal. */
static char *panel_persona_prompt(const config_t *cfg, roundtable_mode_t mode, int model_index)
{
   const char *name = panel_persona_name(cfg, mode, model_index);
   if (!name)
      return NULL;
   char *sys = persona_compose_delegate_prompt(name, NULL, NULL);
   if (!sys)
      aimee_log(LOG_INFO, "roundtable", "panelist %d: persona '%s' compose failed; no persona",
                model_index, name);
   return sys;
}

static int run_round_parallel(agent_config_t *acfg, const config_t *cfg, const char *task,
                              const char *artifact, const char *peer_notes, roundtable_mode_t mode,
                              int round, const char *brief, const char *context,
                              agent_result_t *results, int deadline_ms)
{
   int ref_count = cfg->ensemble_reference_count;
   agent_task_t tasks[ENSEMBLE_MAX_REFS];
   char *prompts[ENSEMBLE_MAX_REFS];
   char *personas[ENSEMBLE_MAX_REFS];
   memset(tasks, 0, sizeof(tasks));
   memset(prompts, 0, sizeof(prompts));
   memset(personas, 0, sizeof(personas));
   for (int i = 0; i < ref_count; i++)
   {
      prompts[i] = build_round_prompt(task, artifact, peer_notes, mode, round, brief, context);
      if (!prompts[i])
         goto fail;
      /* Per-participant persona is the system prompt (draft: engineer; review:
       * the diverse lineup); the round prompt still drives the output shape. */
      personas[i] = panel_persona_prompt(cfg, mode, i);
      tasks[i].role = mode == ROUNDTABLE_REVIEW ? "review" : "draft";
      tasks[i].agent = cfg->ensemble_reference_models[i];
      tasks[i].system_prompt = personas[i];
      tasks[i].user_prompt = prompts[i];
      tasks[i].temperature = 0.3 + (0.05 * i);
      tasks[i].max_tokens = 0;
   }
   /* deadline_ms bounds the panel barrier so one hung panelist can't wedge the
    * whole round past the roundtable deadline. */
   agent_run_parallel(acfg, tasks, ref_count, results, deadline_ms);
   /* Account each participant's run onto the originating session, like a delegate. */
   for (int i = 0; i < ref_count; i++)
      ensemble_fold_cost(acfg, &results[i], cfg->ensemble_reference_models[i]);
   for (int i = 0; i < ref_count; i++)
   {
      free(prompts[i]);
      free(personas[i]);
   }
   return 0;
fail:
   for (int i = 0; i < ref_count; i++)
   {
      free(prompts[i]);
      free(personas[i]);
   }
   return -1;
}

static int run_round_sequential(agent_config_t *acfg, const config_t *cfg, const char *task,
                                const char *artifact, char **peer_notes, roundtable_mode_t mode,
                                int round, const char *brief, const char *context,
                                agent_result_t *results)
{
   int ref_count = cfg->ensemble_reference_count;
   int order[ENSEMBLE_MAX_REFS];
   for (int i = 0; i < ref_count; i++)
      order[i] = i;
   shuffle_indices(order, ref_count);

   for (int oi = 0; oi < ref_count; oi++)
   {
      int i = order[oi];
      char *prompt = build_round_prompt(task, artifact, *peer_notes, mode, round, brief, context);
      if (!prompt)
         return -1;
      /* Persona binds to the stable model index `i`, not the shuffled slot. */
      char *persona = panel_persona_prompt(cfg, mode, i);
      memset(&results[i], 0, sizeof(results[i]));
      agent_run_named(acfg, cfg->ensemble_reference_models[i],
                      mode == ROUNDTABLE_REVIEW ? "review" : "draft", persona, prompt, 0,
                      0.3 + (0.05 * i), &results[i]);
      free(persona);
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

/* Is `ag` allowed to sit on a roundtable/ensemble panel? Enabled + named, and —
 * for claude-CLI — AUTHORIZED as a delegate (claude_cli_delegate_enabled, the
 * explicit, ToS-sensitive operator opt-in) AND able to run server-side
 * (is_server_hosted, via `aimee agent add claude-oauth`). A client-only claude
 * has no server endpoint and would just burn a slot on a "failed to build request
 * URL" participant; server-hosting is capability, NOT authorization, so both are
 * required. So an unauthorized or disabled claude is never seated, even after a
 * server-side OAuth setup. Other enabled agents are eligible.
 *
 * The PRIMARY agent is also never seated: a roundtable exists for an INDEPENDENT
 * second opinion, so the model the operator runs as primary (config.provider)
 * must not sit on — or aggregate — its own review panel and quietly grade its own
 * work. Any agent that resolves to the primary provider (by agent name — the
 * primary passthrough is named after the provider — or by its provider tag) is
 * excluded, structurally, regardless of what ensemble.reference_models lists. */
int ensemble_panelist_eligible(const config_t *cfg, const agent_t *ag)
{
   if (!ag || !ag->enabled || !ag->name[0])
      return 0;
   if (cfg->provider[0] && (strcasecmp(ag->name, cfg->provider) == 0 ||
                            (ag->provider[0] && strcasecmp(ag->provider, cfg->provider) == 0)))
      return 0; /* the primary must never sit on its own panel */
   if (agent_is_claude_cli(ag) && (!cfg->claude_cli_delegate_enabled || !ag->is_server_hosted))
      return 0;
   return 1;
}

void ensemble_default_panel_from_agents(config_t *cfg, const agent_config_t *acfg)
{
   if (cfg->ensemble_reference_count > 0)
      return;
   int n = 0;
   for (int i = 0; i < acfg->agent_count && n < ENSEMBLE_MAX_REFS; i++)
   {
      if (!ensemble_panelist_eligible(cfg, &acfg->agents[i]))
         continue;
      snprintf(cfg->ensemble_reference_models[n], sizeof(cfg->ensemble_reference_models[n]), "%s",
               acfg->agents[i].name);
      n++;
   }
   cfg->ensemble_reference_count = n;
   if (!cfg->ensemble_aggregator[0] && n > 0)
      snprintf(cfg->ensemble_aggregator, sizeof(cfg->ensemble_aggregator), "%s",
               cfg->ensemble_reference_models[0]);
}

void ensemble_resolve_random_seats(config_t *cfg, const agent_config_t *acfg)
{
   /* Replace each "$random" seat with a concretely-picked review-capable agent,
    * excluding models already seated (pinned seats + prior random picks) for
    * diversity. A $random seat that cannot be filled (roster exhausted) is DROPPED
    * — an interactive roundtable degrades rather than fails; the fail-on-
    * unfulfillable rule is the autonomous gate's, where a pinned model is a hard
    * requirement. Pinned seats pass through untouched (the availability filter
    * below drops an unavailable pinned model). Runs before the authorization /
    * availability filters so a resolved seat is a real, filterable agent. */
   char models[ENSEMBLE_MAX_REFS][sizeof cfg->ensemble_reference_models[0]];
   char personas[ENSEMBLE_MAX_REFS][sizeof cfg->ensemble_reference_personas[0]];
   const char *used[ENSEMBLE_MAX_REFS];
   int n = 0, nused = 0;

   for (int i = 0; i < cfg->ensemble_reference_count && i < ENSEMBLE_MAX_REFS; i++)
      if (!rt_seat_is_random(cfg->ensemble_reference_models[i]))
         used[nused++] = cfg->ensemble_reference_models[i];

   for (int i = 0; i < cfg->ensemble_reference_count && i < ENSEMBLE_MAX_REFS; i++)
   {
      const char *persona =
          (i < cfg->ensemble_reference_persona_count) ? cfg->ensemble_reference_personas[i] : "";
      if (!rt_seat_is_random(cfg->ensemble_reference_models[i]))
      {
         snprintf(models[n], sizeof models[n], "%s", cfg->ensemble_reference_models[i]);
         snprintf(personas[n], sizeof personas[n], "%s", persona);
         n++;
         continue;
      }
      int idx = delegate_pick_for_role((agent_config_t *)acfg, "review", used, nused);
      if (idx < 0)
      {
         aimee_log(LOG_WARN, "delegate.panel",
                   "no review-capable agent left for a $random seat -> dropping it");
         continue;
      }
      snprintf(models[n], sizeof models[n], "%s", acfg->agents[idx].name);
      snprintf(personas[n], sizeof personas[n], "%s", persona);
      if (nused < ENSEMBLE_MAX_REFS)
         used[nused++] = acfg->agents[idx].name; /* stable pointer into acfg for this call */
      n++;
   }

   for (int i = 0; i < n; i++)
   {
      snprintf(cfg->ensemble_reference_models[i], sizeof cfg->ensemble_reference_models[i], "%s",
               models[i]);
      snprintf(cfg->ensemble_reference_personas[i], sizeof cfg->ensemble_reference_personas[i],
               "%s", personas[i]);
   }
   cfg->ensemble_reference_count = n;
   cfg->ensemble_reference_persona_count = n;

   /* An aggregator explicitly set to "$random" resolves the same way. */
   if (cfg->ensemble_aggregator[0] && strcmp(cfg->ensemble_aggregator, RT_SEAT_RANDOM) == 0)
   {
      int idx = delegate_pick_for_role((agent_config_t *)acfg, "review", used, nused);
      if (idx >= 0)
         snprintf(cfg->ensemble_aggregator, sizeof cfg->ensemble_aggregator, "%s",
                  acfg->agents[idx].name);
      else
         cfg->ensemble_aggregator[0] = '\0';
   }
}

void ensemble_filter_panel_authorization(config_t *cfg, const agent_config_t *acfg)
{
   /* Resolve "$random" seats to concrete agents FIRST, so the eligibility check
    * below (and the availability filter after it) see real, filterable agents. */
   ensemble_resolve_random_seats(cfg, acfg);
   /* Applies the same eligibility rule to an EXPLICITLY configured
    * ensemble.reference_models list: drop any entry that names a configured agent
    * which is not panel-eligible (an unauthorized / disabled / client-only
    * claude). An entry that is not a configured agent (an ad-hoc model id) is
    * left as-is. Keeps reference_models and the aggregator consistent. */
   int n = 0;
   for (int i = 0; i < cfg->ensemble_reference_count; i++)
   {
      const char *name = cfg->ensemble_reference_models[i];
      const agent_t *ag = agent_find((agent_config_t *)acfg, name);
      if (ag && !ensemble_panelist_eligible(cfg, ag))
      {
         aimee_log(LOG_WARN, "delegate.panel",
                   "dropping unauthorized panelist '%s' from the roundtable panel", name);
         continue;
      }
      if (n != i)
         snprintf(cfg->ensemble_reference_models[n], sizeof(cfg->ensemble_reference_models[n]),
                  "%s", name);
      n++;
   }
   cfg->ensemble_reference_count = n;
   /* If the aggregator named a now-dropped agent, repoint it to the first seat. */
   if (cfg->ensemble_aggregator[0])
   {
      const agent_t *agg = agent_find((agent_config_t *)acfg, cfg->ensemble_aggregator);
      if (agg && !ensemble_panelist_eligible(cfg, agg))
         cfg->ensemble_aggregator[0] = '\0';
   }
   if (!cfg->ensemble_aggregator[0] && n > 0)
      snprintf(cfg->ensemble_aggregator, sizeof(cfg->ensemble_aggregator), "%s",
               cfg->ensemble_reference_models[0]);
}

void ensemble_filter_panel_availability(config_t *cfg, const agent_config_t *acfg)
{
   /* Runtime gate (distinct from the authorization gate above): drop any
    * configured panelist that is not currently USABLE — an HTTP agent with no
    * resolvable key, a CLI agent whose command/tmux is absent, or one the health
    * breaker marked DOWN. Otherwise it burns a panel seat on a guaranteed-to-fail
    * participant and silently degrades the round (the bug that left a roundtable
    * "2/3 participants failed" with unkeyed models in the list). An ad-hoc model
    * id that is not a configured agent is left as-is (we cannot check it). Same
    * predicate single-delegate routing uses: agent_is_available_for_routing. */
   int n = 0;
   for (int i = 0; i < cfg->ensemble_reference_count; i++)
   {
      const char *name = cfg->ensemble_reference_models[i];
      const agent_t *ag = agent_find((agent_config_t *)acfg, name);
      if (ag && !agent_is_available_for_routing(ag))
      {
         aimee_log(LOG_WARN, "delegate.panel",
                   "dropping unavailable panelist '%s' (no key / unhealthy / command missing)",
                   name);
         continue;
      }
      if (n != i)
         snprintf(cfg->ensemble_reference_models[n], sizeof(cfg->ensemble_reference_models[n]),
                  "%s", name);
      n++;
   }
   cfg->ensemble_reference_count = n;
   if (cfg->ensemble_aggregator[0])
   {
      const agent_t *agg = agent_find((agent_config_t *)acfg, cfg->ensemble_aggregator);
      if (agg && !agent_is_available_for_routing(agg))
         cfg->ensemble_aggregator[0] = '\0';
   }
   if (!cfg->ensemble_aggregator[0] && n > 0)
      snprintf(cfg->ensemble_aggregator, sizeof(cfg->ensemble_aggregator), "%s",
               cfg->ensemble_reference_models[0]);
}

int delegate_ensemble_run(agent_config_t *acfg, const config_t *cfg, const char *prompt,
                          delegate_ensemble_result_t *out)
{
   if (!acfg || !cfg || !prompt || !out)
      return -1;

   memset(out, 0, sizeof(*out));

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

   agent_run_parallel(acfg, tasks, ref_count, results, 0 /* MoA aggregate: no deadline */);
   /* Account each participant's run onto the originating session, like a delegate. */
   for (int i = 0; i < ref_count; i++)
      ensemble_fold_cost(acfg, &results[i], cfg->ensemble_reference_models[i]);

   /* Partial-failure metadata: how many of the fanned-out participants returned
    * no usable response. Set before any early return so degraded/cost-capped
    * results still carry it. */
   out->participants_total = ref_count;
   out->participants_failed = ref_count - count_successful(results, ref_count);

   double cost = estimate_cost(acfg, results, cfg->ensemble_reference_models, ref_count);
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

   /* Size the synthesis buffer to the actual content (was a fixed 16 KB stack
    * buffer that returned -1 — collapsing the whole round — whenever the original
    * prompt plus the panel's answers exceeded it, e.g. any real code review). */
   size_t syn_cap = strlen(prompt) + 1024;
   for (int i = 0; i < ref_count; i++)
      if (results[i].response)
         syn_cap += strlen(results[i].response) + 256;
   char *synthesis_buf = malloc(syn_cap);
   if (!synthesis_buf || build_synthesis_prompt(synthesis_buf, syn_cap, prompt, results, order,
                                                cfg->ensemble_reference_models, ref_count) != 0)
   {
      aimee_log(LOG_ERROR, "delegate_ensemble", "failed to build synthesis prompt");
      free(synthesis_buf);
      for (int i = 0; i < ref_count; i++)
         free(results[i].response);
      return -1;
   }

   for (int i = 0; i < ref_count; i++)
      free(results[i].response);

   agent_result_t agg_result;
   int agg_rc = run_aggregator(acfg, cfg, synthesis_buf, 0, &agg_result);
   free(synthesis_buf);

   if (agg_rc == 0 && agg_result.response && agg_result.response[0])
   {
      snprintf(out->response, sizeof(out->response), "%s", agg_result.response);
      out->cost_usd += result_token_cost(acfg, &agg_result, cfg->ensemble_aggregator);
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

/* Assemble a consolidated review artifact from the already-deduped panel items,
 * so a single-round review needs no synthesis LLM call (the panel's structured
 * findings already ARE the review). Groups by severity; caller owns the result. */
static char *assemble_review_artifact(const roundtable_result_t *out)
{
   dstr_t s;
   dstr_init(&s);
   static const char *const sevs[] = {"blocking", "suggestion", "nit"};
   static const char *const heads[] = {"## Blocking", "## Suggestions", "## Nits"};
   int emitted = 0;
   char done[ROUNDTABLE_MAX_REVIEW_ITEMS];
   memset(done, 0, sizeof(done));
   for (int g = 0; g < 3; g++)
   {
      int group_started = 0;
      for (int i = 0; i < out->item_count && i < ROUNDTABLE_MAX_REVIEW_ITEMS; i++)
      {
         const roundtable_review_item_t *it = &out->items[i];
         if (strcmp(it->severity, sevs[g]) != 0)
            continue;
         done[i] = 1;
         if (!group_started)
         {
            dstr_appendf(&s, "%s%s\n", emitted ? "\n" : "", heads[g]);
            group_started = 1;
         }
         dstr_appendf(&s, "- **%s**", it->category[0] ? it->category : "review");
         if (it->location[0])
            dstr_appendf(&s, " (%s)", it->location);
         dstr_appendf(&s, ": %s", it->summary);
         if (it->recommendation[0])
            dstr_appendf(&s, " — %s", it->recommendation);
         dstr_append_char(&s, '\n');
         emitted++;
      }
   }
   /* Any item with an unexpected severity still gets reported. */
   for (int i = 0; i < out->item_count && i < ROUNDTABLE_MAX_REVIEW_ITEMS; i++)
   {
      if (done[i])
         continue;
      const roundtable_review_item_t *it = &out->items[i];
      dstr_appendf(&s, "%s- **%s**: %s\n", emitted ? "" : "## Findings\n",
                   it->category[0] ? it->category : "review", it->summary);
      emitted++;
   }
   if (!emitted)
      dstr_append_str(&s, "No issues found by the panel.\n");
   return dstr_steal(&s);
}

int delegate_roundtable_run(agent_config_t *acfg, const config_t *cfg, const char *task,
                            const roundtable_opts_t *opts, roundtable_result_t *out)
{
   if (!acfg || !cfg || !task || !out)
      return -1;
   memset(out, 0, sizeof(*out));

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
      local.max_rounds = cfg->roundtable_max_rounds > 0 ? cfg->roundtable_max_rounds : 1;
      local.converge_threshold = cfg->roundtable_converge_threshold;
      local.deadline_ms = cfg->roundtable_deadline_ms;
   }
   if (local.max_rounds <= 0)
      local.max_rounds = 1;
   if (local.converge_threshold < 0)
      local.converge_threshold = 10;
   if (local.brief_truncated)
      out->truncated = 1;

   /* Bind the originating session for this thread so panel + aggregator runs fold
    * their cost onto it. Set unconditionally on every entry (NULL when there's no
    * parent), so a reused worker thread can't inherit a prior turn's session; it's
    * only read while this call runs the panel/aggregator on this same thread. */
   g_rt_parent_sid = local.parent_session_id;

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

   out->participants_total = ref_count; /* panel size per round (== MoA source) */
   out->participants_failed = 0;        /* set to the best round's count as it is chosen */

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
      /* Progress: emit a round-boundary marker so a multi-minute run is observably
       * advancing (in the server log) rather than a silent block. */
      aimee_log(LOG_INFO, "roundtable.progress",
                "round %d/%d: dispatching %d %s panelists (elapsed %lds)", round, local.max_rounds,
                ref_count, local.mode == ROUNDTABLE_REVIEW ? "review" : "draft",
                (long)((monotonic_ms() - start_ms) / 1000));
      if (cfg->ensemble_max_cost_usd > 0.0)
      {
         double preflight = estimated_round_cost(acfg, cfg, ref_count, 768);
         if (out->cost_usd + preflight > cfg->ensemble_max_cost_usd)
            aimee_log(LOG_WARN, "delegate_roundtable",
                      "round cost estimate $%.4f would exceed cap $%.4f; continuing until "
                      "observed cost is available",
                      out->cost_usd + preflight, cfg->ensemble_max_cost_usd);
      }
      /* Compress only the ACCUMULATED CARRYOVER (artifact + peer_notes), never the
       * task. The task is the fixed, incompressible review target / draft brief;
       * counting it here made any single-shot review whose target exceeds ~22 KB
       * trip summarize_forward on round 1 (carryover empty), which burned an
       * aggregator call against the panel deadline, falsely flagged the run
       * truncated+degraded, and starved the panelists into an empty (items:0)
       * result. Carryover only, so a large task passes through to the panel intact. */
      if (strlen(artifact) + strlen(peer_notes) > 22000)
      {
         if (summarize_forward(acfg, cfg, task, &artifact, &peer_notes, &out->cost_usd) == 0)
         {
            out->truncated = 1;
            out->degraded = 1;
         }
      }

      agent_result_t results[ENSEMBLE_MAX_REFS];
      memset(results, 0, sizeof(results));
      /* Remaining roundtable budget bounds this round's parallel panel so a hung
       * panelist is abandoned at the deadline instead of wedging the barrier
       * forever (floor at 5s so a near-exhausted budget still attempts the panel).
       * 0 when no deadline is configured. */
      int panel_deadline_ms = 0;
      if (local.deadline_ms > 0)
      {
         long remaining = (long)local.deadline_ms - (monotonic_ms() - start_ms);
         panel_deadline_ms = remaining > 5000 ? (int)remaining : 5000;
      }
      int rc = local.turns == ROUNDTABLE_SEQUENTIAL
                   ? run_round_sequential(acfg, cfg, task, artifact, &peer_notes, local.mode, round,
                                          local.brief, local.context, results)
                   : run_round_parallel(acfg, cfg, task, artifact, peer_notes, local.mode, round,
                                        local.brief, local.context, results, panel_deadline_ms);
      if (rc != 0)
      {
         for (int i = 0; i < ref_count; i++)
            free(results[i].response);
         goto fail;
      }
      /* Partial-failure metadata: participants that returned no usable response
       * this round (raw model results, before review-JSON repair). Recorded into
       * out->participants_failed wherever this round is adopted as best_round, so
       * the surfaced count matches the returned artifact's provenance. */
      int round_failed = ref_count - count_successful(results, ref_count);
      aimee_log(LOG_INFO, "roundtable.progress",
                "round %d/%d: %d/%d panelists responded; synthesizing", round, local.max_rounds,
                ref_count - round_failed, ref_count);
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
         capture_round_review_items(results, ref_count, out, round);
      }

      double round_cost = estimate_cost(acfg, results, cfg->ensemble_reference_models, ref_count);
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
            out->participants_failed = round_failed;
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
            /* Single-round: no cross-round comparison, so skip the scorer call. */
            int score = local.max_rounds > 1
                            ? run_quality_scorer(acfg, task, results[best].response, &out->cost_usd)
                            : 0;
            if (local.max_rounds <= 1 || score > best_score)
            {
               free(best_artifact);
               best_artifact = xstrdup0(results[best].response);
               best_score = score;
               out->best_round = round;
               out->participants_failed = round_failed;
            }
         }
         for (int i = 0; i < ref_count; i++)
            free(results[i].response);
         continue;
      }

      /* Single-round review: the deduped panel items already hold the findings
       * (captured above, no LLM call). Assemble the consolidated artifact from
       * them and skip the synthesis LLM call entirely — a 1-round review is then
       * just the parallel panel, no serial aggregator pass. Multi-round reviews
       * and all draft runs still synthesize (each round feeds the next). */
      if (local.mode == ROUNDTABLE_REVIEW && local.max_rounds <= 1)
      {
         free(best_artifact);
         /* Replay verification (Part A): re-ground each captured item against the
          * code index and re-derive severity before the artifact is built, so the
          * artifact reflects only kept items; the rejected appendix is appended
          * after. Gated; off => identical to prior behavior. */
         if (cfg->roundtable_replay_verify_enabled)
            roundtable_verify_items(out);
         best_artifact = assemble_review_artifact(out);
         if (cfg->roundtable_replay_verify_enabled)
            roundtable_artifact_append_rejected(&best_artifact, out);
         out->best_round = round;
         out->participants_failed = round_failed;
         for (int i = 0; i < ref_count; i++)
            free(results[i].response);
         break;
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
      /* Bound synthesis by the roundtable's own deadline so the fallback loop
       * cannot run long past it (the panel already respects this deadline). */
      long agg_deadline_abs = local.deadline_ms > 0 ? start_ms + local.deadline_ms : 0;
      int agg_rc = run_aggregator(acfg, cfg, synthesis_prompt, agg_deadline_abs, &agg_result);
      free(synthesis_prompt);
      if (agg_rc != 0 || !agg_result.response || !agg_result.response[0])
      {
         out->degraded = 1;
         free(agg_result.response);
         continue;
      }
      out->cost_usd += result_token_cost(acfg, &agg_result, cfg->ensemble_aggregator);

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
      /* The quality scorer exists only to compare candidates across rounds and
       * pick the best. A single-round run has exactly one candidate, so skip the
       * extra (large, sequential, external-LLM) scorer call and adopt this
       * round's synthesis unconditionally — this removes a whole serial pass from
       * the common `rounds:1` review. */
      int score = local.max_rounds > 1
                      ? run_quality_scorer(acfg, task, agg_result.response, &out->cost_usd)
                      : 0;
      if (local.max_rounds <= 1 || score > best_score)
      {
         free(best_artifact);
         best_artifact = xstrdup0(agg_result.response);
         best_score = score;
         out->best_round = round;
         out->participants_failed = round_failed;
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
            out->cost_usd += result_token_cost(acfg, &apply_result, "draft");
         }
         free(apply_result.response);
         free(full);
      }
   }

   out->artifact = best_artifact ? best_artifact : xstrdup0(artifact);
   out->artifact_round = out->best_round > 0 ? out->best_round : out->rounds_run;
   if (out->artifact && local.mode == ROUNDTABLE_REVIEW)
   {
      /* Questions are a review-mode concept (directed PR review), so a draft run
       * never triggers the pass even if a brief carried questions. The question
       * pass is an extra reason-role LLM call: skip it when the run was cancelled
       * (respect the stop) or already hit the cost cap (do not spend past the
       * budget), but still report the questions as gaps so the
       * answered_questions/coverage_gaps contract stays populated. */
      if (out->cancelled || out->cost_capped)
         mark_question_gaps(&local, out);
      else
         answer_roundtable_questions(acfg, task, &local, out);
   }
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
