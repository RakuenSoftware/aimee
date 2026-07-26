#define _POSIX_C_SOURCE 200809L
#include "kb_mgmt_token_authority_service.h"

#include "kb_mgmt_token_authority.h"
#include "kb_mgmt_token_public.h"
#include "kb_vault_protected_use.h"
#include "vault_server_key.h"

#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <string.h>

typedef struct
{
   const kb_mgmt_token_authority_record_t *record;
   kb_mgmt_token_authority_output_t output;
   kb_mgmt_token_authority_result_t result;
} protected_issue_t;

static int exact_hex(const char *s, size_t n)
{
   if (!s || strnlen(s, n + 1) != n)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int random_hex(char out[65])
{
   static const char digits[] = "0123456789abcdef";
   unsigned char raw[32];
   if (RAND_bytes(raw, sizeof(raw)) != 1)
      return -1;
   for (size_t i = 0; i < sizeof(raw); ++i)
   {
      out[i * 2] = digits[raw[i] >> 4];
      out[i * 2 + 1] = digits[raw[i] & 15];
   }
   out[64] = 0;
   OPENSSL_cleanse(raw, sizeof(raw));
   return 0;
}

static kb_mgmt_token_authority_ipc_result_t map_db(db2_management_token_authority_result_t result)
{
   switch (result)
   {
   case DB2_MANAGEMENT_TOKEN_AUTHORITY_OK:
      return KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
   case DB2_MANAGEMENT_TOKEN_AUTHORITY_DENIED:
      return KB_MGMT_TOKEN_AUTHORITY_IPC_DENIED;
   case DB2_MANAGEMENT_TOKEN_AUTHORITY_CONFLICT:
      return KB_MGMT_TOKEN_AUTHORITY_IPC_CONFLICT;
   case DB2_MANAGEMENT_TOKEN_AUTHORITY_EXPIRED:
      return KB_MGMT_TOKEN_AUTHORITY_IPC_EXPIRED;
   case DB2_MANAGEMENT_TOKEN_AUTHORITY_SEALED:
      return KB_MGMT_TOKEN_AUTHORITY_IPC_SEALED;
   case DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY:
      return KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
   case DB2_MANAGEMENT_TOKEN_AUTHORITY_ABSENT:
      return KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
   case DB2_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS:
      return KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
   case DB2_MANAGEMENT_TOKEN_AUTHORITY_UNAVAILABLE:
      return KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
   }
   return KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
}

/* Takes the signer's result by value rather than the issue struct, so the
 * management and identity paths - which carry different record types - share one
 * mapping instead of drifting apart. */
static kb_mgmt_token_authority_ipc_result_t
map_protected(kb_vault_key_use_status_t result, kb_mgmt_token_authority_result_t issue_result)
{
   if (result == KB_VAULT_KEY_USE_OK && issue_result == KB_MGMT_TOKEN_AUTHORITY_OK)
      return KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
   if (result == KB_VAULT_KEY_USE_SEALED)
      return KB_MGMT_TOKEN_AUTHORITY_IPC_SEALED;
   if (result == KB_VAULT_KEY_USE_INTEGRITY || result == KB_VAULT_KEY_USE_UNATTESTED ||
       result == KB_VAULT_KEY_USE_REPLAY ||
       (result == KB_VAULT_KEY_USE_CALLBACK_FAILED &&
        (issue_result == KB_MGMT_TOKEN_AUTHORITY_INVALID ||
         issue_result == KB_MGMT_TOKEN_AUTHORITY_KEY_MISMATCH)))
      return KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
   return KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
}

/* The identity counterpart of protected_issue_t. Separate struct because the
 * record type differs; the signer contract is otherwise identical. */
typedef struct
{
   const kb_identity_token_authority_record_t *record;
   kb_mgmt_token_authority_output_t output;
   kb_mgmt_token_authority_result_t result;
} protected_identity_issue_t;

static int protected_identity_sign(const unsigned char *plaintext, size_t plaintext_len,
                                   void *opaque)
{
   protected_identity_issue_t *issue = opaque;
   if (!issue || !issue->record)
      return -1;
   /* The output buffer is management-sized (8192) and therefore larger than an
    * identity token needs; kb_identity_token_build enforces its own 4096 wire
    * ceiling regardless of the cap it is handed. */
   issue->result = kb_identity_token_authority_sign_pkcs8(
       issue->record, plaintext, plaintext_len, issue->output.jwt, sizeof(issue->output.jwt),
       &issue->output.jwt_len);
   return issue->result == KB_MGMT_TOKEN_AUTHORITY_OK ? 0 : -1;
}

static int same_identity_admission(const kb_identity_token_authority_record_t *a,
                                   const kb_identity_token_authority_record_t *b)
{
   kb_identity_token_authority_record_t left = *a, right = *b;
   left.newly_admitted = 0;
   right.newly_admitted = 0;
   int same = CRYPTO_memcmp(&left, &right, sizeof(left)) == 0;
   OPENSSL_cleanse(&left, sizeof(left));
   OPENSSL_cleanse(&right, sizeof(right));
   return same;
}

static int protected_sign(const unsigned char *plaintext, size_t plaintext_len, void *opaque)
{
   protected_issue_t *issue = opaque;
   if (!issue || !issue->record)
      return -1;
   issue->result = kb_mgmt_token_authority_sign_pkcs8(issue->record, plaintext, plaintext_len,
                                                      issue->output.jwt, sizeof(issue->output.jwt),
                                                      &issue->output.jwt_len);
   return issue->result == KB_MGMT_TOKEN_AUTHORITY_OK ? 0 : -1;
}

static int same_admission(const kb_mgmt_token_authority_record_t *a,
                          const kb_mgmt_token_authority_record_t *b)
{
   kb_mgmt_token_authority_record_t left = *a, right = *b;
   left.newly_admitted = 0;
   right.newly_admitted = 0;
   int same = CRYPTO_memcmp(&left, &right, sizeof(left)) == 0;
   OPENSSL_cleanse(&left, sizeof(left));
   OPENSSL_cleanse(&right, sizeof(right));
   return same;
}

static int reopen(kb_mgmt_token_authority_service_t *service)
{
   return service->reopen_db && service->reopen_db(service->reopen_opaque, service->db) == 0;
}

static kb_mgmt_token_authority_ipc_result_t read_issue(kb_mgmt_token_authority_service_t *service,
                                                       const char *correlation_id, const char *jti,
                                                       kb_mgmt_token_authority_output_t *out)
{
   kb_mgmt_token_authority_output_t retained;
   kb_mgmt_token_authority_record_t use;
   protected_issue_t issue;
   uint8_t fresh_attestation[KB_MGMT_ROOT_ATTEST_MAX];
   uint8_t token_aad[KB_MGMT_TOKEN_ROOT_AAD_MAX];
   char lease_owner[65];
   memset(&retained, 0, sizeof(retained));
   memset(&use, 0, sizeof(use));
   memset(&issue, 0, sizeof(issue));
   memset(fresh_attestation, 0, sizeof(fresh_attestation));
   memset(token_aad, 0, sizeof(token_aad));
   memset(lease_owner, 0, sizeof(lease_owner));

   db2_management_token_authority_result_t db_result =
       db2_management_token_read_readback(service->db, correlation_id, jti, &retained);
   if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      *out = retained;
      return KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
   }
   if (db_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_ABSENT)
      return map_db(db_result);
   if (random_hex(lease_owner) != 0)
      return KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;

   db_result = db2_management_token_read_claim(service->db, correlation_id, jti, lease_owner, &use);
   if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS)
   {
      if (!reopen(service))
         return KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
      db_result = db2_management_token_read_readback(service->db, correlation_id, jti, &retained);
      if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      {
         *out = retained;
         return KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
      }
      return KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
   }
   if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_ABSENT)
   {
      db_result = db2_management_token_read_readback(service->db, correlation_id, jti, &retained);
      if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      {
         *out = retained;
         return KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
      }
      return KB_MGMT_TOKEN_AUTHORITY_IPC_EXPIRED;
   }
   if (db_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      return map_db(db_result);

   kb_mgmt_token_authority_ipc_result_t result = KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
   uint64_t hwm_version = 0;
   size_t fresh_attestation_len = 0;
   if (vault_hwm_read(use.token_custody_key_id, &hwm_version, fresh_attestation,
                      sizeof(fresh_attestation), &fresh_attestation_len) != 0)
      goto done;
   if (hwm_version == 0 || hwm_version > INT64_MAX || hwm_version != (uint64_t)use.token_version ||
       !fresh_attestation_len || fresh_attestation_len != use.hwm_attestation_len ||
       CRYPTO_memcmp(fresh_attestation, use.hwm_attestation, fresh_attestation_len) != 0 ||
       vault_hwm_verify(use.token_custody_key_id, hwm_version, fresh_attestation,
                        fresh_attestation_len) != 0 ||
       vault_hwm_verify(use.token_custody_key_id, hwm_version, use.hwm_attestation,
                        use.hwm_attestation_len) != 0)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
      goto done;
   }
   if (vault_primary_epoch_initialize((uint64_t)use.vault_seal_epoch) != VAULT_MAINTENANCE_OK)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
      goto done;
   }
   uint64_t live_epoch = vault_use_epoch_snapshot();
   if (!live_epoch)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_SEALED;
      goto done;
   }
   issue.record = &use;
   issue.result = KB_MGMT_TOKEN_AUTHORITY_CRYPTO_UNAVAILABLE;
   size_t token_aad_len = 0;
   if (kb_mgmt_token_root_aad(use.envelope.version, token_aad, sizeof(token_aad), &token_aad_len))
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
      goto done;
   }
   result = map_protected(kb_vault_protected_use_with_aad(live_epoch, &use.envelope, token_aad,
                                                          token_aad_len, protected_sign, &issue),
                          issue.result);
   if (result != KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
      goto done;

   db_result = db2_management_token_read_finalize(service->db, correlation_id, jti, lease_owner,
                                                  issue.output.jwt);
   if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS)
   {
      if (!reopen(service))
      {
         result = KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
         goto done;
      }
   }
   else if (db_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      result = map_db(db_result);
      goto done;
   }
   db_result = db2_management_token_read_readback(service->db, correlation_id, jti, &retained);
   if (db_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      result = db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_ABSENT
                   ? KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS
                   : map_db(db_result);
      goto done;
   }
   *out = retained;
   result = KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
done:
   if (result != KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
      OPENSSL_cleanse(out, sizeof(*out));
   OPENSSL_cleanse(&retained, sizeof(retained));
   OPENSSL_cleanse(&use, sizeof(use));
   OPENSSL_cleanse(&issue, sizeof(issue));
   OPENSSL_cleanse(fresh_attestation, sizeof(fresh_attestation));
   OPENSSL_cleanse(token_aad, sizeof(token_aad));
   OPENSSL_cleanse(lease_owner, sizeof(lease_owner));
   return result;
}

/* The data-plane identity mint (proposal per-user-remote-writes-authz.md §4).
 * Structurally the same as the action path above - admit, re-verify the HWM and
 * seal epoch, sign under vault custody, finalize - but over the identity record.
 *
 * The authorization it enforces is NOT carried by the request: the SQL snapshot
 * behind admit/use/finalize re-reads the live write-tier grant every time, so a
 * grant revoked mid-flight makes finalize refuse rather than release a token. */
static kb_mgmt_token_authority_ipc_result_t
identity_issue(kb_mgmt_token_authority_service_t *service, const char *correlation_id,
               const char *jti, kb_mgmt_token_authority_output_t *out)
{
   kb_mgmt_token_authority_ipc_result_t result = KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
   kb_identity_token_authority_record_t admitted, use;
   protected_identity_issue_t issue;
   uint8_t fresh_attestation[KB_MGMT_ROOT_ATTEST_MAX];
   uint8_t token_aad[KB_MGMT_TOKEN_ROOT_AAD_MAX];
   memset(&admitted, 0, sizeof(admitted));
   memset(&use, 0, sizeof(use));
   memset(&issue, 0, sizeof(issue));
   memset(fresh_attestation, 0, sizeof(fresh_attestation));
   memset(token_aad, 0, sizeof(token_aad));

   db2_management_token_authority_result_t db_result =
       db2_management_identity_authority_admit(service->db, correlation_id, jti, &admitted);
   if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS)
   {
      /* Same reasoning as the action path: only an independently reopened exact
       * readback can prove this invocation admitted the tuple. Absence means the
       * COMMIT did not land and retrying the same identifiers is safe; presence
       * cannot distinguish "we inserted it" from "we replayed someone else's",
       * so prefer terminal availability loss over a possible duplicate key use. */
      if (!reopen(service))
      {
         result = KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
         goto done;
      }
      db_result =
          db2_management_identity_authority_readback(service->db, correlation_id, jti, &admitted);
      if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_ABSENT)
      {
         db_result =
             db2_management_identity_authority_admit(service->db, correlation_id, jti, &admitted);
         if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK && !admitted.newly_admitted)
         {
            result = KB_MGMT_TOKEN_AUTHORITY_IPC_ALREADY_USED;
            goto done;
         }
      }
      else if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      {
         result = KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
         goto done;
      }
      if (db_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      {
         result = KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
         goto done;
      }
   }
   else if (db_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      result = map_db(db_result);
      goto done;
   }
   else if (!admitted.newly_admitted)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_ALREADY_USED;
      goto done;
   }

   db_result = db2_management_identity_authority_use_begin(service->db, correlation_id, jti, &use);
   if (db_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      result = map_db(db_result);
      goto done;
   }
   if (!same_identity_admission(&admitted, &use))
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
      goto abort;
   }

   uint64_t hwm_version = 0;
   size_t fresh_attestation_len = 0;
   if (vault_hwm_read(use.token_custody_key_id, &hwm_version, fresh_attestation,
                      sizeof(fresh_attestation), &fresh_attestation_len) != 0)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
      goto abort;
   }
   if (hwm_version == 0 || hwm_version > INT64_MAX || hwm_version != (uint64_t)use.token_version ||
       !fresh_attestation_len || fresh_attestation_len != use.hwm_attestation_len ||
       CRYPTO_memcmp(fresh_attestation, use.hwm_attestation, fresh_attestation_len) != 0 ||
       vault_hwm_verify(use.token_custody_key_id, hwm_version, fresh_attestation,
                        fresh_attestation_len) != 0 ||
       vault_hwm_verify(use.token_custody_key_id, hwm_version, use.hwm_attestation,
                        use.hwm_attestation_len) != 0)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
      goto abort;
   }
   if (vault_primary_epoch_initialize((uint64_t)use.vault_seal_epoch) != VAULT_MAINTENANCE_OK)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
      goto abort;
   }
   uint64_t live_epoch = vault_use_epoch_snapshot();
   if (!live_epoch)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_SEALED;
      goto abort;
   }
   issue.record = &use;
   issue.result = KB_MGMT_TOKEN_AUTHORITY_CRYPTO_UNAVAILABLE;
   size_t token_aad_len = 0;
   if (kb_mgmt_token_root_aad(use.envelope.version, token_aad, sizeof(token_aad), &token_aad_len))
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
      goto abort;
   }
   result = map_protected(kb_vault_protected_use_with_aad(live_epoch, &use.envelope, token_aad,
                                                          token_aad_len, protected_identity_sign,
                                                          &issue),
                          issue.result);
   if (result != KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
      goto abort;

   db_result = db2_management_identity_authority_finalize(service->db);
   if (db_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      result = map_db(db_result);
      goto done;
   }

   memcpy(out->jwt, issue.output.jwt, issue.output.jwt_len + 1);
   out->jwt_len = issue.output.jwt_len;
   result = KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
   goto done;

abort:
   db2_management_token_authority_abort(service->db);
done:
   if (result != KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
      OPENSSL_cleanse(out, sizeof(*out));
   OPENSSL_cleanse(&issue, sizeof(issue));
   OPENSSL_cleanse(&use, sizeof(use));
   OPENSSL_cleanse(&admitted, sizeof(admitted));
   OPENSSL_cleanse(fresh_attestation, sizeof(fresh_attestation));
   OPENSSL_cleanse(token_aad, sizeof(token_aad));
   return result;
}

kb_mgmt_token_authority_ipc_result_t
kb_mgmt_token_authority_service_issue(const char *correlation_id, const char *jti,
                                      kb_mgmt_token_authority_output_t *out, void *opaque)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   kb_mgmt_token_authority_service_t *service = opaque;
   if (!out || !service || !service->db || !exact_hex(correlation_id, 64) || !exact_hex(jti, 64))
      return KB_MGMT_TOKEN_AUTHORITY_IPC_INVALID;
   if (!service->db->connection &&
       (!service->reopen_db || service->reopen_db(service->reopen_opaque, service->db) != 0))
      return KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;

   int old_cancel_state = PTHREAD_CANCEL_ENABLE;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state) != 0)
      return KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;

   db2_management_token_intent_kind_t kind = 0;
   db2_management_token_authority_result_t kind_result =
       db2_management_token_authority_kind(service->db, correlation_id, jti, &kind);
   if (kind_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      (void)pthread_setcancelstate(old_cancel_state, NULL);
      return map_db(kind_result);
   }
   if (kind == DB2_MANAGEMENT_TOKEN_INTENT_READ)
   {
      kb_mgmt_token_authority_ipc_result_t read_result =
          read_issue(service, correlation_id, jti, out);
      (void)pthread_setcancelstate(old_cancel_state, NULL);
      return read_result;
   }
   if (kind == DB2_MANAGEMENT_TOKEN_INTENT_IDENTITY)
   {
      kb_mgmt_token_authority_ipc_result_t identity_result =
          identity_issue(service, correlation_id, jti, out);
      (void)pthread_setcancelstate(old_cancel_state, NULL);
      return identity_result;
   }
   if (kind != DB2_MANAGEMENT_TOKEN_INTENT_ACTION)
   {
      (void)pthread_setcancelstate(old_cancel_state, NULL);
      return KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
   }

   kb_mgmt_token_authority_ipc_result_t result = KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
   kb_mgmt_token_authority_record_t admitted, use;
   protected_issue_t issue;
   uint8_t fresh_attestation[KB_MGMT_ROOT_ATTEST_MAX];
   uint8_t token_aad[KB_MGMT_TOKEN_ROOT_AAD_MAX];
   memset(&admitted, 0, sizeof(admitted));
   memset(&use, 0, sizeof(use));
   memset(&issue, 0, sizeof(issue));
   memset(fresh_attestation, 0, sizeof(fresh_attestation));
   memset(token_aad, 0, sizeof(token_aad));

   db2_management_token_authority_result_t db_result =
       db2_management_token_authority_admit(service->db, correlation_id, jti, &admitted);
   if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS)
   {
      /* The adapter destroyed the ambiguous session. Only an independently
       * reopened exact readback can prove that this invocation admitted the
       * immutable tuple. Absence/denial is terminal rather than a blind retry. */
      if (!service->reopen_db || service->reopen_db(service->reopen_opaque, service->db) != 0)
      {
         result = KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
         goto done;
      }
      db_result =
          db2_management_token_authority_readback(service->db, correlation_id, jti, &admitted);
      if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_ABSENT)
      {
         /* The readback transaction proved the first COMMIT did not land.
          * Retrying the same immutable identifiers is the sole safe retry. */
         db_result =
             db2_management_token_authority_admit(service->db, correlation_id, jti, &admitted);
         if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK && !admitted.newly_admitted)
         {
            result = KB_MGMT_TOKEN_AUTHORITY_IPC_ALREADY_USED;
            goto done;
         }
      }
      else if (db_result == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      {
         /* Readback proves the row exists, but cannot prove whether this
          * invocation inserted it or merely replayed a prior admission before
          * losing the COMMIT acknowledgement. Signing could therefore be a
          * duplicate private-key use; prefer terminal availability loss. */
         result = KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
         goto done;
      }
      if (db_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      {
         result = KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS;
         goto done;
      }
   }
   else if (db_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      result = map_db(db_result);
      goto done;
   }
   else if (!admitted.newly_admitted)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_ALREADY_USED;
      goto done;
   }

   db_result = db2_management_token_authority_use_begin(service->db, correlation_id, jti, &use);
   if (db_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      result = map_db(db_result);
      goto done;
   }
   if (!same_admission(&admitted, &use))
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
      goto abort;
   }

   uint64_t hwm_version = 0;
   size_t fresh_attestation_len = 0;
   if (vault_hwm_read(use.token_custody_key_id, &hwm_version, fresh_attestation,
                      sizeof(fresh_attestation), &fresh_attestation_len) != 0)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE;
      goto abort;
   }
   if (hwm_version == 0 || hwm_version > INT64_MAX || hwm_version != (uint64_t)use.token_version ||
       !fresh_attestation_len || fresh_attestation_len != use.hwm_attestation_len ||
       CRYPTO_memcmp(fresh_attestation, use.hwm_attestation, fresh_attestation_len) != 0 ||
       vault_hwm_verify(use.token_custody_key_id, hwm_version, fresh_attestation,
                        fresh_attestation_len) != 0 ||
       vault_hwm_verify(use.token_custody_key_id, hwm_version, use.hwm_attestation,
                        use.hwm_attestation_len) != 0)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
      goto abort;
   }

   if (vault_primary_epoch_initialize((uint64_t)use.vault_seal_epoch) != VAULT_MAINTENANCE_OK)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
      goto abort;
   }
   uint64_t live_epoch = vault_use_epoch_snapshot();
   if (!live_epoch)
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_SEALED;
      goto abort;
   }
   issue.record = &use;
   issue.result = KB_MGMT_TOKEN_AUTHORITY_CRYPTO_UNAVAILABLE;
   size_t token_aad_len = 0;
   if (kb_mgmt_token_root_aad(use.envelope.version, token_aad, sizeof(token_aad), &token_aad_len))
   {
      result = KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY;
      goto abort;
   }
   kb_vault_key_use_status_t protected_result = kb_vault_protected_use_with_aad(
       live_epoch, &use.envelope, token_aad, token_aad_len, protected_sign, &issue);
   result = map_protected(protected_result, issue.result);
   if (result != KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
      goto abort;

   db_result = db2_management_token_authority_finalize(service->db);
   if (db_result != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      result = map_db(db_result);
      goto done;
   }

   memcpy(out->jwt, issue.output.jwt, issue.output.jwt_len + 1);
   out->jwt_len = issue.output.jwt_len;
   result = KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
   goto done;

abort:
   db2_management_token_authority_abort(service->db);
done:
   if (result != KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
      OPENSSL_cleanse(out, sizeof(*out));
   OPENSSL_cleanse(&issue, sizeof(issue));
   OPENSSL_cleanse(&use, sizeof(use));
   OPENSSL_cleanse(&admitted, sizeof(admitted));
   OPENSSL_cleanse(fresh_attestation, sizeof(fresh_attestation));
   OPENSSL_cleanse(token_aad, sizeof(token_aad));
   (void)pthread_setcancelstate(old_cancel_state, NULL);
   return result;
}
