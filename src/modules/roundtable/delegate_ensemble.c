/* Canonical model participants and synthesis for ensemble/roundtable execution. */
#include "delegate_ensemble_internal.h"
#include "aimee.h"
#include "delegate_ensemble.h"
#include "ensemble_request.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "config.h"
#include <aimee/delegates/delegate_credentials.h>
#include "cost_fold.h"
#include "log.h"
#include "persona.h"
#include "dstr.h"
#include "roundtable_seat_resolve.h"
#include "roundtable_preset.h"
#include "runtime_secret.h"
#include "token_tracker.h"

#include <openssl/crypto.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <time.h>
#include <ctype.h>

/* The config arrays the engine fans out over must be sized to ENSEMBLE_MAX_REFS.
 * Checked against config's exported capacity rather than against legacy_config_record's
 * layout, so drift is still caught at compile time without this file knowing
 * the struct's shape. */
_Static_assert(ENSEMBLE_MAX_REFS == CONFIG_ENSEMBLE_MAX_REFS,
               "ENSEMBLE_MAX_REFS must equal CONFIG_ENSEMBLE_MAX_REFS");

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

/* Canonical completion, not a parallel display string, owns usability. */
static int ensemble_usable(const agent_result_t *result)
{
   return result && result->success &&
          !aimee_ir_response_complete_text_valid(&result->canonical_response);
}

static int ensemble_admit_result(agent_result_t *result)
{
   if (ensemble_usable(result))
      return 0;
   result->success = 0;
   return -1;
}

void delegate_ensemble_result_free(delegate_ensemble_result_t *result)
{
   if (!result || !result->canonical_response)
      return;
   aimee_response_free(result->canonical_response);
   free(result->canonical_response);
   result->canonical_response = NULL;
}

static int ensemble_adopt_output(delegate_ensemble_result_t *out, const agent_result_t *source)
{
   if (!out || !source || !source->success ||
       aimee_ir_response_complete_text_valid(&source->canonical_response))
      return -1;
   aimee_response_t *owned = calloc(1, sizeof *owned);
   if (!owned || aimee_ir_response_clone(&source->canonical_response, owned))
   {
      free(owned);
      return -1;
   }
   delegate_ensemble_result_free(out);
   out->canonical_response = owned;
   out->success = 1;
   return 0;
}

static int count_successful(const agent_result_t *results, int count)
{
   /* A usable participant result is exactly a non-empty response. Review mode
    * nulls an unrecoverably malformed response before calling this helper. */
   int n = 0;
   for (int i = 0; i < count; i++)
   {
      if (ensemble_usable(&results[i]))
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
      if (ensemble_usable(&results[i]))
      {
         char *text = aimee_ir_response_text_alloc(&results[i].canonical_response);
         if (!text)
            continue;
         int len = (int)strlen(text);
         free(text);
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
   /* NaN stays unknown through every sum and renders as JSON null. */
   if (agent_result_usage_is_unobserved(result))
      return NAN;
   /* Prefer the model actually served (recorded on the result, and updated
    * across fallback-model swaps) over re-deriving it from the configured agent
    * by name — so observed cost reflects what really ran. */
   if (result->model[0])
      return delegate_cost_estimate_usd(NULL, result->model, result->prompt_tokens,
                                        result->completion_tokens);
   const char *agent = result->agent_name[0] ? result->agent_name : fallback_agent;
   return agent_token_cost(acfg, agent, result->prompt_tokens, result->completion_tokens);
}

static int cost_limit_reached(const ensemble_panel_t *panel, double cost)
{
   return panel->max_cost_usd > 0.0 && (!isfinite(cost) || cost > panel->max_cost_usd);
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
                               const aimee_request_t *request, agent_result_t *out)
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
         char v[MAX_API_KEY_LEN] = "";
         if (runtime_secret_get(leased_env, v, sizeof(v)) && v[0])
            snprintf(ag->api_key, MAX_API_KEY_LEN, "%s", v);
         OPENSSL_cleanse(v, sizeof(v));
         leased = 1;
      }
   }
   int rc = (agent_name && agent_name[0]) ? agent_run_named(acfg, agent_name, role, request, out)
                                          : agent_run(acfg, role, request, out);
   if (rc == 0 && ensemble_admit_result(out) != 0)
      rc = -1;
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

static int build_synthesis_request(const aimee_request_t *task, const agent_result_t *results,
                                   const int *order, const char (*names)[128], int count,
                                   const char *instructions, aimee_request_t *out)
{
   if (aimee_ir_request_clone(task, out))
      return -1;
   for (int i = 0; i < count; i++)
   {
      int index = order ? order[i] : i;
      if (!ensemble_usable(&results[index]))
         continue;
      char label[256];
      snprintf(label, sizeof label, "Participant output: %s [%d]",
               names && names[index][0] ? names[index] : "participant", i + 1);
      if (ensemble_request_add_response(out, label, &results[index].canonical_response))
         goto fail;
   }
   aimee_request_t complete = {0};
   if (ensemble_synthesis_request(out, instructions, &complete))
      goto fail;
   aimee_request_free(out);
   *out = complete;
   return 0;
fail:
   aimee_request_free(out);
   return -1;
}

static int run_aggregator(agent_config_t *acfg, const ensemble_panel_t *panel,
                          const char *synthesis_prompt, long deadline_abs_ms, agent_result_t *out,
                          const aimee_request_t *source)
{
   memset(out, 0, sizeof(*out));
   aimee_request_t request = {0};
   if (synthesis_prompt ? ensemble_synthesis_request(source, synthesis_prompt, &request)
                        : aimee_ir_request_clone(source, &request))
      return AGENT_RC_CONTEXT_REFUSED;

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
    * be dispatched by NAME (agent_run_named): agent_run selects by role and
    * an agent's name is not one of its roles, so a name routed as a role finds
    * nothing and returns empty. A role form (or no aggregator) routes by role. */
   /* Primary: the configured (or default) aggregator. max_tokens 0 = derive from
    * the aggregator model's own output ceiling, so a reasoning aggregator isn't
    * truncated mid-synthesis (was a hard 4096). */
   const char *primary_name = NULL; /* bare-name aggregator we tried, to not retry it */
   if (panel->aggregator[0])
   {
      const char *agg = panel->aggregator;
      const char *at = strchr(agg, '@');
      if (!at)
      {
         primary_name = agg;
         if (delegate_run_inline(&agg_cfg, agg, "review", &request, out) == 0 &&
             ensemble_usable(out))
         {
            aimee_request_free(&request);
            return 0;
         }
      }
      else
      {
         const char *role = (at[1]) ? at + 1 : "review";
         if (delegate_run_inline(&agg_cfg, NULL, role, &request, out) == 0 && ensemble_usable(out))
         {
            aimee_request_free(&request);
            return 0;
         }
      }
   }
   else if (delegate_run_inline(&agg_cfg, NULL, "review", &request, out) == 0 &&
            ensemble_usable(out))
   {
      aimee_request_free(&request);
      return 0;
   }

   if (panel->max_cost_usd > 0.0 && agent_result_usage_is_unobserved(out))
   {
      aimee_request_free(&request);
      return -1;
   }

   /* Fallback: try one alternate panelist. Synthesis/repair is bounded overhead,
    * never authority to retry across an arbitrarily large configured roster. */
   int fallbacks_tried = 0;
   for (int i = 0; i < panel->reference_count; i++)
   {
      if (deadline_abs_ms > 0 && monotonic_ms() >= deadline_abs_ms)
      {
         aimee_log(LOG_WARN, "delegate_roundtable",
                   "aggregator: deadline reached at fallback candidate %d/%d; abandoning the "
                   "remaining %d (a flaky synthesis model must not wedge the roundtable)",
                   i, panel->reference_count, panel->reference_count - i);
         break;
      }
      const char *cand = panel->reference_models[i];
      if (!cand[0] || (primary_name && strcmp(cand, primary_name) == 0))
         continue;
      if (fallbacks_tried++ >= 1)
         break;
      int usage_unobserved = agent_result_usage_is_unobserved(out);
      agent_result_clear_response(out);
      memset(out, 0, sizeof(*out));
      if (delegate_run_inline(&agg_cfg, cand, "review", &request, out) == 0 && ensemble_usable(out))
      {
         out->usage_unobserved |= usage_unobserved;
         aimee_request_free(&request);
         return 0;
      }
      out->usage_unobserved |= usage_unobserved;
   }
   aimee_request_free(&request);
   return -1;
}

#define PANEL_DEFAULT_PERSONA_COUNT                                                                \
   ((int)(sizeof(PANEL_DEFAULT_PERSONAS) / sizeof(PANEL_DEFAULT_PERSONAS[0])))

/* Is `ag` allowed to sit on a roundtable/ensemble panel? Enabled + named, NOT
 * flagged primary-only (agents.json `primary_only` — the per-agent delegate
 * opt-out that replaced the global claude_cli_delegate_enabled; a claude-oauth
 * subscription is pre-flagged primary-only for the ToS reason), and — for
 * claude-CLI — able to run server-side (is_server_hosted, via `aimee agent add
 * claude-oauth`). A client-only claude has no server endpoint and would just
 * burn a slot on a "failed to build request URL" participant; server-hosting is
 * capability, primary_only is authorization, so both must pass. So a
 * primary-only or disabled claude is never seated, even after a server-side
 * OAuth setup. Other enabled agents with the review role are eligible.
 *
 * Role membership is the operator's inclusion policy.  There is deliberately no
 * implicit "primary provider" exclusion: if an enabled agent has the review role
 * and is not marked primary_only, it is a valid unpinned panelist.  Operators
 * exclude it by removing the role, disabling it, or setting primary_only. */
int ensemble_panelist_eligible(const agent_t *ag)
{
   if (!ag || !ag->enabled || !ag->name[0])
      return 0;
   /* A review seat requires the agent to DECLARE the review role (or `all`); an
    * implementation-only delegate is never seated on a review panel by exec-role
    * default. Single rule, shared with routing. */
   if (!agent_has_role(ag, "review"))
      return 0;
   if (ag->primary_only)
      return 0;
   if (agent_is_claude_cli(ag) && !ag->is_server_hosted)
      return 0;
   return 1;
}

int ensemble_validate_panel_pins(const ensemble_panel_t *panel, const agent_config_t *acfg,
                                 char *err, size_t err_n)
{
   if (err && err_n)
      err[0] = '\0';
   if (!panel || !acfg)
      return -1;
   int pinned[MAX_AGENTS];
   memset(pinned, 0, sizeof pinned);
   for (int i = 0; i < panel->reference_count; i++)
   {
      const char *name = panel->reference_models[i];
      if (!name[0] || rt_seat_is_random(name))
         continue;
      const agent_t *ag = agent_find((agent_config_t *)acfg, name);
      const char *reason = NULL;
      if (!ag || !ensemble_panelist_eligible(ag))
         reason = "is not enabled and eligible for review";
      else if (!agent_is_available_for_routing(ag))
         reason = "is not currently available";
      if (!reason)
      {
         int idx = (int)(ag - acfg->agents);
         int capacity = ag->max_parallel > 0 ? ag->max_parallel : AGENT_DEFAULT_MAX_PARALLEL;
         if (idx >= 0 && idx < acfg->agent_count && ++pinned[idx] > capacity)
            reason = "is pinned more times than its configured max_parallel capacity";
      }
      if (!reason)
         continue;
      if (err && err_n)
         snprintf(err, err_n, "required roundtable agent '%s' %s", name, reason);
      return -1;
   }
   return 0;
}

static const char *panel_agent_provider(const agent_t *ag)
{
   return ag->provider[0] ? ag->provider : ag->name;
}

static int panel_provider_seats(const agent_config_t *acfg, const int seated[],
                                const char *provider)
{
   int total = 0;
   for (int i = 0; i < acfg->agent_count; i++)
      if (strcmp(panel_agent_provider(&acfg->agents[i]), provider) == 0)
         total += seated[i];
   return total;
}

static int ensemble_pick_balanced_seat(const agent_config_t *acfg, const int seated[])
{
   int best = -1;
   int best_agent_seats = 0;
   int best_provider_seats = 0;

   /* Represent every provider before assigning a second seat to one. */
   for (int i = 0; i < acfg->agent_count; i++)
   {
      const agent_t *ag = &acfg->agents[i];
      int capacity = ag->max_parallel > 0 ? ag->max_parallel : AGENT_DEFAULT_MAX_PARALLEL;
      if (!ensemble_panelist_eligible(ag) || !agent_is_available_for_routing(ag) ||
          seated[i] >= capacity)
         continue;
      if (panel_provider_seats(acfg, seated, panel_agent_provider(ag)) == 0)
         return i;
   }

   /* Then represent every model, preferring the least represented provider. */
   for (int i = 0; i < acfg->agent_count; i++)
   {
      const agent_t *ag = &acfg->agents[i];
      int capacity = ag->max_parallel > 0 ? ag->max_parallel : AGENT_DEFAULT_MAX_PARALLEL;
      if (seated[i] != 0 || !ensemble_panelist_eligible(ag) ||
          !agent_is_available_for_routing(ag) || seated[i] >= capacity)
         continue;
      int provider_seats = panel_provider_seats(acfg, seated, panel_agent_provider(ag));
      if (best < 0 || provider_seats < best_provider_seats)
      {
         best = i;
         best_provider_seats = provider_seats;
      }
   }
   if (best >= 0)
      return best;

   /* Finally cycle across already represented models. Lower per-model count is
    * primary; provider count is the tie-breaker that keeps providers balanced. */
   for (int i = 0; i < acfg->agent_count; i++)
   {
      const agent_t *ag = &acfg->agents[i];
      int capacity = ag->max_parallel > 0 ? ag->max_parallel : AGENT_DEFAULT_MAX_PARALLEL;
      if (!ensemble_panelist_eligible(ag) || !agent_is_available_for_routing(ag) ||
          seated[i] >= capacity)
         continue;
      int provider_seats = panel_provider_seats(acfg, seated, panel_agent_provider(ag));
      if (best < 0 || seated[i] < best_agent_seats ||
          (seated[i] == best_agent_seats && provider_seats < best_provider_seats))
      {
         best = i;
         best_agent_seats = seated[i];
         best_provider_seats = provider_seats;
      }
   }
   return best;
}

void ensemble_resolve_random_seats(ensemble_panel_t *panel, const agent_config_t *acfg)
{
   /* Fill every configured seat. Provider diversity comes first, then model
    * diversity, then balanced reuse up to each model's max_parallel capacity.
    * A seat is dropped only when total eligible capacity is genuinely exhausted;
    * the caller compares the result with the preset's exact seat count and fails
    * closed instead of running a partial configured roundtable. */
   char models[ENSEMBLE_MAX_REFS][sizeof panel->reference_models[0]];
   char personas[ENSEMBLE_MAX_REFS][sizeof panel->reference_personas[0]];
   int seated[MAX_AGENTS];
   int n = 0;
   memset(seated, 0, sizeof seated);

   for (int i = 0; i < panel->reference_count && i < ENSEMBLE_MAX_REFS; i++)
      if (!rt_seat_is_random(panel->reference_models[i]))
      {
         const agent_t *ag = agent_find((agent_config_t *)acfg, panel->reference_models[i]);
         if (ag)
         {
            int idx = (int)(ag - acfg->agents);
            if (idx >= 0 && idx < acfg->agent_count)
               seated[idx]++;
         }
      }

   for (int i = 0; i < panel->reference_count && i < ENSEMBLE_MAX_REFS; i++)
   {
      const char *persona =
          (i < panel->reference_persona_count) ? panel->reference_personas[i] : "";
      if (!rt_seat_is_random(panel->reference_models[i]))
      {
         snprintf(models[n], sizeof models[n], "%s", panel->reference_models[i]);
         snprintf(personas[n], sizeof personas[n], "%s", persona);
         n++;
         continue;
      }
      int idx = ensemble_pick_balanced_seat(acfg, seated);
      if (idx < 0)
      {
         aimee_log(LOG_WARN, "delegate.panel",
                   "roundtable capacity exhausted before every $random seat could be filled");
         continue;
      }
      snprintf(models[n], sizeof models[n], "%s", acfg->agents[idx].name);
      snprintf(personas[n], sizeof personas[n], "%s", persona);
      seated[idx]++;
      n++;
   }

   for (int i = 0; i < n; i++)
   {
      snprintf(panel->reference_models[i], sizeof panel->reference_models[i], "%s", models[i]);
      snprintf(panel->reference_personas[i], sizeof panel->reference_personas[i], "%s",
               personas[i]);
   }
   panel->reference_count = n;
   panel->reference_persona_count = n;

   /* An aggregator explicitly set to "$random" resolves the same way. */
   if (panel->aggregator[0] && strcmp(panel->aggregator, RT_SEAT_RANDOM) == 0)
   {
      int idx = ensemble_pick_balanced_seat(acfg, seated);
      if (idx >= 0)
         snprintf(panel->aggregator, sizeof panel->aggregator, "%s", acfg->agents[idx].name);
      else
         panel->aggregator[0] = '\0';
   }
}

void ensemble_filter_panel_authorization(ensemble_panel_t *panel, const agent_config_t *acfg)
{
   /* Resolve "$random" seats to concrete agents FIRST, so the eligibility check
    * below (and the availability filter after it) see real, filterable agents. */
   ensemble_resolve_random_seats(panel, acfg);
   /* Applies the same eligibility rule to an EXPLICITLY configured
    * ensemble.reference_models list: drop any entry that names a configured agent
    * which is not panel-eligible (an unauthorized / disabled / client-only
    * claude). An entry that is not a configured agent (an ad-hoc model id) is
    * dropped: a positive pin must name a configured, eligible agent. Keeps
    * reference_models and the aggregator consistent. */
   int n = 0;
   for (int i = 0; i < panel->reference_count; i++)
   {
      const char *name = panel->reference_models[i];
      const agent_t *ag = agent_find((agent_config_t *)acfg, name);
      if (!ag || !ensemble_panelist_eligible(ag))
      {
         aimee_log(LOG_WARN, "delegate.panel",
                   "dropping unauthorized panelist '%s' from the roundtable panel", name);
         continue;
      }
      if (n != i)
      {
         snprintf(panel->reference_models[n], sizeof(panel->reference_models[n]), "%s", name);
         snprintf(panel->reference_personas[n], sizeof(panel->reference_personas[n]), "%s",
                  panel->reference_personas[i]);
      }
      n++;
   }
   panel->reference_count = n;
   /* If the aggregator named a now-dropped agent, repoint it to the first seat. */
   if (panel->aggregator[0])
   {
      const agent_t *agg = agent_find((agent_config_t *)acfg, panel->aggregator);
      if (!agg || !ensemble_panelist_eligible(agg))
         panel->aggregator[0] = '\0';
   }
   if (!panel->aggregator[0] && n > 0)
      snprintf(panel->aggregator, sizeof(panel->aggregator), "%s", panel->reference_models[0]);
}

void ensemble_filter_panel_availability(ensemble_panel_t *panel, const agent_config_t *acfg)
{
   /* Runtime gate (distinct from the authorization gate above): drop any
    * configured panelist that is not currently USABLE — an HTTP agent with no
    * resolvable key, a CLI agent whose command/tmux is absent, or one the health
    * breaker marked DOWN. Otherwise it burns a panel seat on a guaranteed-to-fail
    * participant and silently degrades the round (the bug that left a roundtable
    * "2/3 participants failed" with unkeyed models in the list). An ad-hoc model
    * id that is not a configured agent is dropped: pins never create shadow
    * agents outside agents.json. Same predicate single-delegate routing uses:
    * agent_is_available_for_routing. */
   int n = 0;
   for (int i = 0; i < panel->reference_count; i++)
   {
      const char *name = panel->reference_models[i];
      const agent_t *ag = agent_find((agent_config_t *)acfg, name);
      if (!ag || !agent_is_available_for_routing(ag))
      {
         aimee_log(LOG_WARN, "delegate.panel",
                   "dropping unavailable panelist '%s' (no key / unhealthy / command missing)",
                   name);
         continue;
      }
      if (n != i)
      {
         snprintf(panel->reference_models[n], sizeof(panel->reference_models[n]), "%s", name);
         snprintf(panel->reference_personas[n], sizeof(panel->reference_personas[n]), "%s",
                  panel->reference_personas[i]);
      }
      n++;
   }
   panel->reference_count = n;
   if (panel->aggregator[0])
   {
      const agent_t *agg = agent_find((agent_config_t *)acfg, panel->aggregator);
      if (!agg || !agent_is_available_for_routing(agg))
         panel->aggregator[0] = '\0';
   }
   if (!panel->aggregator[0] && n > 0)
      snprintf(panel->aggregator, sizeof(panel->aggregator), "%s", panel->reference_models[0]);
}

void ensemble_fill_implicit_panel(ensemble_panel_t *panel, const agent_config_t *acfg)
{
   if (!panel || !acfg)
      return;

   /* No saved preset means there is no authority for a larger panel. Discard
    * legacy ensemble.reference_models rather than treating them as implicit
    * pins that can bypass the two-seat contract. */
   panel->reference_count = 0;
   panel->reference_persona_count = 0;
   panel->aggregator[0] = '\0';
   int seated[MAX_AGENTS];
   memset(seated, 0, sizeof seated);

   /* Provider diversity first. A missing provider name is scoped to the agent
    * name so unrelated legacy agents are never collapsed together. */
   for (int i = 0; i < acfg->agent_count && panel->reference_count < 2; i++)
   {
      const agent_t *ag = &acfg->agents[i];
      if (seated[i] > 0 || !ensemble_panelist_eligible(ag) || !agent_is_available_for_routing(ag))
         continue;
      const char *provider = ag->provider[0] ? ag->provider : ag->name;
      int provider_already_seated = 0;
      for (int j = 0; j < acfg->agent_count; j++)
      {
         if (seated[j] <= 0)
            continue;
         const agent_t *other = &acfg->agents[j];
         const char *other_provider = other->provider[0] ? other->provider : other->name;
         if (strcmp(provider, other_provider) == 0)
         {
            provider_already_seated = 1;
            break;
         }
      }
      if (provider_already_seated)
         continue;
      int pos = panel->reference_count++;
      snprintf(panel->reference_models[pos], sizeof(panel->reference_models[pos]), "%s", ag->name);
      panel->reference_personas[pos][0] = '\0';
      seated[i] = 1;
   }

   /* If only one provider exists, use a second distinct eligible agent. */
   for (int i = 0; i < acfg->agent_count && panel->reference_count < 2; i++)
   {
      const agent_t *ag = &acfg->agents[i];
      if (seated[i] > 0 || !ensemble_panelist_eligible(ag) || !agent_is_available_for_routing(ag))
         continue;
      int pos = panel->reference_count++;
      snprintf(panel->reference_models[pos], sizeof(panel->reference_models[pos]), "%s", ag->name);
      panel->reference_personas[pos][0] = '\0';
      seated[i] = 1;
   }

   panel->reference_persona_count = panel->reference_count;
   if (panel->reference_count > 0)
      snprintf(panel->aggregator, sizeof(panel->aggregator), "%s", panel->reference_models[0]);
}

int ensemble_prepare_runtime_panel(const char *requested, ensemble_panel_t *panel,
                                   const agent_config_t *acfg, char *err, size_t err_n)
{
   if (err && err_n)
      err[0] = '\0';
   int acquired = roundtable_preset_resolve_runtime(requested, panel, NULL, 0, err, err_n);
   if (acquired < 0)
      return -1;
   if (!acquired)
   {
      ensemble_fill_implicit_panel(panel, acfg);
      if (panel->reference_count > 0)
         return 0;
      if (err && err_n)
         snprintf(err, err_n, "no enabled review agent is currently available");
      return -1;
   }

   const int exact_seats = panel->reference_count;
   if (ensemble_validate_panel_pins(panel, acfg, err, err_n) != 0)
      return -1;
   ensemble_filter_panel_authorization(panel, acfg);
   ensemble_filter_panel_availability(panel, acfg);
   if (panel->reference_count != exact_seats)
   {
      if (err && err_n)
         snprintf(err, err_n, "roundtable requires %d seats but only %d are currently available",
                  exact_seats, panel->reference_count);
      return -1;
   }
   return 0;
}

void ensemble_panel_from_config(ensemble_panel_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   int n = config_ensemble_reference_count();
   if (n > ENSEMBLE_PANEL_MAX_SEATS)
      n = ENSEMBLE_PANEL_MAX_SEATS;
   for (int i = 0; i < n; i++)
      snprintf(out->reference_models[i], sizeof(out->reference_models[i]), "%s",
               config_ensemble_reference_models(i));
   int pn = config_ensemble_reference_persona_count();
   if (pn > ENSEMBLE_PANEL_MAX_SEATS)
      pn = ENSEMBLE_PANEL_MAX_SEATS;
   for (int i = 0; i < pn; i++)
      snprintf(out->reference_personas[i], sizeof(out->reference_personas[i]), "%s",
               config_ensemble_reference_personas(i));
   out->reference_count = n;
   out->reference_persona_count = pn;
   snprintf(out->aggregator, sizeof(out->aggregator), "%s", config_ensemble_aggregator());
   out->min_successful = config_ensemble_min_successful();
   out->max_cost_usd = config_ensemble_max_cost_usd();
   out->max_rounds = config_roundtable_max_rounds();
   out->converge_threshold = config_roundtable_converge_threshold();
   out->deadline_ms = config_roundtable_deadline_ms();
   out->chair_synthesis = config_roundtable_chair_synthesis();
   out->require_evidence = config_roundtable_require_evidence();
   out->replay_verify_enabled = config_roundtable_replay_verify_enabled();
   snprintf(out->turns, sizeof(out->turns), "%s", config_roundtable_turns());
}

int delegate_ensemble_run(agent_config_t *acfg, const ensemble_panel_t *panel,
                          const aimee_request_t *request, delegate_ensemble_result_t *out)
{
   if (!acfg || !panel || !out || aimee_ir_request_admit(request))
      return -1;

   memset(out, 0, sizeof(*out));

   int ref_count = panel->reference_count;
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
      tasks[i].agent = panel->reference_models[i];
      (void)aimee_ir_request_clone(request, &tasks[i].request);
   }

   agent_result_t results[ENSEMBLE_MAX_REFS];
   memset(results, 0, sizeof(results));

   agent_run_parallel(acfg, tasks, ref_count, results, 0 /* MoA aggregate: no deadline */);
   for (int i = 0; i < ref_count; i++)
      aimee_request_free(&tasks[i].request);
   /* Account each participant's run onto the originating session, like a delegate. */
   for (int i = 0; i < ref_count; i++)
   {
      (void)ensemble_admit_result(&results[i]);
      ensemble_fold_cost(acfg, &results[i], panel->reference_models[i]);
   }

   /* Partial-failure metadata: how many of the fanned-out participants returned
    * no usable response. Set before any early return so degraded/cost-capped
    * results still carry it. */
   out->participants_total = ref_count;
   out->participants_failed = ref_count - count_successful(results, ref_count);

   double cost = estimate_cost(acfg, results, panel->reference_models, ref_count);
   out->cost_usd = cost;

   /* Check cost cap before doing more work */
   if (cost_limit_reached(panel, cost))
   {
      aimee_log(LOG_INFO, "delegate_ensemble", "cost cap exceeded: $%.4f > $%.4f", cost,
                panel->max_cost_usd);
      out->cost_capped = 1;
      int best = best_candidate(results, ref_count);
      if (best >= 0 && ensemble_usable(&results[best]))
      {
         (void)ensemble_adopt_output(out, &results[best]);
      }
      for (int i = 0; i < ref_count; i++)
         agent_result_clear_response(&results[i]);
      return 0;
   }

   int successful = count_successful(results, ref_count);
   int min_ok = panel->min_successful > 0 ? panel->min_successful : 2;

   if (successful < min_ok)
   {
      aimee_log(LOG_INFO, "delegate_ensemble", "insufficient refs: %d < %d, degrading", successful,
                min_ok);
      out->degraded = 1;
      int best = best_candidate(results, ref_count);
      if (best >= 0 && ensemble_usable(&results[best]))
      {
         (void)ensemble_adopt_output(out, &results[best]);
      }
      for (int i = 0; i < ref_count; i++)
         agent_result_clear_response(&results[i]);
      return 0;
   }

   /* Shuffle candidate order to avoid position bias */
   int order[ENSEMBLE_MAX_REFS];
   for (int i = 0; i < ref_count; i++)
      order[i] = i;
   shuffle_indices(order, ref_count);

   aimee_request_t synthesis = {0};
   int build_rc = build_synthesis_request(
       request, results, order, panel->reference_models, ref_count,
       "You are a synthesis aggregator for a Mixture-of-Agents ensemble. Produce one "
       "reconciled answer incorporating the best elements of the labelled candidate "
       "answers. Do not mention the ensemble process.",
       &synthesis);
   for (int i = 0; i < ref_count; i++)
      agent_result_clear_response(&results[i]);
   if (build_rc)
      return -1;
   agent_result_t agg_result = {0};
   int agg_rc = run_aggregator(acfg, panel, NULL, 0, &agg_result, &synthesis);
   aimee_request_free(&synthesis);
   out->cost_usd += result_token_cost(acfg, &agg_result, panel->aggregator);

   if (agg_rc == 0 && ensemble_usable(&agg_result))
   {
      (void)ensemble_adopt_output(out, &agg_result);
   }
   else
   {
      aimee_log(LOG_ERROR, "delegate_ensemble", "aggregator failed: %s", agg_result.error);
      out->degraded = 1;
   }

   agent_result_clear_response(&agg_result);
   return 0;
}
