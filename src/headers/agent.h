#ifndef DEC_AGENT_H
#define DEC_AGENT_H 1

/* Credential/subscription failure shared by one provider registration. */
int agent_error_is_registration_failure(const char *error);

/*
 * Umbrella header: includes all agent subsystem headers.
 * Prefer including the specific narrow header you need:
 *   agent_types.h  - shared types and constants
 *   agent_config.h - config loading, routing, auth
 *   agent_exec.h   - execution, context, SSH, policy, metrics
 *   agent_tasks.h  - plan IR, durable jobs, cache, hints, two-phase execution
 *   agent_eval.h   - eval harness
 *   agent_coord.h  - multi-agent coordination, directives, coord-jobs
 *   agent_tools.h  - tool execution, checkpoints
 */

#include "agent_types.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_tasks.h"
#include "agent_eval.h"
#include "agent_coord.h"
#include <aimee/tools/agent_tools.h>

#endif /* DEC_AGENT_H */
