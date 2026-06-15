/* delegate_ensemble.h: Mixture-of-Agents ensemble fan-out and synthesis. */
#ifndef DEC_DELEGATE_ENSEMBLE_H
#define DEC_DELEGATE_ENSEMBLE_H 1

#include "agent_config.h"
#include "config.h"

#define ENSEMBLE_MAX_REFS           8
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
   const char **questions;
   int question_count;
   int (*cancel_requested)(void *ctx);
   void *cancel_ctx;
   /* Originating session: panel + aggregator runs fold their cost onto it via
    * db1_cost_fold_record, so the ensemble is accounted like a delegate. */
   const char *parent_session_id;
} roundtable_opts_t;

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
} roundtable_result_t;

int delegate_roundtable_run(agent_config_t *acfg, const config_t *cfg, const char *task,
                            const roundtable_opts_t *opts, roundtable_result_t *out);
void delegate_roundtable_result_free(roundtable_result_t *r);

/* Seed cfg->ensemble_reference_models from the enabled agents when no panel is
 * configured (no-op if ensemble_reference_count > 0). Skips agents that cannot
 * run as a server-side HTTP delegate (claude-CLI unless
 * claude_cli_delegate_enabled). Defaults the aggregator to the first seated
 * model. */
void ensemble_default_panel_from_agents(config_t *cfg, const agent_config_t *acfg);

/* Persona name for review panelist `model_index`: a configured
 * ensemble.reference_personas[model_index] if set, else a round-robin over the
 * engine's diverse default lineup keyed on the stable model index. Returns NULL
 * for non-review modes (draft/aggregate keep their NULL-persona behavior).
 * Borrowed pointer (string literal or config field) — do not free. Exposed for
 * tests. */
const char *panel_persona_name(const config_t *cfg, roundtable_mode_t mode, int model_index);

#endif /* DEC_DELEGATE_ENSEMBLE_H */
