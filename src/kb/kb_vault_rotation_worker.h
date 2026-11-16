/* kb_vault_rotation_worker.h: the KB daemon's org-vault credential rotation loop.
 *
 * An admin files a request with `aimee-kb vault rotate` (or `remediate`); the
 * daemon, which holds custody and reads the slot's signed high-water mark, takes
 * it up, starts the fenced rotation, and steps it through provision, probe,
 * activate, revoke and retire. A rotation interrupted by a restart, or by a
 * vendor answer that left it uncertain, resumes on a later tick. */
#ifndef AIMEE_KB_VAULT_ROTATION_WORKER_H
#define AIMEE_KB_VAULT_ROTATION_WORKER_H 1

#include "kb_vault_rotation_aws.h"

/* Registers the AWS IAM provider and starts the loop thread. Returns 0, or -1
 * when the thread cannot start. Call once, after custody is selected. */
int kb_vault_rotation_worker_start(void);
void kb_vault_rotation_worker_stop(void);

/* One pass: file due schedules, settle pending requests, then step every
 * unclaimed rotation. The loop thread runs this; tests call it directly. */
void kb_vault_rotation_worker_tick(void);

/* Test seams: the next tick checks schedules at once; the AWS context
 * adoption uses (its transport and authorities); the provider registration
 * that worker_start performs. */
void kb_vault_rotation_worker_schedule_now(void);
void kb_vault_rotation_worker_set_aws(const kb_vault_rotation_aws_t *aws);
int kb_vault_rotation_worker_register_aws(void);

#endif
