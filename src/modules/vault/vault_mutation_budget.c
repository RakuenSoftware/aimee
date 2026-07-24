#include "vault_mutation_budget.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

static _Thread_local vault_mutation_budget_t *current_budget;
static _Thread_local unsigned current_depth;

static int64_t monotonic_ms(void *opaque)
{
   (void)opaque;
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
       (uint64_t)now.tv_sec > (uint64_t)INT64_MAX / 1000u)
      return -1;
   return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int64_t now_ms(vault_mutation_budget_t *budget)
{
   return budget && budget->clock ? budget->clock(budget->clock_opaque) : -1;
}

static int add_ms(int64_t base, uint64_t delta, int64_t *out)
{
   if (!out || base < 0 || delta > (uint64_t)INT64_MAX || base > INT64_MAX - (int64_t)delta)
      return -1;
   *out = base + (int64_t)delta;
   return 0;
}

int vault_mutation_budget_init_with_clock(vault_mutation_budget_t *budget,
                                          vault_mutation_budget_clock_fn clock, void *opaque)
{
   if (!budget || !clock)
      return -1;
   memset(budget, 0, sizeof(*budget));
   budget->clock = clock;
   budget->clock_opaque = opaque;
   budget->started_ms = now_ms(budget);
   if (budget->started_ms < 0 ||
       add_ms(budget->started_ms, VAULT_MUTATION_BUDGET_STALL_MS, &budget->stall_deadline_ms) != 0)
   {
      memset(budget, 0, sizeof(*budget));
      return -1;
   }
   return 0;
}

int vault_mutation_budget_init(vault_mutation_budget_t *budget)
{
   return vault_mutation_budget_init_with_clock(budget, monotonic_ms, NULL);
}

static int checked_add(uint64_t *value, uint64_t add)
{
   if (!value || *value > UINT64_MAX - add)
      return -1;
   *value += add;
   return 0;
}

static int checked_mul_add(uint64_t *value, uint64_t count, uint64_t cost)
{
   return count > UINT64_MAX / cost ? -1 : checked_add(value, count * cost);
}

int vault_mutation_budget_bind_inventory(vault_mutation_budget_t *budget, uint64_t secret_count,
                                         uint64_t check_count, const uint8_t inventory_digest[32])
{
   if (!budget || !budget->clock || !inventory_digest)
      return -1;
   if (budget->inventory_bound)
      return budget->secret_count == secret_count && budget->check_count == check_count &&
                     memcmp(budget->inventory_digest, inventory_digest, 32) == 0
                 ? 0
                 : -1;

   uint64_t secret_pages = secret_count / 128u + (secret_count % 128u != 0);
   uint64_t check_pages = check_count / 128u + (check_count % 128u != 0);
   uint64_t db_calls = 64, crypto_ops = 0, allowance = VAULT_MUTATION_BUDGET_FIXED_MS;
   if (checked_add(&db_calls, secret_count) != 0 || checked_add(&db_calls, check_count) != 0 ||
       checked_mul_add(&db_calls, secret_pages, 2) != 0 ||
       checked_mul_add(&db_calls, check_pages, 3) != 0 ||
       checked_mul_add(&crypto_ops, secret_count, 3) != 0 ||
       checked_mul_add(&crypto_ops, check_count, 4) != 0 ||
       checked_mul_add(&allowance, db_calls, VAULT_MUTATION_BUDGET_DB_CALL_MS) != 0 ||
       checked_mul_add(&allowance, crypto_ops, VAULT_MUTATION_BUDGET_CRYPTO_MS) != 0 ||
       allowance > (uint64_t)INT64_MAX)
      return -1;
   if (add_ms(budget->started_ms, allowance, &budget->hard_deadline_ms) != 0)
      return -1;
   budget->secret_count = secret_count;
   budget->check_count = check_count;
   memcpy(budget->inventory_digest, inventory_digest, 32);
   budget->inventory_bound = 1;
   return vault_mutation_budget_expired(budget) ? -1 : 0;
}

int vault_mutation_budget_progress(vault_mutation_budget_t *budget, uint64_t phase,
                                   uint64_t position)
{
   int64_t now = now_ms(budget);
   if (now < 0 || vault_mutation_budget_expired(budget) ||
       (budget->progress_valid &&
        (phase < budget->progress_phase ||
         (phase == budget->progress_phase && position <= budget->progress_position))))
      return -1;
   int64_t stall = 0;
   if (add_ms(now, VAULT_MUTATION_BUDGET_STALL_MS, &stall) != 0)
      return -1;
   if (budget->inventory_bound && stall > budget->hard_deadline_ms)
      stall = budget->hard_deadline_ms;
   budget->progress_phase = phase;
   budget->progress_position = position;
   budget->progress_valid = 1;
   budget->stall_deadline_ms = stall;
   return 0;
}

int64_t vault_mutation_budget_deadline_ms(vault_mutation_budget_t *budget, uint32_t per_call_ms)
{
   int64_t now = now_ms(budget), end = 0;
   if (now < 0 || !per_call_ms || add_ms(now, per_call_ms, &end) != 0)
      return -1;
   if (budget->cleanup_active)
   {
      if (now >= budget->cleanup_deadline_ms)
         return -1;
      return end < budget->cleanup_deadline_ms ? end : budget->cleanup_deadline_ms;
   }
   if (now >= budget->stall_deadline_ms ||
       (budget->inventory_bound && now >= budget->hard_deadline_ms))
      return -1;
   if (end > budget->stall_deadline_ms)
      end = budget->stall_deadline_ms;
   if (budget->inventory_bound && end > budget->hard_deadline_ms)
      end = budget->hard_deadline_ms;
   return end;
}

int vault_mutation_budget_expired(vault_mutation_budget_t *budget)
{
   int64_t now = now_ms(budget);
   if (now < 0)
      return 1;
   if (budget->cleanup_active)
      return now >= budget->cleanup_deadline_ms;
   return now >= budget->stall_deadline_ms ||
          (budget->inventory_bound && now >= budget->hard_deadline_ms);
}

int vault_mutation_budget_begin_cleanup(vault_mutation_budget_t *budget)
{
   int64_t now = now_ms(budget);
   if (now < 0)
      return -1;
   if (!budget->cleanup_active)
   {
      if (add_ms(now, VAULT_MUTATION_BUDGET_CLEANUP_MS, &budget->cleanup_deadline_ms) != 0)
         return -1;
      budget->cleanup_active = 1;
   }
   return now < budget->cleanup_deadline_ms ? 0 : -1;
}

vault_mutation_budget_t *vault_mutation_budget_current(void)
{
   return current_budget;
}

int vault_mutation_budget_enter(vault_mutation_budget_t *budget)
{
   if (!budget || (current_budget && current_budget != budget) || current_depth == UINT_MAX)
      return -1;
   current_budget = budget;
   current_depth++;
   return 0;
}

int vault_mutation_budget_leave(vault_mutation_budget_t *budget)
{
   if (!budget || current_budget != budget || !current_depth)
      return -1;
   if (!--current_depth)
      current_budget = NULL;
   return 0;
}
