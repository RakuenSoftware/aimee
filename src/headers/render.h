#ifndef DEC_RENDER_H
#define DEC_RENDER_H 1

#include "cJSON.h"
#include "agent_types.h"
#include "checkpoints.h"
#include "decisions.h"
#include "decision_log.h" /* kb_store/decision_log.h via -Ikb_store */

/* Emit a cJSON value as compact JSON to stdout, applying the given field
 * filter and response profile. Frees the cJSON object after printing. */
void emit_json_ctx(cJSON *json, const char *fields, const char *profile);

/* Emit a simple {"status":"ok"} response. */
void emit_ok_ctx(const char *fields, const char *profile);

/* Emit a {"status":"ok","key":"value"} response. */
void emit_ok_kv_ctx(const char *key, const char *value, const char *fields, const char *profile);

/* Create a cJSON object from a memory_t. */
cJSON *memory_to_json(const memory_t *m);

/* Create a cJSON object from a search_result_t. */
cJSON *search_result_to_json(const search_result_t *r);

/* Create a cJSON object from a aimee_task_t. */
cJSON *aimee_task_to_json(const aimee_task_t *t);

/* Create a cJSON object from a KB_STORE decision_log row. */
cJSON *decision_to_json(const kb_store_decision_log_row_t *d);

/* Create a cJSON object from a db1_checkpoint_t. */
cJSON *checkpoint_to_json(const db1_checkpoint_t *c);

/* Create a cJSON object from an anti_pattern_t. */
cJSON *anti_pattern_to_json(const anti_pattern_t *a);

/* Create a cJSON object from a conflict_t. */
cJSON *conflict_to_json(const conflict_t *c);

/* Create a cJSON object from an agent_result_t. Caller owns result. */
cJSON *agent_result_to_json(const agent_result_t *result);

#endif /* DEC_RENDER_H */
