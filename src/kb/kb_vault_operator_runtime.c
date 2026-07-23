#include "kb_vault_operator_runtime.h"

#include "kb_vault_protected_secret.h"
#include "modules/vault/vault_crypto.h"
#include "modules/vault/vault_kek_check.h"

#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <string.h>

#define ACTOR "uid:0"

typedef struct
{
   kb_vault_operator_runtime_t *runtime;
   const vault_tpm2_reseal_receipt_t *receipt;
   int64_t expected_count;
   int verify_receipt_digest;
} verify_kek_context_t;

static void cache_activation_proof(kb_vault_operator_runtime_t *,
                                   const db2_vault_operator_open_result_t *,
                                   const db2_vault_operator_open_event_t *, int);
static void bytes_hex(const uint8_t *, size_t, char *);

static int production_provider_status(void *unused, db2_vault_provider_status_t *value)
{
   (void)unused;
   switch (vault_custody_selected_local_status())
   {
   case VAULT_CUSTODY_LOCAL_AVAILABLE_SEALED:
      *value = DB2_VAULT_PROVIDER_AVAILABLE_SEALED;
      return 0;
   case VAULT_CUSTODY_LOCAL_AVAILABLE_UNSEALED:
      *value = DB2_VAULT_PROVIDER_AVAILABLE_UNSEALED;
      return 0;
   case VAULT_CUSTODY_LOCAL_UNAVAILABLE:
      *value = DB2_VAULT_PROVIDER_UNAVAILABLE;
      return 0;
   case VAULT_CUSTODY_LOCAL_MALFORMED:
      *value = DB2_VAULT_PROVIDER_MALFORMED;
      return 0;
   }
   return -1;
}

static int production_read_status(db2_vault_operator_runtime_t *database,
                                  kb_vault_operator_status_t *out)
{
   db2_vault_operator_status_t status;
   if (!database || !out ||
       db2_vault_operator_runtime_status(database, production_provider_status, NULL, &status) !=
           DB2_VAULT_OPERATOR_OK)
      return -1;
   memset(out, 0, sizeof(*out));
   out->state = (kb_vault_operator_state_t)status.state;
   out->operation_state = (kb_vault_operator_operation_state_t)status.snapshot.operation_state;
   out->remediation = (kb_vault_operator_remediation_t)status.remediation;
   out->flags = status.snapshot.operation_present ? 1u : 0u;
   out->seal_epoch = (uint64_t)status.snapshot.seal_epoch;
   out->control_fence = (uint64_t)status.snapshot.control_fence;
   out->old_generation = (uint64_t)status.snapshot.old_generation;
   out->new_generation = (uint64_t)status.snapshot.new_generation;
   out->last_opened_fence = (uint64_t)status.snapshot.last_opened_fence;
   memcpy(out->operation_id, status.snapshot.operation_id, sizeof(out->operation_id));
   return kb_vault_operator_status_validate(out) ? 0 : -1;
}

static int production_singleton(kb_vault_tpm_runtime_lock_t *lock)
{
   return kb_vault_tpm_runtime_lock_revalidate(lock) == KB_VAULT_TPM_RUNTIME_LOCK_OK ? 0 : -1;
}

static const kb_vault_operator_runtime_platform_t production_platform = {
    .read_status = production_read_status,
    .singleton_revalidate = production_singleton,
    .random = vault_crypto_random,
    .authorization_preflight = vault_custody_selected_authorization_preflight,
    .authorization_preflight_current = vault_custody_selected_authorization_preflight_current,
    .dispatch = db2_vault_operator_dispatch,
    .reserve = db2_vault_operator_reserve,
    .active = db2_vault_operator_active,
    .completed = db2_vault_operator_completed,
    .completed_active = db2_vault_operator_completed_active,
    .current_check_page = db2_vault_operator_current_check_page,
    .open_completed = db2_vault_operator_open_completed,
    .open_idle = db2_vault_operator_open_idle,
    .open_event = db2_vault_operator_open_event,
    .orchestrator_run = vault_reseal_orchestrator_run,
    .receipt_decode = vault_reseal_receipt_decode,
    .receipt_status = vault_custody_tpm2_reseal_status,
    .receipt_cleanup = vault_custody_tpm2_reseal_cleanup,
    .guard_begin = vault_maintenance_guard_begin,
    .guard_sync = vault_maintenance_guard_sync_primary_epoch,
    .guard_unseal = vault_maintenance_guard_unseal_typed,
    .guard_with_kek = vault_maintenance_guard_with_active_kek,
    .guard_end = vault_maintenance_guard_end,
    .guard_end_operational = vault_maintenance_guard_end_operational,
    .local_status = vault_custody_selected_local_status,
    .kek_check_verify = vault_kek_check_verify,
    .seal = vault_seal,
    .publish = kb_vault_activation_latch_publish,
};

static int platform_valid(const kb_vault_operator_runtime_platform_t *p)
{
   return p && p->read_status && p->singleton_revalidate && p->random &&
          p->authorization_preflight && p->authorization_preflight_current && p->dispatch &&
          p->reserve && p->active && p->completed && p->completed_active && p->current_check_page &&
          p->open_completed && p->open_idle && p->open_event && p->orchestrator_run &&
          p->receipt_decode && p->receipt_status && p->receipt_cleanup && p->guard_begin &&
          p->guard_sync && p->guard_unseal && p->guard_with_kek && p->guard_end &&
          p->guard_end_operational && p->local_status && p->kek_check_verify && p->seal &&
          p->publish;
}

static kb_vault_mutation_db_result_t map_db(int result, int absent)
{
   switch ((db2_vault_rewrap_result_t)result)
   {
   case DB2_VAULT_REWRAP_OK:
      return absent ? KB_VAULT_MUTATION_DB_NOT_FOUND : KB_VAULT_MUTATION_DB_OK;
   case DB2_VAULT_REWRAP_NOT_FOUND:
      return KB_VAULT_MUTATION_DB_NOT_FOUND;
   case DB2_VAULT_REWRAP_BUSY:
      return KB_VAULT_MUTATION_DB_BUSY;
   case DB2_VAULT_REWRAP_CONFLICT:
      return KB_VAULT_MUTATION_DB_INTEGRITY;
   case DB2_VAULT_REWRAP_TRANSIENT:
   case DB2_VAULT_REWRAP_ERROR:
      return KB_VAULT_MUTATION_DB_TRANSIENT;
   case DB2_VAULT_REWRAP_INVALID:
      return KB_VAULT_MUTATION_DB_INVALID;
   case DB2_VAULT_REWRAP_INTEGRITY:
   default:
      return KB_VAULT_MUTATION_DB_INTEGRITY;
   }
}

static int status_equal(const kb_vault_operator_status_t *a, const kb_vault_operator_status_t *b)
{
   return a && b && a->state == b->state && a->operation_state == b->operation_state &&
          a->remediation == b->remediation && a->flags == b->flags &&
          a->seal_epoch == b->seal_epoch && a->control_fence == b->control_fence &&
          a->old_generation == b->old_generation && a->new_generation == b->new_generation &&
          a->last_opened_fence == b->last_opened_fence &&
          CRYPTO_memcmp(a->operation_id, b->operation_id, 16) == 0;
}

static int binding_copy(kb_vault_mutation_binding_t *out,
                        const db2_vault_operator_rewrap_binding_t *in)
{
   if (!out || !in || in->seal_epoch < 1 || in->fencing_token < 1 || in->old_generation < 0 ||
       in->old_generation == INT64_MAX || in->new_generation != in->old_generation + 1)
      return -1;
   memset(out, 0, sizeof(*out));
   memcpy(out->request_id, in->request_id, 16);
   memcpy(out->operation_id, in->operation_id, 16);
   out->old_generation = (uint64_t)in->old_generation;
   out->new_generation = (uint64_t)in->new_generation;
   out->seal_epoch = (uint64_t)in->seal_epoch;
   out->fence = (uint64_t)in->fencing_token;
   if (in->state == DB2_VAULT_REWRAP_COMPLETED)
      out->state = KB_VAULT_MUTATION_BINDING_COMPLETED;
   else if (in->state >= DB2_VAULT_REWRAP_PREPARING && in->state <= DB2_VAULT_REWRAP_PROMOTED)
      out->state = KB_VAULT_MUTATION_BINDING_ACTIVE;
   else
      return -1;
   return 0;
}

static int all_zero(const uint8_t *bytes, size_t length)
{
   uint8_t value = 0;
   for (size_t i = 0; i < length; ++i)
      value |= bytes[i];
   return value == 0;
}

static void put_be64(uint8_t out[8], uint64_t value)
{
   for (int i = 7; i >= 0; --i)
   {
      out[i] = (uint8_t)value;
      value >>= 8;
   }
}

static int completed_event_row_hash_valid(const db2_vault_operator_open_event_t *event)
{
   static const char domain[] = "aimee-vault-open-row-v1";
   static const char kind[] = "completed_opened";
   static const char actor[] = ACTOR;
   static const char digits[] = "0123456789abcdef";
   uint8_t input[(sizeof(domain) - 1) + (sizeof(kind) - 1) + 32 + 32 + (sizeof(actor) - 1) + 24];
   uint8_t digest[SHA256_DIGEST_LENGTH];
   size_t offset = 0;
#define APPEND_LITERAL(value)                                                                      \
   do                                                                                              \
   {                                                                                               \
      memcpy(input + offset, (value), sizeof(value) - 1);                                          \
      offset += sizeof(value) - 1;                                                                 \
   } while (0)
   APPEND_LITERAL(domain);
   APPEND_LITERAL(kind);
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
   APPEND_LITERAL(actor);
#undef APPEND_LITERAL
   put_be64(input + offset, (uint64_t)event->operation_fence);
   offset += 8;
   put_be64(input + offset, (uint64_t)event->opened.opened_epoch);
   offset += 8;
   put_be64(input + offset, (uint64_t)event->opened.opened_fence);
   offset += 8;
   int valid = offset == sizeof(input) && SHA256(input, sizeof(input), digest) &&
               CRYPTO_memcmp(digest, event->opened.row_hash, sizeof(digest)) == 0;
   OPENSSL_cleanse(digest, sizeof(digest));
   OPENSSL_cleanse(input, sizeof(input));
   return valid;
}

static int runtime_read_status(kb_vault_operator_status_t *status, void *opaque)
{
   kb_vault_operator_runtime_t *r = opaque;
   return r && r->initialized ? r->platform->read_status(r->database, status) : -1;
}

static int runtime_singleton(void *opaque)
{
   kb_vault_operator_runtime_t *r = opaque;
   return r && r->initialized ? r->platform->singleton_revalidate(r->singleton) : -1;
}

static int protected_cstring(const uint8_t *secret, size_t length,
                             kb_vault_protected_secret_t *copy)
{
   if (!secret || !length || length > KB_VAULT_OPERATOR_SECRET_MAX || memchr(secret, 0, length) ||
       kb_vault_protected_secret_open(copy, length + 1) != 0)
      return -1;
   memcpy(copy->bytes, secret, length);
   copy->bytes[length] = 0;
   return kb_vault_protected_secret_set_length(copy, length);
}

static uint64_t status_expected_generation(const kb_vault_operator_status_t *status)
{
   if (!status || !(status->flags & 1u))
      return 0;
   switch (status->operation_state)
   {
   case KB_VAULT_OPERATOR_OPERATION_PREPARING:
   case KB_VAULT_OPERATOR_OPERATION_CUSTODY_PREPARED:
   case KB_VAULT_OPERATOR_OPERATION_WRAPS_STAGED:
      return status->old_generation;
   case KB_VAULT_OPERATOR_OPERATION_RESEAL_COMMITTING:
   case KB_VAULT_OPERATOR_OPERATION_RESEALED:
   case KB_VAULT_OPERATOR_OPERATION_PROMOTED:
   case KB_VAULT_OPERATOR_OPERATION_COMPLETED:
      return status->new_generation;
   default:
      return 0;
   }
}

static kb_vault_mutation_auth_result_t runtime_auth(kb_vault_operator_opcode_t opcode,
                                                    const kb_vault_operator_status_t *status,
                                                    const uint8_t *secret, size_t secret_len,
                                                    uint64_t *authorized_generation, void *opaque)
{
   (void)opcode;
   kb_vault_operator_runtime_t *r = opaque;
   uint64_t generation = status_expected_generation(status);
   if (!r || !r->initialized || !authorized_generation || runtime_singleton(r) != 0 || !secret ||
       !secret_len || secret_len > KB_VAULT_OPERATOR_SECRET_MAX || memchr(secret, 0, secret_len))
      return KB_VAULT_MUTATION_AUTH_INTEGRITY;
   vault_custody_auth_result_t result =
       generation ? r->platform->authorization_preflight(secret, secret_len, generation)
                  : r->platform->authorization_preflight_current(secret, secret_len, &generation);
   *authorized_generation = result == VAULT_CUSTODY_AUTHORIZED ? generation : 0;
   switch (result)
   {
   case VAULT_CUSTODY_AUTHORIZED:
      return KB_VAULT_MUTATION_AUTHORIZED;
   case VAULT_CUSTODY_AUTH_WRONG_SECRET:
      return KB_VAULT_MUTATION_AUTH_WRONG_SECRET;
   case VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE:
      return KB_VAULT_MUTATION_AUTH_BACKEND_UNAVAILABLE;
   case VAULT_CUSTODY_AUTH_UNSUPPORTED:
      return KB_VAULT_MUTATION_AUTH_UNSUPPORTED;
   case VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE:
   default:
      return KB_VAULT_MUTATION_AUTH_INTEGRITY;
   }
}

static kb_vault_mutation_db_result_t runtime_lookup(const uint8_t request_id[16], int locked,
                                                    kb_vault_mutation_binding_t *binding,
                                                    void *opaque)
{
   (void)locked; /* The private function always takes advisory + row SHARE locks. */
   kb_vault_operator_runtime_t *r = opaque;
   db2_vault_operator_rewrap_binding_t row = {0};
   int found = 0;
   int rc = r->platform->dispatch(request_id, &row, &found);
   if (rc != DB2_VAULT_REWRAP_OK || !found)
      return map_db(rc, !found);
   if (binding_copy(binding, &row) != 0)
      return KB_VAULT_MUTATION_DB_INTEGRITY;
   if (binding->state == KB_VAULT_MUTATION_BINDING_COMPLETED)
   {
      kb_vault_operator_status_t status = {0};
      uint8_t event_input[sizeof("aimee-vault-open-completed-v1") - 1 + 32 + 1];
      uint8_t event_id[SHA256_DIGEST_LENGTH];
      db2_vault_operator_open_event_t event = {0};
      memcpy(event_input, "aimee-vault-open-completed-v1",
             sizeof("aimee-vault-open-completed-v1") - 1);
      bytes_hex(row.operation_id, 16,
                (char *)event_input + sizeof("aimee-vault-open-completed-v1") - 1);
      if (!SHA256(event_input, sizeof(event_input) - 1, event_id) ||
          r->platform->read_status(r->database, &status) != 0 ||
          status.state != KB_VAULT_OPERATOR_STATE_OPERATIONAL || status.flags != 0 ||
          status.seal_epoch < 1 || status.control_fence < 1 ||
          status.last_opened_fence != binding->fence ||
          r->platform->open_event(event_id, &event) != DB2_VAULT_REWRAP_OK ||
          !event.completed_open || !event.operation_present ||
          event.operation_fence != row.fencing_token ||
          event.opened.opened_epoch != (int64_t)status.seal_epoch ||
          event.opened.opened_fence != (int64_t)status.control_fence ||
          CRYPTO_memcmp(event.opened.event_id, event_id, 32) != 0 ||
          CRYPTO_memcmp(event.operation_id, row.operation_id, 16) != 0 ||
          CRYPTO_memcmp(event.request_id, row.request_id, 16) != 0 ||
          !completed_event_row_hash_valid(&event))
      {
         OPENSSL_cleanse(event_input, sizeof(event_input));
         OPENSSL_cleanse(event_id, sizeof(event_id));
         OPENSSL_cleanse(&event, sizeof(event));
         return KB_VAULT_MUTATION_DB_INTEGRITY;
      }
      binding->state = KB_VAULT_MUTATION_BINDING_OPENED;
      cache_activation_proof(r, &event.opened, &event, 1);
      OPENSSL_cleanse(event_input, sizeof(event_input));
      OPENSSL_cleanse(event_id, sizeof(event_id));
      OPENSSL_cleanse(&event, sizeof(event));
   }
   return KB_VAULT_MUTATION_DB_OK;
}

static kb_vault_mutation_db_result_t
runtime_reserve(const uint8_t request_id[16], const uint8_t candidate[16], uint64_t old_generation,
                uint64_t new_generation, kb_vault_mutation_binding_t *binding, int *created,
                void *opaque)
{
   kb_vault_operator_runtime_t *r = opaque;
   db2_vault_operator_rewrap_binding_t row = {0};
   if (old_generation > INT64_MAX || new_generation > INT64_MAX)
      return KB_VAULT_MUTATION_DB_INVALID;
   int rc = r->platform->reserve(request_id, candidate, (int64_t)old_generation,
                                 (int64_t)new_generation, &row, created);
   if (rc != DB2_VAULT_REWRAP_OK)
      return map_db(rc, 0);
   return binding_copy(binding, &row) == 0 ? KB_VAULT_MUTATION_DB_OK
                                           : KB_VAULT_MUTATION_DB_INTEGRITY;
}

static kb_vault_mutation_db_result_t runtime_active_common(kb_vault_mutation_binding_t *binding,
                                                           int completed, void *opaque)
{
   kb_vault_operator_runtime_t *r = opaque;
   db2_vault_operator_rewrap_binding_t row = {0};
   int found = 0;
   int rc = r->platform->active(&row, &found);
   if (rc != DB2_VAULT_REWRAP_OK || !found)
      return map_db(rc, !found);
   if (completed && row.state != DB2_VAULT_REWRAP_COMPLETED)
      return KB_VAULT_MUTATION_DB_INTEGRITY;
   return binding_copy(binding, &row) == 0 ? KB_VAULT_MUTATION_DB_OK
                                           : KB_VAULT_MUTATION_DB_INTEGRITY;
}

static kb_vault_mutation_db_result_t runtime_active(kb_vault_mutation_binding_t *binding,
                                                    void *opaque)
{
   return runtime_active_common(binding, 0, opaque);
}

/* Completed discovery has no client identity. The status operation ID is
 * copied into the context by the mutation callback immediately before this
 * call through the sole-active discovery path in the private DB function. */
static kb_vault_mutation_db_result_t runtime_completed_binding(kb_vault_mutation_binding_t *binding,
                                                               void *opaque)
{
   kb_vault_operator_runtime_t *r = opaque;
   kb_vault_operator_status_t status = {0};
   db2_vault_operator_completed_t completed = {0};
   if (r->platform->read_status(r->database, &status) != 0 || !(status.flags & 1u) ||
       status.state != KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED)
      return KB_VAULT_MUTATION_DB_INTEGRITY;
   int rc = r->platform->completed_active(status.operation_id, &completed);
   if (rc != DB2_VAULT_REWRAP_OK)
      return map_db(rc, 0);
   if (binding_copy(binding, &completed.binding) != 0 ||
       CRYPTO_memcmp(binding->operation_id, status.operation_id, 16) != 0)
   {
      OPENSSL_cleanse(&completed, sizeof(completed));
      return KB_VAULT_MUTATION_DB_INTEGRITY;
   }
   OPENSSL_cleanse(&completed, sizeof(completed));
   return KB_VAULT_MUTATION_DB_OK;
}

static int runtime_random(uint8_t out[16], void *opaque)
{
   kb_vault_operator_runtime_t *r = opaque;
   return r->platform->random(out, 16);
}

static void bytes_hex(const uint8_t *bytes, size_t length, char *hex)
{
   static const char digits[] = "0123456789abcdef";
   for (size_t i = 0; i < length; ++i)
   {
      hex[i * 2] = digits[bytes[i] >> 4];
      hex[i * 2 + 1] = digits[bytes[i] & 15];
   }
   hex[length * 2] = 0;
}

static kb_vault_mutation_step_result_t runtime_reseal(kb_vault_mutation_reseal_mode_t mode,
                                                      const uint8_t request_id[16],
                                                      const kb_vault_mutation_binding_t *binding,
                                                      const uint8_t *secret, size_t secret_len,
                                                      void *opaque)
{
   kb_vault_operator_runtime_t *r = opaque;
   char request_hex[33];
   vault_reseal_orchestrator_request_t request = {0};
   vault_reseal_orchestrator_output_t output = {0};
   bytes_hex(request_id, 16, request_hex);
   request.mode = mode == KB_VAULT_MUTATION_RESEAL_START ? VAULT_RESEAL_ORCHESTRATOR_START
                                                         : VAULT_RESEAL_ORCHESTRATOR_RESUME;
   memcpy(request.operation_id, binding->operation_id, 16);
   request.actor = ACTOR;
   request.request_id = request_hex;
   request.provider_secret = secret;
   request.provider_secret_len = secret_len;
   vault_reseal_orchestrator_result_t result =
       r->platform->orchestrator_run(&request, &r->orchestrator_deps, &output);
   OPENSSL_cleanse(&output, sizeof(output));
   OPENSSL_cleanse(request_hex, sizeof(request_hex));
   switch (result)
   {
   case VAULT_RESEAL_ORCHESTRATOR_COMPLETED:
      return KB_VAULT_MUTATION_STEP_COMPLETED;
   case VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY:
      return KB_VAULT_MUTATION_STEP_SAFE_RETRY;
   case VAULT_RESEAL_ORCHESTRATOR_BUSY:
      return KB_VAULT_MUTATION_STEP_BUSY;
   case VAULT_RESEAL_ORCHESTRATOR_ABORTED:
      return KB_VAULT_MUTATION_STEP_ABORTED;
   case VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED:
      return KB_VAULT_MUTATION_STEP_RECOVERY_REQUIRED;
   case VAULT_RESEAL_ORCHESTRATOR_INTEGRITY:
      return KB_VAULT_MUTATION_STEP_INTEGRITY;
   case VAULT_RESEAL_ORCHESTRATOR_INVALID:
      return KB_VAULT_MUTATION_STEP_INVALID;
   case VAULT_RESEAL_ORCHESTRATOR_UNSUPPORTED:
      return KB_VAULT_MUTATION_STEP_UNSUPPORTED;
   case VAULT_RESEAL_ORCHESTRATOR_ERROR:
   default:
      return KB_VAULT_MUTATION_STEP_BACKEND_UNAVAILABLE;
   }
}

static int verify_kek(const uint8_t kek[VAULT_KEK_LEN], void *opaque)
{
   verify_kek_context_t *context = opaque;
   kb_vault_operator_runtime_t *r = context->runtime;
   uint8_t digest[SHA256_DIGEST_LENGTH];
   db2_vault_rewrap_cursor_t cursor = {{0}, 0};
   int64_t consumed = 0;
   if (context->verify_receipt_digest &&
       (!context->receipt || !SHA256(kek, VAULT_KEK_LEN, digest) ||
        CRYPTO_memcmp(digest, context->receipt->new_kek_digest, sizeof(digest)) != 0))
   {
      OPENSSL_cleanse(digest, sizeof(digest));
      return -1;
   }
   OPENSSL_cleanse(digest, sizeof(digest));
   for (;;)
   {
      db2_vault_rewrap_check_t rows[DB2_VAULT_REWRAP_PAGE_MAX];
      db2_vault_rewrap_cursor_t next = {{0}, 0};
      size_t count = 0;
      int64_t total = 0;
      memset(rows, 0, sizeof(rows));
      int rc = r->platform->current_check_page(&cursor, DB2_VAULT_REWRAP_PAGE_MAX, rows,
                                               DB2_VAULT_REWRAP_PAGE_MAX, &count, &next, &total);
      if (context->expected_count < 0 && rc == DB2_VAULT_REWRAP_OK)
         context->expected_count = total;
      if (rc != DB2_VAULT_REWRAP_OK || total != context->expected_count || count > 128 ||
          consumed > total - (int64_t)count)
      {
         db2_vault_rewrap_check_clear(rows, DB2_VAULT_REWRAP_PAGE_MAX);
         db2_vault_rewrap_cursor_clear(&next);
         db2_vault_rewrap_cursor_clear(&cursor);
         return -1;
      }
      for (size_t i = 0; i < count; ++i)
         if (rows[i].kek_check_len != 0 &&
             (rows[i].kek_check_len != VAULT_WRAPPED_DEK_LEN ||
              r->platform->kek_check_verify(kek, rows[i].kek_check) != 0))
         {
            db2_vault_rewrap_check_clear(rows, DB2_VAULT_REWRAP_PAGE_MAX);
            db2_vault_rewrap_cursor_clear(&next);
            db2_vault_rewrap_cursor_clear(&cursor);
            return -1;
         }
      consumed += (int64_t)count;
      db2_vault_rewrap_check_clear(rows, DB2_VAULT_REWRAP_PAGE_MAX);
      if (!count)
      {
         db2_vault_rewrap_cursor_clear(&next);
         db2_vault_rewrap_cursor_clear(&cursor);
         return consumed == total ? 0 : -1;
      }
      cursor = next;
      db2_vault_rewrap_cursor_clear(&next);
   }
}

static void cache_activation_proof(kb_vault_operator_runtime_t *runtime,
                                   const db2_vault_operator_open_result_t *opened,
                                   const db2_vault_operator_open_event_t *event, int has_event)
{
   pthread_mutex_lock(&runtime->mutex);
   runtime->activation_open = *opened;
   if (has_event)
      runtime->activation_event = *event;
   else
      memset(&runtime->activation_event, 0, sizeof(runtime->activation_event));
   runtime->activation_has_event = has_event;
   runtime->activation_proof_valid = 1;
   pthread_mutex_unlock(&runtime->mutex);
}

static int event_matches(const db2_vault_operator_open_result_t *opened,
                         const db2_vault_operator_open_event_t *event,
                         const db2_vault_operator_completed_t *completed, int completed_open)
{
   return opened->opened_epoch == event->opened.opened_epoch &&
          opened->opened_fence == event->opened.opened_fence &&
          CRYPTO_memcmp(opened->event_id, event->opened.event_id, 32) == 0 &&
          CRYPTO_memcmp(opened->row_hash, event->opened.row_hash, 32) == 0 &&
          event->completed_open == completed_open &&
          (!completed_open ||
           (completed && event->operation_present &&
            event->operation_fence == completed->binding.fencing_token &&
            CRYPTO_memcmp(event->operation_id, completed->binding.operation_id, 16) == 0 &&
            CRYPTO_memcmp(event->request_id, completed->binding.request_id, 16) == 0));
}

static kb_vault_operator_result_t typed_result(vault_custody_auth_result_t result)
{
   switch (result)
   {
   case VAULT_CUSTODY_AUTH_WRONG_SECRET:
      return KB_VAULT_OPERATOR_RESULT_WRONG_SECRET;
   case VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE:
      return KB_VAULT_OPERATOR_RESULT_BACKEND_UNAVAILABLE;
   case VAULT_CUSTODY_AUTH_UNSUPPORTED:
      return KB_VAULT_OPERATOR_RESULT_UNSUPPORTED;
   case VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE:
   default:
      return KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   }
}

static kb_vault_operator_result_t runtime_finalize(const kb_vault_mutation_binding_t *binding,
                                                   const kb_vault_operator_status_t *status,
                                                   const uint8_t *secret, size_t secret_len,
                                                   void *opaque)
{
   kb_vault_operator_runtime_t *r = opaque;
   db2_vault_operator_completed_t completed = {0};
   vault_tpm2_reseal_receipt_t receipt = {0};
   kb_vault_protected_secret_t copy = {0};
   vault_maintenance_guard_t *guard = NULL;
   db2_vault_operator_open_result_t opened = {0};
   db2_vault_operator_open_event_t event = {0};
   kb_vault_operator_result_t result = KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   if (!binding || !status || runtime_singleton(r) != 0 ||
       r->platform->completed(binding->request_id, binding->operation_id, &completed) !=
           DB2_VAULT_REWRAP_OK ||
       binding_copy(&(kb_vault_mutation_binding_t){0}, &completed.binding) != 0 ||
       CRYPTO_memcmp(binding->request_id, completed.binding.request_id, 16) != 0 ||
       CRYPTO_memcmp(binding->operation_id, completed.binding.operation_id, 16) != 0 ||
       binding->seal_epoch != (uint64_t)completed.binding.seal_epoch ||
       binding->fence != (uint64_t)completed.binding.fencing_token ||
       binding->old_generation != (uint64_t)completed.binding.old_generation ||
       binding->new_generation != (uint64_t)completed.binding.new_generation ||
       r->platform->receipt_decode(completed.receipt, sizeof(completed.receipt), &receipt) != 0 ||
       CRYPTO_memcmp(receipt.operation_id, binding->operation_id, 16) != 0 ||
       receipt.old_generation != binding->old_generation ||
       receipt.new_generation != binding->new_generation ||
       protected_cstring(secret, secret_len, &copy) != 0)
      goto out;
   vault_custody_auth_result_t auth =
       r->platform->authorization_preflight(secret, secret_len, binding->new_generation);
   if (auth != VAULT_CUSTODY_AUTHORIZED)
   {
      result = typed_result(auth);
      goto out;
   }
   if (runtime_singleton(r) != 0 || r->platform->guard_begin(&guard) != VAULT_MAINTENANCE_OK ||
       r->platform->guard_sync(guard, status->seal_epoch) != VAULT_MAINTENANCE_OK)
      goto out;
   vault_tpm2_reseal_status_t artifact = VAULT_TPM2_RESEAL_ABSENT;
   if (r->platform->receipt_status(&receipt, (const char *)copy.bytes, &artifact) !=
           VAULT_TPM2_RESEAL_OK ||
       (artifact != VAULT_TPM2_RESEAL_INSTALLED && artifact != VAULT_TPM2_RESEAL_CLEANED))
      goto out;
   if (artifact == VAULT_TPM2_RESEAL_INSTALLED)
   {
      if (r->platform->receipt_cleanup(&receipt, (const char *)copy.bytes,
                                       VAULT_TPM2_CLEANUP_TERMINAL_COMPLETED) !=
              VAULT_TPM2_RESEAL_OK ||
          r->platform->receipt_status(&receipt, (const char *)copy.bytes, &artifact) !=
              VAULT_TPM2_RESEAL_OK ||
          artifact != VAULT_TPM2_RESEAL_CLEANED)
         goto out;
   }
   auth = r->platform->guard_unseal(guard, secret, secret_len);
   if (auth != VAULT_CUSTODY_AUTHORIZED)
   {
      result = typed_result(auth);
      goto out;
   }
   verify_kek_context_t verify = {.runtime = r,
                                  .receipt = &receipt,
                                  .expected_count = completed.check_count,
                                  .verify_receipt_digest = 1};
   if (r->platform->guard_with_kek(guard, verify_kek, &verify) != 0 ||
       r->platform->open_completed(&completed, &opened) != DB2_VAULT_REWRAP_OK ||
       r->platform->open_event(opened.event_id, &event) != DB2_VAULT_REWRAP_OK ||
       !event_matches(&opened, &event, &completed, 1) ||
       r->platform->guard_sync(guard, (uint64_t)opened.opened_epoch) != VAULT_MAINTENANCE_OK ||
       runtime_singleton(r) != 0 ||
       r->platform->local_status() != VAULT_CUSTODY_LOCAL_AVAILABLE_UNSEALED ||
       r->platform->guard_end_operational(&guard, (uint64_t)opened.opened_epoch) !=
           VAULT_MAINTENANCE_OK)
      goto out;
   cache_activation_proof(r, &opened, &event, 1);
   result = KB_VAULT_OPERATOR_RESULT_OPERATIONAL;
out:
   if (guard)
      (void)r->platform->guard_end(&guard);
   kb_vault_protected_secret_close(&copy);
   OPENSSL_cleanse(&event, sizeof(event));
   OPENSSL_cleanse(&opened, sizeof(opened));
   OPENSSL_cleanse(&receipt, sizeof(receipt));
   OPENSSL_cleanse(&completed, sizeof(completed));
   return result;
}

static kb_vault_operator_result_t unseal_common(const kb_vault_operator_status_t *status,
                                                const uint8_t *secret, size_t secret_len, int idle,
                                                void *opaque)
{
   kb_vault_operator_runtime_t *r = opaque;
   vault_maintenance_guard_t *guard = NULL;
   uint8_t request_id[16] = {0};
   db2_vault_operator_open_result_t opened = {0};
   db2_vault_operator_open_event_t event = {0};
   kb_vault_operator_result_t result = KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE;
   if (!status || runtime_singleton(r) != 0 ||
       (idle && (r->platform->random(request_id, sizeof(request_id)) != 0 ||
                 all_zero(request_id, sizeof(request_id)))) ||
       r->platform->guard_begin(&guard) != VAULT_MAINTENANCE_OK ||
       r->platform->guard_sync(guard, status->seal_epoch) != VAULT_MAINTENANCE_OK)
      goto out;
   vault_custody_auth_result_t auth = r->platform->guard_unseal(guard, secret, secret_len);
   if (auth != VAULT_CUSTODY_AUTHORIZED)
   {
      result = typed_result(auth);
      goto out;
   }
   verify_kek_context_t verify = {
       .runtime = r, .receipt = NULL, .expected_count = -1, .verify_receipt_digest = 0};
   /* Idle/local paths have no receipt KEK digest; verify all retained checks. */
   if (r->platform->guard_with_kek(guard, verify_kek, &verify) != 0)
      goto out;
   if (idle)
   {
      if (r->platform->open_idle(request_id, (int64_t)status->seal_epoch,
                                 (int64_t)status->control_fence, (int64_t)status->last_opened_fence,
                                 &opened) != DB2_VAULT_REWRAP_OK ||
          r->platform->open_event(opened.event_id, &event) != DB2_VAULT_REWRAP_OK ||
          !event_matches(&opened, &event, NULL, 0) ||
          CRYPTO_memcmp(event.request_id, request_id, 16) != 0 ||
          r->platform->guard_sync(guard, (uint64_t)opened.opened_epoch) != VAULT_MAINTENANCE_OK)
         goto out;
   }
   else
   {
      kb_vault_operator_status_t after = {0};
      if (r->platform->read_status(r->database, &after) != 0 ||
          after.state != KB_VAULT_OPERATOR_STATE_OPERATIONAL ||
          after.seal_epoch != status->seal_epoch || after.control_fence != status->control_fence ||
          after.last_opened_fence != status->last_opened_fence)
         goto out;
      opened.opened_epoch = (int64_t)status->seal_epoch;
      opened.opened_fence = (int64_t)status->control_fence;
   }
   if (runtime_singleton(r) != 0 ||
       r->platform->local_status() != VAULT_CUSTODY_LOCAL_AVAILABLE_UNSEALED ||
       r->platform->guard_end_operational(&guard, (uint64_t)opened.opened_epoch) !=
           VAULT_MAINTENANCE_OK)
      goto out;
   cache_activation_proof(r, &opened, &event, idle);
   result = KB_VAULT_OPERATOR_RESULT_OPERATIONAL;
out:
   if (guard)
      (void)r->platform->guard_end(&guard);
   OPENSSL_cleanse(&event, sizeof(event));
   OPENSSL_cleanse(&opened, sizeof(opened));
   OPENSSL_cleanse(request_id, sizeof(request_id));
   return result;
}

static kb_vault_operator_result_t runtime_unseal_idle(const kb_vault_operator_status_t *status,
                                                      const uint8_t *secret, size_t secret_len,
                                                      void *opaque)
{
   return unseal_common(status, secret, secret_len, 1, opaque);
}

static kb_vault_operator_result_t runtime_unseal_local(const kb_vault_operator_status_t *status,
                                                       const uint8_t *secret, size_t secret_len,
                                                       void *opaque)
{
   return unseal_common(status, secret, secret_len, 0, opaque);
}

static int runtime_publish(const kb_vault_operator_status_t *status, void *opaque)
{
   kb_vault_operator_runtime_t *r = opaque;
   if (!r || !status || status->state != KB_VAULT_OPERATOR_STATE_OPERATIONAL ||
       runtime_singleton(r) != 0 ||
       r->platform->local_status() != VAULT_CUSTODY_LOCAL_AVAILABLE_UNSEALED)
      return -1;
   pthread_mutex_lock(&r->mutex);
   r->activation_status = *status;
   int general_serving = r->general_serving;
   pthread_mutex_unlock(&r->mutex);
   return general_serving ? 0 : r->platform->publish(r->activation, status);
}

static void runtime_seal(void *opaque)
{
   kb_vault_operator_runtime_t *r = opaque;
   if (r && r->initialized)
      (void)r->platform->seal();
}

int kb_vault_operator_runtime_init_with_platform(
    kb_vault_operator_runtime_t *runtime, db2_vault_operator_runtime_t *database,
    kb_vault_tpm_runtime_lock_t *singleton, kb_vault_activation_latch_t *activation,
    const kb_vault_operator_runtime_platform_t *platform,
    const vault_reseal_orchestrator_deps_t *orchestrator_deps)
{
   if (!runtime || !database || !singleton || !activation || !platform_valid(platform))
      return -1;
   memset(runtime, 0, sizeof(*runtime));
   if (pthread_mutex_init(&runtime->mutex, NULL) != 0)
      return -1;
   runtime->database = database;
   runtime->singleton = singleton;
   runtime->activation = activation;
   runtime->platform = platform;
   runtime->orchestrator_deps =
       orchestrator_deps
           ? *orchestrator_deps
           : (vault_reseal_orchestrator_deps_t){.db = &db2_vault_operator_rewrap_ops,
                                                .custody = &vault_reseal_custody_default_ops};
   if (!runtime->orchestrator_deps.db || !runtime->orchestrator_deps.custody)
   {
      pthread_mutex_destroy(&runtime->mutex);
      memset(runtime, 0, sizeof(*runtime));
      return -1;
   }
   runtime->initialized = 1;
   return 0;
}

int kb_vault_operator_runtime_init(kb_vault_operator_runtime_t *runtime,
                                   db2_vault_operator_runtime_t *database,
                                   kb_vault_tpm_runtime_lock_t *singleton,
                                   kb_vault_activation_latch_t *activation)
{
   if (!runtime || !database || db2_vault_operator_rewrap_bind(database) != 0)
      return -1;
   int rc = kb_vault_operator_runtime_init_with_platform(runtime, database, singleton, activation,
                                                         &production_platform, NULL);
   if (rc != 0)
      db2_vault_operator_rewrap_unbind(database);
   else
      runtime->database_bound = 1;
   return rc;
}

void kb_vault_operator_runtime_destroy(kb_vault_operator_runtime_t *runtime)
{
   if (!runtime || !runtime->initialized)
      return;
   runtime->initialized = 0;
   if (runtime->database_bound)
      db2_vault_operator_rewrap_unbind(runtime->database);
   pthread_mutex_destroy(&runtime->mutex);
   OPENSSL_cleanse(runtime, sizeof(*runtime));
}

void kb_vault_operator_runtime_fill_deps(kb_vault_operator_runtime_t *runtime,
                                         kb_vault_operator_mutation_deps_t *deps)
{
   if (!deps)
      return;
   memset(deps, 0, sizeof(*deps));
   if (!runtime || !runtime->initialized)
      return;
   *deps = (kb_vault_operator_mutation_deps_t){
       .read_status = runtime_read_status,
       .singleton_revalidate = runtime_singleton,
       .authorization_preflight = runtime_auth,
       .start_lookup = runtime_lookup,
       .start_reserve = runtime_reserve,
       .discover_active = runtime_active,
       .lookup_completed = runtime_completed_binding,
       .random_operation_id = runtime_random,
       .run_reseal = runtime_reseal,
       .finalize_completed = runtime_finalize,
       .unseal_idle = runtime_unseal_idle,
       .unseal_local = runtime_unseal_local,
       .publish_activation = runtime_publish,
       .fail_closed_seal = runtime_seal,
   };
}

int kb_vault_operator_runtime_read_status(kb_vault_operator_status_t *status, void *opaque)
{
   return runtime_read_status(status, opaque);
}

int kb_vault_operator_runtime_activation_validate(kb_vault_operator_runtime_t *runtime,
                                                  const kb_vault_operator_status_t *latched)
{
   if (!runtime || !runtime->initialized || !latched ||
       latched->state != KB_VAULT_OPERATOR_STATE_OPERATIONAL)
      return -1;
   db2_vault_operator_open_result_t opened;
   db2_vault_operator_open_event_t event;
   kb_vault_operator_status_t published;
   int valid, has_event;
   pthread_mutex_lock(&runtime->mutex);
   opened = runtime->activation_open;
   event = runtime->activation_event;
   published = runtime->activation_status;
   valid = runtime->activation_proof_valid;
   has_event = runtime->activation_has_event;
   pthread_mutex_unlock(&runtime->mutex);
   if (!valid || !status_equal(latched, &published) ||
       latched->seal_epoch != (uint64_t)opened.opened_epoch ||
       latched->control_fence != (uint64_t)opened.opened_fence || runtime_singleton(runtime) != 0 ||
       runtime->platform->local_status() != VAULT_CUSTODY_LOCAL_AVAILABLE_UNSEALED)
      return -1;
   if (has_event)
   {
      db2_vault_operator_open_event_t fresh = {0};
      if (runtime->platform->open_event(opened.event_id, &fresh) != DB2_VAULT_REWRAP_OK ||
          fresh.opened.opened_epoch != opened.opened_epoch ||
          fresh.opened.opened_fence != opened.opened_fence ||
          CRYPTO_memcmp(fresh.opened.event_id, opened.event_id, 32) != 0 ||
          CRYPTO_memcmp(fresh.opened.row_hash, opened.row_hash, 32) != 0 ||
          CRYPTO_memcmp(&fresh, &event, sizeof(fresh)) != 0)
      {
         OPENSSL_cleanse(&fresh, sizeof(fresh));
         return -1;
      }
      OPENSSL_cleanse(&fresh, sizeof(fresh));
   }
   kb_vault_operator_status_t fresh_status = {0};
   return runtime->platform->read_status(runtime->database, &fresh_status) == 0 &&
                  status_equal(&fresh_status, latched)
              ? 0
              : -1;
}

int kb_vault_operator_runtime_mark_general_serving(kb_vault_operator_runtime_t *runtime)
{
   kb_vault_operator_status_t status = {0};
   if (!runtime || !runtime->initialized ||
       runtime->platform->read_status(runtime->database, &status) != 0 ||
       status.state != KB_VAULT_OPERATOR_STATE_OPERATIONAL || runtime_singleton(runtime) != 0 ||
       runtime->platform->local_status() != VAULT_CUSTODY_LOCAL_AVAILABLE_UNSEALED)
      return -1;
   pthread_mutex_lock(&runtime->mutex);
   runtime->general_serving = 1;
   pthread_mutex_unlock(&runtime->mutex);
   return 0;
}
