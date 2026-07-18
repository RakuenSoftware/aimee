#include "aimee.h"
#include "agent_config.h" /* agent_request_cancelled — server-owned turn lifecycle */
#include "aimee_errors.h"
#include "db1.h"
#include "delegate_role.h"
#include "skill_curator.h"
#include "skill_review.h"
#include "provider_catalog.h"
#include "db2/agent_hints.h"
#include "db2/agent_outcomes.h"
#include "db2/memory_query.h"
#include "db2/rules.h"
#include "db2/tasks.h"
#include "kb_client.h"
#include "agent.h"
#include "agent_protocol.h"
#include "agent_request_shaping.h"
#include "agent_tools.h"
#include "agent_tunnel.h"
#include "config.h"
#include "delegate_driver.h"
#include "http_retry.h"
#include "log.h"
#include "model_sampling.h"
#include "payload_rewrite.h"
#include "headers/plugin_c_hook.h"
#include "prompts.h"
#include "util.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
void agent_store_feedback(const agent_result_t *result, const char *role,
                          const char *prompt_summary);
const char *delegation_active_id(void);
void agent_reject_degenerate_plain_response(agent_result_t *out, const char *agent_name);
static int active_delegation_stopped(char *buf, size_t bufsz)
{
   const char *delegation_id = delegation_active_id();
   if (!delegation_id || !delegation_id[0])
      return 0;
   char reason[32];
   if (db1_delegation_spawn_stop_reason(delegation_id, reason, sizeof(reason)) != 1)
      return 0;
   if (buf && bufsz > 0)
      snprintf(buf, bufsz, "delegate %s (%s)", reason, delegation_id);
   return 1;
}

static int agent_uses_provider_cli(const agent_t *agent)
{
   return agent && (strcmp(agent->backend, AGENT_BACKEND_PROVIDER_CLI) == 0 ||
                    strcmp(agent->backend, AGENT_BACKEND_CLI_STDIO) == 0 ||
                    strcmp(agent->backend, AGENT_BACKEND_TMUX_CLI) == 0);
}

static int agent_uses_mistral_delegate_path(const agent_t *agent)
{
   if (!agent)
      return 0;
   if (strcmp(agent->provider, "mistral") == 0)
      return 1;
   return strcmp(agent->backend, AGENT_BACKEND_PROVIDER_CLI) == 0 &&
          (strcmp(agent->cli_kind, "mistral") == 0 ||
           strcmp(agent->cli_kind, "mistral-plan") == 0 || strcmp(agent->cli_kind, "vibe") == 0 ||
           strcmp(agent->cli_kind, "vibe-plan") == 0);
}

static void agent_apply_runtime_config(agent_t *agent)
{
   if (!agent)
      return;
   config_t runtime_cfg;
   if (config_load(&runtime_cfg) == 0)
      agent->autonomous = runtime_cfg.autonomous;
}

static void classify_outcome(const agent_result_t *result, int max_turns, agent_outcome_t *outcome)
{
   memset(outcome, 0, sizeof(*outcome));
   outcome->turns_used = result->turns;
   outcome->tools_called = result->tool_calls;
   outcome->tokens_used = result->prompt_tokens + result->completion_tokens;

   if (result->abstained)
   {
      outcome->outcome = OUTCOME_PARTIAL;
      snprintf(outcome->reason, sizeof(outcome->reason), "abstained: %s", result->abstain_reason);
      return;
   }

   if (!result->success)
   {
      if (result->error[0])
      {
         outcome->outcome = OUTCOME_ERROR;
         snprintf(outcome->reason, sizeof(outcome->reason), "%.250s", result->error);

         const char *tool_err = strstr(result->error, "tool ");
         if (tool_err)
            snprintf(outcome->tool_error_pattern, sizeof(outcome->tool_error_pattern), "%.120s",
                     tool_err);
      }
      else
      {
         outcome->outcome = OUTCOME_FAILURE;
         snprintf(outcome->reason, sizeof(outcome->reason), "execution failed");
      }
      return;
   }

   if (max_turns > 0 && result->turns >= max_turns)
   {
      outcome->outcome = OUTCOME_PARTIAL;
      snprintf(outcome->reason, sizeof(outcome->reason), "max turns reached (%d)", max_turns);
      return;
   }

   outcome->outcome = OUTCOME_SUCCESS;
   snprintf(outcome->reason, sizeof(outcome->reason), "completed in %d turns", result->turns);
}

static void record_outcome(const char *agent_name, const char *role, const agent_outcome_t *outcome)
{
   static const char *outcome_str[] = {"success", "partial", "failure", "error"};
   (void)kb_client_agent_outcome_record(agent_name, role, outcome_str[outcome->outcome],
                                        outcome->reason, outcome->turns_used, outcome->tools_called,
                                        outcome->tokens_used, outcome->tool_error_pattern);
}

/* THE single per-agent turn executor — see agent_exec.h. Every model turn (panel
 * lens, ensemble reference, delegate, fallback peer, chat/responses passthrough,
 * aux) routes through here so the per-agent max_parallel cap and provider-health
 * recording are enforced in ONE place instead of being re-implemented (and
 * forgotten) per dispatch path.
 *
 * Health-domain note: this deliberately WIDENS provider-health accounting to the
 * OpenAI-compatible ingress and the aux router, which previously bypassed it — a
 * failing model on those paths now degrades its catalog health like any other
 * turn (intended: one health signal per provider, whatever drove the call).
 *
 * Slot safety: there is no non-local exit between acquire and release — the
 * executors (agent_execute / agent_execute_with_tools_for_role) return normally
 * (C has no exceptions), so the release always runs and a slot is never leaked. */
int agent_dispatch_one(const agent_t *ag, const agent_network_t *net, const char *role,
                       const char *system_prompt, const char *user_prompt, int max_tokens,
                       double temperature, int use_tools, agent_result_t *out)
{
   if (!ag || !out)
      return -1;
   /* Own `out` from the first instruction, exactly like the executors: memset it so
    * EVERY early-return path (at-limit, bad-precondition) leaves a clean result —
    * out->response == NULL — and a caller that frees out->response after a non-zero
    * return can never hit stale/garbage. (No caller passes a live out->response
    * here; the executors also memset, so this is the established contract.) */
   memset(out, 0, sizeof(*out));
   /* The tools executor dereferences `net`; a plain completion ignores it. Enforce
    * the precondition (net != NULL when use_tools) so a future caller that flips
    * use_tools without supplying a network fails cleanly instead of crashing. */
   if (use_tools && !net)
   {
      snprintf(out->error, sizeof(out->error), "agent_dispatch_one: use_tools requires network");
      return -1;
   }
   /* Concurrency choke point: acquire the agent's max_parallel slot. At the limit,
    * report a DISTINCT at-limit signal (so a fan-out/retry caller picks a different
    * agent) and return BEFORE any health recording — saturation is a load signal,
    * not a provider fault. acquire() returns "unlimited" for max_parallel<=0 /
    * unknown agents, so single-dispatch callers are unaffected. */
   if (!provider_catalog_concurrent_acquire(ag->name, ag->max_parallel))
   {
      snprintf(out->agent_name, MAX_AGENT_NAME, "%s", ag->name);
      /* Aimee-internal back-pressure, not a provider fault: tag it with the aimee
       * error SLUG (not the numeric code) so it's identifiable wherever out->error
       * surfaces. The slug carries no digits, so it can't collide with
       * agent_error_is_retryable's "502"/"503"/... substring scan the way a numeric
       * code could; the "at concurrency limit" substring stays intact too. */
      snprintf(out->error, sizeof(out->error),
               "agent '%s' at concurrency limit (max_parallel=%d) [aimee_err=%s]", ag->name,
               ag->max_parallel, aimee_err_slug(AIMEE_ERR_CONCURRENCY_LIMIT));
      return AGENT_RC_AT_LIMIT;
   }
   int rc = use_tools ? agent_execute_with_tools_for_role(ag, net, role, system_prompt, user_prompt,
                                                          max_tokens, temperature, out)
                      : agent_execute(ag, system_prompt, user_prompt, max_tokens, temperature, out);
   provider_catalog_concurrent_release(ag->name);
   if (rc == 0)
      provider_catalog_record_success(ag->name);
   else
   {
      const char *ec = agent_error_is_retryable(out->error) ? "retryable" : "error";
      provider_catalog_record_failure(ag->name, ec);
   }
   return rc;
}

/* Per-thread tool-mode override for agent_run_ex; see agent_run_force_no_tools. */
static __thread int tl_force_no_tools = 0;
void agent_run_force_no_tools(int on)
{
   tl_force_no_tools = on ? 1 : 0;
}

int agent_run_ex(agent_config_t *cfg, const char *role, const char *system_prompt,
                 const char *user_prompt, int max_tokens, double temperature, agent_result_t *out)
{
   memset(out, 0, sizeof(*out));

   int cache_enabled = delegate_role_result_cache_enabled(role);
   if (cache_enabled)
   {
      char *cached = db1_agent_cache_get(role, user_prompt);
      if (cached)
      {
         out->response = cached;
         out->success = 1;
         snprintf(out->agent_name, MAX_AGENT_NAME, "cache");
         return 0;
      }
   }

   /* Route to the best agent for the role (cost-tier), then retry other viable
    * agents at random until one succeeds. This replaces the configured
    * fallback_chain: eligibility + retry-until-viable is the whole mechanism. A
    * DOWN agent is never tried (agent_is_available_for_routing filters it). */
   const char *tried[MAX_AGENTS];
   int ntried = 0;
   for (int attempt = 0; attempt <= MAX_AGENTS; attempt++)
   {
      agent_t *ag;
      if (attempt == 0)
         ag = agent_route(cfg, role); /* preferred (cost-tier) pick first */
      else
      {
         int idx = delegate_pick_for_role(cfg, role, tried, ntried);
         ag = idx >= 0 ? &cfg->agents[idx] : NULL;
      }
      if (!ag)
      {
         if (attempt == 0)
            continue; /* no primary route -> fall through to the random picker */
         break;       /* no viable agent remains */
      }

      /* tl_force_no_tools: the delegate no-tools path (CLI --no-tools ->
       * force_tools=0) sets this so a tools-capable agent on an exec role does
       * not silently re-enable tools here and ignore the caller's decision. */
      int use_tools = tl_force_no_tools ? 0
                                        : (agent_uses_provider_cli(ag) ||
                                           (ag->tools_enabled && agent_is_exec_role(ag, role)));
      agent_apply_runtime_config(ag);
      ag->ablation = cfg->ablation;
      ag->write_capable = use_tools && delegate_role_is_write(role) ? 1 : 0;

      char *hint = agent_uses_mistral_delegate_path(ag)
                       ? NULL
                       : kb_client_agent_hint_consume(role, user_prompt);
      const char *effective_prompt = user_prompt;
      char *enhanced = NULL;
      if (hint && use_tools)
      {
         size_t elen = strlen(user_prompt) + strlen(hint) + 4;
         enhanced = malloc(elen);
         if (enhanced)
         {
            snprintf(enhanced, elen, "%s\n\n%s", user_prompt, hint);
            effective_prompt = enhanced;
         }
      }
      free(hint);

      /* The turn goes through the single guarded executor: it enforces max_parallel
       * and records provider health. Returns 0 (ok), AGENT_RC_AT_LIMIT (agent
       * saturated — no health recorded), or -1 (run failure — health recorded). */
      int rc = agent_dispatch_one(ag, &cfg->network, role, system_prompt, effective_prompt,
                                  max_tokens, temperature, use_tools, out);
      free(enhanced);

      if (rc == 0)
      {
         agent_log_call(out, role);
         agent_store_feedback(out, role, user_prompt);

         agent_outcome_t oc;
         classify_outcome(out, ag->max_turns, &oc);
         record_outcome(out->agent_name, role, &oc);

         if (cache_enabled && out->response)
            db1_agent_cache_put(role, user_prompt, out->response);
         return 0;
      }
      /* At-limit or a real failure: either way skip this agent and try the next
       * (health already recorded by agent_dispatch_one for a real failure). */
      if (ntried < MAX_AGENTS)
         tried[ntried++] = ag->name;
      free(out->response);
      out->response = NULL;
   }

   agent_store_feedback(out, role, user_prompt);

   {
      agent_outcome_t oc;
      classify_outcome(out, 0, &oc);
      record_outcome(out->agent_name, role, &oc);
   }

   if (!out->error[0])
      snprintf(out->error, sizeof(out->error), "no agent available for role '%s'", role);
   return -1;
}

/* Thin wrapper preserving the historical 0.3 sampling temperature, so existing
 * agent_run call sites are byte-unchanged while agent_run_ex carries a real
 * temperature parameter for the parallel fan-out path. */
int agent_run(agent_config_t *cfg, const char *role, const char *system_prompt,
              const char *user_prompt, int max_tokens, agent_result_t *out)
{
   return agent_run_ex(cfg, role, system_prompt, user_prompt, max_tokens, 0.3, out);
}

/* One-shot, TOOL-FREE text generation for UI drafting (e.g. "draft this proposal
 * with a delegate"). Deliberately NOT the agentic delegate path: it selects a
 * single non-CLI (HTTP-provider) agent — preferring `agent_name` if given, else
 * the default, else the first enabled non-CLI agent — clones it, forces
 * write_capable off, and calls the plain-completion agent_execute() directly.
 * No provider-CLI, no exec role, no worktree, no tools: the model can only return
 * text. Returns 0 on success (out->response holds the text), -1 otherwise.
 * Provider-CLI agents are refused as drafters precisely because they are agentic. */
int agent_generate(agent_config_t *cfg, const char *agent_name, const char *system_prompt,
                   const char *user_prompt, int max_tokens, double temperature, agent_result_t *out)
{
   memset(out, 0, sizeof(*out));
   if (!cfg || !user_prompt || !user_prompt[0])
   {
      snprintf(out->error, sizeof(out->error), "empty prompt");
      return -1;
   }
   /* Pick a non-CLI (non-agentic) drafter. */
   agent_t *pick = NULL;
   if (agent_name && agent_name[0])
   {
      agent_t *a = agent_find(cfg, agent_name);
      if (a && a->enabled && !agent_uses_provider_cli(a))
         pick = a;
   }
   if (!pick && cfg->default_agent[0])
   {
      agent_t *a = agent_find(cfg, cfg->default_agent);
      if (a && a->enabled && !agent_uses_provider_cli(a))
         pick = a;
   }
   for (int i = 0; !pick && i < cfg->agent_count; i++)
      if (cfg->agents[i].enabled && !agent_uses_provider_cli(&cfg->agents[i]))
         pick = &cfg->agents[i];
   if (!pick)
   {
      snprintf(out->error, sizeof(out->error), "no non-CLI delegate available for drafting");
      return -1;
   }

   agent_t local = *pick; /* clone before mutating (workers never touch shared cfg);
                           * agent_t holds only fixed-size buffers, so a shallow copy
                           * owns no shared pointers. */
   agent_apply_runtime_config(&local);
   local.write_capable = 0; /* belt-and-suspenders; not the primary safeguard */
   /* THE tool-safety guarantee: dispatch with use_tools=0, so the single executor
    * runs the plain agent_execute() completion path — NO tool loop at all (tool
    * execution lives only in agent_execute_with_tools_for_role, reached ONLY when
    * use_tools=1). So regardless of the agent's tools_enabled flag, a draft can
    * only return text: it cannot run a tool, read/write files, or touch a repo.
    * The non-CLI filter above is secondary. (agent_dispatch_one also enforces the
    * drafter's max_parallel + records its health, like every other turn.) */
   int rc = agent_dispatch_one(&local, &cfg->network, NULL /* role */, system_prompt, user_prompt,
                               max_tokens, temperature, 0 /* use_tools: plain completion */, out);
   /* Stamp the drafter name for the caller even on the early-guard paths that
    * return before the executor sets it (idempotent when it was already set). */
   snprintf(out->agent_name, MAX_AGENT_NAME, "%s", local.name);
   return rc;
}

/* Run one fan-out task on a specifically named configured agent (resolved like
 * aux_router.c), not by role. The selected agent_t is CLONED before any runtime
 * mutation so concurrent parallel workers never write the shared cfg-owned
 * struct (closes the data race that the same-agent fan-out otherwise has). A
 * missing or disabled named agent is a failed participant — it does NOT silently
 * fall back to another agent, because that would collapse a diverse panel back
 * into duplicate participants. Defined here (next to the static execution
 * helpers it needs) but called from agent_parallel.c, so it is not static. */
int agent_run_named(agent_config_t *cfg, const char *name, const char *role,
                    const char *system_prompt, const char *user_prompt, int max_tokens,
                    double temperature, agent_result_t *out)
{
   memset(out, 0, sizeof(*out));

   agent_t *src = agent_find(cfg, name);
   if (!src || !src->enabled)
   {
      snprintf(out->agent_name, MAX_AGENT_NAME, "%s", name ? name : "");
      snprintf(out->error, sizeof(out->error), "named participant '%s' not found or disabled",
               name ? name : "(null)");
      return -1;
   }

   /* Clone before mutating: each worker owns its agent_t copy, so runtime-config
    * / ablation / write_capable writes never race on the shared struct. Rate-limit
    * and provider-health accounting stay on the process-wide provider_catalog_*
    * tables (keyed by agent name), so siblings still see each other's signals. */
   agent_t local = *src;
   agent_apply_runtime_config(&local);
   local.ablation = cfg->ablation;
   local.write_capable = 0; /* ensemble references answer a prompt; no write tools */

   /* Plain completion (no tools) through the single guarded executor: it enforces
    * local.max_parallel and records provider health. An AGENT_RC_AT_LIMIT return
    * (agent saturated) propagates as a non-zero rc; the parallel panel/ensemble
    * caller treats a non-response as "this lens unfilled" and retries a different
    * agent, so a saturated model is spread rather than piled on. */
   int rc = agent_dispatch_one(&local, &cfg->network, role, system_prompt, user_prompt, max_tokens,
                               temperature, 0 /* use_tools */, out);
   {
      agent_outcome_t oc;
      classify_outcome(out, local.max_turns, &oc);
      record_outcome(out->agent_name, role ? role : "ensemble", &oc);
   }
   return rc;
}

int agent_run_named_with_tools(agent_config_t *cfg, const char *name, const char *role,
                               const char *system_prompt, const char *user_prompt, int max_tokens,
                               double temperature, agent_result_t *out)
{
   memset(out, 0, sizeof(*out));

   agent_t *src = agent_find(cfg, name);
   if (!src || !src->enabled)
   {
      snprintf(out->agent_name, MAX_AGENT_NAME, "%s", name ? name : "");
      snprintf(out->error, sizeof(out->error), "named participant '%s' not found or disabled",
               name ? name : "(null)");
      return -1;
   }

   agent_t local = *src; /* clone before mutating — see agent_run_named */
   agent_apply_runtime_config(&local);
   local.ablation = cfg->ablation;
   /* write_capable stays 0: this exists for REVIEWERS, and a reviewer that can edit
    * the code it is judging is not a reviewer — the tree that passed the gate would
    * no longer be the tree that was judged. The role's toolset (review -> the
    * index-only `review_indexed`) is what actually decides which tools appear. */
   local.write_capable = 0;

   int rc = agent_dispatch_one(&local, &cfg->network, role, system_prompt, user_prompt, max_tokens,
                               temperature, 1 /* use_tools */, out);
   {
      agent_outcome_t oc;
      classify_outcome(out, local.max_turns, &oc);
      record_outcome(out->agent_name, role ? role : "ensemble", &oc);
   }
   return rc;
}

static int agent_run_with_tools_internal(agent_config_t *cfg, const char *role,
                                         const char *system_prompt, const char *user_prompt,
                                         int max_tokens, int enforce_writes, agent_result_t *out)
{
   memset(out, 0, sizeof(*out));

   /* Cooperative cancellation (server-owned turn lifecycle): abort before a
    * (potentially long) provider call if the turn was already cancelled
    * (session close / shutdown / graceful_cancel). Finer-grained mid-call
    * interruption of the in-process path is a follow-up; the CLI/subprocess
    * path is interrupted directly via its poll loop. */
   if (agent_request_cancelled())
   {
      snprintf(out->error, sizeof(out->error), "turn cancelled");
      return -1;
   }

   agent_t *ag = agent_route(cfg, role);
   if (!ag)
   {
      snprintf(out->error, sizeof(out->error), "no agent available for role '%s'", role);
      return -1;
   }
   agent_apply_runtime_config(ag);
   ag->ablation = cfg->ablation;
   ag->write_capable = enforce_writes && delegate_role_is_write(role) ? 1 : 0;

   char *hint = (agent_tools_role_current_code_only(role) || agent_uses_mistral_delegate_path(ag))
                    ? NULL
                    : kb_client_agent_hint_consume(role, user_prompt);
   const char *effective_prompt = user_prompt;
   char *enhanced = NULL;
   if (hint)
   {
      size_t elen = strlen(user_prompt) + strlen(hint) + 4;
      enhanced = malloc(elen);
      if (enhanced)
      {
         snprintf(enhanced, elen, "%s\n\n%s", user_prompt, hint);
         effective_prompt = enhanced;
      }
   }
   free(hint);

   /* The delegate turn through the single guarded executor (tools enabled): it
    * enforces ag->max_parallel and records provider health. */
   int rc = agent_dispatch_one(ag, &cfg->network, role, system_prompt, effective_prompt, max_tokens,
                               0.3, 1 /* use_tools */, out);
   free(enhanced);

   if (agent_rc_should_try_another(rc, out->error) && cfg->fallback_count > 0)
   {
      aimee_log(LOG_INFO, "agent",
                "delegate agent '%s' retryable/at-limit, trying fallback chain (%d entries)",
                ag->name, cfg->fallback_count);

      for (int fi = 0; fi < cfg->fallback_count && rc != 0; fi++)
      {
         if (agent_request_cancelled())
            break; /* stop trying fallbacks once the turn is cancelled */
         agent_t *fb = agent_find(cfg, cfg->fallback_chain[fi]);
         if (!fb || !fb->enabled || fb == ag)
            continue;
         if (provider_catalog_get_health(fb->name) == CATALOG_HEALTH_DOWN)
         {
            aimee_log(LOG_DEBUG, "agent", "skipping DOWN agent '%s' in fallback", fb->name);
            continue;
         }
         fb->write_capable = enforce_writes && delegate_role_is_write(role) ? 1 : 0;

         free(out->response);
         out->response = NULL;
         out->error[0] = '\0';

         aimee_log(LOG_INFO, "agent", "fallback: trying agent '%s'", fb->name);
         rc = agent_dispatch_one(fb, &cfg->network, role, system_prompt, user_prompt, max_tokens,
                                 0.3, 1 /* use_tools */, out);
         if (rc == 0)
            ag = fb; /* update ag for post-run logging (health recorded by dispatch_one) */
      }
   }

   rc = agent_try_same_tier_fallback(cfg, &ag, role, system_prompt, user_prompt, max_tokens,
                                     enforce_writes, out, rc);

   /* health is recorded per-turn inside agent_dispatch_one (main + fallbacks +
    * same-tier), so no final record_success here. */
   agent_log_call(out, role);
   agent_store_feedback(out, role, user_prompt);

   {
      agent_outcome_t oc;
      classify_outcome(out, ag->max_turns, &oc);
      record_outcome(out->agent_name, role, &oc);
   }
   if (rc == 0 && delegate_role_result_cache_enabled(role) && out->response)
      db1_agent_cache_put(role, user_prompt, out->response);

   return rc;
}

/* Like agent_run but always uses tool execution, even if the agent config
 * does not have tools_enabled set. Used by the delegate command. */
int agent_run_with_tools(agent_config_t *cfg, const char *role, const char *system_prompt,
                         const char *user_prompt, int max_tokens, agent_result_t *out)
{
   return agent_run_with_tools_internal(cfg, role, system_prompt, user_prompt, max_tokens, 1, out);
}

int agent_run_with_tools_write_enforce(agent_config_t *cfg, const char *role,
                                       const char *system_prompt, const char *user_prompt,
                                       int max_tokens, int enforce_writes, agent_result_t *out)
{
   return agent_run_with_tools_internal(cfg, role, system_prompt, user_prompt, max_tokens,
                                        enforce_writes, out);
}

/* The parallel fan-out machinery (parallel_worker, agent_run_parallel) lives in
 * agent_parallel.c — extracted to keep this file under the 2000-line cap. It
 * calls the now-non-static agent_run_named / agent_run_ex above. */

void agent_store_feedback(const agent_result_t *result, const char *role,
                          const char *prompt_summary)
{
   if (result->success && (!result->response || !result->response[0]))
      return;

   const char *r = (role && role[0]) ? role : "unknown";

   /* Store outcome as L1 episode memory for future context */
   char key[256];
   snprintf(key, sizeof(key), "delegate_%s_%s_%s", result->agent_name, r,
            result->success ? "ok" : "fail");

   char excerpt[256] = "";
   if (prompt_summary && prompt_summary[0])
   {
      snprintf(excerpt, sizeof(excerpt), "%.200s", prompt_summary);
      /* Truncate at last space to avoid mid-word cuts */
      if (strlen(prompt_summary) > 200)
      {
         char *last_sp = strrchr(excerpt, ' ');
         if (last_sp && last_sp > excerpt + 100)
            *last_sp = '\0';
      }
   }

   char content[2048];
   if (result->success)
   {
      snprintf(content, sizeof(content),
               "Delegation [%s] via %s succeeded. %d turns, %d tool calls, %dms. "
               "Confidence: %d. Task: %s",
               r, result->agent_name, result->turns, result->tool_calls, result->latency_ms,
               result->confidence, excerpt);
   }
   else
   {
      snprintf(content, sizeof(content),
               "Delegation [%s] via %s failed. %d turns, %d tool calls, %dms. "
               "Error: %.512s. Task: %s",
               r, result->agent_name, result->turns, result->tool_calls, result->latency_ms,
               result->error, excerpt);
   }

   memory_t mem;
   if (kb_client_memory_insert(TIER_L1, KIND_EPISODE, key, content, result->success ? 0.8 : 0.4,
                               "agent_feedback", &mem) == 0)
   {
      /* Causal Linking: if successful, link this episode to the original task */
      if (result->success)
      {
         /* Look for the L2 task memory matching this role/prompt */
         char task_key[256];
         snprintf(task_key, sizeof(task_key), "task:%s", excerpt);
         char norm_task_key[512];
         normalize_key(task_key, norm_task_key, sizeof(norm_task_key));

         int64_t task_id = kb_client_memory_find_id_by_key_kind(norm_task_key, "task");
         if (task_id > 0)
            kb_client_memory_link_create(mem.id, task_id, "fixes");
      }
   }
}

/* ================================================================
 * From: agent_context.c
 * ================================================================ */
#include "workspace.h"
#include "tasks.h"
#include "token_tracker.h"
#include "conversation_context.h"
#include <time.h>

/* --- Request/response (simple, non-tool path) --- */

static int is_chatgpt_provider(const agent_t *agent)
{
   return strcmp(agent->provider, "chatgpt") == 0;
}

static int is_anthropic_provider(const agent_t *agent)
{
   return strcmp(agent->provider, "anthropic") == 0;
}

static cJSON *build_request_openai(const agent_t *agent, const char *system_prompt,
                                   const char *user_prompt, int max_tokens, double temperature)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "model", agent->model);

   cJSON *messages = cJSON_CreateArray();
   if (system_prompt && system_prompt[0])
   {
      cJSON *sys = cJSON_CreateObject();
      cJSON_AddStringToObject(sys, "role", "system");
      cJSON_AddStringToObject(sys, "content", system_prompt);
      cJSON_AddItemToArray(messages, sys);
   }
   cJSON *user = cJSON_CreateObject();
   cJSON_AddStringToObject(user, "role", "user");
   char *shaped_prompt = agent_request_shape_user_prompt(agent, user_prompt);
   cJSON_AddStringToObject(user, "content", shaped_prompt ? shaped_prompt : user_prompt);
   free(shaped_prompt);
   cJSON_AddItemToArray(messages, user);

   cJSON_AddItemToObject(req, "messages", messages);

   cJSON_AddNumberToObject(req, "max_tokens", agent_request_max_tokens(agent, max_tokens));
   model_sampling_apply_openai(agent, req, temperature);

   return req;
}

static cJSON *build_request_responses(const agent_t *agent, const char *system_prompt,
                                      const char *user_prompt, int max_tokens, double temperature)
{
   (void)max_tokens;
   (void)temperature;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "model", agent->model);

   /* ChatGPT Codex backend requires instructions, store=false, stream=true */
   if (system_prompt && system_prompt[0])
      cJSON_AddStringToObject(req, "instructions", system_prompt);
   else
      cJSON_AddStringToObject(req, "instructions", "You are a helpful assistant.");

   cJSON_AddBoolToObject(req, "store", 0);
   cJSON_AddBoolToObject(req, "stream", 1);

   /* Responses API uses "input" as an array of messages */
   cJSON *input = cJSON_CreateArray();
   cJSON *user = cJSON_CreateObject();
   cJSON_AddStringToObject(user, "role", "user");
   cJSON_AddStringToObject(user, "content", user_prompt);
   cJSON_AddItemToArray(input, user);

   cJSON_AddItemToObject(req, "input", input);

   return req;
}

static cJSON *build_request_anthropic(const agent_t *agent, const char *system_prompt,
                                      const char *user_prompt, int max_tokens, double temperature)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "model", agent->model);

   cJSON_AddNumberToObject(req, "max_tokens", agent_request_max_tokens(agent, max_tokens));

   /* §3 cache-aware shaping: mark the stable system prefix cacheable (cache_control)
    * only when the flag is on, matching the tool-bearing builder. Default-off so
    * this request-mutating optimization lands dark behind the flag (config load is
    * cheap/mtime-cached). The min-size floor is applied to the stable prefix inside
    * the helper, not the whole prompt. */
   config_t cs_cfg;
   int cs_marking = (config_load(&cs_cfg) == 0 && cs_cfg.cache_shaping_enabled) ? 1 : 0;
   agent_anthropic_set_system(req, system_prompt, cs_marking,
                              cs_marking ? cs_cfg.cache_min_chars : 0);

   cJSON *messages = cJSON_CreateArray();
   cJSON *user = cJSON_CreateObject();
   cJSON_AddStringToObject(user, "role", "user");
   cJSON_AddStringToObject(user, "content", user_prompt);
   cJSON_AddItemToArray(messages, user);
   cJSON_AddItemToObject(req, "messages", messages);

   model_sampling_apply_anthropic(agent, req, temperature);

   return req;
}

static cJSON *build_request(const agent_t *agent, const char *system_prompt,
                            const char *user_prompt, int max_tokens, double temperature)
{
   if (is_chatgpt_provider(agent))
      return build_request_responses(agent, system_prompt, user_prompt, max_tokens, temperature);
   if (is_anthropic_provider(agent))
      return build_request_anthropic(agent, system_prompt, user_prompt, max_tokens, temperature);
   return build_request_openai(agent, system_prompt, user_prompt, max_tokens, temperature);
}

static int text_prompt_token_estimate(const char *system_prompt, const char *user_prompt)
{
   size_t len = 0;
   if (system_prompt)
      len += strlen(system_prompt);
   if (user_prompt)
      len += strlen(user_prompt);
   return (int)(len / 4) + 1;
}

static void track_simple_anthropic_payload_rewrite(const delegate_driver_t *driver,
                                                   const agent_t *agent, const char *system_prompt,
                                                   const char *user_prompt)
{
   driver_caps_t caps;
   delegate_get_caps(driver, agent, &caps);
   payload_rewrite_track_request(system_prompt, NULL,
                                 text_prompt_token_estimate(system_prompt, user_prompt),
                                 caps.context_limit);
}

static int parse_error(cJSON *root, agent_result_t *out)
{
   cJSON *err = cJSON_GetObjectItem(root, "error");
   if (!err || cJSON_IsNull(err))
      return 0;

   if (cJSON_IsObject(err))
   {
      cJSON *msg = cJSON_GetObjectItem(err, "message");
      if (msg && cJSON_IsString(msg))
         snprintf(out->error, sizeof(out->error), "%s", msg->valuestring);
      else
         snprintf(out->error, sizeof(out->error), "API error");
   }
   else if (cJSON_IsString(err))
   {
      snprintf(out->error, sizeof(out->error), "%s", err->valuestring);
   }
   else
   {
      snprintf(out->error, sizeof(out->error), "API error");
   }
   return 1;
}

static void parse_response_openai(cJSON *root, agent_result_t *out)
{
   parsed_response_t parsed;
   /* IR is the sole parser for the openai wire (zeroed *parsed on failure). This
    * simple-completion runtime has no tool loop, so no XML rescue (rescue_mode < 0). */
   agent_ir_parse_json_response(root, 0 /*openai*/, -1, NULL, &parsed);

   out->prompt_tokens = parsed.prompt_tokens;
   out->completion_tokens = parsed.completion_tokens;
   out->cache_write_tokens = parsed.cache_write_tokens;
   out->cache_read_tokens = parsed.cache_read_tokens;
   /* Prefer the provider-reported model over the served alias set at entry. */
   if (parsed.model[0])
      snprintf(out->model, MAX_MODEL_LEN, "%s", parsed.model);
   if (parsed.stop_reason[0])
      snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", parsed.stop_reason);

   if (parsed.content && parsed.content[0])
   {
      out->response = parsed.content;
      parsed.content = NULL;
      out->success = 1;
   }

   if (out->cache_read_tokens > 0)
      aimee_log(LOG_DEBUG, "agent_context", "prompt cache hit: %d tokens read from cache",
                out->cache_read_tokens);
   else if (out->cache_write_tokens > 0)
      aimee_log(LOG_DEBUG, "agent_context", "prompt cache miss: %d tokens written to cache",
                out->cache_write_tokens);

   agent_free_parsed_response(&parsed);
}

static void parse_response_object(cJSON *root, agent_result_t *out)
{
   /* Responses API: output[].content[].text where type == "output_text" */
   cJSON *output = cJSON_GetObjectItem(root, "output");
   if (output && cJSON_IsArray(output))
   {
      int n = cJSON_GetArraySize(output);
      for (int i = 0; i < n; i++)
      {
         cJSON *item = cJSON_GetArrayItem(output, i);
         cJSON *type = cJSON_GetObjectItem(item, "type");
         if (!type || !cJSON_IsString(type) || strcmp(type->valuestring, "message") != 0)
            continue;

         cJSON *content = cJSON_GetObjectItem(item, "content");
         if (!content || !cJSON_IsArray(content))
            continue;

         int cn = cJSON_GetArraySize(content);
         for (int j = 0; j < cn; j++)
         {
            cJSON *part = cJSON_GetArrayItem(content, j);
            cJSON *pt = cJSON_GetObjectItem(part, "type");
            if (!pt || !cJSON_IsString(pt) || strcmp(pt->valuestring, "output_text") != 0)
               continue;

            cJSON *text = cJSON_GetObjectItem(part, "text");
            if (text && cJSON_IsString(text))
            {
               out->response = strdup(text->valuestring);
               out->success = 1;
               break;
            }
         }
         if (out->success)
            break;
      }
   }

   /* Responses API usage: input_tokens, output_tokens */
   cJSON *usage = cJSON_GetObjectItem(root, "usage");
   if (usage)
   {
      cJSON *it = cJSON_GetObjectItem(usage, "input_tokens");
      cJSON *ot = cJSON_GetObjectItem(usage, "output_tokens");
      if (it && cJSON_IsNumber(it))
         out->prompt_tokens = it->valueint;
      if (ot && cJSON_IsNumber(ot))
         out->completion_tokens = ot->valueint;
   }
}

/* Parse SSE stream body for the Responses API.
 *
 * Strategy:
 * 1. Scan response.output_item.done events — these carry the completed output items
 *    with full content arrays (item.content[].text where type == "output_text").
 * 2. Fall back to response.completed if output_item.done yields nothing (the
 *    completed event includes a full response object but the API sometimes sends
 *    it with an empty output[] array, so this is a secondary check).
 * 3. If neither has content, try parsing as plain JSON (non-SSE fallback). */
static void parse_response_responses(const char *body, agent_result_t *out)
{
   if (!body)
      return;

   /* Pass 1: collect text from all response.output_item.done events */
   const char *p = body;
   while ((p = strstr(p, "event: response.output_item.done\n")) != NULL)
   {
      const char *data = strstr(p, "data: ");
      if (!data)
         break;
      data += 6;
      cJSON *ev = cJSON_Parse(data);
      if (ev)
      {
         cJSON *item = cJSON_GetObjectItem(ev, "item");
         if (item)
         {
            cJSON *content = cJSON_GetObjectItem(item, "content");
            if (content && cJSON_IsArray(content))
            {
               int cn = cJSON_GetArraySize(content);
               for (int j = 0; j < cn; j++)
               {
                  cJSON *part = cJSON_GetArrayItem(content, j);
                  cJSON *pt = cJSON_GetObjectItem(part, "type");
                  if (!pt || !cJSON_IsString(pt) || strcmp(pt->valuestring, "output_text") != 0)
                     continue;
                  cJSON *text = cJSON_GetObjectItem(part, "text");
                  if (text && cJSON_IsString(text) && text->valuestring[0])
                  {
                     free(out->response);
                     out->response = strdup(text->valuestring);
                     out->success = 1;
                  }
               }
            }
         }
         cJSON_Delete(ev);
      }
      p++; /* advance past current match */
   }

   if (out->success)
   {
      /* Collect usage from response.completed even if we already have content */
      const char *completed = strstr(body, "event: response.completed\n");
      if (completed)
      {
         const char *dl = strstr(completed, "data: ");
         if (dl)
         {
            cJSON *ev = cJSON_Parse(dl + 6);
            if (ev)
            {
               cJSON *resp = cJSON_GetObjectItem(ev, "response");
               if (resp)
               {
                  cJSON *usage = cJSON_GetObjectItem(resp, "usage");
                  if (usage)
                  {
                     cJSON *it = cJSON_GetObjectItem(usage, "input_tokens");
                     cJSON *ot = cJSON_GetObjectItem(usage, "output_tokens");
                     if (it && cJSON_IsNumber(it))
                        out->prompt_tokens = it->valueint;
                     if (ot && cJSON_IsNumber(ot))
                        out->completion_tokens = ot->valueint;
                  }
               }
               cJSON_Delete(ev);
            }
         }
      }
      return;
   }

   /* Pass 2: try response.completed (output[] may be populated in some versions) */
   const char *completed = strstr(body, "event: response.completed\n");
   if (completed)
   {
      const char *dl = strstr(completed, "data: ");
      if (dl)
      {
         cJSON *ev = cJSON_Parse(dl + 6);
         if (ev)
         {
            cJSON *resp = cJSON_GetObjectItem(ev, "response");
            if (resp)
            {
               if (!parse_error(resp, out))
                  parse_response_object(resp, out);
            }
            cJSON_Delete(ev);
         }
      }
      if (out->success)
         return;
   }

   /* Pass 3: plain JSON fallback (non-SSE response) */
   cJSON *root = cJSON_Parse(body);
   if (root)
   {
      if (!parse_error(root, out))
         parse_response_object(root, out);
      cJSON_Delete(root);
   }
   else if (!out->error[0])
   {
      snprintf(out->error, sizeof(out->error), "no content in response stream");
   }
}

static void parse_response_anthropic(cJSON *root, agent_result_t *out)
{
   /* Anthropic: {"content":[{"type":"text","text":"..."}],"usage":{...}} */
   cJSON *content = cJSON_GetObjectItem(root, "content");
   if (content && cJSON_IsArray(content))
   {
      int n = cJSON_GetArraySize(content);
      for (int i = 0; i < n; i++)
      {
         cJSON *block = cJSON_GetArrayItem(content, i);
         cJSON *type = cJSON_GetObjectItem(block, "type");
         if (type && cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0)
         {
            cJSON *text = cJSON_GetObjectItem(block, "text");
            if (text && cJSON_IsString(text))
            {
               out->response = strdup(text->valuestring);
               out->success = 1;
               break;
            }
         }
      }
   }

   cJSON *usage = cJSON_GetObjectItem(root, "usage");
   if (usage)
   {
      cJSON *it = cJSON_GetObjectItem(usage, "input_tokens");
      cJSON *ot = cJSON_GetObjectItem(usage, "output_tokens");
      if (it && cJSON_IsNumber(it))
         out->prompt_tokens = it->valueint;
      if (ot && cJSON_IsNumber(ot))
         out->completion_tokens = ot->valueint;
   }

   /* Prefer the provider-reported model over the served alias set at entry. */
   cJSON *mdl = cJSON_GetObjectItem(root, "model");
   if (mdl && cJSON_IsString(mdl) && mdl->valuestring)
      snprintf(out->model, MAX_MODEL_LEN, "%s", mdl->valuestring);
   cJSON *sr = cJSON_GetObjectItem(root, "stop_reason");
   if (sr && cJSON_IsString(sr) && sr->valuestring)
      snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", sr->valuestring);
}

static void parse_response(const char *body, const agent_t *agent, agent_result_t *out)
{
   if (is_chatgpt_provider(agent))
   {
      /* SSE stream or plain JSON, handled internally */
      parse_response_responses(body, out);
      return;
   }

   cJSON *root = cJSON_Parse(body);
   if (!root)
   {
      snprintf(out->error, sizeof(out->error), "invalid JSON response");
      return;
   }

   if (parse_error(root, out))
   {
      cJSON_Delete(root);
      return;
   }

   if (is_anthropic_provider(agent))
      parse_response_anthropic(root, out);
   else
      parse_response_openai(root, out);
   cJSON_Delete(root);
}

/* --- Simple (non-tool) execution --- */

int agent_execute(const agent_t *agent, const char *system_prompt, const char *user_prompt,
                  int max_tokens, double temperature, agent_result_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->agent_name, MAX_AGENT_NAME, "%s", agent->name);
   snprintf(out->model, MAX_MODEL_LEN, "%s", agent->model);
   snprintf(out->served_model, MAX_MODEL_LEN, "%s", agent->model);

   if (!user_prompt || !user_prompt[0])
   {
      snprintf(out->error, sizeof(out->error), "empty prompt");
      return -1;
   }
   char stop_reason[128];
   if (active_delegation_stopped(stop_reason, sizeof(stop_reason)))
   {
      snprintf(out->error, sizeof(out->error), "%s before request", stop_reason);
      return -1;
   }

   /* Build URL via the provider driver so host-root and trailing-slash
    * OpenAI-compatible endpoints normalize the same way everywhere. */
   char url[MAX_ENDPOINT_LEN + 64];
   delegate_drivers_init();
   const delegate_driver_t *driver = delegate_driver_get(agent->provider);
   if (delegate_build_url(driver, agent, url, sizeof(url)) != 0)
   {
      snprintf(out->error, sizeof(out->error), "failed to build request URL");
      return -1;
   }

   /* Resolve auth */
   char auth_header[MAX_API_KEY_LEN + 32];
   if (agent_resolve_auth(agent, auth_header, sizeof(auth_header)) != 0)
   {
      /* Prefer an explicit, actionable reason (e.g. codex REAUTH_REQUIRED) over the
       * generic message so the operator knows the remedy (D6). */
      const char *why = agent_request_auth_error();
      snprintf(out->error, sizeof(out->error), "%s", why ? why : "auth resolution failed");
      return -1;
   }
   char extra_headers[512];
   agent_build_extra_headers(agent, extra_headers, sizeof(extra_headers));
   /* Run PRE_LLM_CALL hooks — appends ephemeral context to user msg, never system_prompt. */
   char *augmented_prompt = plugin_chook_apply_pre_llm(user_prompt);
   const char *effective_user = augmented_prompt ? augmented_prompt : user_prompt;
   /* Build request body — derive the output cap from the model when unpinned. */
   int tok = agent_request_max_tokens(agent, max_tokens);
   if (is_anthropic_provider(agent))
      track_simple_anthropic_payload_rewrite(driver, agent, system_prompt, effective_user);
   cJSON *req = build_request(agent, system_prompt, effective_user, tok, temperature);
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   free(augmented_prompt);
   if (!body)
   {
      snprintf(out->error, sizeof(out->error), "failed to build request");
      return -1;
   }

   /* Measure latency */
   struct timespec start, end;
   clock_gettime(CLOCK_MONOTONIC, &start);

   /* HTTP POST with retry */
   char *response_body = NULL;
   config_t retry_cfg;
   config_load(&retry_cfg);
   int ra =
       retry_cfg.retry_max_attempts > 0 ? retry_cfg.retry_max_attempts : HTTP_RETRY_MAX_ATTEMPTS;
   int rb = retry_cfg.retry_base_ms > 0 ? retry_cfg.retry_base_ms : HTTP_RETRY_BASE_MS;
   int rm = retry_cfg.retry_max_ms > 0 ? retry_cfg.retry_max_ms : HTTP_RETRY_MAX_MS;
   int http_status = http_retry_post_context(url, auth_header, body, &response_body,
                                             agent->timeout_ms, extra_headers, ra, rb, rm,
                                             agent->provider, agent->model, session_id());
   free(body);
   if (active_delegation_stopped(stop_reason, sizeof(stop_reason)))
   {
      snprintf(out->error, sizeof(out->error), "%s after request", stop_reason);
      free(response_body);
      return -1;
   }

   /* Model fallback: if primary model fails with 400, retry with fallback_model */
   if (http_status == 400 && agent->fallback_model[0])
   {
      free(response_body);
      response_body = NULL;

      /* Rebuild request with fallback model */
      agent_t fb_agent;
      memcpy(&fb_agent, agent, sizeof(fb_agent));
      snprintf(fb_agent.model, MAX_MODEL_LEN, "%s", agent->fallback_model);
      fb_agent.fallback_model[0] = '\0';
      /* The fallback model is now the served model — record it for accounting. */
      snprintf(out->model, MAX_MODEL_LEN, "%s", fb_agent.model);
      snprintf(out->served_model, MAX_MODEL_LEN, "%s", fb_agent.model);

      if (is_anthropic_provider(&fb_agent))
         track_simple_anthropic_payload_rewrite(driver, &fb_agent, system_prompt, user_prompt);
      cJSON *fb_req = build_request(&fb_agent, system_prompt, user_prompt, tok, temperature);
      char *fb_body = cJSON_PrintUnformatted(fb_req);
      cJSON_Delete(fb_req);
      if (fb_body)
      {
         http_status = http_retry_post_context(url, auth_header, fb_body, &response_body,
                                               agent->timeout_ms, extra_headers, ra, rb, rm,
                                               fb_agent.provider, fb_agent.model, session_id());
         free(fb_body);
      }
   }
   if (active_delegation_stopped(stop_reason, sizeof(stop_reason)))
   {
      snprintf(out->error, sizeof(out->error), "%s after request", stop_reason);
      free(response_body);
      return -1;
   }

   clock_gettime(CLOCK_MONOTONIC, &end);
   out->latency_ms =
       (int)((end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000);
   aimee_log(LOG_INFO, "agent_runtime", "model call end: http=%d ms=%d provider=%s model=%s",
             http_status, out->latency_ms, agent->provider, agent->model);
   if (http_status < 0 || !response_body)
   {
      snprintf(out->error, sizeof(out->error),
               "HTTP request failed (status %d: connection failed or local HTTP client "
               "unavailable)",
               http_status);
      free(response_body);
      return -1;
   }

   if (http_status != 200)
   {
      char snippet[256] = {0};
      if (response_body)
         snprintf(snippet, sizeof(snippet), "%.200s", response_body);
      /* Try to extract a STRUCTURED provider error ({"error":{"message":...}}).
       * If the body is not JSON, parse_response() sets the generic "invalid JSON
       * response" — which previously CLOBBERED the informative HTTP status here,
       * making every upstream 4xx/5xx (rate limits, gateway errors, HTML error
       * pages) undiagnosable in the field. Keep the HTTP status + body snippet
       * unless a real structured message was extracted. */
      out->error[0] = '\0';
      parse_response(response_body, agent, out);
      if (!out->error[0] || strcmp(out->error, "invalid JSON response") == 0)
         snprintf(out->error, sizeof(out->error), "HTTP %d (non-JSON body): %s", http_status,
                  snippet);
      free(response_body);
      return -1;
   }

   parse_response(response_body, agent, out);
   /* A 200 whose body is not JSON (e.g. an SSE stream leaking through, or a
    * truncated reply) is also masked by the bare "invalid JSON response"; make it
    * say so and show a snippet, so the cause is visible. */
   if (out->error[0] && strcmp(out->error, "invalid JSON response") == 0)
      snprintf(out->error, sizeof(out->error), "provider returned 200 with a non-JSON body: %.180s",
               response_body ? response_body : "");
   free(response_body);

   agent_reject_degenerate_plain_response(out, agent->name);

   if (!out->success && !out->error[0])
      snprintf(out->error, sizeof(out->error), "no content in response");

   /* Note: callers (agent_run, agent_run_with_tools) handle logging
    * with the correct role. Do not log here to avoid double-logging. */

   return out->success ? 0 : -1;
}

/* --- Task type classification --- */

typedef struct
{
   const char *word;
   task_type_t type;
} task_keyword_t;

static const task_keyword_t task_keywords[] = {
    {"fix", TASK_TYPE_BUG_FIX},         {"bug", TASK_TYPE_BUG_FIX},
    {"error", TASK_TYPE_BUG_FIX},       {"crash", TASK_TYPE_BUG_FIX},
    {"fail", TASK_TYPE_BUG_FIX},        {"broken", TASK_TYPE_BUG_FIX},
    {"regression", TASK_TYPE_BUG_FIX},  {"refactor", TASK_TYPE_REFACTOR},
    {"rename", TASK_TYPE_REFACTOR},     {"extract", TASK_TYPE_REFACTOR},
    {"reorganize", TASK_TYPE_REFACTOR}, {"clean", TASK_TYPE_REFACTOR},
    {"add", TASK_TYPE_FEATURE},         {"implement", TASK_TYPE_FEATURE},
    {"create", TASK_TYPE_FEATURE},      {"new", TASK_TYPE_FEATURE},
    {"build", TASK_TYPE_FEATURE},       {"support", TASK_TYPE_FEATURE},
    {"review", TASK_TYPE_REVIEW},       {"check", TASK_TYPE_REVIEW},
    {"audit", TASK_TYPE_REVIEW},        {"verify", TASK_TYPE_REVIEW},
    {"validate", TASK_TYPE_REVIEW},     {"test", TASK_TYPE_TEST},
    {"coverage", TASK_TYPE_TEST},       {"assert", TASK_TYPE_TEST},
    {"spec", TASK_TYPE_TEST},           {NULL, TASK_TYPE_GENERAL}};

task_type_t task_type_classify(const char *prompt)
{
   if (!prompt || !prompt[0])
      return TASK_TYPE_GENERAL;

   /* Lowercase copy for matching */
   char lower[512];
   size_t len = strlen(prompt);
   if (len >= sizeof(lower))
      len = sizeof(lower) - 1;
   for (size_t i = 0; i < len; i++)
      lower[i] = (char)((prompt[i] >= 'A' && prompt[i] <= 'Z') ? prompt[i] + 32 : prompt[i]);
   lower[len] = '\0';

   /* Scan for keyword matches (first match wins) */
   for (int i = 0; task_keywords[i].word; i++)
   {
      const char *kw = task_keywords[i].word;
      size_t kwlen = strlen(kw);
      const char *p = lower;
      while ((p = strstr(p, kw)) != NULL)
      {
         /* Check word boundary: must be at start or after non-alpha */
         int at_start = (p == lower) || (*(p - 1) == ' ' || *(p - 1) == '\t' || *(p - 1) == '-' ||
                                         *(p - 1) == '_' || *(p - 1) == '/');
         /* Check end boundary: must be at end or before non-alpha */
         char after = p[kwlen];
         int at_end = (after == '\0' || after == ' ' || after == '\t' || after == '-' ||
                       after == '_' || after == '/' || after == '.' || after == ',' ||
                       after == 'e' || after == 'i' || after == 's');
         if (at_start && at_end)
            return task_keywords[i].type;
         p += kwlen;
      }
   }

   return TASK_TYPE_GENERAL;
}

const char *task_type_name(task_type_t type)
{
   switch (type)
   {
   case TASK_TYPE_BUG_FIX:
      return "bug_fix";
   case TASK_TYPE_REFACTOR:
      return "refactor";
   case TASK_TYPE_FEATURE:
      return "feature";
   case TASK_TYPE_REVIEW:
      return "review";
   case TASK_TYPE_TEST:
      return "test";
   default:
      return "general";
   }
}

/* Context category indices */
enum
{
   CTX_CAT_ERRORS = 0, /* Recent errors/episodes */
   CTX_CAT_ARCH,       /* Architecture/structure (memory context) */
   CTX_CAT_CODE,       /* Related code symbols */
   CTX_CAT_PROCEDURES, /* Procedures/how-to (rules) */
   CTX_CAT_RECENT,     /* Recent changes/failures */
   CTX_CAT_COUNT
};

/* Weight table: [task_type][category] */
static const int ctx_weights[TASK_TYPE_COUNT][CTX_CAT_COUNT] = {
    /* GENERAL */ {CTX_WEIGHT_MED, CTX_WEIGHT_MED, CTX_WEIGHT_MED, CTX_WEIGHT_MED, CTX_WEIGHT_MED},
    /* BUG_FIX */
    {CTX_WEIGHT_HIGH, CTX_WEIGHT_LOW, CTX_WEIGHT_HIGH, CTX_WEIGHT_MED, CTX_WEIGHT_HIGH},
    /* REFACTOR */
    {CTX_WEIGHT_LOW, CTX_WEIGHT_HIGH, CTX_WEIGHT_HIGH, CTX_WEIGHT_LOW, CTX_WEIGHT_MED},
    /* FEATURE */ {CTX_WEIGHT_LOW, CTX_WEIGHT_HIGH, CTX_WEIGHT_MED, CTX_WEIGHT_MED, CTX_WEIGHT_LOW},
    /* REVIEW */
    {CTX_WEIGHT_MED, CTX_WEIGHT_HIGH, CTX_WEIGHT_HIGH, CTX_WEIGHT_LOW, CTX_WEIGHT_HIGH},
    /* TEST */ {CTX_WEIGHT_MED, CTX_WEIGHT_LOW, CTX_WEIGHT_HIGH, CTX_WEIGHT_HIGH, CTX_WEIGHT_MED},
};

/* Compute budget allocation for a category given the task type.
 * Returns max chars for that category from the total content budget. */
static size_t ctx_category_budget(task_type_t type, int category, size_t total_budget)
{
   int weight = ctx_weights[type][category];
   int total_weight = 0;
   for (int i = 0; i < CTX_CAT_COUNT; i++)
      total_weight += ctx_weights[type][i];
   if (total_weight == 0)
      return total_budget / CTX_CAT_COUNT;
   return (total_budget * (size_t)weight) / (size_t)total_weight;
}

static void ctx_appendf(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
   if (!buf || cap == 0 || !pos || *pos >= cap - 1)
      return;

   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
   va_end(ap);

   if (n < 0)
      return;
   if ((size_t)n >= cap - *pos)
   {
      *pos = cap - 1;
      buf[*pos] = '\0';
      return;
   }
   *pos += (size_t)n;
}

static void ctx_append_bytes(char *buf, size_t cap, size_t *pos, const char *src, size_t len)
{
   if (!buf || cap == 0 || !pos || !src || *pos >= cap - 1)
      return;

   size_t avail = cap - 1 - *pos;
   if (len > avail)
      len = avail;
   if (len == 0)
      return;

   memcpy(buf + *pos, src, len);
   *pos += len;
   buf[*pos] = '\0';
}

static const char *agent_context_cwd(char *buf, size_t buf_len)
{
   const char *thread_cwd = run_cmd_get_cwd();
   if (thread_cwd && thread_cwd[0])
      return thread_cwd;
   if (buf && buf_len > 0 && getcwd(buf, buf_len))
      return buf;
   return NULL;
}

/* --- Relevance-scored context (#3) --- */

char *agent_build_exec_context_ex(const agent_t *agent, const agent_network_t *network,
                                  const char *custom_prompt, int skip_kb_context)
{
   /* Classify the task type from the prompt */
   task_type_t task_type = task_type_classify(custom_prompt);
   if (task_type != TASK_TYPE_GENERAL)
      aimee_log(LOG_DEBUG, "agent_context", "context assembly: task_type=%s",
                task_type_name(task_type));

   /* Default execution instructions */
   static const char *default_exec_instructions =
       "# Instructions\n"
       "- Use the bash tool to run commands, including SSH to remote hosts.\n"
       "- Use find_symbol, code_search, or aimee index commands for code discovery before broad "
       "shell search.\n"
       "- Aimee index/search are authoritative for discovery; current source is authoritative for "
       "file contents when they differ.\n"
       "- File ops: read_file; edit_file (old_string/new_string); write_file (overwrite).\n"
       "- Use list_files to explore directories.\n"
       "- When you have completed the task, respond with a final summary.\n"
       "- If you encounter an error, try to diagnose and fix it.\n"
       "- Do not ask for confirmation. Execute the task directly.\n";
   size_t content_budget = agent_exec_context_budget_chars(agent);
   size_t cap = content_budget + 4096; /* extra room for headers */
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t pos = 0;

   /* Compute per-category budgets from the content budget.
    * Fixed overhead (instructions, network, guardrails, etc.) sits outside these limits. */
   size_t budget_errors = ctx_category_budget(task_type, CTX_CAT_ERRORS, content_budget);
   size_t budget_arch = ctx_category_budget(task_type, CTX_CAT_ARCH, content_budget);
   size_t budget_code = ctx_category_budget(task_type, CTX_CAT_CODE, content_budget);
   size_t budget_procedures = ctx_category_budget(task_type, CTX_CAT_PROCEDURES, content_budget);
   size_t budget_recent = ctx_category_budget(task_type, CTX_CAT_RECENT, content_budget);
   const char *skip_kb_env = getenv("AIMEE_CONTEXT_NO_KB");
   int skip_kb_client =
       skip_kb_context || (skip_kb_env && skip_kb_env[0] && strcmp(skip_kb_env, "0") != 0);

   ctx_appendf(buf, cap, &pos,
               "You are an execution agent. Complete the task using the provided tools.\n"
               "IMPORTANT: Always invoke tools (bash, read_file, write_file, "
               "list_files) to act. Never write shell commands or code as plain "
               "text — call the tool instead.\n"
               "Use Aimee index/search tools for discovery. Treat current source as the "
               "file-content authority when it differs from indexed snippets.\n");
   ctx_appendf(buf, cap, &pos, "%s", prompt_principles_text(config_current_mode()));

   char cwd_buf[MAX_PATH_LEN];
   const char *cwd = agent_context_cwd(cwd_buf, sizeof(cwd_buf));
   if (cwd && cwd[0])
   {
      char root[MAX_PATH_LEN] = "";
      config_t ws_cfg;
      if (config_load(&ws_cfg) != 0 || workspace_active_root(&ws_cfg, cwd, root, sizeof(root)) != 0)
         snprintf(root, sizeof(root), "%s", cwd);
      ctx_appendf(buf, cap, &pos,
                  "Workspace root: %s\n"
                  "Use paths relative to this workspace, or absolute paths under this exact "
                  "workspace root. Do not inspect sibling checkouts or parent repository paths "
                  "unless the user explicitly asks for them.\n",
                  root);
   }
   ctx_appendf(buf, cap, &pos, "\n");

   /* Custom prompt override */
   if (custom_prompt && custom_prompt[0])
   {
      ctx_appendf(buf, cap, &pos, "%s\n\n", custom_prompt);
   }

   /* Rules (budget: procedures) — DB2 lives in aimee-kb. */

   char *rules = NULL;
   if (!skip_kb_client)
   {
      char *rules_envelope = kb_client_rules_generate_json();
      cJSON *rules_resp = rules_envelope ? cJSON_Parse(rules_envelope) : NULL;
      free(rules_envelope);
      cJSON *rules_content =
          rules_resp ? cJSON_GetObjectItemCaseSensitive(rules_resp, "content") : NULL;
      const char *rules_str = cJSON_IsString(rules_content) ? rules_content->valuestring : "";
      rules = strdup(rules_str);
      cJSON_Delete(rules_resp);
   }
   if (rules && rules[0] && strcmp(rules, "No rules configured.") != 0)
   {
      size_t rules_start = pos;
      ctx_appendf(buf, cap, &pos, "# Rules\n");
      /* Truncate rules content to budget */
      size_t rules_len = strlen(rules);
      size_t rules_avail =
          budget_procedures > (pos - rules_start) ? budget_procedures - (pos - rules_start) : 0;
      if (rules_len > rules_avail)
         rules_len = rules_avail;
      if (rules_len > 0)
         ctx_append_bytes(buf, cap, &pos, rules, rules_len);
      ctx_appendf(buf, cap, &pos, "\n\n");
   }
   free(rules);

   /* Relevance-scored memory context (#3):
    * If we have a task prompt, search for relevant memories only.
    * Otherwise fall back to the generic context assembly. */
   if (!skip_kb_client && custom_prompt && custom_prompt[0])
   {
      /* Extract keywords from the prompt for targeted search */
      memory_t mems[8];
      int mcount = 0;

      /* Search L2/L3/L5 facts and patterns matching keywords from the prompt.
       * L3 holds slow-changing project/environment facts; L5 holds synthesised
       * patterns across sessions.  Both should flow into the injected context
       * alongside L2 facts. */
      char keyword[64] = {0};
      const char *p = custom_prompt;
      while (*p && (*p == ' ' || !strncmp(p, "Check ", 6) || !strncmp(p, "Deploy ", 7) ||
                    !strncmp(p, "Verify ", 7) || !strncmp(p, "List ", 5)))
      {
         while (*p && *p != ' ')
            p++;
         while (*p == ' ')
            p++;
      }
      snprintf(keyword, sizeof(keyword), "%.*s", 60, p);
      const char *search = keyword[0] ? keyword : custom_prompt;
      mcount = kb_client_memory_search_facts_patterns_by_keyword(search, mems, 5);

      if (mcount > 0)
      {
         size_t arch_start = pos;
         ctx_appendf(buf, cap, &pos, "# Relevant Context\n");
         for (int i = 0; i < mcount && pos < cap - 256 && (pos - arch_start) < budget_arch; i++)
         {
            ctx_appendf(buf, cap, &pos, "- %s: %s\n", mems[i].key, mems[i].content);
         }
         ctx_appendf(buf, cap, &pos, "\n");
      }
   }
   else if (!skip_kb_client)
   {
      /* Fallback: full context assembly (budget: architecture) */
      char *ctx = kb_client_memory_assemble_context(NULL);
      if (ctx && ctx[0])
      {
         size_t ctx_len = strlen(ctx);
         if (ctx_len > budget_arch)
            ctx_len = budget_arch;
         ctx_append_bytes(buf, cap, &pos, ctx, ctx_len);
         ctx_appendf(buf, cap, &pos, "\n\n");
      }
      free(ctx);
   }

   /* Scheduled maintenance + skill ticks. Non-blocking; never delay the reply. */
   {
      config_t maint_cfg;
      if (!skip_kb_client && config_load(&maint_cfg) == 0)
      {
         if (maint_cfg.memory_maintenance_enabled)
         {
            char *resp = kb_client_memory_maintenance_run_json(0, 0, 0);
            free(resp);
         }
         /* Review fires from server.c hook path; curator runs on scheduler tick. */
         if (maint_cfg.skills_curator_enabled)
            skill_curator_maybe();
      }
   }
   int recall_injected = 0;
   {
      config_t recall_cfg;
      if (!skip_kb_client && config_load(&recall_cfg) == 0 && recall_cfg.memory_recall_enabled)
      {
         /* Session-start mode = no task text yet; else the turn prompt is the hint. */
         int session_start = !(custom_prompt && custom_prompt[0]);
         int limit_tokens = session_start ? recall_cfg.memory_recall_limit_tokens_session
                                          : recall_cfg.memory_recall_limit_tokens_turn;
         /* Graph-code fusion is always on for recall. */
         char *recall_envelope =
             kb_client_memory_recall_json_ex(custom_prompt, limit_tokens, session_start, "on");
         cJSON *envelope = recall_envelope ? cJSON_Parse(recall_envelope) : NULL;
         free(recall_envelope);
         cJSON *recall_node =
             envelope ? cJSON_GetObjectItemCaseSensitive(envelope, "recall") : NULL;
         cJSON *recall = recall_node ? cJSON_DetachItemViaPointer(envelope, recall_node) : NULL;
         cJSON_Delete(envelope);
         if (recall)
         {
            /* Flatten the six sections into the prompt exactly in
             * priority order — identity first, directives last — so
             * truncation downstream drops the least-valuable sections
             * first. Each section emits an id+text line per item. */
            static const char *sections[][2] = {{"identity", "Identity"},
                                                {"preferences", "Preferences"},
                                                {"active_context", "Active Context"},
                                                {"open_commitments", "Open Commitments"},
                                                {"reminders", "Reminders"},
                                                {"directives", "Directives"},
                                                {NULL, NULL}};
            int any = 0;
            for (int s = 0; sections[s][0] && pos < cap - 256; s++)
            {
               cJSON *arr = cJSON_GetObjectItemCaseSensitive(recall, sections[s][0]);
               int n = cJSON_GetArraySize(arr);
               if (n <= 0)
                  continue;
               if (!any)
               {
                  ctx_appendf(buf, cap, &pos, "# Recall\n");
                  any = 1;
               }
               ctx_appendf(buf, cap, &pos, "## %s\n", sections[s][1]);
               cJSON *it = NULL;
               cJSON_ArrayForEach(it, arr)
               {
                  const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
                  const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(it, "key"));
                  if (pos >= cap - 128)
                     break;
                  ctx_appendf(buf, cap, &pos, "- %s%s%s\n", text ? text : "",
                              key && key[0] ? " — " : "", key ? key : "");
               }
            }
            if (any)
            {
               ctx_appendf(buf, cap, &pos, "\n");
               recall_injected = 1;
               /* Reminders surfaced in the recall block must still
                * flip `once` reminders to `triggered` so they drop out
                * on the next turn, matching prospective recall behavior. */
               cJSON *rems = cJSON_GetObjectItemCaseSensitive(recall, "reminders");
               cJSON *it = NULL;
               cJSON_ArrayForEach(it, rems)
               {
                  long long id =
                      (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "memory_id"));
                  if (id > 0)
                     kb_client_memory_prospective_mark_triggered((int64_t)id);
               }
            }
            cJSON_Delete(recall);
         }
      }
   }

   /* Legacy prospective-only injection: kept as a fallback for
    * operators who have enabled prospective but haven't flipped on
    * the broader recall block yet. Skipped when the recall bundle
    * already surfaced reminders to avoid duplicating them. */
   if (!recall_injected)
   {
      config_t prosp_cfg;
      if (!skip_kb_client && config_load(&prosp_cfg) == 0 && prosp_cfg.memory_prospective_enabled)
      {
         int cap_matches = prosp_cfg.memory_prospective_max_matches > 0
                               ? prosp_cfg.memory_prospective_max_matches
                               : 3;
         if (cap_matches > MEMORY_PROSPECTIVE_MAX_MATCHES)
            cap_matches = MEMORY_PROSPECTIVE_MAX_MATCHES;
         memory_prospective_t triggered[MEMORY_PROSPECTIVE_MAX_MATCHES];
         int n =
             kb_client_memory_prospective_match(custom_prompt, NULL, NULL, triggered, cap_matches);
         if (n > 0)
         {
            ctx_appendf(buf, cap, &pos, "# Reminders\n");
            for (int i = 0; i < n && pos < cap - 256; i++)
            {
               ctx_appendf(buf, cap, &pos, "- %s (when: %s)\n", triggered[i].action_text,
                           triggered[i].trigger_text);
               kb_client_memory_prospective_mark_triggered(triggered[i].id);
            }
            ctx_appendf(buf, cap, &pos, "\n");
         }
      }
   }

   /* Repo catalog (compact: only list names, not full paths) */
   project_info_t projects[32];
   int pcount = skip_kb_client ? 0 : kb_client_index_list(projects, 32);
   if (pcount > 0)
   {
      ctx_appendf(buf, cap, &pos, "# Repos: ");
      for (int i = 0; i < pcount && pos < cap - 64; i++)
      {
         ctx_appendf(buf, cap, &pos, "%s%s", i > 0 ? ", " : "", projects[i].name);
      }
      ctx_appendf(buf, cap, &pos, "\n\n");
   }

   /* Relevant code symbols: query the index for terms matching the task prompt */
   if (!skip_kb_client && custom_prompt && custom_prompt[0])
   {
      /* Extract words >3 chars from the prompt as search terms */
      char prompt_copy[512];
      snprintf(prompt_copy, sizeof(prompt_copy), "%s", custom_prompt);

      term_hit_t hits[16];
      int total_hits = 0;
      char *saveptr = NULL;
      char *word = strtok_r(prompt_copy, " \t\n,.;:!?()[]{}\"'", &saveptr);

      while (word && total_hits < 16)
      {
         if (strlen(word) > 3)
         {
            int found = kb_client_index_find(word, hits + total_hits, 16 - total_hits);
            total_hits += found;
         }
         word = strtok_r(NULL, " \t\n,.;:!?()[]{}\"'", &saveptr);
      }

      if (total_hits > 0)
      {
         size_t code_start = pos;
         ctx_appendf(buf, cap, &pos, "# Relevant Code\n");
         for (int i = 0; i < total_hits && pos < cap - 256 && (pos - code_start) < budget_code; i++)
         {
            ctx_appendf(buf, cap, &pos, "- %s:%d (%s) [%s]\n", hits[i].file_path, hits[i].line,
                        hits[i].kind, hits[i].project);
         }
         ctx_appendf(buf, cap, &pos, "\n");
      }
   }

   /* Project style guide: inject for current project if available */
   if (pcount > 0)
   {
      /* Determine current project from CWD */
      char cwd_buf[MAX_PATH_LEN];
      const char *cwd = agent_context_cwd(cwd_buf, sizeof(cwd_buf));
      if (cwd && cwd[0])
      {
         for (int i = 0; i < pcount; i++)
         {
            if (strncmp(cwd, projects[i].root, strlen(projects[i].root)) == 0)
            {
               char *sg = style_read(projects[i].name);
               if (sg)
               {
                  /* Skip frontmatter */
                  const char *content = sg;
                  if (strncmp(content, "---", 3) == 0)
                  {
                     const char *end = strstr(content + 3, "---");
                     if (end)
                     {
                        content = end + 3;
                        while (*content == '\n' || *content == '\r')
                           content++;
                     }
                  }
                  size_t sg_len = strlen(content);
                  if (sg_len > 0 && pos + sg_len + 4 < cap)
                  {
                     memcpy(buf + pos, content, sg_len);
                     pos += sg_len;
                     if (pos > 0 && buf[pos - 1] != '\n')
                        buf[pos++] = '\n';
                     buf[pos++] = '\n';
                  }
                  free(sg);
               }
               break;
            }
         }
      }
   }

   /* Network access */
   if (network && network->ssh_entry[0])
   {
      ctx_appendf(buf, cap, &pos, "# Network Access\n");
      ctx_appendf(buf, cap, &pos, "Default entry: %s\n", network->ssh_entry);
      if (network->ssh_key[0])
         ctx_appendf(buf, cap, &pos, "SSH key: %s\n", network->ssh_key);
      ctx_appendf(buf, cap, &pos, "\n");

      if (network->host_count > 0)
      {
         ctx_appendf(buf, cap, &pos, "Hosts:\n");
         for (int i = 0; i < network->host_count && pos < cap - 256; i++)
         {
            const agent_net_host_t *h = &network->hosts[i];

            /* Resolve per-host SSH entry (tunnel or fallback) */
            char host_entry[512] = {0};
            int via_tunnel = 0;
            if (network->tunnel_mgr)
               via_tunnel = agent_tunnel_resolve_entry(network->tunnel_mgr, network, h, host_entry,
                                                       sizeof(host_entry));

            if (via_tunnel && host_entry[0])
            {
               /* Tunnel-routed host: show SSH command with key */
               char ssh_cmd[768];
               if (network->ssh_key[0])
                  snprintf(ssh_cmd, sizeof(ssh_cmd), "%s -i %s", host_entry, network->ssh_key);
               else
                  snprintf(ssh_cmd, sizeof(ssh_cmd), "%s", host_entry);
               ctx_appendf(buf, cap, &pos, "  %-16s %s %s@  %s\n", h->name, ssh_cmd,
                           h->user[0] ? h->user : "root", h->desc);
            }
            else if (h->port > 0)
               ctx_appendf(buf, cap, &pos, "  %-16s %s:%d  %-8s %s\n", h->name, h->ip, h->port,
                           h->user, h->desc);
            else
               ctx_appendf(buf, cap, &pos, "  %-16s %-20s %-8s %s\n", h->name, h->ip, h->user,
                           h->desc);
         }
         ctx_appendf(buf, cap, &pos, "\n");
      }

      if (network->network_count > 0)
      {
         ctx_appendf(buf, cap, &pos, "Networks:\n");
         for (int i = 0; i < network->network_count && pos < cap - 128; i++)
         {
            const agent_net_def_t *nd = &network->networks[i];
            ctx_appendf(buf, cap, &pos, "  %-16s %-20s %s\n", nd->name, nd->cidr, nd->desc);
         }
         ctx_appendf(buf, cap, &pos, "\n");
      }
   }

   /* Working memory context */
   {
      char *wm_ctx = db1_wm_assemble_context(session_id());
      if (wm_ctx && wm_ctx[0])
      {
         ctx_appendf(buf, cap, &pos, "# Working Memory\n%s\n\n", wm_ctx);
      }
      free(wm_ctx);
   }

   /* Virtual context: compacted tool-chain stubs from the current session */
   {
      char *vc = conv_ctx_assemble(session_id(), custom_prompt, 0);
      if (vc && pos < cap - 256)
      {
         size_t vc_len = strlen(vc);
         size_t avail = cap - 1 - pos;
         if (avail > 1)
            ctx_append_bytes(buf, cap, &pos, vc, vc_len < avail - 1 ? vc_len : avail - 1);
         ctx_appendf(buf, cap, &pos, "\n");
      }
      free(vc);
   }

   /* Previous attempts (from delegation attempt log) */
   {
      wm_entry_t attempts[WM_MAX_RESULTS];
      int acount = db1_wm_list(session_id(), "attempt", attempts, WM_MAX_RESULTS);
      if (acount > 0)
      {
         ctx_appendf(buf, cap, &pos, "# Previous Attempts\n");
         for (int i = 0; i < acount && pos < cap - 256; i++)
         {
            cJSON *v = cJSON_Parse(attempts[i].value);
            if (!v)
               continue;

            cJSON *jtc = cJSON_GetObjectItemCaseSensitive(v, "task_context");
            cJSON *jap = cJSON_GetObjectItemCaseSensitive(v, "approach");
            cJSON *joc = cJSON_GetObjectItemCaseSensitive(v, "outcome");
            cJSON *jls = cJSON_GetObjectItemCaseSensitive(v, "lesson");

            const char *tc = cJSON_IsString(jtc) ? jtc->valuestring : "";
            const char *ap = cJSON_IsString(jap) ? jap->valuestring : "";
            const char *oc = cJSON_IsString(joc) ? joc->valuestring : "";
            const char *ls = cJSON_IsString(jls) ? jls->valuestring : "";

            /* Include if prompt matches the attempt context */
            if (custom_prompt && custom_prompt[0] && tc[0])
            {
               if (!strstr(custom_prompt, tc) && !strstr(tc, custom_prompt))
               {
                  cJSON_Delete(v);
                  continue;
               }
            }

            ctx_appendf(buf, cap, &pos, "- Tried: %s\n  Result: %s\n", ap, oc);
            if (ls[0])
               ctx_appendf(buf, cap, &pos, "  Lesson: %s\n", ls);

            cJSON_Delete(v);
         }
         ctx_appendf(buf, cap, &pos, "\n");
      }
   }

   /* Active tasks */
   if (!skip_kb_client)
   {
      aimee_task_t active_tasks[8];
      int tcount = kb_client_task_list(TASK_IN_PROGRESS, NULL, 8, active_tasks, 8);
      if (tcount > 0)
      {
         ctx_appendf(buf, cap, &pos, "# Active Tasks\n");
         for (int i = 0; i < tcount && pos < cap - 256; i++)
         {
            ctx_appendf(buf, cap, &pos, "- [%s] %s\n", active_tasks[i].state,
                        active_tasks[i].title);
         }
         ctx_appendf(buf, cap, &pos, "\n");
      }
   }

   /* Recent failures (budget: recent + errors) — DB1 agent_log. */
   {
      size_t fail_budget = budget_recent + budget_errors;
      db1_agent_log_failure_t fails[3];
      int n = db1_agent_log_failures_since_seconds(3, 300, fails);
      int has_failures = 0;
      size_t fail_start = pos;
      for (int i = 0; i < n && pos < cap - 256 && (pos - fail_start) < fail_budget; i++)
      {
         if (!has_failures)
         {
            ctx_appendf(buf, cap, &pos, "# Recent Failures\n");
            has_failures = 1;
         }
         ctx_appendf(buf, cap, &pos, "- [%s] %.120s\n", fails[i].role[0] ? fails[i].role : "?",
                     fails[i].error[0] ? fails[i].error : "unknown");
      }
      if (has_failures)
         ctx_appendf(buf, cap, &pos, "\n");
   }

   /* Recent execution history: skip. Execution traces from other delegates
    * pollute the context with irrelevant tool calls from unrelated projects.
    * Each delegate starts fresh — it has the task prompt and relevant context
    * from memory/index, which is sufficient. */

   /* Guardrail warnings */
   {
      const char *mode = MODE_APPROVE;
      config_t gcfg;
      if (config_load(&gcfg) == 0)
         mode = config_guardrail_mode(&gcfg);
      if (strcmp(mode, MODE_DENY) == 0)
      {
         ctx_appendf(buf, cap, &pos,
                     "# Guardrails\n"
                     "Mode: strict. Writes to sensitive files and high blast-radius "
                     "operations will be blocked.\n\n");
      }
      else if (strcmp(mode, MODE_APPROVE) == 0)
      {
         ctx_appendf(buf, cap, &pos,
                     "# Guardrails\n"
                     "Mode: approve. Writes to sensitive files will trigger warnings.\n\n");
      }
   }

   /* Project contract */
   char *contract = agent_load_project_contract(NULL);
   if (contract)
   {
      ctx_appendf(buf, cap, &pos, "%s\n", contract);
      free(contract);
   }

   /* Environment capabilities */
   {
      db1_env_capability_t caps[20];
      int n = db1_env_capability_list(caps, 20);
      if (n > 0)
      {
         ctx_appendf(buf, cap, &pos, "# Environment\n");
         for (int i = 0; i < n && pos < cap - 128; i++)
            ctx_appendf(buf, cap, &pos, "  %-20s %s\n", caps[i].key, caps[i].value);
         ctx_appendf(buf, cap, &pos, "\n");
      }
   }

   /* Instructions */
   ctx_appendf(buf, cap, &pos, "%s",
               agent->exec_system_prompt[0] ? agent->exec_system_prompt
                                            : default_exec_instructions);

   return buf;
}

char *agent_build_exec_context(const agent_t *agent, const agent_network_t *network,
                               const char *custom_prompt)
{
   return agent_build_exec_context_ex(agent, network, custom_prompt, 0);
}

/* --- Ephemeral SSH ---
 * Implemented in posix/agent_context.c (POSIX) and windows/agent_context.c (Windows). */

/* --- Context command (Item 10) --- */

void agent_print_context(const agent_config_t *cfg)
{
   if (!cfg || cfg->agent_count == 0)
   {
      printf("No agents configured.\n");
      return;
   }

   agent_t *ag = &((agent_config_t *)cfg)->agents[0]; /* use first agent for context */
   const agent_network_t *net = cfg->network.ssh_entry[0] ? &cfg->network : NULL;

   char *ctx = agent_build_exec_context(ag, net, NULL);
   if (ctx)
   {
      printf("%s\n", ctx);
      free(ctx);
   }
   else
   {
      printf("Failed to assemble context.\n");
   }
}

/* --- Logging ---
 * agent_log_call / agent_record_token_audit / agent_set_ingress_source moved to
 * agent_logging.c (this file is at the line-count limit). */

int agent_get_stats(const char *name, agent_stats_t *out, int max)
{
   db1_agent_log_agent_stats_t rows[64];
   if (max > 64)
      max = 64;
   int n = db1_agent_log_agent_stats((name && name[0]) ? name : NULL, rows, max);
   if (n <= 0)
      return (n < 0) ? 0 : 0;
   for (int i = 0; i < n; i++)
   {
      agent_stats_t *s = &out[i];
      memset(s, 0, sizeof(*s));
      snprintf(s->name, MAX_AGENT_NAME, "%s", rows[i].agent_name);
      s->total_calls = rows[i].total_calls;
      s->total_prompt_tokens = rows[i].total_prompt_tokens;
      s->total_completion_tokens = rows[i].total_completion_tokens;
      s->avg_latency_ms = rows[i].avg_latency_ms;
      s->success_rate = rows[i].success_rate;
      s->total_cache_write_tokens = (int)rows[i].total_cache_write_tokens;
      s->total_cache_read_tokens = (int)rows[i].total_cache_read_tokens;
      s->total_estimated_cost_usd = rows[i].total_estimated_cost_usd;
   }
   return n;
}
