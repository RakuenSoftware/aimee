#ifndef DEC_RENDER_H
#define DEC_RENDER_H 1

#include "cJSON.h"
#include "agent_types.h"
#include "checkpoints.h"
#include "decisions.h"
#include "decision_log.h" /* kb_store/decision_log.h via -Ikb_store */

/* Create a cJSON object from a KB_STORE decision_log row. */
cJSON *decision_to_json(const kb_store_decision_log_row_t *d);

#endif /* DEC_RENDER_H */
