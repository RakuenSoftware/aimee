#include "kb/kb_vault_operator_mutation.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   kb_vault_operator_status_t status;
   kb_vault_mutation_db_result_t lookup_result;
   kb_vault_mutation_binding_t binding;
   int created;
   kb_vault_mutation_auth_result_t auth;
   kb_vault_mutation_step_result_t step;
   kb_vault_operator_result_t effect_result;
   int lookups, locked_lookups, reserves, reseals, finalizes, idle, local;
   int publishes, seals, singleton_calls, auth_calls;
} fixture_t;

/* The production validator is independently covered by the status suite. */
int kb_vault_operator_status_validate(const kb_vault_operator_status_t *s)
{
   return s && s->seal_epoch && s->control_fence;
}

static void operational(kb_vault_operator_status_t *s)
{
   memset(s, 0, sizeof(*s));
   s->state = KB_VAULT_OPERATOR_STATE_OPERATIONAL;
   s->remediation = KB_VAULT_OPERATOR_REMEDIATION_NONE;
   s->seal_epoch = 4;
   s->control_fence = 8;
}

static void completed(fixture_t *f)
{
   memset(&f->status, 0, sizeof(f->status));
   f->status.state = KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED;
   f->status.operation_state = KB_VAULT_OPERATOR_OPERATION_COMPLETED;
   f->status.remediation = KB_VAULT_OPERATOR_REMEDIATION_FINALIZE;
   f->status.flags = 1;
   f->status.seal_epoch = f->binding.seal_epoch;
   f->status.control_fence = f->binding.fence;
   f->status.old_generation = f->binding.old_generation;
   f->status.new_generation = f->binding.new_generation;
   memcpy(f->status.operation_id, f->binding.operation_id, 16);
}

static void active(fixture_t *f)
{
   completed(f);
   f->status.state = KB_VAULT_OPERATOR_STATE_RESUME_REQUIRED;
   f->status.operation_state = KB_VAULT_OPERATOR_OPERATION_PREPARING;
   f->status.remediation = KB_VAULT_OPERATOR_REMEDIATION_RESUME;
}

static int read_status(kb_vault_operator_status_t *s, void *p)
{
   *s = ((fixture_t *)p)->status;
   return 0;
}

static int singleton(void *p)
{
   ((fixture_t *)p)->singleton_calls++;
   return 0;
}

static kb_vault_mutation_auth_result_t auth(kb_vault_operator_opcode_t opcode,
                                            const kb_vault_operator_status_t *status,
                                            const uint8_t *secret, size_t secret_len,
                                            uint64_t *generation, void *p)
{
   fixture_t *f = p;
   (void)opcode;
   (void)status;
   assert(secret && secret_len == 2);
   f->auth_calls++;
   *generation = 7;
   return f->auth;
}

static kb_vault_mutation_db_result_t lookup(const uint8_t request[16], int locked,
                                            kb_vault_mutation_binding_t *binding, void *p)
{
   fixture_t *f = p;
   assert(request);
   f->lookups++;
   f->locked_lookups += locked;
   if (f->lookup_result == KB_VAULT_MUTATION_DB_OK)
      *binding = f->binding;
   return f->lookup_result;
}

static kb_vault_mutation_db_result_t reserve(const uint8_t request[16], const uint8_t candidate[16],
                                             uint64_t old_generation, uint64_t new_generation,
                                             kb_vault_mutation_binding_t *binding, int *created,
                                             void *p)
{
   fixture_t *f = p;
   assert(request && candidate && old_generation == 7 && new_generation == 8);
   f->reserves++;
   *binding = f->binding;
   *created = f->created;
   return KB_VAULT_MUTATION_DB_OK;
}

static kb_vault_mutation_db_result_t discover(kb_vault_mutation_binding_t *binding, void *p)
{
   fixture_t *f = p;
   *binding = f->binding;
   return f->lookup_result;
}

static int random_id(uint8_t out[16], void *p)
{
   (void)p;
   memset(out, 0xa5, 16);
   return 0;
}

static kb_vault_mutation_step_result_t reseal(kb_vault_mutation_reseal_mode_t mode,
                                              const uint8_t request[16],
                                              const kb_vault_mutation_binding_t *binding,
                                              const uint8_t *secret, size_t secret_len, void *p)
{
   fixture_t *f = p;
   (void)binding;
   assert(mode == KB_VAULT_MUTATION_RESEAL_START || mode == KB_VAULT_MUTATION_RESEAL_RESUME);
   assert(request && secret && secret_len == 2);
   f->reseals++;
   if (f->step == KB_VAULT_MUTATION_STEP_COMPLETED)
      completed(f);
   return f->step;
}

static kb_vault_operator_result_t finalize_effect(const kb_vault_mutation_binding_t *binding,
                                                  const kb_vault_operator_status_t *status,
                                                  const uint8_t *secret, size_t secret_len, void *p)
{
   fixture_t *f = p;
   assert(binding && status && secret && secret_len == 2);
   f->finalizes++;
   if (f->effect_result == KB_VAULT_OPERATOR_RESULT_OPERATIONAL)
      operational(&f->status);
   return f->effect_result;
}

static kb_vault_operator_result_t idle_effect(const kb_vault_operator_status_t *status,
                                              const uint8_t *secret, size_t secret_len, void *p)
{
   fixture_t *f = p;
   assert(status && secret && secret_len == 2);
   f->idle++;
   if (f->effect_result == KB_VAULT_OPERATOR_RESULT_OPERATIONAL)
      operational(&f->status);
   return f->effect_result;
}

static kb_vault_operator_result_t local_effect(const kb_vault_operator_status_t *status,
                                               const uint8_t *secret, size_t secret_len, void *p)
{
   fixture_t *f = p;
   assert(status && secret && secret_len == 2);
   f->local++;
   if (f->effect_result == KB_VAULT_OPERATOR_RESULT_OPERATIONAL)
      operational(&f->status);
   return f->effect_result;
}

static int publish(const kb_vault_operator_status_t *status, void *p)
{
   fixture_t *f = p;
   assert(status->state == KB_VAULT_OPERATOR_STATE_OPERATIONAL);
   f->publishes++;
   return 0;
}

static void seal(void *p)
{
   ((fixture_t *)p)->seals++;
}

static void setup(fixture_t *f, kb_vault_operator_mutation_t *m)
{
   memset(f, 0, sizeof(*f));
   f->auth = KB_VAULT_MUTATION_AUTHORIZED;
   f->lookup_result = KB_VAULT_MUTATION_DB_NOT_FOUND;
   f->created = 1;
   f->step = KB_VAULT_MUTATION_STEP_COMPLETED;
   f->effect_result = KB_VAULT_OPERATOR_RESULT_OPERATIONAL;
   operational(&f->status);
   f->binding.state = KB_VAULT_MUTATION_BINDING_ACTIVE;
   f->binding.request_id[0] = 1;
   memset(f->binding.operation_id, 0x44, 16);
   f->binding.old_generation = 7;
   f->binding.new_generation = 8;
   f->binding.seal_epoch = 5;
   f->binding.fence = 9;
   kb_vault_operator_mutation_deps_t deps = {
       .read_status = read_status,
       .singleton_revalidate = singleton,
       .authorization_preflight = auth,
       .start_lookup = lookup,
       .start_reserve = reserve,
       .discover_active = discover,
       .lookup_completed = discover,
       .random_operation_id = random_id,
       .run_reseal = reseal,
       .finalize_completed = finalize_effect,
       .unseal_idle = idle_effect,
       .unseal_local = local_effect,
       .publish_activation = publish,
       .fail_closed_seal = seal,
   };
   assert(kb_vault_operator_mutation_init(m, &deps, f) == 0);
}

static kb_vault_operator_result_t execute(kb_vault_operator_mutation_t *m,
                                          kb_vault_operator_opcode_t opcode)
{
   static const uint8_t request[16] = {1};
   static const uint8_t secret[2] = {'p', 'w'};
   kb_vault_operator_result_t result = KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   assert(kb_vault_operator_mutation_execute(
              opcode, opcode == KB_VAULT_OPERATOR_OPCODE_START ? request : NULL, secret,
              sizeof(secret), &result, m) == 0);
   return result;
}

static void test_start_and_publish(void)
{
   fixture_t f;
   kb_vault_operator_mutation_t m;
   setup(&f, &m);
   assert(execute(&m, KB_VAULT_OPERATOR_OPCODE_START) == KB_VAULT_OPERATOR_RESULT_OPERATIONAL);
   assert(f.lookups == 1 && f.reserves == 1 && f.reseals == 1 && f.finalizes == 1);
   assert(f.publishes == 0); /* The ingress arena still exists. */
   assert(kb_vault_operator_mutation_after_secret_wipe(&m) == 0);
   assert(f.publishes == 1);
   kb_vault_operator_mutation_destroy(&m);
}

static void test_wrong_secret_is_effectless(void)
{
   fixture_t f;
   kb_vault_operator_mutation_t m;
   setup(&f, &m);
   f.auth = KB_VAULT_MUTATION_AUTH_WRONG_SECRET;
   assert(execute(&m, KB_VAULT_OPERATOR_OPCODE_START) == KB_VAULT_OPERATOR_RESULT_WRONG_SECRET);
   assert(f.reserves == 0 && f.reseals == 0 && f.finalizes == 0 && f.seals == 0);
   kb_vault_operator_mutation_destroy(&m);
}

static void test_nonoperational_relookup(void)
{
   fixture_t f;
   kb_vault_operator_mutation_t m;
   setup(&f, &m);
   f.status.state = KB_VAULT_OPERATOR_STATE_SEALED_IDLE;
   f.status.remediation = KB_VAULT_OPERATOR_REMEDIATION_UNSEAL;
   assert(execute(&m, KB_VAULT_OPERATOR_OPCODE_START) == KB_VAULT_OPERATOR_RESULT_INVALID_STATE);
   assert(f.lookups == 2 && f.locked_lookups == 1 && f.reserves == 0);
   kb_vault_operator_mutation_destroy(&m);
}

static void test_resume_exact_active(void)
{
   fixture_t f;
   kb_vault_operator_mutation_t m;
   setup(&f, &m);
   f.lookup_result = KB_VAULT_MUTATION_DB_OK;
   active(&f);
   assert(execute(&m, KB_VAULT_OPERATOR_OPCODE_RESUME) == KB_VAULT_OPERATOR_RESULT_OPERATIONAL);
   assert(f.reseals == 1 && f.finalizes == 1);
   assert(kb_vault_operator_mutation_after_secret_wipe(&m) == 0);
   kb_vault_operator_mutation_destroy(&m);
}

static void test_resume_mismatch(void)
{
   fixture_t f;
   kb_vault_operator_mutation_t m;
   setup(&f, &m);
   f.lookup_result = KB_VAULT_MUTATION_DB_OK;
   active(&f);
   f.status.operation_id[0] ^= 1;
   assert(execute(&m, KB_VAULT_OPERATOR_OPCODE_RESUME) ==
          KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE);
   assert(f.reseals == 0 && f.finalizes == 0);
   kb_vault_operator_mutation_destroy(&m);
}

static void test_start_request_binding_mismatch(void)
{
   fixture_t f;
   kb_vault_operator_mutation_t m;
   setup(&f, &m);
   f.lookup_result = KB_VAULT_MUTATION_DB_OK;
   f.binding.request_id[0] = 2;
   assert(execute(&m, KB_VAULT_OPERATOR_OPCODE_START) ==
          KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE);
   assert(f.reseals == 0 && f.finalizes == 0);
   kb_vault_operator_mutation_destroy(&m);
}

static void test_unseal_shapes(void)
{
   fixture_t f;
   kb_vault_operator_mutation_t m;
   setup(&f, &m);
   f.status.state = KB_VAULT_OPERATOR_STATE_SEALED_IDLE;
   f.status.remediation = KB_VAULT_OPERATOR_REMEDIATION_UNSEAL;
   assert(execute(&m, KB_VAULT_OPERATOR_OPCODE_UNSEAL) == KB_VAULT_OPERATOR_RESULT_OPERATIONAL);
   assert(f.idle == 1 && f.local == 0 && f.publishes == 0);
   assert(kb_vault_operator_mutation_after_secret_wipe(&m) == 0 && f.publishes == 1);
   kb_vault_operator_mutation_destroy(&m);

   setup(&f, &m);
   f.status.state = KB_VAULT_OPERATOR_STATE_LOCAL_UNSEAL_REQUIRED;
   f.status.remediation = KB_VAULT_OPERATOR_REMEDIATION_UNSEAL;
   assert(execute(&m, KB_VAULT_OPERATOR_OPCODE_UNSEAL) == KB_VAULT_OPERATOR_RESULT_OPERATIONAL);
   assert(f.local == 1 && f.idle == 0);
   kb_vault_operator_mutation_destroy(&m); /* Pending activation seals closed. */
   assert(f.seals == 1);
}

int main(void)
{
   test_start_and_publish();
   test_wrong_secret_is_effectless();
   test_nonoperational_relookup();
   test_resume_exact_active();
   test_resume_mismatch();
   test_start_request_binding_mismatch();
   test_unseal_shapes();
   puts("kb vault operator choreography tests: ok");
   return 0;
}
