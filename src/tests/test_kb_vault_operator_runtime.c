#include "kb/kb_vault_operator_runtime.h"

#include <assert.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   kb_vault_operator_status_t status;
   int local_unsealed;
   int publishes;
   int seals;
   int exact_preflights;
   int current_generation_reads;
   int open_calls;
   int event_reads;
   int guard_ends;
   int dispatch_found;
   db2_vault_operator_rewrap_binding_t dispatch_row;
   db2_vault_operator_open_result_t opened;
   db2_vault_operator_open_event_t event;
} fixture_t;

static fixture_t fixture;

/* Production defaults are not exercised by this injected binary. */
const db2_vault_rewrap_ops_t db2_vault_operator_rewrap_ops = {0};
const vault_reseal_custody_ops_t vault_reseal_custody_default_ops = {0};
int db2_vault_operator_rewrap_bind(db2_vault_operator_runtime_t *runtime)
{
   (void)runtime;
   return 0;
}
void db2_vault_operator_rewrap_unbind(db2_vault_operator_runtime_t *runtime)
{
   (void)runtime;
}

int kb_vault_operator_status_validate(const kb_vault_operator_status_t *status)
{
   return status && status->seal_epoch && status->control_fence;
}

void db2_vault_rewrap_check_clear(db2_vault_rewrap_check_t *rows, size_t count)
{
   if (rows)
      memset(rows, 0, count * sizeof(*rows));
}

void db2_vault_rewrap_cursor_clear(db2_vault_rewrap_cursor_t *cursor)
{
   if (cursor)
      memset(cursor, 0, sizeof(*cursor));
}

static int read_status(db2_vault_operator_runtime_t *database, kb_vault_operator_status_t *out)
{
   assert(database == (void *)0x11);
   *out = fixture.status;
   return 0;
}

static int singleton(kb_vault_tpm_runtime_lock_t *lock)
{
   return lock == (void *)0x22 ? 0 : -1;
}

static int random_bytes(uint8_t *out, size_t length)
{
   memset(out, 0x42, length);
   return 0;
}

static vault_custody_auth_result_t preflight(const void *secret, size_t length, uint64_t generation)
{
   fixture.exact_preflights++;
   return length == 2 && memcmp(secret, "ok", 2) == 0 && generation == 7
              ? VAULT_CUSTODY_AUTHORIZED
              : VAULT_CUSTODY_AUTH_WRONG_SECRET;
}

static vault_custody_auth_result_t current_preflight(const void *secret, size_t length,
                                                     uint64_t *out)
{
   fixture.current_generation_reads++;
   if (length != 2 || memcmp(secret, "ok", 2) != 0)
      return VAULT_CUSTODY_AUTH_WRONG_SECRET;
   *out = 7;
   return VAULT_CUSTODY_AUTHORIZED;
}

static int dispatch(const uint8_t request[16], db2_vault_operator_rewrap_binding_t *out, int *found)
{
   (void)request;
   *found = fixture.dispatch_found;
   if (*found)
      *out = fixture.dispatch_row;
   return DB2_VAULT_REWRAP_OK;
}

static int reserve_row(const uint8_t a[16], const uint8_t b[16], int64_t c, int64_t d,
                       db2_vault_operator_rewrap_binding_t *e, int *f)
{
   (void)a;
   (void)b;
   (void)c;
   (void)d;
   (void)e;
   (void)f;
   return DB2_VAULT_REWRAP_INVALID;
}

static int active(db2_vault_operator_rewrap_binding_t *out, int *found)
{
   (void)out;
   *found = 0;
   return DB2_VAULT_REWRAP_OK;
}

static int completed(const uint8_t a[16], const uint8_t b[16], db2_vault_operator_completed_t *out)
{
   (void)a;
   (void)b;
   (void)out;
   return DB2_VAULT_REWRAP_INVALID;
}

static int completed_active(const uint8_t operation_id[16], db2_vault_operator_completed_t *out)
{
   (void)operation_id;
   (void)out;
   return DB2_VAULT_REWRAP_INVALID;
}

static int check_page(const db2_vault_rewrap_cursor_t *after, int limit,
                      db2_vault_rewrap_check_t *rows, size_t capacity, size_t *count,
                      db2_vault_rewrap_cursor_t *next, int64_t *total)
{
   (void)limit;
   (void)rows;
   (void)capacity;
   *count = 0;
   *next = *after;
   *total = 0;
   return DB2_VAULT_REWRAP_OK;
}

static int open_completed(const db2_vault_operator_completed_t *completed,
                          db2_vault_operator_open_result_t *out)
{
   (void)completed;
   (void)out;
   return DB2_VAULT_REWRAP_INVALID;
}

static int open_idle(const uint8_t request[16], int64_t epoch, int64_t fence, int64_t marker,
                     db2_vault_operator_open_result_t *out)
{
   assert(epoch == 5 && fence == 9 && marker == 0);
   fixture.open_calls++;
   memset(&fixture.opened, 0, sizeof(fixture.opened));
   fixture.opened.opened_epoch = 6;
   fixture.opened.opened_fence = 10;
   memset(fixture.opened.event_id, 0x51, 32);
   memset(fixture.opened.row_hash, 0x61, 32);
   fixture.event.opened = fixture.opened;
   memcpy(fixture.event.request_id, request, 16);
   *out = fixture.opened;
   fixture.status.state = KB_VAULT_OPERATOR_STATE_OPERATIONAL;
   fixture.status.remediation = KB_VAULT_OPERATOR_REMEDIATION_NONE;
   fixture.status.seal_epoch = 6;
   fixture.status.control_fence = 10;
   return DB2_VAULT_REWRAP_OK;
}

static int open_event(const uint8_t id[32], db2_vault_operator_open_event_t *out)
{
   assert(memcmp(id, fixture.event.opened.event_id, 32) == 0);
   fixture.event_reads++;
   *out = fixture.event;
   return DB2_VAULT_REWRAP_OK;
}

static vault_reseal_orchestrator_result_t
orchestrate(const vault_reseal_orchestrator_request_t *request,
            const vault_reseal_orchestrator_deps_t *deps, vault_reseal_orchestrator_output_t *out)
{
   (void)request;
   (void)deps;
   (void)out;
   return VAULT_RESEAL_ORCHESTRATOR_UNSUPPORTED;
}

static int receipt_decode(const uint8_t *wire, size_t length, vault_tpm2_reseal_receipt_t *out)
{
   (void)wire;
   (void)length;
   (void)out;
   return -1;
}

static int receipt_status(const vault_tpm2_reseal_receipt_t *receipt, const char *secret,
                          vault_tpm2_reseal_status_t *status)
{
   (void)receipt;
   (void)secret;
   (void)status;
   return -1;
}

static int receipt_cleanup(const vault_tpm2_reseal_receipt_t *receipt, const char *secret,
                           vault_tpm2_cleanup_authorization_t authorization)
{
   (void)receipt;
   (void)secret;
   (void)authorization;
   return -1;
}

static int guard_begin(vault_maintenance_guard_t **guard)
{
   *guard = (void *)0x33;
   return VAULT_MAINTENANCE_OK;
}

static int guard_sync(vault_maintenance_guard_t *guard, uint64_t epoch)
{
   assert(guard == (void *)0x33 && (epoch == 5 || epoch == 6));
   return VAULT_MAINTENANCE_OK;
}

static vault_custody_auth_result_t guard_unseal(vault_maintenance_guard_t *guard,
                                                const void *secret, size_t length)
{
   assert(guard == (void *)0x33 && length == 2 && memcmp(secret, "ok", 2) == 0);
   fixture.local_unsealed = 1;
   return VAULT_CUSTODY_AUTHORIZED;
}

static int guard_with(vault_maintenance_guard_t *guard, vault_maintenance_kek_fn callback,
                      void *opaque)
{
   uint8_t kek[VAULT_KEK_LEN];
   assert(guard == (void *)0x33);
   memset(kek, 0x77, sizeof(kek));
   return callback(kek, opaque);
}

static int guard_end(vault_maintenance_guard_t **guard)
{
   *guard = NULL;
   fixture.local_unsealed = 0;
   return VAULT_MAINTENANCE_OK;
}

static int guard_end_operational(vault_maintenance_guard_t **guard, uint64_t epoch)
{
   assert(epoch == 6);
   *guard = NULL;
   fixture.guard_ends++;
   return VAULT_MAINTENANCE_OK;
}

static vault_custody_local_status_t local_status(void)
{
   return fixture.local_unsealed ? VAULT_CUSTODY_LOCAL_AVAILABLE_UNSEALED
                                 : VAULT_CUSTODY_LOCAL_AVAILABLE_SEALED;
}

static int check_verify(const uint8_t kek[VAULT_KEK_LEN],
                        const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN])
{
   (void)kek;
   (void)wrapped;
   return 0;
}

static int seal(void)
{
   fixture.seals++;
   fixture.local_unsealed = 0;
   return 0;
}

static int publish(kb_vault_activation_latch_t *latch, const kb_vault_operator_status_t *status)
{
   assert(latch == (void *)0x44 && status->state == KB_VAULT_OPERATOR_STATE_OPERATIONAL);
   fixture.publishes++;
   return 0;
}

static const kb_vault_operator_runtime_platform_t platform = {
    .read_status = read_status,
    .singleton_revalidate = singleton,
    .random = random_bytes,
    .authorization_preflight = preflight,
    .authorization_preflight_current = current_preflight,
    .dispatch = dispatch,
    .reserve = reserve_row,
    .active = active,
    .completed = completed,
    .completed_active = completed_active,
    .current_check_page = check_page,
    .open_completed = open_completed,
    .open_idle = open_idle,
    .open_event = open_event,
    .orchestrator_run = orchestrate,
    .receipt_decode = receipt_decode,
    .receipt_status = receipt_status,
    .receipt_cleanup = receipt_cleanup,
    .guard_begin = guard_begin,
    .guard_sync = guard_sync,
    .guard_unseal = guard_unseal,
    .guard_with_kek = guard_with,
    .guard_end = guard_end,
    .guard_end_operational = guard_end_operational,
    .local_status = local_status,
    .kek_check_verify = check_verify,
    .seal = seal,
    .publish = publish,
};

static void opened_event_id(const uint8_t operation_id[16], uint8_t out[32])
{
   static const char domain[] = "aimee-vault-open-completed-v1";
   static const char digits[] = "0123456789abcdef";
   uint8_t input[sizeof(domain) - 1 + 32];
   memcpy(input, domain, sizeof(domain) - 1);
   for (size_t i = 0; i < 16; ++i)
   {
      input[sizeof(domain) - 1 + i * 2] = digits[operation_id[i] >> 4];
      input[sizeof(domain) - 1 + i * 2 + 1] = digits[operation_id[i] & 15];
   }
   assert(SHA256(input, sizeof(input), out) != NULL);
}

static void completed_row_hash(const db2_vault_operator_open_event_t *event, uint8_t out[32])
{
   static const char domain[] = "aimee-vault-open-row-v1";
   static const char kind[] = "completed_opened";
   static const char actor[] = "uid:0";
   static const char digits[] = "0123456789abcdef";
   uint8_t input[(sizeof(domain) - 1) + (sizeof(kind) - 1) + 32 + 32 + (sizeof(actor) - 1) + 24];
   size_t offset = 0;
#define APPEND(value)                                                                              \
   do                                                                                              \
   {                                                                                               \
      memcpy(input + offset, value, sizeof(value) - 1);                                            \
      offset += sizeof(value) - 1;                                                                 \
   } while (0)
   APPEND(domain);
   APPEND(kind);
   for (size_t i = 0; i < 16; ++i)
   {
      input[offset++] = digits[event->operation_id[i] >> 4];
      input[offset++] = digits[event->operation_id[i] & 15];
   }
   for (size_t i = 0; i < 16; ++i)
   {
      input[offset++] = digits[event->request_id[i] >> 4];
      input[offset++] = digits[event->request_id[i] & 15];
   }
   APPEND(actor);
#undef APPEND
   uint64_t values[] = {(uint64_t)event->operation_fence, (uint64_t)event->opened.opened_epoch,
                        (uint64_t)event->opened.opened_fence};
   for (size_t v = 0; v < 3; ++v)
      for (int i = 7; i >= 0; --i)
         input[offset++] = (uint8_t)(values[v] >> (i * 8));
   assert(offset == sizeof(input) && SHA256(input, sizeof(input), out) != NULL);
}

static void test_opened_replay(void)
{
   memset(&fixture, 0, sizeof(fixture));
   fixture.status.state = KB_VAULT_OPERATOR_STATE_OPERATIONAL;
   fixture.status.remediation = KB_VAULT_OPERATOR_REMEDIATION_NONE;
   fixture.status.seal_epoch = 20;
   fixture.status.control_fence = 30;
   fixture.status.last_opened_fence = 7;
   fixture.local_unsealed = 1;
   fixture.dispatch_found = 1;
   memset(fixture.dispatch_row.operation_id, 0x23, 16);
   memset(fixture.dispatch_row.request_id, 0x12, 16);
   fixture.dispatch_row.state = DB2_VAULT_REWRAP_COMPLETED;
   fixture.dispatch_row.seal_epoch = 5;
   fixture.dispatch_row.fencing_token = 7;
   fixture.dispatch_row.old_generation = 7;
   fixture.dispatch_row.new_generation = 8;
   fixture.event.completed_open = 1;
   fixture.event.operation_present = 1;
   memcpy(fixture.event.operation_id, fixture.dispatch_row.operation_id, 16);
   memcpy(fixture.event.request_id, fixture.dispatch_row.request_id, 16);
   fixture.event.operation_fence = 7;
   fixture.event.opened.opened_epoch = 20;
   fixture.event.opened.opened_fence = 30;
   opened_event_id(fixture.dispatch_row.operation_id, fixture.event.opened.event_id);
   completed_row_hash(&fixture.event, fixture.event.opened.row_hash);
   static const db2_vault_rewrap_ops_t fake_db_ops = {0};
   static const vault_reseal_custody_ops_t fake_custody_ops = {0};
   vault_reseal_orchestrator_deps_t orchestrator_deps = {.db = &fake_db_ops,
                                                         .custody = &fake_custody_ops};
   kb_vault_operator_runtime_t runtime;
   assert(kb_vault_operator_runtime_init_with_platform(&runtime, (void *)0x11, (void *)0x22,
                                                       (void *)0x44, &platform,
                                                       &orchestrator_deps) == 0);
   kb_vault_operator_mutation_deps_t deps;
   kb_vault_operator_runtime_fill_deps(&runtime, &deps);
   kb_vault_mutation_binding_t binding;
   assert(deps.start_lookup(fixture.dispatch_row.request_id, 0, &binding, &runtime) ==
          KB_VAULT_MUTATION_DB_OK);
   assert(binding.state == KB_VAULT_MUTATION_BINDING_OPENED);
   assert(runtime.activation_proof_valid && runtime.activation_has_event);
   assert(fixture.event_reads == 1);
   kb_vault_operator_runtime_destroy(&runtime);
}

int main(void)
{
   test_opened_replay();
   memset(&fixture, 0, sizeof(fixture));
   fixture.status.state = KB_VAULT_OPERATOR_STATE_SEALED_IDLE;
   fixture.status.remediation = KB_VAULT_OPERATOR_REMEDIATION_UNSEAL;
   fixture.status.seal_epoch = 5;
   fixture.status.control_fence = 9;
   static const db2_vault_rewrap_ops_t fake_db_ops = {0};
   static const vault_reseal_custody_ops_t fake_custody_ops = {0};
   vault_reseal_orchestrator_deps_t orchestrator_deps = {.db = &fake_db_ops,
                                                         .custody = &fake_custody_ops};
   kb_vault_operator_runtime_t runtime;
   assert(kb_vault_operator_runtime_init_with_platform(&runtime, (void *)0x11, (void *)0x22,
                                                       (void *)0x44, &platform,
                                                       &orchestrator_deps) == 0);
   kb_vault_operator_mutation_deps_t deps;
   kb_vault_operator_runtime_fill_deps(&runtime, &deps);
   kb_vault_operator_mutation_t mutation;
   assert(kb_vault_operator_mutation_init(&mutation, &deps, &runtime) == 0);
   kb_vault_operator_result_t result = KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   assert(kb_vault_operator_mutation_execute(KB_VAULT_OPERATOR_OPCODE_UNSEAL, NULL,
                                             (const uint8_t *)"ok", 2, &result, &mutation) == 0);
   assert(result == KB_VAULT_OPERATOR_RESULT_OPERATIONAL);
   assert(fixture.current_generation_reads == 1 && fixture.exact_preflights == 0);
   assert(fixture.open_calls == 1 && fixture.event_reads == 1 && fixture.guard_ends == 1);
   assert(kb_vault_operator_mutation_after_secret_wipe(&mutation) == 0);
   assert(fixture.publishes == 1 && fixture.seals == 0);
   kb_vault_operator_status_t wrong_fence = fixture.status;
   wrong_fence.control_fence++;
   assert(kb_vault_operator_runtime_activation_validate(&runtime, &wrong_fence) == -1);
   assert(kb_vault_operator_runtime_activation_validate(&runtime, &fixture.status) == 0);
   assert(fixture.event_reads == 2);
   assert(kb_vault_operator_runtime_mark_general_serving(&runtime) == 0);
   assert(deps.publish_activation(&fixture.status, &runtime) == 0);
   assert(fixture.publishes == 1);
   kb_vault_operator_mutation_destroy(&mutation);
   kb_vault_operator_runtime_destroy(&runtime);
   puts("kb_vault_operator_runtime: all tests passed");
   return 0;
}
