#ifndef AIMEE_VAULT_MUTATION_BUDGET_H
#define AIMEE_VAULT_MUTATION_BUDGET_H

#include <stdint.h>

#define VAULT_MUTATION_BUDGET_STEP_MS    2000u
#define VAULT_MUTATION_BUDGET_STALL_MS   15000u
#define VAULT_MUTATION_BUDGET_FIXED_MS   190000u
#define VAULT_MUTATION_BUDGET_DB_CALL_MS 2000u
#define VAULT_MUTATION_BUDGET_CRYPTO_MS  100u
#define VAULT_MUTATION_BUDGET_CLEANUP_MS 5000u

typedef int64_t (*vault_mutation_budget_clock_fn)(void *opaque);

typedef struct
{
   int64_t started_ms;
   int64_t hard_deadline_ms;
   int64_t stall_deadline_ms;
   int64_t cleanup_deadline_ms;
   uint64_t secret_count;
   uint64_t check_count;
   uint64_t progress_phase;
   uint64_t progress_position;
   uint8_t inventory_digest[32];
   vault_mutation_budget_clock_fn clock;
   void *clock_opaque;
   int inventory_bound;
   int progress_valid;
   int cleanup_active;
} vault_mutation_budget_t;

/* Production initialization uses CLOCK_MONOTONIC.  The clock seam exists only
 * so the deadline arithmetic can be exhaustively tested without sleeping. */
int vault_mutation_budget_init(vault_mutation_budget_t *budget);
int vault_mutation_budget_init_with_clock(vault_mutation_budget_t *budget,
                                          vault_mutation_budget_clock_fn clock, void *opaque);

/* Bind the overall ceiling exactly once to authoritative non-negative inventory
 * counts.  An exact replay is idempotent; any changed inventory is rejected. */
int vault_mutation_budget_bind_inventory(vault_mutation_budget_t *budget, uint64_t secret_count,
                                         uint64_t check_count, const uint8_t inventory_digest[32]);

/* Progress is lexicographic and strict.  Neither a repeated token nor a phase
 * regression refreshes the stall deadline. */
int vault_mutation_budget_progress(vault_mutation_budget_t *budget, uint64_t phase,
                                   uint64_t position);

/* Return an absolute CLOCK_MONOTONIC millisecond deadline clipped by the
 * per-call, stall, and overall ceilings.  Minus one means no work may begin. */
int64_t vault_mutation_budget_deadline_ms(vault_mutation_budget_t *budget, uint32_t per_call_ms);
int vault_mutation_budget_expired(vault_mutation_budget_t *budget);

/* Cleanup gets one fixed, non-renewable allowance even when the work budget has
 * expired.  Re-entering cleanup never extends it. */
int vault_mutation_budget_begin_cleanup(vault_mutation_budget_t *budget);

/* The frozen D2 database seam has no context argument.  The operator thread
 * installs its shared budget around the call; nested users restore the prior
 * value explicitly. */
vault_mutation_budget_t *vault_mutation_budget_current(void);
int vault_mutation_budget_enter(vault_mutation_budget_t *budget);
int vault_mutation_budget_leave(vault_mutation_budget_t *budget);

#endif
