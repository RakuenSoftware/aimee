/* delegate_economics.h: supervisor-centric economics for delegate runs. */
#ifndef DEC_DELEGATE_ECONOMICS_H
#define DEC_DELEGATE_ECONOMICS_H 1

#include "aimee.h"
#include "agent_types.h"
#include "coord_jobs.h"
#include "cJSON.h"

#define DELEGATE_ECONOMICS_TIER_BUCKETS 4
#define DELEGATE_ECONOMICS_COST_MODEL   "free_delegate_expensive_supervisor"

typedef struct
{
   int delegate_count;
   int tier_counts[DELEGATE_ECONOMICS_TIER_BUCKETS];
   int unknown_tier_count;
   int prompt_tokens_total;
   int completion_tokens_total;
   int delegate_tokens_estimated;
   int tokenized_delegate_results;
   int supervisor_prompt_tokens_estimated;
   int handoff_count;
   int valid_handoffs;
   int invalid_handoffs;
   int focused_tests_run_by_delegates;
   int delegates_with_focused_tests;
   int manual_integration_events;
   int supervisor_actions_required;
   int reviewer_findings_blocking;
   char verdict[32];
   char recommendation[256];
   /* Rendered by the module alongside the verdict, so a caption cannot drift
    * from the verdict it captions. */
   char verdict_label[64];
   char cost_model_label[64];
} delegate_economics_report_t;

/* The economics rule lives in the delegates module
 * (server-go/modules/delegates/economics.go). This is the seam the C side calls
 * through; with no provider registered the report stays empty and the verdict
 * stays "unclear", which is what the rule itself says about a run it cannot
 * judge. */
typedef void (*delegate_economics_provider_fn)(const db1_coord_task_t *tasks, int task_count,
                                               const agent_config_t *cfg,
                                               delegate_economics_report_t *out);
void delegate_register_economics_provider(delegate_economics_provider_fn provider);

void delegate_economics_build_report(const db1_coord_job_t *job, const db1_coord_task_t *tasks,
                                     int task_count, const agent_config_t *cfg,
                                     delegate_economics_report_t *out);
void delegate_economics_add_json(cJSON *obj, const delegate_economics_report_t *report);

int delegate_economics_is_tier0_heavy(const delegate_economics_report_t *report);

void delegate_economics_add_agent_result_json(cJSON *obj, const agent_config_t *cfg,
                                              const char *role, const agent_result_t *result,
                                              const agent_t *fallback_agent);

#endif /* DEC_DELEGATE_ECONOMICS_H */
