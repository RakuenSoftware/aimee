#include "kb_mgmt_status_custody.h"

#include "kb_vault_policy.h"
#include "kb_vault_protected_use.h"
#include "management_status_key.h"
#include "vault_server_key.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

static pthread_mutex_t g_custody_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
   kb_mgmt_status_t *status;
   db2_management_status_key_ctx_t *database;
   unsigned char *transcript;
   size_t transcript_size;
   unsigned char *request_hash;
   unsigned char *use_hash;
   uint8_t *fresh_att;
   size_t fresh_att_size;
   db2_vault_key_use_envelope_t *candidate;
   db2_vault_key_use_envelope_t *admitted;
   int guard_open;
   int keep_signature;
   int mutex_locked;
} custody_cleanup_t;

static void custody_cleanup(void *opaque)
{
   custody_cleanup_t *c = opaque;
   int ignored;
   (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &ignored);
   if (c->guard_open)
      (void)db2_management_status_key_guard_end(c->database, 0);
   OPENSSL_cleanse(c->transcript, c->transcript_size);
   OPENSSL_cleanse(c->request_hash, 32);
   OPENSSL_cleanse(c->use_hash, 32);
   OPENSSL_cleanse(c->fresh_att, c->fresh_att_size);
   OPENSSL_cleanse(c->candidate, sizeof(*c->candidate));
   OPENSSL_cleanse(c->admitted, sizeof(*c->admitted));
   if (!c->keep_signature)
      OPENSSL_cleanse(c->status->signature, sizeof(c->status->signature));
   if (c->mutex_locked)
      (void)pthread_mutex_unlock(&g_custody_mutex);
}

static int hash_domain(const char *domain, const unsigned char *p, size_t n, unsigned char out[32])
{
   EVP_MD_CTX *m = EVP_MD_CTX_new();
   unsigned int got = 0;
   int ok = m && EVP_DigestInit_ex(m, EVP_sha256(), NULL) == 1 &&
            EVP_DigestUpdate(m, domain, strlen(domain)) == 1 && EVP_DigestUpdate(m, p, n) == 1 &&
            EVP_DigestFinal_ex(m, out, &got) == 1 && got == 32;
   EVP_MD_CTX_free(m);
   return ok ? 0 : -1;
}

static void hex32(const unsigned char in[32], char out[65])
{
   static const char h[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; ++i)
   {
      out[i * 2] = h[in[i] >> 4];
      out[i * 2 + 1] = h[in[i] & 15];
   }
   out[64] = 0;
}

static int sign_only(const unsigned char *secret, size_t len, void *ctx)
{
   return len == KB_MGMT_STATUS_KEY_LEN ? kb_mgmt_status_sign((kb_mgmt_status_t *)ctx, secret) : -1;
}

kb_mgmt_status_custody_result_t kb_mgmt_status_custody_sign(kb_mgmt_status_t *status, void *opaque)
{
   kb_mgmt_status_custody_t *cfg = opaque;
   if (!status || !cfg || !cfg->database || !cfg->custody_key_id || !cfg->custody_key_id[0] ||
       strlen(cfg->custody_key_id) > 600 || !kb_vault_management_status_keys_allowed())
      return KB_MGMT_STATUS_CUSTODY_UNAVAILABLE;

   unsigned char transcript[2048], request_hash[32] = {0}, use_hash[32] = {0};
   char request_digest[65], use_id[65];
   size_t transcript_len = 0;
   uint8_t fresh_att[DB2_VAULT_KEY_USE_ATTEST_MAX] = {0};
   size_t fresh_att_len = 0;
   uint64_t version = 0;
   db2_vault_key_use_envelope_t candidate, admitted;
   memset(&candidate, 0, sizeof(candidate));
   memset(&admitted, 0, sizeof(admitted));
   kb_mgmt_status_custody_result_t rc = KB_MGMT_STATUS_CUSTODY_UNAVAILABLE;
   int old_cancel_state = PTHREAD_CANCEL_ENABLE;
   custody_cleanup_t cleanup = {
       .status = status,
       .database = cfg->database,
       .transcript = transcript,
       .transcript_size = sizeof(transcript),
       .request_hash = request_hash,
       .use_hash = use_hash,
       .fresh_att = fresh_att,
       .fresh_att_size = sizeof(fresh_att),
       .candidate = &candidate,
       .admitted = &admitted,
   };

   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state) ||
       pthread_mutex_lock(&g_custody_mutex))
   {
      OPENSSL_cleanse(status->signature, sizeof(status->signature));
      (void)pthread_setcancelstate(old_cancel_state, NULL);
      return KB_MGMT_STATUS_CUSTODY_UNAVAILABLE;
   }
   cleanup.mutex_locked = 1;
   pthread_cleanup_push(custody_cleanup, &cleanup);

   if (kb_mgmt_status_transcript(status, transcript, sizeof(transcript), &transcript_len) ||
       hash_domain("aimee.management.status.request.v1\n", transcript, transcript_len,
                   request_hash) ||
       hash_domain("aimee.management.status.use.v1\n", transcript, transcript_len, use_hash))
      goto done;
   hex32(request_hash, request_digest);
   hex32(use_hash, use_id);
   if (vault_hwm_read(cfg->custody_key_id, &version, fresh_att, sizeof(fresh_att),
                      &fresh_att_len) ||
       !version || version > INT64_MAX ||
       vault_hwm_verify(cfg->custody_key_id, version, fresh_att, fresh_att_len) ||
       db2_management_status_key_candidate(cfg->database, cfg->custody_key_id, status->key_id,
                                           (int64_t)version, &candidate) ||
       candidate.version != (int64_t)version ||
       vault_hwm_verify(cfg->custody_key_id, version, candidate.hwm_attestation,
                        candidate.hwm_attestation_len))
      goto done;

   uint64_t local_epoch = vault_use_epoch_snapshot();
   db2_management_status_admission_t p = {
       .use_id = use_id,
       .custody_key_id = cfg->custody_key_id,
       .wire_key_id = status->key_id,
       .version = (int64_t)version,
       .request_digest = request_digest,
       .caller_issuer = status->caller_issuer,
       .caller_serial_norm = status->caller_serial_norm,
       .caller_fingerprint = status->caller_fingerprint,
       .target_server_id = status->target_server_id,
       .target_mgmt_fingerprint = status->target_mgmt_fingerprint,
       .revocation_generation = (int64_t)status->revocation_generation,
       .hwm_attestation = candidate.hwm_attestation,
       .hwm_attestation_len = candidate.hwm_attestation_len,
   };
   int admitted_rc = db2_management_status_key_admit(cfg->database, &p, &admitted);
   if (admitted_rc == 0)
   {
      rc = KB_MGMT_STATUS_CUSTODY_CONFLICT;
      goto done;
   }
   if (admitted_rc < 0)
   {
      rc = admitted_rc == DB2_VAULT_KEY_USE_INTEGRITY ? KB_MGMT_STATUS_CUSTODY_INTEGRITY
                                                       : KB_MGMT_STATUS_CUSTODY_UNAVAILABLE;
      goto done;
   }
   if (admitted_rc != 1 || admitted.version != (int64_t)version ||
       admitted.hwm_attestation_len != candidate.hwm_attestation_len ||
       CRYPTO_memcmp(admitted.hwm_attestation, candidate.hwm_attestation,
                     candidate.hwm_attestation_len))
   {
      rc = KB_MGMT_STATUS_CUSTODY_INTEGRITY;
      goto done;
   }
   if (db2_management_status_key_guard_begin(cfg->database, admitted.seal_epoch))
      goto done;
   cleanup.guard_open = 1;
   if (kb_vault_protected_use(local_epoch, "org:p5-status", "management", "ed25519", &admitted,
                              sign_only, status) != KB_VAULT_KEY_USE_OK)
      goto done;
   if (db2_management_status_key_guard_end(cfg->database, 1))
   {
      cleanup.guard_open = 0;
      goto done;
   }
   cleanup.guard_open = 0;
   rc = KB_MGMT_STATUS_CUSTODY_OK;
done:
   /* Keep the cleanup handler installed while cancellation is restored: a
    * pending cancellation must roll back the guard and erase the signature. */
   (void)pthread_setcancelstate(old_cancel_state, NULL);
   if (old_cancel_state == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   cleanup.keep_signature = rc == KB_MGMT_STATUS_CUSTODY_OK;
   pthread_cleanup_pop(1);
   return rc;
}
