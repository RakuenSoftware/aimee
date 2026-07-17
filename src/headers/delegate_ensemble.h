/* delegate_ensemble.h: Mixture-of-Agents ensemble fan-out and synthesis — the
 * one-shot AGGREGATE and ROUNDTABLE panel modes of the ensemble concept (see
 * docs/ENSEMBLE.md). The persistent, templated, turn-based SESSION mode lives in
 * db1/ensemble.{c,h}; a delegate's output can feed a channel-bound session. */
#ifndef DEC_DELEGATE_ENSEMBLE_H
#define DEC_DELEGATE_ENSEMBLE_H 1

#include "agent_config.h"
#include "config.h"

/* Max panelists in a roundtable/ensemble fan-out. Must match the
 * ensemble_reference_models / ensemble_reference_personas array dims in config.h
 * (a _Static_assert in delegate_ensemble.c enforces this). The fan-out arrays
 * (agent_result_t results[N] ~1.4KB each, etc.) live on the compute-pool worker
 * stack, which is 32 MB — so 32 panelists (~50KB frame) is well within budget. */
#define ENSEMBLE_MAX_REFS           32
#define ROUNDTABLE_MAX_REVIEW_ITEMS 128
#define ROUNDTABLE_MAX_QUESTIONS    16

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

typedef enum
{
   ROUNDTABLE_DRAFT = 0,
   ROUNDTABLE_REVIEW = 1
} roundtable_mode_t;

typedef enum
{
   ROUNDTABLE_PARALLEL = 0,
   ROUNDTABLE_SEQUENTIAL = 1
} roundtable_turns_t;

typedef struct
{
   roundtable_mode_t mode;
   roundtable_turns_t turns;
   int max_rounds;
   int converge_threshold;
   int deadline_ms;
   int apply_review;
   const char *brief;
   int brief_truncated;
   /* Optional pre-assembled read-only context (aimee memory recall + code-graph
    * snippets) injected into every panelist prompt — a useful SEED, no longer the
    * only window: REVIEW-mode panelists now run with aimee's index-only toolset
    * (`review_indexed`) and can look things up themselves. Drafting panelists are
    * still tool-less, so for them this remains the only view of memory and the code
    * graph. The caller owns the string for the duration of the run; NULL = none. */
   const char *context;
   const char **questions;
   int question_count;
   int (*cancel_requested)(void *ctx);
   void *cancel_ctx;
   /* Originating session: panel + aggregator runs fold their cost onto it via
    * db1_cost_fold_record, so the ensemble is accounted like a delegate. */
   const char *parent_session_id;
} roundtable_opts_t;

/* Replay-verification evidence (Part A of the replayable-verification proposal).
 * A panelist attaches a STRUCTURED query — never a free-form command — so a fresh
 * verifier can replay it deterministically over the read-only code index
 * (index_find / index_find_callers / index_code_search) and re-ground the claim.
 * Plain (no pointers): memset-zeroing and struct copy stay valid. */
typedef enum
{
   EV_NONE = 0, /* no replayable evidence -> item is interpretive (caps at concern) */
   EV_SYMBOL,   /* does symbol `target` exist? -> index_find */
   EV_REFS,     /* how many call sites of `target`? -> index_find_callers (the workhorse) */
   EV_SEARCH    /* lexical code search for `target` -> index_code_search */
} ev_kind_t;

typedef struct
{
   ev_kind_t kind;
   char target[256];  /* symbol (EV_SYMBOL/EV_REFS) or search query (EV_SEARCH) */
   char project[128]; /* index project scope ("" = all indexed projects) */
   int count;         /* the count the panelist claims (EV_REFS/EV_SEARCH) */
   char idkey[65];    /* sha256-hex[:64] of sorted "file:line", or "" if unset */
   int factual;       /* 1 = replayable claim; 0 = interpretive (caps at concern) */
} review_evidence_t;

typedef struct
{
   char severity[16];
   char category[32];
   char location[128];
   char summary[256];
   char recommendation[256];
   char identity_key[128];
   char sources[256];
   int count;
   review_evidence_t evidence; /* Part A: structured replay evidence (zeroed = EV_NONE) */
} roundtable_review_item_t;

typedef struct
{
   char question[512];
   char answer[1024];
   char evidence[512];
   int answered;
} roundtable_answered_question_t;

typedef struct
{
   char *artifact;
   int rounds_run;
   int converged;
   int degraded;
   int truncated;
   int cost_capped;
   int deadline_hit;
   int cancelled;
   int best_round;
   int participants_total;  /* reference models per round (panel size) */
   int participants_failed; /* participants that returned no usable response in the final round run
                             */
   double cost_usd;
   roundtable_review_item_t items[ROUNDTABLE_MAX_REVIEW_ITEMS];
   int item_count;
   int items_round;
   int artifact_round;
   roundtable_answered_question_t answered_questions[ROUNDTABLE_MAX_QUESTIONS];
   int answered_question_count;
   char coverage_gaps[ROUNDTABLE_MAX_QUESTIONS][512];
   int coverage_gap_count;
   /* Replay verification (Part A). Items whose structured evidence did not
    * reproduce against the code index are moved here (not silently dropped), with
    * a parallel reason code. Fixed arrays (no heap; no change to result_free). */
   roundtable_review_item_t rejected[ROUNDTABLE_MAX_REVIEW_ITEMS];
   char rejected_reason[ROUNDTABLE_MAX_REVIEW_ITEMS][24];
   int rejected_count;
   int verified_count; /* kept items whose factual evidence reproduced */
   int degraded_count; /* kept but unverified (index unavailable) */
   int capped_count;   /* interpretive items capped at suggestion */
} roundtable_result_t;

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
