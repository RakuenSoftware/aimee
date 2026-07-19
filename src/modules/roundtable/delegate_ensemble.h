/* delegate_ensemble.h: Mixture-of-Agents ensemble fan-out and synthesis — the
 * one-shot AGGREGATE and ROUNDTABLE panel modes of the ensemble concept (see
 * docs/ENSEMBLE.md). The persistent, templated, turn-based SESSION mode lives in
 * db1/ensemble.{c,h}; a delegate's output can feed a channel-bound session. */
#ifndef DEC_DELEGATE_ENSEMBLE_H
#define DEC_DELEGATE_ENSEMBLE_H 1

#include "agent_config.h"
#include "config.h"
#include "roundtable_types.h" /* roundtable_opts_t / roundtable_result_t (owned by the roundtable module) */

/* Max panelists in a roundtable/ensemble fan-out. Must match the
 * ensemble_reference_models / ensemble_reference_personas array dims in config.h
 * (a _Static_assert in delegate_ensemble.c enforces this). The fan-out arrays
 * (agent_result_t results[N] ~1.4KB each, etc.) live on the compute-pool worker
 * stack, which is 32 MB — so 32 panelists (~50KB frame) is well within budget. */
#define ENSEMBLE_MAX_REFS 32

typedef struct
{
   char response[8192];
   int success;
   double cost_usd;
   int degraded;            /* 1 = returned best single candidate, not synthesized */
   int cost_capped;         /* 1 = aborted before aggregation due to cost cap */
   int participants_total;  /* reference models fanned out this run */
   int participants_failed; /* participants that returned no usable response (partial failure) */
} delegate_ensemble_result_t;

int delegate_ensemble_run(agent_config_t *acfg, const config_t *cfg, const char *prompt,
                          delegate_ensemble_result_t *out);

double delegate_ensemble_cost_usd(const delegate_ensemble_result_t *r);

/* Estimate the USD cost of a single delegate/model call. Routes through the
 * shared cache-aware pricing authority (token_estimate_cost), falling back to
 * the model registry for providers it does not cover, then to a coarse flat
 * rate for genuinely unpriced models. provider may be NULL (inferred). */
double delegate_cost_estimate_usd(const char *provider, const char *model, int prompt_tokens,
                                  int completion_tokens);

/* roundtable_opts_t, roundtable_result_t and their review-item / evidence /
 * answered-question types (plus ROUNDTABLE_MAX_*) now live in the roundtable
 * module's roundtable_types.h, included above. They were moved out of this
 * delegates header to break the header cycle with the roundtable module (whose
 * chair/verify headers need the result type). The delegate_roundtable_* entry
 * points below still produce and free those types. */

int delegate_roundtable_run(agent_config_t *acfg, const config_t *cfg, const char *task,
                            const roundtable_opts_t *opts, roundtable_result_t *out);
void delegate_roundtable_result_free(roundtable_result_t *r);

/* Seed cfg->ensemble_reference_models from the enabled agents when no panel is
 * configured (no-op if ensemble_reference_count > 0). Skips agents flagged
 * primary-only and claude-CLI agents that cannot run server-side. Defaults the
 * aggregator to the first seated model. */
void ensemble_default_panel_from_agents(config_t *cfg, const agent_config_t *acfg);

/* 1 if `ag` may sit on a panel: enabled + named, NOT primary-only (agents.json
 * `primary_only`), and a claude-CLI only when server-hosted (is_server_hosted). */
int ensemble_panelist_eligible(const config_t *cfg, const agent_t *ag);

/* Replace each "$random" seat in ensemble.reference_models with a concretely
 * picked review-capable agent (excluding already-seated models for diversity);
 * drop a $random seat that cannot be filled. Pinned seats pass through. Called
 * first by ensemble_filter_panel_authorization so downstream filters see real
 * agents. Exposed for tests. */
void ensemble_resolve_random_seats(config_t *cfg, const agent_config_t *acfg);

/* Drop unauthorized/ineligible configured agents (e.g. an unauthorized claude)
 * from an EXPLICIT ensemble.reference_models list and fix up the aggregator. Run
 * after ensemble_default_panel_from_agents so both auto and explicit panels are
 * authorization-gated. Resolves "$random" seats first (see above). */
void ensemble_filter_panel_authorization(config_t *cfg, const agent_config_t *acfg);

/* Drop currently-UNAVAILABLE panelists (unkeyed HTTP agent, missing CLI/tmux,
 * health-breaker DOWN) from the panel — runtime gate via
 * agent_is_available_for_routing, distinct from the authorization gate above.
 * Run after the seed + authorization filter so a configured-but-broken or
 * auto-seeded-but-unkeyed model never burns a seat and degrades the round. */
void ensemble_filter_panel_availability(config_t *cfg, const agent_config_t *acfg);

/* Persona name for panelist `model_index`: a configured
 * ensemble.reference_personas[model_index] if set (any mode), else a mode default
 * — DRAFT uses the built-in `engineer` persona for every panelist, REVIEW
 * round-robins the diverse default lineup keyed on the stable model index.
 * Borrowed pointer (string literal or config field) — do not free. Exposed for
 * tests. */
const char *panel_persona_name(const config_t *cfg, roundtable_mode_t mode, int model_index);

#endif /* DEC_DELEGATE_ENSEMBLE_H */
