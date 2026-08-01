/* panel_provider.h: required-core contract for optional panel execution. */
#ifndef AIMEE_DELEGATES_PANEL_PROVIDER_H
#define AIMEE_DELEGATES_PANEL_PROVIDER_H 1

#include "aimee.h"
#include <aimee/ir/panel_result.h>

#include "agent_types.h"
#include "roundtable_types.h" /* ensemble_panel_t */

typedef enum
{
   AIMEE_PANEL_DRAFT = 0,
   AIMEE_PANEL_REVIEW = 1
} aimee_panel_mode_t;

typedef enum
{
   AIMEE_PANEL_PARALLEL = 0,
   AIMEE_PANEL_SEQUENTIAL = 1
} aimee_panel_turns_t;

typedef struct
{
   aimee_panel_mode_t mode;
   aimee_panel_turns_t turns;
   int max_rounds;
   int converge_threshold;
   int deadline_ms;
   int apply_review;
   const char *brief;
   int brief_truncated;
   const char *context;
   const char **questions;
   int question_count;
   int (*cancel_requested)(void *ctx);
   void *cancel_ctx;
   const char *parent_session_id;
} aimee_panel_options_t;

typedef struct
{
   char response[8192];
   int success;
   double cost_usd;
   int degraded;
   int cost_capped;
   int participants_total;
   int participants_failed;
} aimee_panel_aggregate_result_t;

typedef enum
{
   AIMEE_PANEL_PROVIDER_OK = 0,
   AIMEE_PANEL_PROVIDER_ERROR = -1,
   AIMEE_PANEL_PROVIDER_UNAVAILABLE = -2,
   AIMEE_PANEL_PROVIDER_INVALID = -3,
   AIMEE_PANEL_PROVIDER_BUSY = -4
} aimee_panel_provider_status_t;

typedef struct aimee_panel_provider
{
   int (*aggregate)(agent_config_t *agents, const ensemble_panel_t *panel, const char *prompt,
                    aimee_panel_aggregate_result_t *out);
   int (*run)(agent_config_t *agents, const ensemble_panel_t *panel, const char *task,
              const aimee_panel_options_t *options, aimee_panel_result_t *out);
   void (*release)(aimee_panel_result_t *result);
} aimee_panel_provider_t;

/* Registration is startup-only: the provider and its callback context must
 * outlive every call and result release. There is deliberately one provider;
 * duplicate registration is rejected rather than introducing hidden policy. */
int aimee_panel_provider_register(const aimee_panel_provider_t *provider);
int aimee_panel_provider_unregister(const aimee_panel_provider_t *provider);
int aimee_panel_provider_available(void);

/* Inputs are borrowed for the duration of the call. On failure, outputs are
 * zeroed. A successful panel result owns artifact through the provider and must
 * be released exactly once with aimee_panel_result_release before unregister. */
int aimee_panel_aggregate(agent_config_t *agents, const ensemble_panel_t *panel, const char *prompt,
                          aimee_panel_aggregate_result_t *out);
int aimee_panel_run(agent_config_t *agents, const ensemble_panel_t *panel, const char *task,
                    const aimee_panel_options_t *options, aimee_panel_result_t *out);
void aimee_panel_result_release(aimee_panel_result_t *result);

#endif /* AIMEE_DELEGATES_PANEL_PROVIDER_H */
