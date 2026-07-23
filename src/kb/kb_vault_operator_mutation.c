#include "kb_vault_operator_mutation.h"

#include <limits.h>
#include <openssl/crypto.h>
#include <string.h>
#include <time.h>

static int monotonic_now(struct timespec *now)
{
   return now && clock_gettime(CLOCK_MONOTONIC, now) == 0 ? 0 : -1;
}

static int deadline_after_ms(struct timespec *deadline, unsigned milliseconds)
{
   if (monotonic_now(deadline) != 0)
      return -1;
   deadline->tv_sec += (time_t)(milliseconds / 1000u);
   deadline->tv_nsec += (long)(milliseconds % 1000u) * 1000000L;
   if (deadline->tv_nsec >= 1000000000L)
   {
      deadline->tv_sec++;
      deadline->tv_nsec -= 1000000000L;
   }
   return 0;
}

static int deadline_valid(const struct timespec *deadline)
{
   struct timespec now;
   return deadline && monotonic_now(&now) == 0 &&
          (now.tv_sec < deadline->tv_sec ||
           (now.tv_sec == deadline->tv_sec && now.tv_nsec <= deadline->tv_nsec));
}

static int all_zero(const uint8_t *p, size_t n)
{
   uint8_t v = 0;
   for (size_t i = 0; i < n; ++i)
      v |= p[i];
   return v == 0;
}

static int binding_valid(const kb_vault_mutation_binding_t *b)
{
   return b && b->state >= KB_VAULT_MUTATION_BINDING_ACTIVE &&
          b->state <= KB_VAULT_MUTATION_BINDING_RECOVERY_REQUIRED && !all_zero(b->request_id, 16) &&
          !all_zero(b->operation_id, 16) && b->old_generation < INT64_MAX &&
          b->new_generation == b->old_generation + 1 && b->seal_epoch &&
          b->seal_epoch <= INT64_MAX && b->fence && b->fence <= INT64_MAX;
}

static int same_status(const kb_vault_operator_status_t *a, const kb_vault_operator_status_t *b)
{
   return a->state == b->state && a->operation_state == b->operation_state &&
          a->remediation == b->remediation && a->flags == b->flags &&
          a->seal_epoch == b->seal_epoch && a->control_fence == b->control_fence &&
          a->old_generation == b->old_generation && a->new_generation == b->new_generation &&
          a->last_opened_fence == b->last_opened_fence &&
          CRYPTO_memcmp(a->operation_id, b->operation_id, 16) == 0;
}

static int same_operation(const kb_vault_operator_status_t *s, const kb_vault_mutation_binding_t *b)
{
   return (s->flags & 1) && CRYPTO_memcmp(s->operation_id, b->operation_id, 16) == 0 &&
          s->old_generation == b->old_generation && s->new_generation == b->new_generation &&
          s->seal_epoch == b->seal_epoch && s->control_fence == b->fence;
}

static kb_vault_operator_result_t db_result(kb_vault_mutation_db_result_t result)
{
   switch (result)
   {
   case KB_VAULT_MUTATION_DB_BUSY:
      return KB_VAULT_OPERATOR_RESULT_BUSY;
   case KB_VAULT_MUTATION_DB_TRANSIENT:
      return KB_VAULT_OPERATOR_RESULT_SAFE_RETRY;
   case KB_VAULT_MUTATION_DB_UNSUPPORTED:
      return KB_VAULT_OPERATOR_RESULT_UNSUPPORTED;
   case KB_VAULT_MUTATION_DB_INVALID:
      return KB_VAULT_OPERATOR_RESULT_INVALID_STATE;
   case KB_VAULT_MUTATION_DB_INTEGRITY:
   case KB_VAULT_MUTATION_DB_NOT_FOUND:
   case KB_VAULT_MUTATION_DB_OK:
   default:
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   }
}

static kb_vault_operator_result_t auth_result(kb_vault_mutation_auth_result_t result)
{
   switch (result)
   {
   case KB_VAULT_MUTATION_AUTH_WRONG_SECRET:
      return KB_VAULT_OPERATOR_RESULT_WRONG_SECRET;
   case KB_VAULT_MUTATION_AUTH_BACKEND_UNAVAILABLE:
      return KB_VAULT_OPERATOR_RESULT_BACKEND_UNAVAILABLE;
   case KB_VAULT_MUTATION_AUTH_UNSUPPORTED:
      return KB_VAULT_OPERATOR_RESULT_UNSUPPORTED;
   case KB_VAULT_MUTATION_AUTH_INTEGRITY:
   case KB_VAULT_MUTATION_AUTHORIZED:
   default:
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   }
}

static kb_vault_operator_result_t status_refusal(const kb_vault_operator_status_t *s)
{
   switch (s->state)
   {
   case KB_VAULT_OPERATOR_STATE_RECOVERY_REQUIRED:
      return KB_VAULT_OPERATOR_RESULT_RECOVERY_REQUIRED;
   case KB_VAULT_OPERATOR_STATE_BACKEND_UNAVAILABLE:
      return KB_VAULT_OPERATOR_RESULT_BACKEND_UNAVAILABLE;
   case KB_VAULT_OPERATOR_STATE_INTEGRITY_FAILURE:
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   default:
      return KB_VAULT_OPERATOR_RESULT_INVALID_STATE;
   }
}

static kb_vault_operator_result_t step_result(kb_vault_mutation_step_result_t result)
{
   switch (result)
   {
   case KB_VAULT_MUTATION_STEP_SAFE_RETRY:
      return KB_VAULT_OPERATOR_RESULT_SAFE_RETRY;
   case KB_VAULT_MUTATION_STEP_BUSY:
      return KB_VAULT_OPERATOR_RESULT_BUSY;
   case KB_VAULT_MUTATION_STEP_ABORTED:
      return KB_VAULT_OPERATOR_RESULT_INVALID_STATE;
   case KB_VAULT_MUTATION_STEP_RECOVERY_REQUIRED:
      return KB_VAULT_OPERATOR_RESULT_RECOVERY_REQUIRED;
   case KB_VAULT_MUTATION_STEP_INTEGRITY:
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   case KB_VAULT_MUTATION_STEP_INVALID:
      return KB_VAULT_OPERATOR_RESULT_INVALID_STATE;
   case KB_VAULT_MUTATION_STEP_UNSUPPORTED:
      return KB_VAULT_OPERATOR_RESULT_UNSUPPORTED;
   case KB_VAULT_MUTATION_STEP_BACKEND_UNAVAILABLE:
      return KB_VAULT_OPERATOR_RESULT_BACKEND_UNAVAILABLE;
   case KB_VAULT_MUTATION_STEP_COMPLETED:
   default:
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   }
}

static void fail_closed(kb_vault_operator_mutation_t *m)
{
   if (m->activation_mutex_initialized)
      pthread_mutex_lock(&m->activation_mutex);
   m->pending_activation_valid = 0;
   m->activation_transition = 0;
   memset(&m->pending_activation, 0, sizeof(m->pending_activation));
   memset(&m->activation_deadline, 0, sizeof(m->activation_deadline));
   if (m->activation_mutex_initialized)
      pthread_mutex_unlock(&m->activation_mutex);
   if (m->deps.fail_closed_seal)
      m->deps.fail_closed_seal(m->opaque);
}

static kb_vault_operator_result_t stage_operational(kb_vault_operator_mutation_t *m)
{
   kb_vault_operator_status_t status = {0};
   if (m->deps.read_status(&status, m->opaque) != 0 ||
       !kb_vault_operator_status_validate(&status) ||
       status.state != KB_VAULT_OPERATOR_STATE_OPERATIONAL ||
       m->deps.singleton_revalidate(m->opaque) != 0)
   {
      fail_closed(m);
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   }
   pthread_mutex_lock(&m->activation_mutex);
   if (!m->general_serving)
   {
      if (deadline_after_ms(&m->activation_deadline, KB_VAULT_ACTIVATION_PUBLISH_WINDOW_MS) != 0)
      {
         pthread_mutex_unlock(&m->activation_mutex);
         fail_closed(m);
         return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
      }
      m->pending_activation = status;
      m->pending_activation_valid = 1;
      m->activation_transition = 1;
   }
   pthread_mutex_unlock(&m->activation_mutex);
   return KB_VAULT_OPERATOR_RESULT_OPERATIONAL;
}

static kb_vault_operator_result_t finalize(kb_vault_operator_mutation_t *m,
                                           const kb_vault_mutation_binding_t *binding,
                                           const kb_vault_operator_status_t *status,
                                           const uint8_t *secret, size_t secret_len)
{
   kb_vault_operator_result_t r =
       m->deps.finalize_completed(binding, status, secret, secret_len, m->opaque);
   if (r < KB_VAULT_OPERATOR_RESULT_OPERATIONAL || r > KB_VAULT_OPERATOR_RESULT_UNSUPPORTED)
   {
      fail_closed(m);
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   }
   return r == KB_VAULT_OPERATOR_RESULT_OPERATIONAL ? stage_operational(m) : r;
}

static kb_vault_operator_result_t route_binding(kb_vault_operator_mutation_t *m,
                                                const kb_vault_mutation_binding_t *binding,
                                                const uint8_t request_id[16],
                                                const kb_vault_operator_status_t *status,
                                                const uint8_t *secret, size_t secret_len)
{
   if (!binding_valid(binding))
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   if (request_id && CRYPTO_memcmp(request_id, binding->request_id, 16) != 0)
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;

   if (binding->state == KB_VAULT_MUTATION_BINDING_ABORTED)
      return KB_VAULT_OPERATOR_RESULT_INVALID_STATE;
   if (binding->state == KB_VAULT_MUTATION_BINDING_RECOVERY_REQUIRED)
      return status->state == KB_VAULT_OPERATOR_STATE_RECOVERY_REQUIRED &&
                     same_operation(status, binding)
                 ? KB_VAULT_OPERATOR_RESULT_RECOVERY_REQUIRED
                 : KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;

   if (binding->state == KB_VAULT_MUTATION_BINDING_OPENED)
      return status->state == KB_VAULT_OPERATOR_STATE_OPERATIONAL
                 ? stage_operational(m)
                 : KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;

   if (!same_operation(status, binding))
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;

   if (status->state == KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED &&
       (binding->state == KB_VAULT_MUTATION_BINDING_ACTIVE ||
        binding->state == KB_VAULT_MUTATION_BINDING_COMPLETED))
      return finalize(m, binding, status, secret, secret_len);

   if (binding->state != KB_VAULT_MUTATION_BINDING_ACTIVE ||
       status->state != KB_VAULT_OPERATOR_STATE_RESUME_REQUIRED)
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;

   kb_vault_mutation_step_result_t step = m->deps.run_reseal(
       KB_VAULT_MUTATION_RESEAL_RESUME, request_id ? request_id : binding->request_id, binding,
       secret, secret_len, m->opaque);
   if (step != KB_VAULT_MUTATION_STEP_COMPLETED)
      return step_result(step);
   kb_vault_operator_status_t completed = {0};
   if (m->deps.read_status(&completed, m->opaque) != 0 ||
       !kb_vault_operator_status_validate(&completed) ||
       completed.state != KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED ||
       !same_operation(&completed, binding))
   {
      fail_closed(m);
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   }
   return finalize(m, binding, &completed, secret, secret_len);
}

static kb_vault_operator_result_t execute_start(kb_vault_operator_mutation_t *m,
                                                const uint8_t request_id[16], const uint8_t *secret,
                                                size_t secret_len)
{
   kb_vault_mutation_binding_t binding = {0};
   kb_vault_mutation_db_result_t lookup = m->deps.start_lookup(request_id, 0, &binding, m->opaque);
   if (lookup != KB_VAULT_MUTATION_DB_OK && lookup != KB_VAULT_MUTATION_DB_NOT_FOUND)
      return db_result(lookup);

   if (m->deps.singleton_revalidate(m->opaque) != 0)
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   kb_vault_operator_status_t status = {0};
   if (m->deps.read_status(&status, m->opaque) != 0 || !kb_vault_operator_status_validate(&status))
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   uint64_t generation = 0;
   kb_vault_mutation_auth_result_t auth = m->deps.authorization_preflight(
       KB_VAULT_OPERATOR_OPCODE_START, &status, secret, secret_len, &generation, m->opaque);
   if (auth != KB_VAULT_MUTATION_AUTHORIZED)
      return auth_result(auth);
   if (!generation || generation > INT64_MAX)
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;

   if (lookup == KB_VAULT_MUTATION_DB_OK)
   {
      if (CRYPTO_memcmp(binding.request_id, request_id, 16) != 0)
         return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
      return route_binding(m, &binding, request_id, &status, secret, secret_len);
   }

   if (status.state != KB_VAULT_OPERATOR_STATE_OPERATIONAL)
   {
      memset(&binding, 0, sizeof(binding));
      lookup = m->deps.start_lookup(request_id, 1, &binding, m->opaque);
      if (lookup == KB_VAULT_MUTATION_DB_OK)
      {
         kb_vault_operator_status_t replay = {0};
         if (CRYPTO_memcmp(binding.request_id, request_id, 16) != 0)
            return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
         /* The locked lookup may have observed a concurrent winner whose
          * durable transition happened after the snapshot above.  Route only
          * against a snapshot taken after that lookup. */
         if (m->deps.read_status(&replay, m->opaque) != 0 ||
             !kb_vault_operator_status_validate(&replay))
            return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
         return route_binding(m, &binding, request_id, &replay, secret, secret_len);
      }
      if (lookup != KB_VAULT_MUTATION_DB_NOT_FOUND)
         return db_result(lookup);
      return status_refusal(&status);
   }
   if (generation == UINT64_MAX || generation >= INT64_MAX)
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;

   uint8_t candidate[16] = {0};
   if (m->deps.random_operation_id(candidate, m->opaque) != 0 || all_zero(candidate, 16))
   {
      OPENSSL_cleanse(candidate, sizeof(candidate));
      return KB_VAULT_OPERATOR_RESULT_BACKEND_UNAVAILABLE;
   }
   int created = 0;
   memset(&binding, 0, sizeof(binding));
   kb_vault_mutation_db_result_t reserved = m->deps.start_reserve(
       request_id, candidate, generation, generation + 1, &binding, &created, m->opaque);
   OPENSSL_cleanse(candidate, sizeof(candidate));
   if (reserved != KB_VAULT_MUTATION_DB_OK)
      return db_result(reserved);
   if (!binding_valid(&binding) || (created != 0 && created != 1) ||
       CRYPTO_memcmp(binding.request_id, request_id, 16) != 0 ||
       binding.old_generation != generation || binding.new_generation != generation + 1)
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   if (!created)
   {
      kb_vault_operator_status_t replay = {0};
      if (m->deps.read_status(&replay, m->opaque) != 0 ||
          !kb_vault_operator_status_validate(&replay))
         return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
      return route_binding(m, &binding, request_id, &replay, secret, secret_len);
   }
   kb_vault_mutation_step_result_t step = m->deps.run_reseal(
       KB_VAULT_MUTATION_RESEAL_START, request_id, &binding, secret, secret_len, m->opaque);
   if (step != KB_VAULT_MUTATION_STEP_COMPLETED)
      return step_result(step);
   kb_vault_operator_status_t completed = {0};
   if (m->deps.read_status(&completed, m->opaque) != 0 ||
       !kb_vault_operator_status_validate(&completed) ||
       completed.state != KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED ||
       !same_operation(&completed, &binding))
   {
      fail_closed(m);
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   }
   return finalize(m, &binding, &completed, secret, secret_len);
}

static kb_vault_operator_result_t execute_resume(kb_vault_operator_mutation_t *m,
                                                 const uint8_t *secret, size_t secret_len)
{
   if (m->deps.singleton_revalidate(m->opaque) != 0)
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   kb_vault_operator_status_t status = {0};
   if (m->deps.read_status(&status, m->opaque) != 0 || !kb_vault_operator_status_validate(&status))
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   uint64_t generation = 0;
   kb_vault_mutation_auth_result_t auth = m->deps.authorization_preflight(
       KB_VAULT_OPERATOR_OPCODE_RESUME, &status, secret, secret_len, &generation, m->opaque);
   if (auth != KB_VAULT_MUTATION_AUTHORIZED)
      return auth_result(auth);
   if (!generation || generation > INT64_MAX)
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   if (status.state != KB_VAULT_OPERATOR_STATE_RESUME_REQUIRED &&
       status.state != KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED)
      return status_refusal(&status);

   kb_vault_mutation_binding_t binding = {0};
   kb_vault_mutation_db_result_t found = status.state == KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED
                                             ? m->deps.lookup_completed(&binding, m->opaque)
                                             : m->deps.discover_active(&binding, m->opaque);
   if (found != KB_VAULT_MUTATION_DB_OK)
      return db_result(found);
   return route_binding(m, &binding, NULL, &status, secret, secret_len);
}

static kb_vault_operator_result_t execute_unseal(kb_vault_operator_mutation_t *m,
                                                 const uint8_t *secret, size_t secret_len)
{
   if (m->deps.singleton_revalidate(m->opaque) != 0)
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   kb_vault_operator_status_t status = {0};
   if (m->deps.read_status(&status, m->opaque) != 0 || !kb_vault_operator_status_validate(&status))
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   uint64_t generation = 0;
   kb_vault_mutation_auth_result_t auth = m->deps.authorization_preflight(
       KB_VAULT_OPERATOR_OPCODE_UNSEAL, &status, secret, secret_len, &generation, m->opaque);
   if (auth != KB_VAULT_MUTATION_AUTHORIZED)
      return auth_result(auth);
   if (!generation || generation > INT64_MAX)
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;

   kb_vault_operator_result_t result;
   if (status.state == KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED)
   {
      kb_vault_mutation_binding_t binding = {0};
      kb_vault_mutation_db_result_t found = m->deps.lookup_completed(&binding, m->opaque);
      if (found != KB_VAULT_MUTATION_DB_OK)
         return db_result(found);
      return route_binding(m, &binding, NULL, &status, secret, secret_len);
   }
   else if (status.state == KB_VAULT_OPERATOR_STATE_SEALED_IDLE)
      result = m->deps.unseal_idle(&status, secret, secret_len, m->opaque);
   else if (status.state == KB_VAULT_OPERATOR_STATE_LOCAL_UNSEAL_REQUIRED)
      result = m->deps.unseal_local(&status, secret, secret_len, m->opaque);
   else
      return status_refusal(&status);
   if (result < KB_VAULT_OPERATOR_RESULT_OPERATIONAL ||
       result > KB_VAULT_OPERATOR_RESULT_UNSUPPORTED)
   {
      fail_closed(m);
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   }
   return result == KB_VAULT_OPERATOR_RESULT_OPERATIONAL ? stage_operational(m) : result;
}

int kb_vault_operator_mutation_init(kb_vault_operator_mutation_t *m,
                                    const kb_vault_operator_mutation_deps_t *d, void *opaque)
{
   if (!m || !d || !d->read_status || !d->singleton_revalidate || !d->authorization_preflight ||
       !d->start_lookup || !d->start_reserve || !d->discover_active || !d->lookup_completed ||
       !d->random_operation_id || !d->run_reseal || !d->finalize_completed || !d->unseal_idle ||
       !d->unseal_local || !d->publish_activation || !d->fail_closed_seal)
      return -1;
   memset(m, 0, sizeof(*m));
   m->deps = *d;
   m->opaque = opaque;
   if (pthread_mutex_init(&m->activation_mutex, NULL) != 0)
   {
      OPENSSL_cleanse(m, sizeof(*m));
      return -1;
   }
   m->activation_mutex_initialized = 1;
   return 0;
}

void kb_vault_operator_mutation_destroy(kb_vault_operator_mutation_t *m)
{
   if (!m)
      return;
   pthread_mutex_lock(&m->activation_mutex);
   int incomplete_activation = m->pending_activation_valid || m->activation_transition;
   pthread_mutex_unlock(&m->activation_mutex);
   if (incomplete_activation)
      fail_closed(m);
   if (m->activation_mutex_initialized)
      pthread_mutex_destroy(&m->activation_mutex);
   OPENSSL_cleanse(m, sizeof(*m));
}

int kb_vault_operator_mutation_execute(kb_vault_operator_opcode_t opcode,
                                       const uint8_t request_id[16], const uint8_t *secret,
                                       size_t secret_len, kb_vault_operator_result_t *result,
                                       void *opaque)
{
   kb_vault_operator_mutation_t *m = opaque;
   if (!m || !result || !secret || !secret_len || secret_len > KB_VAULT_OPERATOR_SECRET_MAX ||
       (opcode == KB_VAULT_OPERATOR_OPCODE_START && !request_id) ||
       (opcode != KB_VAULT_OPERATOR_OPCODE_START && request_id) ||
       (opcode != KB_VAULT_OPERATOR_OPCODE_START && opcode != KB_VAULT_OPERATOR_OPCODE_RESUME &&
        opcode != KB_VAULT_OPERATOR_OPCODE_UNSEAL))
      return -1;
   pthread_mutex_lock(&m->activation_mutex);
   int admission_blocked = m->pending_activation_valid || m->activation_transition;
   pthread_mutex_unlock(&m->activation_mutex);
   if (admission_blocked)
   {
      *result = KB_VAULT_OPERATOR_RESULT_BUSY;
      return 0;
   }
   memset(&m->pending_activation, 0, sizeof(m->pending_activation));
   vault_mutation_budget_t budget;
   if (vault_mutation_budget_init(&budget) != 0 || vault_mutation_budget_enter(&budget) != 0)
   {
      *result = KB_VAULT_OPERATOR_RESULT_BACKEND_UNAVAILABLE;
      return 0;
   }
   switch (opcode)
   {
   case KB_VAULT_OPERATOR_OPCODE_START:
      *result = execute_start(m, request_id, secret, secret_len);
      break;
   case KB_VAULT_OPERATOR_OPCODE_RESUME:
      *result = execute_resume(m, secret, secret_len);
      break;
   case KB_VAULT_OPERATOR_OPCODE_UNSEAL:
      *result = execute_unseal(m, secret, secret_len);
      break;
   default:
      (void)vault_mutation_budget_leave(&budget);
      return -1;
   }
   if (vault_mutation_budget_leave(&budget) != 0)
   {
      fail_closed(m);
      *result = KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   }
   return 0;
}

int kb_vault_operator_mutation_after_secret_wipe(kb_vault_operator_mutation_t *m)
{
   if (!m)
      return -1;
   pthread_mutex_lock(&m->activation_mutex);
   int pending = m->pending_activation_valid;
   int within_window = !pending || deadline_valid(&m->activation_deadline);
   pthread_mutex_unlock(&m->activation_mutex);
   if (!pending)
      return 0;
   if (!within_window)
   {
      fail_closed(m);
      return -1;
   }
   kb_vault_operator_status_t status = {0};
   if (m->deps.read_status(&status, m->opaque) != 0 ||
       !kb_vault_operator_status_validate(&status) ||
       status.state != KB_VAULT_OPERATOR_STATE_OPERATIONAL ||
       !same_status(&status, &m->pending_activation) ||
       m->deps.singleton_revalidate(m->opaque) != 0)
   {
      fail_closed(m);
      return -1;
   }
   /* Keep the transition mutex across publication. The latch may wake the
    * main thread inside publish_activation; it must not observe the latch
    * before pending_activation_valid is cleared. */
   pthread_mutex_lock(&m->activation_mutex);
   if (!deadline_valid(&m->activation_deadline) ||
       m->deps.publish_activation(&status, m->opaque) != 0 ||
       !deadline_valid(&m->activation_deadline))
   {
      pthread_mutex_unlock(&m->activation_mutex);
      fail_closed(m);
      return -1;
   }
   m->pending_activation_valid = 0;
   memset(&m->pending_activation, 0, sizeof(m->pending_activation));
   pthread_mutex_unlock(&m->activation_mutex);
   return 0;
}

int kb_vault_operator_mutation_activation_window_valid(kb_vault_operator_mutation_t *m)
{
   if (!m || !m->activation_mutex_initialized)
      return -1;
   pthread_mutex_lock(&m->activation_mutex);
   int valid = m->activation_transition && !m->pending_activation_valid &&
               deadline_valid(&m->activation_deadline);
   pthread_mutex_unlock(&m->activation_mutex);
   return valid ? 0 : -1;
}

int kb_vault_operator_mutation_mark_general_serving(kb_vault_operator_mutation_t *m)
{
   if (!m || !m->activation_mutex_initialized)
      return -1;
   pthread_mutex_lock(&m->activation_mutex);
   int valid = m->general_serving || (!m->pending_activation_valid && !m->activation_transition) ||
               (m->activation_transition && !m->pending_activation_valid &&
                deadline_valid(&m->activation_deadline));
   if (valid)
   {
      m->general_serving = 1;
      m->activation_transition = 0;
      memset(&m->activation_deadline, 0, sizeof(m->activation_deadline));
   }
   pthread_mutex_unlock(&m->activation_mutex);
   if (!valid)
   {
      fail_closed(m);
      return -1;
   }
   return 0;
}
