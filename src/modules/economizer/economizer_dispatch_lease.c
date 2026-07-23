#include "economizer_dispatch_lease.h"

#include <pthread.h>
#include <stdlib.h>

struct econ_dispatch_state
{
   pthread_rwlock_t lock;
   econ_dispatch_facts_t facts;
};

static int facts_valid(const econ_dispatch_facts_t *f)
{
   return f && f->tenant_id && f->account_id && f->registry_generation && f->pricing_generation &&
          f->contract_generation && f->tokenizer_generation && f->cohort_generation &&
          f->kill_switch_generation && (f->enabled == 0 || f->enabled == 1);
}

static int facts_equal(const econ_dispatch_facts_t *a, const econ_dispatch_facts_t *b)
{
   return a->tenant_id == b->tenant_id && a->account_id == b->account_id &&
          a->registry_generation == b->registry_generation &&
          a->pricing_generation == b->pricing_generation &&
          a->contract_generation == b->contract_generation &&
          a->tokenizer_generation == b->tokenizer_generation &&
          a->cohort_generation == b->cohort_generation &&
          a->kill_switch_generation == b->kill_switch_generation && a->enabled == b->enabled;
}

int econ_dispatch_state_create(const econ_dispatch_facts_t *initial, econ_dispatch_state_t **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!facts_valid(initial))
      return -1;
   econ_dispatch_state_t *state = calloc(1, sizeof(*state));
   if (!state)
      return -1;
   if (pthread_rwlock_init(&state->lock, NULL) != 0)
   {
      free(state);
      return -1;
   }
   state->facts = *initial;
   *out = state;
   return 0;
}

void econ_dispatch_state_destroy(econ_dispatch_state_t *state)
{
   if (!state)
      return;
   pthread_rwlock_destroy(&state->lock);
   free(state);
}

int econ_dispatch_state_replace(econ_dispatch_state_t *state, const econ_dispatch_facts_t *next)
{
   if (!state || !facts_valid(next) || pthread_rwlock_wrlock(&state->lock) != 0)
      return -1;
   const econ_dispatch_facts_t *old = &state->facts;
   int valid = next->tenant_id == old->tenant_id && next->account_id == old->account_id &&
               next->registry_generation >= old->registry_generation &&
               next->pricing_generation >= old->pricing_generation &&
               next->contract_generation >= old->contract_generation &&
               next->tokenizer_generation >= old->tokenizer_generation &&
               next->cohort_generation >= old->cohort_generation &&
               next->kill_switch_generation >= old->kill_switch_generation &&
               (next->enabled == old->enabled ||
                next->kill_switch_generation > old->kill_switch_generation);
   if (valid)
      state->facts = *next;
   pthread_rwlock_unlock(&state->lock);
   return valid ? 0 : -1;
}

int econ_dispatch_lease_begin(econ_dispatch_state_t *state, const econ_dispatch_facts_t *expected,
                              econ_dispatch_lease_t *lease)
{
   if (!state || !facts_valid(expected) || !lease || lease->held)
      return -1;
   lease->state = NULL;
   lease->held = 0;
   if (pthread_rwlock_rdlock(&state->lock) != 0)
      return -1;
   if (!expected->enabled || !facts_equal(&state->facts, expected))
   {
      pthread_rwlock_unlock(&state->lock);
      return 0;
   }
   lease->state = state;
   lease->held = 1;
   return 1;
}

void econ_dispatch_lease_end(econ_dispatch_lease_t *lease)
{
   if (!lease || !lease->held || !lease->state)
      return;
   pthread_rwlock_unlock(&lease->state->lock);
   lease->state = NULL;
   lease->held = 0;
}
