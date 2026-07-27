#ifndef AIMEE_KB_VAULT_ACTIVATION_LATCH_H
#define AIMEE_KB_VAULT_ACTIVATION_LATCH_H

#include "kb_vault_operator_status.h"

#include <pthread.h>
#include <stdint.h>

typedef struct
{
   pthread_mutex_t mutex;
   pthread_cond_t condition;
   kb_vault_operator_status_t status;
   int initialized;
   int activated;
} kb_vault_activation_latch_t;

int kb_vault_activation_latch_init(kb_vault_activation_latch_t *latch);
/* One-way release publication. Replays must carry the byte-identical status. */
int kb_vault_activation_latch_publish(kb_vault_activation_latch_t *latch,
                                      const kb_vault_operator_status_t *status);
/* Returns 1 after acquire-observing activation, 0 on timeout, and -1 on error. */
int kb_vault_activation_latch_wait(kb_vault_activation_latch_t *latch, unsigned timeout_ms,
                                   kb_vault_operator_status_t *status);
void kb_vault_activation_latch_destroy(kb_vault_activation_latch_t *latch);

#endif
