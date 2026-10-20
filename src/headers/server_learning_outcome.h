#ifndef AIMEE_SERVER_LEARNING_OUTCOME_H
#define AIMEE_SERVER_LEARNING_OUTCOME_H
#include "cJSON.h"
/* Call only after the canonical terminal append succeeds. Learning owns the
 * projection; DB1 owns observation persistence. The canonical log stays the
 * source, and observation failure cannot rewrite execution success/failure. */
void server_learning_observe_terminal(const char *session, const char *turn, const char *sequence,
                                      const char *event_id, const char *actor, const cJSON *event);
/* Host-bound parent reference is durable before this observational projection.
 * Recovery first replays the exact child terminal, including owned proofs. */
void server_learning_observe_child(const char *session, const char *turn, const char *sequence,
                                   const char *event_id, const char *actor, const cJSON *event);
#endif
