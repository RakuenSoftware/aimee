/* kb_planner.h: Deliberate planning — MCTS plan search and SMT constraint validation.
 * See docs/proposals/accepted/deliberate-planning-search-and-constraint-execution.md
 */
#ifndef DEC_KB_PLANNER_H
#define DEC_KB_PLANNER_H 1

#include "config.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Run bounded MCTS plan search via planner_search_command sidecar.
    *
    *   request_json: full sidecar request body (object with "goal",
    *                 "risk_classes", "budget", "exploration_constant",
    *                 "seed_candidates").
    *   out_json / out_len: receive malloc'd sidecar stdout (caller frees).
    *
    * Returns 0 on success; -1 on error (command empty, exec fail, sidecar
    * non-zero exit). Caller frees *out_json. */
   int kb_planner_search(const config_t *cfg, const char *request_json, char **out_json,
                         size_t *out_len);

   /* Validate a plan against constraint_pack via constraint_solver_command sidecar.
    *
    *   request_json: full sidecar request body (object with "plan" and "constraints").
    *   out_json / out_len: receive malloc'd sidecar stdout (caller frees).
    *
    * Returns 0 on success; -1 on error. */
   int kb_planner_validate(const config_t *cfg, const char *request_json, char **out_json,
                           size_t *out_len);

   /* Write a plan_candidate or plan_template artifact in 'proposed' state.
    *
    *   kind:        "plan_candidate" or "plan_template" (other values rejected).
    *   scope_id:    decision-point or task ID the plan addresses.
    *   payload:     full plan JSON (goal, subgoals[], risk_classes[],
    *                required_verifications[], rollback_plan{}, constraint_pack[]).
    *   id_out / id_out_len: receives generated artifact id (>= 33 bytes); NULL ok.
    *
    * Returns 0 on success; -1 on error (DB unavailable, bad kind, bad input). */
   int kb_planner_artifact_write(const char *kind, const char *scope_id, const char *payload,
                                 char *id_out, size_t id_out_len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_PLANNER_H */
