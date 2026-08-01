/* aux_router.h: auxiliary model routing for cheap per-task completions. */
#ifndef DEC_AUX_ROUTER_H
#define DEC_AUX_ROUTER_H 1

#include "config.h"

/* Route a one-shot completion to the configured auxiliary provider/model for
 * the given task name.  Falls back to NULL on any error or when aux routing
 * is disabled so the caller can fall back to the primary model.
 *
 * task_name: e.g. "memory_briefing", "title_generation", "compaction_summary".
 * prompt:    user/instruction text for the completion.
 * max_tokens: 0 = use task config or agent default.
 *
 * Returns malloc'd string on success (caller frees), NULL otherwise. */
char *aux_call(const char *task_name, const char *prompt, int max_tokens);

/* Print the resolved task->provider/model mapping to stdout. */
void aux_config_show(void);

#endif /* DEC_AUX_ROUTER_H */
