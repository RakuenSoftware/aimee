#include "modules/vault/vault_mutation_budget.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int64_t fake_now(void *opaque)
{
   return *(int64_t *)opaque;
}

int main(void)
{
   int64_t now = 1000;
   uint8_t digest[32], other[32];
   memset(digest, 0x51, sizeof(digest));
   memset(other, 0x52, sizeof(other));
   vault_mutation_budget_t budget, nested;
   assert(vault_mutation_budget_init_with_clock(&budget, fake_now, &now) == 0);
   assert(budget.stall_deadline_ms == 16000);
   assert(vault_mutation_budget_bind_inventory(&budget, 128, 128, digest) == 0);
   /* 190000 + 2000*(64+128+128+2*1+3*1) + 100*(3*128+4*128). */
   assert(budget.hard_deadline_ms == 930600);
   assert(vault_mutation_budget_bind_inventory(&budget, 128, 128, digest) == 0);
   assert(vault_mutation_budget_bind_inventory(&budget, 128, 128, other) != 0);
   assert(vault_mutation_budget_progress(&budget, 1, 1) == 0);
   assert(vault_mutation_budget_progress(&budget, 1, 1) != 0);
   assert(vault_mutation_budget_progress(&budget, 1, 2) == 0);
   assert(vault_mutation_budget_deadline_ms(&budget, 2000) == 3000);
   assert(vault_mutation_budget_init_with_clock(&nested, fake_now, &now) == 0);
   assert(vault_mutation_budget_enter(&budget) == 0);
   assert(vault_mutation_budget_enter(&budget) == 0);
   assert(vault_mutation_budget_enter(&nested) != 0);
   assert(vault_mutation_budget_leave(&budget) == 0);
   assert(vault_mutation_budget_leave(&budget) == 0);
   assert(vault_mutation_budget_current() == NULL);
   now = budget.stall_deadline_ms;
   assert(vault_mutation_budget_expired(&budget));
   assert(vault_mutation_budget_begin_cleanup(&budget) == 0);
   assert(vault_mutation_budget_deadline_ms(&budget, 2000) == now + 2000);
   int64_t cleanup = budget.cleanup_deadline_ms;
   assert(vault_mutation_budget_begin_cleanup(&budget) == 0 &&
          budget.cleanup_deadline_ms == cleanup);
   now = cleanup;
   assert(vault_mutation_budget_expired(&budget));

   now = 0;
   assert(vault_mutation_budget_init_with_clock(&budget, fake_now, &now) == 0);
   assert(vault_mutation_budget_bind_inventory(&budget, UINT64_MAX, UINT64_MAX, digest) != 0);
   puts("vault mutation budget tests passed");
   return 0;
}
