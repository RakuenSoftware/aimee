/* delegate_ensemble.h: Mixture-of-Agents ensemble fan-out and synthesis. */
#ifndef DEC_DELEGATE_ENSEMBLE_H
#define DEC_DELEGATE_ENSEMBLE_H 1

#include "agent_config.h"
#include "config.h"

#define ENSEMBLE_MAX_REFS 8

typedef struct
{
   char response[8192];
   int success;
   double cost_usd;
   int degraded;    /* 1 = returned best single candidate, not synthesized */
   int cost_capped; /* 1 = aborted before aggregation due to cost cap */
} delegate_ensemble_result_t;

int delegate_ensemble_run(agent_config_t *acfg, const config_t *cfg, const char *prompt,
                          delegate_ensemble_result_t *out);

double delegate_ensemble_cost_usd(const delegate_ensemble_result_t *r);

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
} roundtable_opts_t;

typedef struct
{
   char *artifact;
   int rounds_run;
   int converged;
   int degraded;
   int truncated;
   int cost_capped;
   int deadline_hit;
   int best_round;
   double cost_usd;
} roundtable_result_t;

int delegate_roundtable_run(agent_config_t *acfg, const config_t *cfg, const char *task,
                            const roundtable_opts_t *opts, roundtable_result_t *out);
void delegate_roundtable_result_free(roundtable_result_t *r);

#endif /* DEC_DELEGATE_ENSEMBLE_H */
