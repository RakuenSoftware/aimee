/* tasks_compose.h: high-level task helpers that compose data from
 * multiple stores. Task graph storage lives in KB_STORE; checkpoint storage
 * lives in DB1 because checkpoints are user/session rewind state. */
#ifndef DEC_TASKS_COMPOSE_H
#define DEC_TASKS_COMPOSE_H 1

#include "tasks.h"
#include "checkpoints.h"

/* Compose a checkpoint snapshot from active tasks, L2 facts, and recent
 * decisions and persist it as a checkpoint row. */
int tasks_checkpoint_create(const char *label, const char *session_id, int64_t task_id,
                            db1_checkpoint_t *out);

/* Restore a checkpoint by inserting its snapshot back into memory as an
 * L0 scratch row. */
int tasks_checkpoint_restore(int64_t id, const char *session_id);

#endif /* DEC_TASKS_COMPOSE_H */
