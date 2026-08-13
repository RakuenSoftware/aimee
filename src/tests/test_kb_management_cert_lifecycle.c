#include "kb/kb_management_cert_binding.h"
#include "kb/kb_management_cert_codec.h"
#include "kb/kb_management_cert_crypto.h"
#include "kb/kb_management_cert_storage.h"
#include "kb/kb_management_cert_lifecycle_test.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#ifdef AIMEE_MANAGEMENT_CERT_TESTING
/* The real symbol is db2_lease_begin_at; db2_lease_begin is a macro in db2.h
 * that records the caller's file:line for stuck-lease attribution. */
void db2_lease_begin_at(const char *site)
{
}
void db2_lease_end(void)
{
}
int vault_server_kek(uint8_t kek[32])
{
   memset(kek, 0, 32);
   return 0;
}
int vault_secret_encrypt(const uint8_t dek[32], const uint8_t *aad, size_t aad_len,
                         const uint8_t *plaintext, size_t plaintext_len, uint8_t nonce[12],
                         uint8_t *ciphertext, uint8_t tag[16])
{
   (void)dek;
   (void)aad;
   (void)aad_len;
   memset(nonce, 0x44, 12);
   memset(tag, 0x44, 16);
   for (size_t i = 0; i < plaintext_len; ++i)
      ciphertext[i] = plaintext[i] ^ 0x44;
   return 0;
}
int vault_secret_decrypt(const uint8_t dek[32], const uint8_t *aad, size_t aad_len,
                         const uint8_t nonce[12], const uint8_t *ciphertext, size_t ciphertext_len,
                         const uint8_t tag[16], uint8_t *plaintext)
{
   (void)dek;
   (void)aad;
   (void)aad_len;
   for (size_t i = 0; i < 12; ++i)
      if (nonce[i] != 0x44)
         return -1;
   for (size_t i = 0; i < 16; ++i)
      if (tag[i] != 0x44)
         return -1;
   for (size_t i = 0; i < ciphertext_len; ++i)
      plaintext[i] = ciphertext[i] ^ 0x44;
   return 0;
}

db2_management_client_instance_result_t
db2_management_client_instance_binding_init(const char *issuer, const char *subject,
                                            const uint8_t proof[32], const uint8_t custody[32],
                                            db2_management_client_instance_binding_t *out)
{
   if (!issuer || !subject || !proof || !custody || !out)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID;
   memset(out, 0, sizeof(*out));
   strcpy(out->issuer, issuer);
   strcpy(out->subject, subject);
   memcpy(out->proof_anchor, proof, 32);
   memcpy(out->custody_anchor, custody, 32);
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   unsigned n = 0;
   assert(md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
          EVP_DigestUpdate(md, issuer, strlen(issuer)) == 1 &&
          EVP_DigestUpdate(md, subject, strlen(subject)) == 1 &&
          EVP_DigestUpdate(md, proof, 32) == 1 && EVP_DigestUpdate(md, custody, 32) == 1 &&
          EVP_DigestFinal_ex(md, out->binding_digest, &n) == 1 && n == 32);
   EVP_MD_CTX_free(md);
   return DB2_MANAGEMENT_CLIENT_INSTANCE_OK;
}

#define DB_UNUSED(name, request_type, output_type)                                                 \
   db2_management_client_instance_result_t name(const request_type *r, output_type *o)             \
   {                                                                                               \
      (void)r;                                                                                     \
      if (o)                                                                                       \
         memset(o, 0, sizeof(*o));                                                                 \
      return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;                                           \
   }
DB_UNUSED(db2_management_client_instance_grant_preflight,
          db2_management_client_grant_preflight_request_t, db2_management_client_grant_preflight_t)
DB_UNUSED(db2_management_client_instance_begin_initial, db2_management_client_initial_request_t,
          db2_management_client_pending_t)
DB_UNUSED(db2_management_client_instance_begin_renewal, db2_management_client_renewal_request_t,
          db2_management_client_pending_t)
DB_UNUSED(db2_management_client_instance_activate, db2_management_client_activation_request_t,
          db2_management_client_active_t)
db2_management_client_instance_result_t
db2_management_client_instance_snapshot(const char id[33],
                                        const db2_management_client_instance_binding_t *binding,
                                        db2_management_client_active_t *out)
{
   (void)id;
   (void)binding;
   memset(out, 0, sizeof(*out));
   return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
}
#undef DB_UNUSED

typedef struct
{
   int64_t now;
   unsigned random_counter;
   kb_workload_identity_t identity;
   char lineage[33];
   int pending;
   int active;
   db2_management_client_pending_t issue;
   db2_management_client_active_t enrollment;
   unsigned begin_count;
   unsigned activate_count;
   unsigned snapshot_count;
   unsigned mutate_snapshot_once;
   unsigned mutate_snapshot_pairs;
   unsigned mutate_snapshot_pair_call;
   int mutate_identity_once;
   int mutate_attested_identity_once;
   kb_workload_result_t attest_failure_once;
   db2_management_client_instance_result_t snapshot_failure;
   unsigned snapshot_failure_count;
   kb_management_cert_crash_point_t crash_point;
   int crash_armed;
   int arena_fail_step;
   db2_management_client_issue_state_t terminal_on_begin;
   int storage_fd;
   int corrupt_terminal_clear;
} lifecycle_mock_t;

static int64_t mock_now(void *opaque)
{
   return ((lifecycle_mock_t *)opaque)->now;
}

static int mock_random(void *opaque, uint8_t *out, size_t len)
{
   lifecycle_mock_t *mock = opaque;
   unsigned seed = ++mock->random_counter;
   for (size_t i = 0; i < len; ++i)
      out[i] = (uint8_t)(seed + i * 17U);
   return 0;
}

static kb_workload_result_t mock_attest(void *opaque, const uint8_t challenge[32],
                                        const uint8_t binding[32], kb_workload_identity_t *out)
{
   (void)challenge;
   (void)binding;
   lifecycle_mock_t *mock = opaque;
   if (mock->attest_failure_once)
   {
      kb_workload_result_t result = mock->attest_failure_once;
      mock->attest_failure_once = 0;
      return result;
   }
   *out = mock->identity;
   if (mock->mutate_attested_identity_once)
   {
      mock->mutate_attested_identity_once = 0;
      strcpy(out->subject, "spiffe://example.test/kb/changed");
      out->proof_anchor_id[0] ^= 0xff;
   }
   return KB_WORKLOAD_OK;
}

static kb_workload_result_t mock_copy(void *opaque, const uint8_t challenge[32],
                                      const uint8_t binding[32], const void *input,
                                      size_t input_len, kb_workload_identity_t *identity,
                                      uint8_t *output, size_t cap, size_t *len)
{
   (void)challenge;
   (void)binding;
   if (input_len > cap)
      return KB_WORKLOAD_INVALID;
   memcpy(output, input, input_len);
   *len = input_len;
   *identity = ((lifecycle_mock_t *)opaque)->identity;
   return KB_WORKLOAD_OK;
}

static db2_management_client_instance_result_t
mock_preflight(void *opaque, const db2_management_client_grant_preflight_request_t *request,
               db2_management_client_grant_preflight_t *out)
{
   lifecycle_mock_t *mock = opaque;
   memset(out, 0, sizeof(*out));
   memcpy(out->installation_id, request->installation_id, 33);
   memcpy(out->replacement_lineage_id, mock->lineage, 33);
   out->expires_at_epoch = mock->now + 300;
   return DB2_MANAGEMENT_CLIENT_INSTANCE_OK;
}

static void mock_pending_common(lifecycle_mock_t *mock, const char operation[65],
                                const char installation[33],
                                const db2_management_client_instance_binding_t *binding,
                                int64_t generation, db2_management_client_issue_kind_t kind,
                                const uint8_t csr[32], const uint8_t spki[32])
{
   memset(&mock->issue, 0, sizeof(mock->issue));
   memcpy(mock->issue.installation_id, installation, 33);
   memcpy(mock->issue.replacement_lineage_id, mock->lineage, 33);
   memcpy(mock->issue.binding_digest, binding->binding_digest, 32);
   memcpy(mock->issue.operation_id, operation, 65);
   mock->issue.team_id = 1;
   mock->issue.generation = generation;
   mock->issue.issue_kind = kind;
   mock->issue.issue_state = DB2_MANAGEMENT_CLIENT_ISSUE_PENDING;
   memcpy(mock->issue.csr_digest, csr, 32);
   memcpy(mock->issue.csr_spki_digest, spki, 32);
   mock->issue.pending_expires_at_epoch = mock->now + 600;
   mock->pending = 1;
}

static db2_management_client_instance_result_t
mock_begin_initial(void *opaque, const db2_management_client_initial_request_t *request,
                   db2_management_client_pending_t *out)
{
   lifecycle_mock_t *mock = opaque;
   mock->begin_count++;
   if (!mock->pending)
   {
      mock_pending_common(mock, request->operation_id, request->installation_id, &request->binding,
                          1, DB2_MANAGEMENT_CLIENT_ISSUE_INITIAL, request->csr_digest,
                          request->csr_spki_digest);
      memcpy(mock->issue.authority_id, request->authority_id, 33);
   }
   else if (strcmp(mock->issue.operation_id, request->operation_id))
      return DB2_MANAGEMENT_CLIENT_INSTANCE_CONFLICT;
   *out = mock->issue;
   if (mock->terminal_on_begin)
   {
      mock->issue.issue_state = mock->terminal_on_begin;
      out->issue_state = mock->terminal_on_begin;
      mock->pending = 0;
   }
   if (mock->active)
      out->issue_state = DB2_MANAGEMENT_CLIENT_ISSUE_ACTIVE;
   return DB2_MANAGEMENT_CLIENT_INSTANCE_OK;
}

static db2_management_client_instance_result_t
mock_begin_renewal(void *opaque, const db2_management_client_renewal_request_t *request,
                   db2_management_client_pending_t *out)
{
   lifecycle_mock_t *mock = opaque;
   mock->begin_count++;
   if (!mock->pending)
   {
      mock_pending_common(mock, request->operation_id, request->installation_id, &request->binding,
                          request->generation, DB2_MANAGEMENT_CLIENT_ISSUE_RENEW,
                          request->csr_digest, request->csr_spki_digest);
      memcpy(mock->issue.authority_id, mock->enrollment.authority_id, 33);
      mock->issue.has_previous = 1;
      mock->issue.previous_enrollment_id = request->previous_enrollment_id;
      strcpy(mock->issue.previous_cert_issuer, request->previous_cert_issuer);
      strcpy(mock->issue.previous_cert_serial_norm, request->previous_cert_serial_norm);
      memcpy(mock->issue.previous_cert_fingerprint, request->previous_cert_fingerprint, 32);
   }
   else if (strcmp(mock->issue.operation_id, request->operation_id))
      return DB2_MANAGEMENT_CLIENT_INSTANCE_CONFLICT;
   *out = mock->issue;
   if (mock->terminal_on_begin)
   {
      mock->issue.issue_state = mock->terminal_on_begin;
      out->issue_state = mock->terminal_on_begin;
      mock->pending = 0;
   }
   if (mock->active && !strcmp(mock->enrollment.operation_id, request->operation_id))
      out->issue_state = DB2_MANAGEMENT_CLIENT_ISSUE_ACTIVE;
   return DB2_MANAGEMENT_CLIENT_INSTANCE_OK;
}

static db2_management_client_instance_result_t
mock_activate(void *opaque, const db2_management_client_activation_request_t *request,
              db2_management_client_active_t *out)
{
   lifecycle_mock_t *mock = opaque;
   mock->activate_count++;
   if (mock->active && !strcmp(mock->enrollment.operation_id, request->operation_id))
   {
      *out = mock->enrollment;
      out->replayed = 1;
      return DB2_MANAGEMENT_CLIENT_INSTANCE_OK;
   }
   memset(&mock->enrollment, 0, sizeof(mock->enrollment));
   memcpy(mock->enrollment.installation_id, request->installation_id, 33);
   memcpy(mock->enrollment.replacement_lineage_id, mock->lineage, 33);
   memcpy(mock->enrollment.authority_id, mock->issue.authority_id, 33);
   mock->enrollment.team_id = 1;
   memcpy(mock->enrollment.binding_digest, request->binding.binding_digest, 32);
   mock->enrollment.generation = request->generation;
   mock->enrollment.enrollment_id = request->generation;
   memcpy(mock->enrollment.operation_id, request->operation_id, 65);
   mock->enrollment.issue_kind = request->issue_kind;
   mock->enrollment.issue_state = DB2_MANAGEMENT_CLIENT_ISSUE_ACTIVE;
   memcpy(mock->enrollment.csr_digest, request->csr_digest, 32);
   memcpy(mock->enrollment.csr_spki_digest, request->csr_spki_digest, 32);
   memcpy(mock->enrollment.public_bundle_digest, request->public_bundle_digest, 32);
   strcpy(mock->enrollment.cert_issuer, request->leaf_issuer);
   strcpy(mock->enrollment.cert_serial_norm, request->leaf_serial_norm);
   int identity_len =
       snprintf(mock->enrollment.cert_identity, sizeof(mock->enrollment.cert_identity),
                "cert:%s:%s", request->leaf_issuer, request->leaf_serial_norm);
   assert(identity_len > 0 && (size_t)identity_len < sizeof(mock->enrollment.cert_identity));
   memcpy(mock->enrollment.cert_fingerprint, request->leaf_fingerprint, 32);
   memcpy(mock->enrollment.cert_spki_digest, request->leaf_spki_digest, 32);
   mock->enrollment.cert_not_before_epoch = request->leaf_not_before_epoch;
   mock->enrollment.cert_not_after_epoch = request->leaf_not_after_epoch;
   mock->enrollment.revocation_generation = request->generation;
   mock->enrollment.activated_at_epoch = mock->now;
   mock->active = 1;
   mock->pending = 0;
   *out = mock->enrollment;
   return DB2_MANAGEMENT_CLIENT_INSTANCE_OK;
}

static db2_management_client_instance_result_t
mock_snapshot(void *opaque, const char installation[33],
              const db2_management_client_instance_binding_t *binding,
              db2_management_client_active_t *out)
{
   lifecycle_mock_t *mock = opaque;
   (void)installation;
   (void)binding;
   mock->snapshot_count++;
   if (mock->snapshot_failure_count)
   {
      mock->snapshot_failure_count--;
      memset(out, 0, sizeof(*out));
      return mock->snapshot_failure;
   }
   if (!mock->active)
   {
      memset(out, 0, sizeof(*out));
      return DB2_MANAGEMENT_CLIENT_INSTANCE_DENIED;
   }
   *out = mock->enrollment;
   if (mock->mutate_identity_once)
   {
      mock->mutate_identity_once = 0;
      strcpy(out->cert_identity, "not-the-management-identity");
   }
   if (mock->mutate_snapshot_pairs && ++mock->mutate_snapshot_pair_call == 2)
   {
      out->revocation_generation++;
      mock->mutate_snapshot_pair_call = 0;
      mock->mutate_snapshot_pairs--;
   }
   if (mock->mutate_snapshot_once && --mock->mutate_snapshot_once == 0)
      out->revocation_generation++;
   return DB2_MANAGEMENT_CLIENT_INSTANCE_OK;
}

static int mock_crash(void *opaque, kb_management_cert_crash_point_t point)
{
   lifecycle_mock_t *mock = opaque;
   if (point == KB_MANAGEMENT_CERT_CRASH_BEFORE_TERMINAL_CLEAR && mock->corrupt_terminal_clear)
   {
      mock->corrupt_terminal_clear = 0;
      assert(fchmodat(mock->storage_fd, "pending", 0400, 0) == 0);
      return 0;
   }
   if (mock->crash_armed && mock->crash_point == point)
   {
      mock->crash_armed = 0;
      return 1;
   }
   return 0;
}

static int mock_arena_fail(void *opaque, int step)
{
   return ((lifecycle_mock_t *)opaque)->arena_fail_step == step;
}

static const kb_management_cert_test_ops_t mock_ops = {
    .now = mock_now,
    .random = mock_random,
    .attest = mock_attest,
    .wrap = mock_copy,
    .unwrap = mock_copy,
    .preflight = mock_preflight,
    .begin_initial = mock_begin_initial,
    .begin_renewal = mock_begin_renewal,
    .activate = mock_activate,
    .snapshot = mock_snapshot,
    .crash = mock_crash,
    .arena_fail = mock_arena_fail,
};
#endif

static int zeroed(const void *p, size_t n)
{
   const unsigned char *bytes = p;
   unsigned char any = 0;
   for (size_t i = 0; i < n; ++i)
      any |= bytes[i];
   return any == 0;
}

static void fill_hex(char *out, size_t n, char value)
{
   memset(out, value, n);
   out[n] = 0;
}

static void test_plaintext_codecs(void)
{
   uint8_t key[73], csr[91], leaf[101], ca[83], encoded[1024];
   memset(key, 1, sizeof(key));
   memset(csr, 2, sizeof(csr));
   memset(leaf, 3, sizeof(leaf));
   memset(ca, 4, sizeof(ca));
   size_t n = 0;
   assert(kb_management_cert_key_intent_encode(key, sizeof(key), csr, sizeof(csr), encoded,
                                               sizeof(encoded), &n) == 0);
   kb_management_cert_key_intent_view_t intent;
   assert(kb_management_cert_key_intent_decode(encoded, n, &intent) == 0);
   assert(intent.key_der_len == sizeof(key) && !memcmp(intent.key_der, key, sizeof(key)));
   assert(intent.csr_der_len == sizeof(csr) && !memcmp(intent.csr_der, csr, sizeof(csr)));
   uint8_t encoded_before[sizeof(encoded)];
   memcpy(encoded_before, encoded, sizeof(encoded));
   assert(kb_management_cert_key_intent_decode(
              encoded, n, (kb_management_cert_key_intent_view_t *)encoded) != 0);
   assert(!memcmp(encoded, encoded_before, sizeof(encoded)));
   assert(kb_management_cert_key_intent_decode(encoded, n - 1, &intent) != 0);
   assert(zeroed(&intent, sizeof(intent)));
   encoded[n] = 0;
   assert(kb_management_cert_key_intent_decode(encoded, n + 1, &intent) != 0);

   assert(kb_management_cert_bundle_encode(key, sizeof(key), leaf, sizeof(leaf), ca, sizeof(ca),
                                           encoded, sizeof(encoded), &n) == 0);
   kb_management_cert_bundle_view_t bundle;
   assert(kb_management_cert_bundle_decode(encoded, n, &bundle) == 0);
   assert(bundle.key_der_len == sizeof(key) && bundle.leaf_der_len == sizeof(leaf) &&
          bundle.ca_der_len == sizeof(ca));
   memset(encoded, 0xa5, sizeof(encoded));
   n = 99;
   assert(kb_management_cert_bundle_encode(NULL, 1, leaf, sizeof(leaf), ca, sizeof(ca), encoded,
                                           sizeof(encoded), &n) != 0);
   assert(n == 0 && zeroed(encoded, sizeof(encoded)));
}

static void base_intent(kb_management_cert_intent_view_t *v, uint8_t cipher[64])
{
   memset(v, 0, sizeof(*v));
   fill_hex(v->installation_id, 32, '1');
   fill_hex(v->lineage_id, 32, '2');
   fill_hex(v->operation_id, 64, '3');
   fill_hex(v->authority_id, 32, '4');
   fill_hex(v->storage_id, 32, '5');
   v->generation = 7;
   v->provider_kind = KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1;
   memset(v->nonce, 6, sizeof(v->nonce));
   memset(v->binding_digest, 7, sizeof(v->binding_digest));
   memset(v->csr_digest, 8, sizeof(v->csr_digest));
   memset(v->csr_spki_digest, 9, sizeof(v->csr_spki_digest));
   memset(v->custody_binding_digest, 10, sizeof(v->custody_binding_digest));
   memset(cipher, 11, 64);
   v->ciphertext = cipher;
   v->ciphertext_len = 64;
}

static void base_candidate(kb_management_cert_candidate_view_t *v, const char operation[65],
                           uint8_t cipher[64], uint8_t cipher_byte)
{
   memset(v, 0, sizeof(*v));
   fill_hex(v->installation_id, 32, '1');
   fill_hex(v->lineage_id, 32, '2');
   memcpy(v->operation_id, operation, 65);
   fill_hex(v->authority_id, 32, '4');
   fill_hex(v->storage_id, 32, '5');
   v->generation = 7;
   v->provider_kind = KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1;
   memset(v->nonce, 6, 32);
   memset(v->binding_digest, 7, 32);
   memset(v->csr_digest, 8, 32);
   memset(v->csr_spki_digest, 9, 32);
   memset(v->public_bundle_digest, 10, 32);
   memset(v->custody_binding_digest, 11, 32);
   strcpy(v->issuer, "/CN=aimee-kb-ca");
   strcpy(v->ca_issuer, "/CN=aimee-kb-ca");
   strcpy(v->serial_norm, "01abcdef");
   memset(v->fingerprint, 12, 32);
   memset(v->spki_digest, 13, 32);
   memset(v->ca_fingerprint, 14, 32);
   v->not_before_epoch = 1000;
   v->not_after_epoch = 4600;
   memset(cipher, cipher_byte, 64);
   v->ciphertext = cipher;
   v->ciphertext_len = 64;
}

static void test_record_codecs(void)
{
   uint8_t cipher[64], encoded[4096];
   kb_management_cert_intent_view_t intent, decoded;
   base_intent(&intent, cipher);
   size_t n = 0;
   assert(kb_management_cert_intent_encode(&intent, encoded, sizeof(encoded), &n) == 0);
   assert(kb_management_cert_intent_decode(encoded, n, &decoded) == 0);
   assert(!strcmp(decoded.authority_id, intent.authority_id) && decoded.generation == 7 &&
          decoded.provider_kind == KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1 &&
          decoded.ciphertext_len == sizeof(cipher) &&
          !memcmp(decoded.ciphertext, cipher, sizeof(cipher)));
   for (size_t i = 0; i < n; ++i)
   {
      assert(kb_management_cert_intent_decode(encoded, i, &decoded) != 0);
      assert(zeroed(&decoded, sizeof(decoded)));
   }
   uint8_t alias[256];
   memset(alias, 0x5a, sizeof(alias));
   uint8_t alias_before[sizeof(alias)];
   memcpy(alias_before, alias, sizeof(alias));
   size_t alias_len = 17;
   assert(kb_management_cert_key_intent_encode(alias, 16, cipher, sizeof(cipher), alias,
                                               sizeof(alias), &alias_len) != 0);
   assert(alias_len == 17 && !memcmp(alias, alias_before, sizeof(alias)));

   kb_management_cert_candidate_view_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   memcpy(candidate.installation_id, intent.installation_id, 33);
   memcpy(candidate.lineage_id, intent.lineage_id, 33);
   memcpy(candidate.operation_id, intent.operation_id, 65);
   memcpy(candidate.authority_id, intent.authority_id, 33);
   memcpy(candidate.storage_id, intent.storage_id, 33);
   candidate.generation = intent.generation;
   candidate.provider_kind = intent.provider_kind;
   memcpy(candidate.nonce, intent.nonce, 32);
   memcpy(candidate.binding_digest, intent.binding_digest, 32);
   memcpy(candidate.csr_digest, intent.csr_digest, 32);
   memcpy(candidate.csr_spki_digest, intent.csr_spki_digest, 32);
   memset(candidate.public_bundle_digest, 12, 32);
   memcpy(candidate.custody_binding_digest, intent.custody_binding_digest, 32);
   strcpy(candidate.issuer, "/CN=aimee-kb-ca");
   strcpy(candidate.ca_issuer, "/CN=aimee-kb-ca");
   strcpy(candidate.serial_norm, "01abcdef");
   memset(candidate.fingerprint, 13, 32);
   memset(candidate.spki_digest, 14, 32);
   memset(candidate.ca_fingerprint, 15, 32);
   candidate.not_before_epoch = 1000;
   candidate.not_after_epoch = 4600;
   candidate.ciphertext = cipher;
   candidate.ciphertext_len = sizeof(cipher);
   assert(kb_management_cert_candidate_encode(&candidate, encoded, sizeof(encoded), &n) == 0);
   kb_management_cert_candidate_view_t candidate_out;
   assert(kb_management_cert_candidate_decode(encoded, n, &candidate_out) == 0);
   assert(!strcmp(candidate_out.authority_id, candidate.authority_id) &&
          candidate_out.provider_kind == candidate.provider_kind &&
          !strcmp(candidate_out.issuer, candidate.issuer) &&
          !strcmp(candidate_out.ca_issuer, candidate.ca_issuer) &&
          !memcmp(candidate_out.ca_fingerprint, candidate.ca_fingerprint, 32));
   for (size_t i = 0; i < n; ++i)
   {
      assert(kb_management_cert_candidate_decode(encoded, i, &candidate_out) != 0);
      assert(zeroed(&candidate_out, sizeof(candidate_out)));
   }
   encoded[n] = 0;
   assert(kb_management_cert_candidate_decode(encoded, n + 1, &candidate_out) != 0);

   kb_management_cert_manifest_t manifest = {.generation = 7};
   memcpy(manifest.operation_id, intent.operation_id, 65);
   memset(manifest.public_bundle_digest, 12, 32);
   assert(kb_management_cert_manifest_encode(&manifest, encoded, sizeof(encoded), &n) == 0);
   kb_management_cert_manifest_t manifest_out;
   assert(kb_management_cert_manifest_decode(encoded, n, &manifest_out) == 0);
   assert(manifest_out.generation == 7 &&
          !memcmp(manifest_out.public_bundle_digest, manifest.public_bundle_digest, 32));
   assert(kb_management_cert_manifest_decode(NULL, n, &manifest_out) != 0);
   assert(zeroed(&manifest_out, sizeof(manifest_out)));

   kb_management_cert_pending_manifest_t pending = {.generation = 7,
                                                    .issue_kind = KB_MANAGEMENT_CERT_ISSUE_INITIAL};
   memcpy(pending.installation_id, intent.installation_id, 33);
   memcpy(pending.lineage_id, intent.lineage_id, 33);
   memcpy(pending.operation_id, intent.operation_id, 65);
   memcpy(pending.authority_id, intent.authority_id, 33);
   memset(pending.binding_digest, 0x31, 32);
   memset(pending.intent_record_digest, 0x32, 32);
   assert(kb_management_cert_pending_encode(&pending, encoded, sizeof(encoded), &n) == 0);
   kb_management_cert_pending_manifest_t pending_out;
   assert(kb_management_cert_pending_decode(encoded, n, &pending_out) == 0);
   assert(pending_out.issue_kind == KB_MANAGEMENT_CERT_ISSUE_INITIAL &&
          !memcmp(pending_out.intent_record_digest, pending.intent_record_digest, 32));
   for (size_t i = 0; i < n; ++i)
   {
      assert(kb_management_cert_pending_decode(encoded, i, &pending_out) != 0);
      assert(zeroed(&pending_out, sizeof(pending_out)));
   }
   encoded[n] = 0;
   assert(kb_management_cert_pending_decode(encoded, n + 1, &pending_out) != 0);
   assert(zeroed(&pending_out, sizeof(pending_out)));
   uint8_t pending_before_bytes[sizeof(encoded)];
   memcpy(pending_before_bytes, encoded, sizeof(encoded));
   assert(kb_management_cert_pending_decode(encoded, n,
                                            (kb_management_cert_pending_manifest_t *)encoded) != 0);
   assert(!memcmp(encoded, pending_before_bytes, sizeof(encoded)));
   pending.issue_kind = (kb_management_cert_issue_kind_t)3;
   assert(kb_management_cert_pending_encode(&pending, encoded, sizeof(encoded), &n) != 0);
   pending.issue_kind = KB_MANAGEMENT_CERT_ISSUE_INITIAL;
   kb_management_cert_pending_manifest_t pending_before = pending;
   assert(kb_management_cert_pending_encode(&pending, encoded, sizeof(encoded),
                                            (size_t *)&pending.generation) != 0);
   assert(!memcmp(&pending, &pending_before, sizeof(pending)));
}

static void base_binding(kb_management_cert_intent_binding_t *v)
{
   memset(v, 0, sizeof(*v));
   fill_hex(v->installation_id, 32, '1');
   fill_hex(v->lineage_id, 32, '2');
   fill_hex(v->operation_id, 64, '3');
   fill_hex(v->authority_id, 32, '4');
   fill_hex(v->storage_id, 32, '5');
   v->generation = INT64_C(0x0102030405060708);
   v->provider_kind = KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1;
   strcpy(v->workload_issuer, "https://issuer.example");
   strcpy(v->workload_subject, "spiffe://example/kb");
   memset(v->binding_digest, 0x06, 32);
   memset(v->proof_anchor, 0x07, 32);
   memset(v->custody_anchor, 0x08, 32);
   memset(v->csr_digest, 0x09, 32);
   memset(v->csr_spki_digest, 0x0a, 32);
   memset(v->nonce, 0x0b, 32);
}

static void assert_digest(const uint8_t digest[32], const char expected[65])
{
   static const char hex[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; ++i)
   {
      assert(expected[2 * i] == hex[digest[i] >> 4]);
      assert(expected[2 * i + 1] == hex[digest[i] & 15]);
   }
   assert(expected[64] == 0);
}

static void test_binding_transcripts(void)
{
   uint8_t transcript[KB_MANAGEMENT_CERT_TRANSCRIPT_MAX], digest[32], changed[32];
   size_t n = 0;
   char installation[33];
   fill_hex(installation, 32, '1');
   assert(kb_management_cert_attest_transcript(installation, KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1,
                                               transcript, sizeof(transcript), &n) == 0);
   assert(n == strlen("aimee.p5.management-attest.v1") + 4 + 32 + 4);
   assert(!memcmp(transcript, "aimee.p5.management-attest.v1", 29));
   assert(transcript[29] == 0 && transcript[30] == 0 && transcript[31] == 0 &&
          transcript[32] == 32);
   assert(kb_management_cert_attest_binding(installation, KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1,
                                            digest) == 0);
   assert_digest(digest, "f5855d045ab7b08f080829ecc54c00088b4d367aacd05e3118060b1175ff32fa");

   kb_management_cert_intent_binding_t intent;
   base_binding(&intent);
   assert(kb_management_cert_intent_transcript(&intent, transcript, sizeof(transcript), &n) == 0);
   assert(n > 400 && !memcmp(transcript, "aimee.p5.management-key-intent-custody.v1", 41));
   assert(kb_management_cert_intent_binding(&intent, digest) == 0);
   assert_digest(digest, "8d8e376e5364b59f1f4dbddb41baa439c5b140e460a16b283ea49428432bd9b2");
   intent.authority_id[0] = '6';
   assert(kb_management_cert_intent_binding(&intent, changed) == 0);
   assert(CRYPTO_memcmp(digest, changed, 32) != 0);
   intent.authority_id[0] = '4';
   intent.csr_spki_digest[0] ^= 1;
   assert(kb_management_cert_intent_binding(&intent, changed) == 0);
   assert(CRYPTO_memcmp(digest, changed, 32) != 0);
   intent.csr_spki_digest[0] ^= 1;
   kb_management_cert_intent_binding_t intent_before = intent;
   assert(kb_management_cert_intent_binding(&intent, intent.binding_digest) != 0);
   assert(!memcmp(&intent, &intent_before, sizeof(intent)));

   kb_management_cert_candidate_binding_t candidate = {0};
   candidate.intent = intent;
   strcpy(candidate.ca_issuer, "/CN=aimee-kb-ca");
   strcpy(candidate.leaf_issuer, "/CN=aimee-kb-ca");
   strcpy(candidate.leaf_serial_norm, "01abcdef");
   memset(candidate.ca_fingerprint, 0x0c, 32);
   memset(candidate.leaf_fingerprint, 0x0d, 32);
   memset(candidate.leaf_spki_digest, 0x0e, 32);
   candidate.not_before_epoch = 1000;
   candidate.not_after_epoch = 4600;
   memset(candidate.public_bundle_digest, 0x0f, 32);
   assert(kb_management_cert_candidate_binding(&candidate, digest) == 0);
   assert_digest(digest, "c5fd7ec116dd78a74fdc66f0d9b57af2c6203355cb39b34c0f6c196b00ed67ae");
   candidate.ca_fingerprint[0] ^= 1;
   assert(kb_management_cert_candidate_binding(&candidate, changed) == 0);
   assert(CRYPTO_memcmp(digest, changed, 32) != 0);
   candidate.ca_fingerprint[0] ^= 1;
   candidate.intent.operation_id[0] = '7';
   assert(kb_management_cert_candidate_binding(&candidate, changed) == 0);
   assert(CRYPTO_memcmp(digest, changed, 32) != 0);

   size_t alias_len = 77;
   uint8_t alias_before[sizeof(intent)];
   memcpy(alias_before, &intent, sizeof(intent));
   assert(kb_management_cert_intent_transcript(&intent, (uint8_t *)&intent, sizeof(intent),
                                               &alias_len) != 0);
   assert(alias_len == 77 && !memcmp(&intent, alias_before, sizeof(intent)));
   memset(transcript, 0xa5, sizeof(transcript));
   n = 1;
   intent.provider_kind = KB_WORKLOAD_PROVIDER_NONE;
   assert(kb_management_cert_intent_transcript(&intent, transcript, sizeof(transcript), &n) != 0);
   assert(n == 0 && zeroed(transcript, sizeof(transcript)));
   transcript[0] = 0x5a;
   n = 19;
   assert(kb_management_cert_attest_transcript(installation, KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1,
                                               transcript, KB_MANAGEMENT_CERT_TRANSCRIPT_MAX + 1U,
                                               &n) != 0);
   assert(transcript[0] == 0x5a && n == 19);
   base_binding(&intent);
   intent_before = intent;
   assert(kb_management_cert_intent_transcript(&intent, transcript, sizeof(transcript),
                                               (size_t *)&intent.generation) != 0);
   assert(!memcmp(&intent, &intent_before, sizeof(intent)));
}

static void test_key_and_csr(void)
{
   kb_management_cert_key_material_t generated, recovered;
   assert(kb_management_cert_key_generate(&generated) == 0);
   assert(generated.key_der_len > 1000 && generated.csr_der_len > 500 &&
          strstr(generated.csr_pem, "BEGIN CERTIFICATE REQUEST"));
   assert(kb_management_cert_key_intent_verify(generated.key_der, generated.key_der_len,
                                               generated.csr_der, generated.csr_der_len,
                                               &recovered) == 0);
   assert(!memcmp(generated.csr_digest, recovered.csr_digest, 32) &&
          !memcmp(generated.csr_spki_digest, recovered.csr_spki_digest, 32));
   uint8_t corrupt[4096];
   memcpy(corrupt, generated.csr_der, generated.csr_der_len);
   corrupt[generated.csr_der_len / 2] ^= 1;
   assert(kb_management_cert_key_intent_verify(generated.key_der, generated.key_der_len, corrupt,
                                               generated.csr_der_len, &recovered) != 0);
   assert(zeroed(&recovered, sizeof(recovered)));
   kb_management_cert_key_material_clear(&generated);
   assert(zeroed(&generated, sizeof(generated)));

   uint8_t hash_alias[64], hash_alias_before[64];
   memset(hash_alias, 0x5a, sizeof(hash_alias));
   memcpy(hash_alias_before, hash_alias, sizeof(hash_alias));
   assert(kb_management_cert_sha256(hash_alias, sizeof(hash_alias), hash_alias) != 0);
   assert(!memcmp(hash_alias, hash_alias_before, sizeof(hash_alias)));

   kb_management_cert_bundle_t secret;
   memset(&secret, 0xa5, sizeof(secret));
   kb_management_cert_bundle_clear(&secret);
   assert(zeroed(&secret, sizeof(secret)));
}

static void test_lifecycle_constructor_guards(void)
{
   kb_management_cert_lifecycle_t *lifecycle = (kb_management_cert_lifecycle_t *)1;
   kb_management_cert_config_t config = {0};
   fill_hex(config.installation_id, 32, '1');
   config.custodied_ca_dir = "/root/aimee-ca";
   config.bundle_dir = "/root/aimee-management-bundle";
   assert(kb_management_cert_lifecycle_open(&config, &lifecycle) == KB_MANAGEMENT_CERT_DISABLED);
   assert(lifecycle == NULL);
   config.installation_id[0] = 'G';
   lifecycle = (kb_management_cert_lifecycle_t *)1;
   assert(kb_management_cert_lifecycle_open(&config, &lifecycle) == KB_MANAGEMENT_CERT_INVALID);
   assert(lifecycle == NULL);
   fill_hex(config.installation_id, 32, '1');
   config.bundle_dir = "/root/../bad";
   assert(kb_management_cert_lifecycle_open(&config, &lifecycle) == KB_MANAGEMENT_CERT_INVALID);
   assert(lifecycle == NULL);
}

#ifdef AIMEE_MANAGEMENT_CERT_TESTING
static void test_active_identity_canonicalization(void)
{
   assert(kb_management_cert_identity_matches_for_test("/CN=aimee-kb-ca", "0123abcd",
                                                       "cert:/CN=aimee-kb-ca:0123abcd"));
   assert(kb_management_cert_identity_matches_for_test("issuer:%name", "ab:cd%ef",
                                                       "cert:issuer%3A%25name:ab%3Acd%25ef"));
   assert(!kb_management_cert_identity_matches_for_test("issuer:%name", "ab:cd%ef",
                                                        "cert:issuer:%name:ab:cd%ef"));
   char issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   memset(issuer, '%', sizeof(issuer) - 1);
   issuer[sizeof(issuer) - 1] = '\0';
   assert(!kb_management_cert_identity_matches_for_test(issuer, "aa", "cert:overflow:aa"));
}
#endif

#ifdef AIMEE_MANAGEMENT_CERT_TESTING
static void remove_tree_files(const char *path)
{
   DIR *dir = opendir(path);
   assert(dir);
   struct dirent *entry;
   while ((entry = readdir(dir)) != NULL)
   {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
         continue;
      char file[512];
      assert(snprintf(file, sizeof(file), "%s/%s", path, entry->d_name) > 0);
      assert(unlink(file) == 0);
   }
   closedir(dir);
   assert(rmdir(path) == 0);
}

static unsigned directory_file_count(int dir_fd)
{
   int copy = dup(dir_fd);
   assert(copy >= 0);
   DIR *dir = fdopendir(copy);
   assert(dir);
   unsigned count = 0;
   struct dirent *entry;
   while ((entry = readdir(dir)) != NULL)
      if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
         count++;
   closedir(dir);
   return count;
}

static void test_noncanonical_custodied_hex(void)
{
   char dir[256];
   snprintf(dir, sizeof dir, "%s/aimee-p5b2c-hex.XXXXXX", platform_tmpdir());
   assert(mkdtemp(dir));
   kb_pki_ca_t ca, loaded;
   assert(kb_pki_ca_generate(&ca) == 0 && kb_pki_ca_save_custodied(dir, &ca) == 0);
   char path[512];
   assert(snprintf(path, sizeof(path), "%s/ca-key.vault", dir) > 0);
   int fd = open(path, O_RDWR | O_CLOEXEC);
   assert(fd >= 0);
   char envelope[KB_PKI_KEY_PEM_MAX * 2 + 256];
   ssize_t n = read(fd, envelope, sizeof(envelope));
   assert(n > 0 && (size_t)n < sizeof(envelope));
   char *digit = memchr(envelope, '4', (size_t)n);
   assert(digit);
   *digit = '['; /* The former decoder incorrectly mapped '[' to nibble 4. */
   assert(lseek(fd, 0, SEEK_SET) == 0 && write(fd, envelope, (size_t)n) == n &&
          ftruncate(fd, n) == 0 && close(fd) == 0);
   memset(&loaded, 0xa5, sizeof(loaded));
   assert(kb_pki_ca_load_custodied_ex(dir, &loaded) == KB_PKI_CA_LOAD_INTEGRITY);
   assert(zeroed(&loaded, sizeof(loaded)));
   OPENSSL_cleanse(&ca, sizeof(ca));
   remove_tree_files(dir);
}

static void test_lifecycle_orchestration(void)
{
   char ca_dir[256];
   snprintf(ca_dir, sizeof ca_dir, "%s/aimee-p5b2c-ca.XXXXXX", platform_tmpdir());
   char bundle_dir[256];
   snprintf(bundle_dir, sizeof bundle_dir, "%s/aimee-p5b2c-bundle.XXXXXX", platform_tmpdir());
   assert(mkdtemp(ca_dir) && mkdtemp(bundle_dir));
   assert(chmod(ca_dir, 0700) == 0 && chmod(bundle_dir, 0700) == 0);
   kb_pki_ca_t ca;
   assert(kb_pki_ca_generate(&ca) == 0 && kb_pki_ca_save(ca_dir, &ca) == 0);
   OPENSSL_cleanse(&ca, sizeof(ca));
   int dir_fd = open(bundle_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   assert(dir_fd >= 0);

   lifecycle_mock_t mock = {.now = (int64_t)time(NULL)};
   mock.storage_fd = dir_fd;
   strcpy(mock.identity.issuer, "https://issuer.example");
   strcpy(mock.identity.subject, "spiffe://example.test/kb/alpha");
   memset(mock.identity.proof_anchor_id, 0x31, 32);
   memset(mock.identity.custody_anchor_id, 0x42, 32);
   fill_hex(mock.lineage, 32, '2');
   kb_management_cert_config_t config = {0};
   fill_hex(config.installation_id, 32, '1');
   config.custodied_ca_dir = ca_dir;
   config.bundle_dir = bundle_dir;
   kb_management_cert_lifecycle_t *lifecycle = NULL;
   assert(kb_management_cert_lifecycle_open_for_test(&config, KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1,
                                                     dir_fd, &mock_ops, &mock,
                                                     &lifecycle) == KB_MANAGEMENT_CERT_OK);

   mock.arena_fail_step = 2;
   kb_management_cert_active_t active;
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
          KB_MANAGEMENT_CERT_UNAVAILABLE);
   assert(zeroed(&active, sizeof(active)) && mock.snapshot_count == 0 && mock.begin_count == 0);
   mock.arena_fail_step = 0;

   /* A crash after authoritative begin must retain and replay the exact intent
    * rather than generating a second operation/key. */
   mock.crash_point = KB_MANAGEMENT_CERT_CRASH_AFTER_BEGIN;
   mock.crash_armed = 1;
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
          KB_MANAGEMENT_CERT_UNAVAILABLE);
   assert(zeroed(&active, sizeof(active)) && mock.pending && mock.begin_count == 1);
   char first_operation[65];
   memcpy(first_operation, mock.issue.operation_id, sizeof(first_operation));
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) == KB_MANAGEMENT_CERT_OK);
   assert(active.generation == 1 && mock.active && mock.begin_count == 2 &&
          mock.activate_count == 1 && !strcmp(first_operation, mock.enrollment.operation_id));

   kb_management_cert_bundle_t bundle;
   memset(&bundle, 0xa5, sizeof(bundle));
   kb_management_cert_active_t loaded;
   mock.mutate_snapshot_once = 2; /* whole two-snapshot load retries once */
   unsigned snapshots_before = mock.snapshot_count;
   assert(kb_management_cert_load_active(lifecycle, &bundle, &loaded) == KB_MANAGEMENT_CERT_OK);
   assert(loaded.generation == 1 && bundle.key_pem_len > 1000 &&
          strstr(bundle.key_pem, "BEGIN PRIVATE KEY") &&
          mock.snapshot_count == snapshots_before + 4);
   kb_management_cert_bundle_clear(&bundle);

   /* A second before/after change exhausts the single whole-load retry and is
    * stable contradictory state, not a third retry or a released bundle. */
   memset(&bundle, 0xa5, sizeof(bundle));
   memset(&loaded, 0xa5, sizeof(loaded));
   mock.mutate_snapshot_pairs = 2;
   mock.mutate_snapshot_pair_call = 0;
   snapshots_before = mock.snapshot_count;
   assert(kb_management_cert_load_active(lifecycle, &bundle, &loaded) ==
          KB_MANAGEMENT_CERT_INTEGRITY);
   assert(zeroed(&bundle, sizeof(bundle)) && zeroed(&loaded, sizeof(loaded)) &&
          mock.snapshot_count == snapshots_before + 4);

   /* The independently verified leaf identity is part of the release proof. */
   memset(&bundle, 0xa5, sizeof(bundle));
   memset(&loaded, 0xa5, sizeof(loaded));
   mock.mutate_identity_once = 1;
   assert(kb_management_cert_load_active(lifecycle, &bundle, &loaded) ==
          KB_MANAGEMENT_CERT_INTEGRITY);
   assert(zeroed(&bundle, sizeof(bundle)) && zeroed(&loaded, sizeof(loaded)));

   /* Inclusive threshold starts renewal. A crash after activation leaves the
    * old current plus durable pending/candidate, then restart promotes exactly
    * the DB-current generation without another activation. */
   mock.now = mock.enrollment.cert_not_after_epoch - 1200;
   mock.crash_point = KB_MANAGEMENT_CERT_CRASH_AFTER_ACTIVATE;
   mock.crash_armed = 1;
   unsigned activates_before = mock.activate_count;
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
          KB_MANAGEMENT_CERT_UNAVAILABLE);
   assert(zeroed(&active, sizeof(active)) && mock.enrollment.generation == 2 &&
          mock.activate_count == activates_before + 1);
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) == KB_MANAGEMENT_CERT_OK);
   assert(active.generation == 2 && mock.activate_count == activates_before + 1);
   assert(kb_management_cert_load_active(lifecycle, &bundle, &loaded) == KB_MANAGEMENT_CERT_OK);
   assert(loaded.generation == 2);
   kb_management_cert_bundle_clear(&bundle);

   /* A terminal renewal is cleared only after a fresh snapshot proves the
    * exact previous generation remains active. Contradiction and checked-file
    * failure retain the pending coordinate. */
   mock.now = mock.enrollment.cert_not_after_epoch - 1200;
   mock.terminal_on_begin = DB2_MANAGEMENT_CLIENT_ISSUE_EXPIRED;
   mock.mutate_snapshot_once = 1;
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
          KB_MANAGEMENT_CERT_INTEGRITY);
   mock.crash_point = KB_MANAGEMENT_CERT_CRASH_BEFORE_TERMINAL_CLEAR;
   mock.crash_armed = 1;
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
          KB_MANAGEMENT_CERT_UNAVAILABLE);
   assert(faccessat(dir_fd, "pending", F_OK, 0) == 0 && faccessat(dir_fd, "cleanup", F_OK, 0) == 0);
   mock.corrupt_terminal_clear = 1;
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
          KB_MANAGEMENT_CERT_INTEGRITY);
   assert(fchmodat(dir_fd, "pending", 0600, 0) == 0);
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
          KB_MANAGEMENT_CERT_DENIED);
   mock.terminal_on_begin = 0;
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) == KB_MANAGEMENT_CERT_OK);
   assert(active.generation == 3);

   /* Recovered renewals reject a maximal authoritative generation before the
    * +1 comparison, avoiding signed overflow even under UBSAN. */
   mock.now = mock.enrollment.cert_not_after_epoch - 1200;
   mock.crash_point = KB_MANAGEMENT_CERT_CRASH_AFTER_PENDING;
   mock.crash_armed = 1;
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
          KB_MANAGEMENT_CERT_UNAVAILABLE);
   int64_t saved_generation = mock.enrollment.generation;
   mock.enrollment.generation = INT64_MAX;
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
          KB_MANAGEMENT_CERT_INTEGRITY);
   assert(zeroed(&active, sizeof(active)));
   mock.enrollment.generation = saved_generation;
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) == KB_MANAGEMENT_CERT_OK);

   /* Every durability boundary is restartable without a duplicate activation
    * or a second operation coordinate. AFTER_BEGIN/AFTER_ACTIVATE are covered
    * above; cover the remaining boundaries on successive renewals. */
   const kb_management_cert_crash_point_t boundaries[] = {
       KB_MANAGEMENT_CERT_CRASH_AFTER_PREPARE, KB_MANAGEMENT_CERT_CRASH_AFTER_INTENT,
       KB_MANAGEMENT_CERT_CRASH_AFTER_PENDING, KB_MANAGEMENT_CERT_CRASH_AFTER_CANDIDATE,
       KB_MANAGEMENT_CERT_CRASH_AFTER_PROMOTE};
   for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i)
   {
      mock.now = mock.enrollment.cert_not_after_epoch - 1200;
      mock.crash_point = boundaries[i];
      mock.crash_armed = 1;
      unsigned prior_activations = mock.activate_count;
      assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
             KB_MANAGEMENT_CERT_UNAVAILABLE);
      assert(zeroed(&active, sizeof(active)));
      assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
             KB_MANAGEMENT_CERT_OK);
      assert(mock.activate_count == prior_activations + 1);
   }

   /* Provider unavailability, DB retry exhaustion, and a changed attested
    * identity all fail closed and clear caller outputs. */
   memset(&bundle, 0xa5, sizeof(bundle));
   memset(&loaded, 0xa5, sizeof(loaded));
   mock.attest_failure_once = KB_WORKLOAD_UNAVAILABLE;
   assert(kb_management_cert_load_active(lifecycle, &bundle, &loaded) ==
          KB_MANAGEMENT_CERT_UNAVAILABLE);
   assert(zeroed(&bundle, sizeof(bundle)) && zeroed(&loaded, sizeof(loaded)));
   memset(&bundle, 0xa5, sizeof(bundle));
   memset(&loaded, 0xa5, sizeof(loaded));
   mock.snapshot_failure = DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
   mock.snapshot_failure_count = 3;
   assert(kb_management_cert_load_active(lifecycle, &bundle, &loaded) ==
          KB_MANAGEMENT_CERT_UNAVAILABLE);
   assert(zeroed(&bundle, sizeof(bundle)) && zeroed(&loaded, sizeof(loaded)));
   memset(&bundle, 0xa5, sizeof(bundle));
   memset(&loaded, 0xa5, sizeof(loaded));
   mock.snapshot_failure = DB2_MANAGEMENT_CLIENT_INSTANCE_RETRY;
   mock.snapshot_failure_count = 3;
   assert(kb_management_cert_load_active(lifecycle, &bundle, &loaded) ==
          KB_MANAGEMENT_CERT_UNAVAILABLE);
   assert(zeroed(&bundle, sizeof(bundle)) && zeroed(&loaded, sizeof(loaded)));
   memset(&bundle, 0xa5, sizeof(bundle));
   memset(&loaded, 0xa5, sizeof(loaded));
   mock.mutate_attested_identity_once = 1;
   assert(kb_management_cert_load_active(lifecycle, &bundle, &loaded) ==
          KB_MANAGEMENT_CERT_INTEGRITY);
   assert(zeroed(&bundle, sizeof(bundle)) && zeroed(&loaded, sizeof(loaded)));

   /* Repeated renewals remain O(1): only current and its candidate survive;
    * cleanup/pending and every superseded private record are gone. */
   assert(directory_file_count(dir_fd) == 2);
   assert(faccessat(dir_fd, "cleanup", F_OK, 0) != 0 && errno == ENOENT);
   assert(faccessat(dir_fd, "pending", F_OK, 0) != 0 && errno == ENOENT);

   /* A present noncanonical cleanup coordinate fails closed before any new
    * issue can be created and is never overwritten. */
   int cleanup_fd = openat(dir_fd, "cleanup", O_WRONLY | O_CREAT | O_EXCL, 0600);
   assert(cleanup_fd >= 0 && write(cleanup_fd, "bad", 3) == 3 && close(cleanup_fd) == 0);
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
          KB_MANAGEMENT_CERT_INTEGRITY);
   assert(unlinkat(dir_fd, "cleanup", 0) == 0);

   kb_management_cert_lifecycle_close(lifecycle);
   remove_tree_files(bundle_dir);

   char initial_dir[256];
   snprintf(initial_dir, sizeof initial_dir, "%s/aimee-p5b2c-initial-terminal.XXXXXX",
            platform_tmpdir());
   assert(mkdtemp(initial_dir) && chmod(initial_dir, 0700) == 0);
   dir_fd = open(initial_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   assert(dir_fd >= 0);
   memset(&mock, 0, sizeof(mock));
   mock.now = (int64_t)time(NULL);
   mock.storage_fd = dir_fd;
   strcpy(mock.identity.issuer, "https://issuer.example");
   strcpy(mock.identity.subject, "spiffe://example.test/kb/alpha");
   memset(mock.identity.proof_anchor_id, 0x31, 32);
   memset(mock.identity.custody_anchor_id, 0x42, 32);
   fill_hex(mock.lineage, 32, '2');
   config.bundle_dir = initial_dir;
   assert(kb_management_cert_lifecycle_open_for_test(&config, KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1,
                                                     dir_fd, &mock_ops, &mock,
                                                     &lifecycle) == KB_MANAGEMENT_CERT_OK);
   mock.terminal_on_begin = DB2_MANAGEMENT_CLIENT_ISSUE_QUARANTINED;
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) ==
          KB_MANAGEMENT_CERT_DENIED);
   assert(faccessat(dir_fd, "pending", F_OK, 0) != 0);
   mock.terminal_on_begin = 0;
   assert(kb_management_cert_reconcile(lifecycle, mock.now + 30, &active) == KB_MANAGEMENT_CERT_OK);
   assert(active.generation == 1);
   kb_management_cert_lifecycle_close(lifecycle);
   remove_tree_files(initial_dir);
   remove_tree_files(ca_dir);
}
#endif

static size_t noncanonical_sequence(const uint8_t *der, size_t len, uint8_t *out, size_t cap)
{
   if (!der || len < 4 || der[0] != 0x30 || (der[1] & 0x80) == 0)
      return 0;
   size_t length_octets = der[1] & 0x7f;
   if (!length_octets || length_octets > 3 || len + 1 > cap)
      return 0;
   out[0] = 0x30;
   out[1] = (uint8_t)(0x80 | (length_octets + 1));
   out[2] = 0; /* BER-valid, deliberately non-minimal length encoding. */
   memcpy(out + 3, der + 2, length_octets);
   memcpy(out + 3 + length_octets, der + 2 + length_octets, len - 2 - length_octets);
   return len + 1;
}

static void test_management_leaf_profile(void)
{
   kb_pki_ca_t ca;
   kb_management_cert_key_material_t material;
   char leaf[KB_PKI_CERT_PEM_MAX];
   assert(kb_pki_ca_generate(&ca) == 0);
   assert(kb_management_cert_key_generate(&material) == 0);
   assert(kb_pki_sign_kb_management_csr(&ca, material.csr_pem, 3600, leaf, sizeof(leaf)) == 0);
   kb_management_cert_verified_t verified;
   assert(kb_management_cert_leaf_verify(&material, leaf, ca.cert_pem, &verified) == 0);
   kb_management_cert_key_material_t oversized = material;
   oversized.key_der_len = sizeof(oversized.key_der) + 1U;
   memset(&verified, 0xa5, sizeof(verified));
   assert(kb_management_cert_leaf_verify(&oversized, leaf, ca.cert_pem, &verified) != 0);
   assert(zeroed(&verified, sizeof(verified)));
   assert(kb_management_cert_leaf_verify(&material, leaf, ca.cert_pem, &verified) == 0);
   assert(!strcmp(verified.ca_issuer, verified.leaf_issuer));
   assert(verified.not_after_epoch - verified.not_before_epoch == 3600);
   assert(!memcmp(verified.leaf_spki_digest, material.csr_spki_digest, 32));
   char appended[KB_PKI_CERT_PEM_MAX * 2];
   assert(snprintf(appended, sizeof(appended), "%sjunk", leaf) > 0);
   assert(kb_management_cert_leaf_verify(&material, appended, ca.cert_pem, &verified) != 0);
   assert(zeroed(&verified, sizeof(verified)));
   assert(snprintf(appended, sizeof(appended), "%s%s", leaf, leaf) > 0);
   assert(kb_management_cert_leaf_verify(&material, appended, ca.cert_pem, &verified) != 0);
   assert(snprintf(appended, sizeof(appended), "%sjunk", ca.cert_pem) > 0);
   assert(kb_management_cert_leaf_verify(&material, leaf, appended, &verified) != 0);
   assert(kb_management_cert_leaf_verify(&material, leaf, ca.cert_pem, &verified) == 0);

   uint8_t plain[KB_MANAGEMENT_CERT_PLAINTEXT_MAX];
   size_t plain_len = 0;
   assert(kb_management_cert_bundle_encode(
              material.key_der, material.key_der_len, verified.leaf_der, verified.leaf_der_len,
              verified.ca_der, verified.ca_der_len, plain, sizeof(plain), &plain_len) == 0);
   kb_management_cert_bundle_view_t view;
   assert(kb_management_cert_bundle_decode(plain, plain_len, &view) == 0);
   kb_management_cert_bundle_t pem;
   kb_management_cert_verified_t persisted;
   assert(kb_management_cert_bundle_verify(plain, plain_len, &persisted, &pem) == 0);
   assert(strstr(pem.key_pem, "BEGIN PRIVATE KEY") && strstr(pem.leaf_pem, "BEGIN CERTIFICATE"));
   assert(!memcmp(persisted.ca_fingerprint, verified.ca_fingerprint, 32) &&
          !memcmp(persisted.leaf_fingerprint, verified.leaf_fingerprint, 32));
   uint8_t expected_bundle_digest[32];
   assert(kb_management_cert_sha256(plain, plain_len, expected_bundle_digest) == 0);
   assert(!memcmp(persisted.public_bundle_digest, expected_bundle_digest, 32));
   kb_management_cert_bundle_clear(&pem);

   uint8_t noncanonical[4097], malformed[KB_MANAGEMENT_CERT_PLAINTEXT_MAX];
   size_t noncanonical_len =
       noncanonical_sequence(view.key_der, view.key_der_len, noncanonical, sizeof(noncanonical));
   assert(noncanonical_len > 0);
   size_t malformed_len = 0;
   assert(kb_management_cert_bundle_encode(noncanonical, noncanonical_len, view.leaf_der,
                                           view.leaf_der_len, view.ca_der, view.ca_der_len,
                                           malformed, sizeof(malformed), &malformed_len) == 0);
   memset(&persisted, 0xa5, sizeof(persisted));
   memset(&pem, 0xa5, sizeof(pem));
   assert(kb_management_cert_bundle_verify(malformed, malformed_len, &persisted, &pem) != 0);
   assert(zeroed(&persisted, sizeof(persisted)) && zeroed(&pem, sizeof(pem)));
   memset(&persisted, 0xa5, sizeof(persisted));
   memset(&pem, 0xa5, sizeof(pem));
   assert(kb_management_cert_bundle_verify((const uint8_t *)&persisted, 16, &persisted, &pem) != 0);
   assert(zeroed(&persisted, sizeof(persisted)) && zeroed(&pem, sizeof(pem)));

   noncanonical_len =
       noncanonical_sequence(view.leaf_der, view.leaf_der_len, noncanonical, sizeof(noncanonical));
   assert(noncanonical_len > 0);
   assert(kb_management_cert_bundle_encode(view.key_der, view.key_der_len, noncanonical,
                                           noncanonical_len, view.ca_der, view.ca_der_len,
                                           malformed, sizeof(malformed), &malformed_len) == 0);
   assert(kb_management_cert_bundle_verify(malformed, malformed_len, &persisted, &pem) != 0);
   assert(zeroed(&persisted, sizeof(persisted)) && zeroed(&pem, sizeof(pem)));
   OPENSSL_cleanse(&ca, sizeof(ca));
   OPENSSL_cleanse(plain, sizeof(plain));
   OPENSSL_cleanse(malformed, sizeof(malformed));
   OPENSSL_cleanse(noncanonical, sizeof(noncanonical));
   kb_management_cert_key_material_clear(&material);
}

static void test_storage_rejects_oversize_and_fifo(void)
{
   char path[256];
   snprintf(path, sizeof path, "%s/aimee-p5b2c-storage.XXXXXX", platform_tmpdir());
   assert(mkdtemp(path));
   kb_management_cert_storage_t storage = {.dir_fd = open(path, O_RDONLY | O_DIRECTORY)};
   assert(storage.dir_fd >= 0);
   char operation[65];
   fill_hex(operation, 64, 'a');
   uint8_t byte = 1;
   assert(kb_management_cert_storage_stage(&storage, "candidate", operation, &byte,
                                           KB_MANAGEMENT_CERT_CANDIDATE_MAX + 1U) ==
          KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(kb_management_cert_storage_stage(&storage, NULL, operation, &byte, 1) ==
          KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(kb_management_cert_storage_stage(&storage, "candidate", operation, &byte, 1) ==
          KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(kb_management_cert_storage_pending_publish(&storage, &byte, 1) ==
          KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(kb_management_cert_storage_promote(&storage, &byte, 1) ==
          KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(faccessat(storage.dir_fd,
                    "candidate.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    F_OK, 0) != 0);

   const char fifo[] = "intent.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
   assert(mkfifoat(storage.dir_fd, fifo, 0600) == 0);
   uint8_t output[32];
   size_t output_len = 9;
   kb_management_cert_storage_t storage_before = storage;
   assert(kb_management_cert_storage_read(&storage, "intent", operation, (uint8_t *)&storage,
                                          sizeof(storage),
                                          &output_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(!memcmp(&storage, &storage_before, sizeof(storage)) && output_len == 9);
   uint64_t output_alias[4], output_alias_before[4];
   memset(output_alias, 0x5a, sizeof(output_alias));
   memcpy(output_alias_before, output_alias, sizeof(output_alias));
   assert(kb_management_cert_storage_read(&storage, "intent", operation, (uint8_t *)output_alias,
                                          sizeof(output_alias), (size_t *)output_alias) ==
          KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(!memcmp(output_alias, output_alias_before, sizeof(output_alias)));
   memset(output, 0xa5, sizeof(output));
   output_len = 9;
   assert(kb_management_cert_storage_read(&storage, NULL, operation, output, sizeof(output),
                                          &output_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(output_len == 0 && zeroed(output, sizeof(output)));
   assert(kb_management_cert_storage_read(&storage, "intent", operation, output, sizeof(output),
                                          &output_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(output_len == 0 && zeroed(output, sizeof(output)));
   assert(unlinkat(storage.dir_fd, fifo, 0) == 0);
   assert(symlinkat("/dev/null", storage.dir_fd, fifo) == 0);
   assert(kb_management_cert_storage_read(&storage, "intent", operation, output, sizeof(output),
                                          &output_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, fifo, 0) == 0);
   close(storage.dir_fd);
   assert(rmdir(path) == 0);
}

static void test_storage_open_and_protocol(void)
{
   char unsafe[256];
   snprintf(unsafe, sizeof unsafe, "%s/aimee-p5b2c-open.XXXXXX", platform_tmpdir());
   assert(mkdtemp(unsafe));
   kb_management_cert_storage_t rejected;
   assert(kb_management_cert_storage_open(unsafe, &rejected) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(rmdir(unsafe) == 0);
   if (geteuid() != 0)
      return;

   char path[] = "/root/aimee-p5b2c-storage.XXXXXX";
   assert(mkdtemp(path));
   assert(chmod(path, 0700) == 0);
   char component_link[sizeof(path) + 8];
   assert(snprintf(component_link, sizeof(component_link), "%s.link", path) > 0);
   assert(symlink(path, component_link) == 0);
   kb_management_cert_storage_t component_rejected;
   assert(kb_management_cert_storage_open(component_link, &component_rejected) ==
          KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlink(component_link) == 0);
   kb_management_cert_storage_t storage, locked;
   assert(kb_management_cert_storage_open(path, &storage) == KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_open(path, &locked) == KB_MANAGEMENT_STORAGE_CONFLICT);

   char operation[65];
   fill_hex(operation, 64, 'b');
   uint8_t candidate_cipher[64], record[4096];
   kb_management_cert_candidate_view_t staged_candidate;
   base_candidate(&staged_candidate, operation, candidate_cipher, 1);
   size_t record_len = 0;
   assert(kb_management_cert_candidate_encode(&staged_candidate, record, sizeof(record),
                                              &record_len) == 0);
   assert(kb_management_cert_storage_stage(&storage, "candidate", operation, record, record_len) ==
          KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_stage(&storage, "candidate", operation, record, record_len) ==
          KB_MANAGEMENT_STORAGE_OK);
   uint8_t different_cipher[64], different[4096];
   base_candidate(&staged_candidate, operation, different_cipher, 2);
   size_t different_len = 0;
   assert(kb_management_cert_candidate_encode(&staged_candidate, different, sizeof(different),
                                              &different_len) == 0);
   assert(kb_management_cert_storage_stage(&storage, "candidate", operation, different,
                                           different_len) == KB_MANAGEMENT_STORAGE_CONFLICT);
   uint8_t readback[1024];
   size_t readback_len = 0;
   assert(kb_management_cert_storage_read(&storage, "candidate", operation, readback,
                                          sizeof(readback),
                                          &readback_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(readback_len == record_len && !memcmp(readback, record, record_len));

   kb_management_cert_manifest_t current = {.generation = 1};
   memcpy(current.operation_id, operation, sizeof(current.operation_id));
   memset(current.public_bundle_digest, 9, sizeof(current.public_bundle_digest));
   uint8_t manifest[1024];
   size_t manifest_len = 0;
   assert(kb_management_cert_manifest_encode(&current, manifest, sizeof(manifest), &manifest_len) ==
          0);
   assert(kb_management_cert_storage_promote(&storage, manifest, manifest_len) ==
          KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_current(&storage, readback, sizeof(readback), &readback_len) ==
          KB_MANAGEMENT_STORAGE_OK);
   assert(readback_len == manifest_len && !memcmp(readback, manifest, manifest_len));

   memset(readback, 0xa5, sizeof(readback));
   readback_len = 9;
   assert(kb_management_cert_storage_pending_read(&storage, readback, sizeof(readback),
                                                  &readback_len) == KB_MANAGEMENT_STORAGE_MISSING);
   assert(readback_len == 0 && zeroed(readback, sizeof(readback)));

   char pending_operation[65];
   fill_hex(pending_operation, 64, 'd');
   uint8_t intent_cipher[64], intent_record[4096];
   kb_management_cert_intent_view_t staged_intent;
   base_intent(&staged_intent, intent_cipher);
   memcpy(staged_intent.operation_id, pending_operation, sizeof(staged_intent.operation_id));
   size_t intent_record_len = 0;
   assert(kb_management_cert_intent_encode(&staged_intent, intent_record, sizeof(intent_record),
                                           &intent_record_len) == 0);
   kb_management_cert_pending_manifest_t pending = {.generation = 1,
                                                    .issue_kind = KB_MANAGEMENT_CERT_ISSUE_INITIAL};
   fill_hex(pending.installation_id, 32, '1');
   fill_hex(pending.lineage_id, 32, '2');
   memcpy(pending.operation_id, pending_operation, sizeof(pending.operation_id));
   fill_hex(pending.authority_id, 32, '4');
   memset(pending.binding_digest, 0x61, 32);
   assert(kb_management_cert_sha256(intent_record, intent_record_len,
                                    pending.intent_record_digest) == 0);
   uint8_t pending_record[1024];
   size_t pending_record_len = 0;
   assert(kb_management_cert_pending_encode(&pending, pending_record, sizeof(pending_record),
                                            &pending_record_len) == 0);
   /* PREPARING is the durable O(1) coordinate for the intent->pending gap.
    * Exact replay is accepted; no-pending apply removes an orphan intent. */
   assert(kb_management_cert_storage_cleanup_prepare_intent(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_cleanup_prepare_intent(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_stage(&storage, "intent", pending_operation, intent_record,
                                           intent_record_len) == KB_MANAGEMENT_STORAGE_OK);
   uint8_t tampered_intent[4096];
   intent_cipher[0] ^= 0x5a;
   size_t tampered_intent_len = 0;
   assert(kb_management_cert_intent_encode(&staged_intent, tampered_intent, sizeof(tampered_intent),
                                           &tampered_intent_len) == 0);
   intent_cipher[0] ^= 0x5a;
   char intent_name[80];
   assert(snprintf(intent_name, sizeof(intent_name), "intent.%s", pending_operation) > 0);
   int tampered_fd = openat(storage.dir_fd, intent_name, O_WRONLY | O_TRUNC | O_CLOEXEC);
   assert(tampered_fd >= 0 &&
          write(tampered_fd, tampered_intent, tampered_intent_len) ==
              (ssize_t)tampered_intent_len &&
          close(tampered_fd) == 0);
   assert(kb_management_cert_storage_cleanup_apply(&storage) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   tampered_fd = openat(storage.dir_fd, intent_name, O_WRONLY | O_TRUNC | O_CLOEXEC);
   assert(tampered_fd >= 0 &&
          write(tampered_fd, intent_record, intent_record_len) == (ssize_t)intent_record_len &&
          close(tampered_fd) == 0);
   assert(linkat(storage.dir_fd, intent_name, storage.dir_fd, "intent.cleanup-linked", 0) == 0);
   assert(kb_management_cert_storage_cleanup_apply(&storage) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, "intent.cleanup-linked", 0) == 0);
   assert(kb_management_cert_storage_cleanup_apply(&storage) == KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_read(&storage, "intent", pending_operation, readback,
                                          sizeof(readback),
                                          &readback_len) == KB_MANAGEMENT_STORAGE_MISSING);
   assert(kb_management_cert_storage_cleanup_prepare_intent(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_stage(&storage, "intent", pending_operation, intent_record,
                                           intent_record_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_pending_publish(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_cleanup_apply(&storage) == KB_MANAGEMENT_STORAGE_CONFLICT);
   assert(kb_management_cert_storage_cleanup_finish_intent(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_pending_publish(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_CONFLICT);
   assert(kb_management_cert_storage_pending_read(&storage, readback, sizeof(readback),
                                                  &readback_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(readback_len == pending_record_len &&
          !memcmp(readback, pending_record, pending_record_len));
   uint8_t wrong_pending[1024];
   memcpy(wrong_pending, pending_record, pending_record_len);
   wrong_pending[pending_record_len - 1] ^= 1;
   assert(kb_management_cert_storage_pending_clear_exact(
              &storage, wrong_pending, pending_record_len) == KB_MANAGEMENT_STORAGE_CONFLICT);
   assert(kb_management_cert_storage_pending_read(&storage, readback, sizeof(readback),
                                                  &readback_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_pending_clear_exact(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_pending_clear_exact(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_MISSING);
   assert(kb_management_cert_storage_read(&storage, "intent", pending_operation, readback,
                                          sizeof(readback),
                                          &readback_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(readback_len == intent_record_len);

   assert(symlinkat("/dev/null", storage.dir_fd, "cleanup") == 0);
   assert(kb_management_cert_storage_cleanup_prepare_intent(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, "cleanup", 0) == 0);
   int oversized_cleanup = openat(storage.dir_fd, "cleanup", O_WRONLY | O_CREAT | O_EXCL, 0600);
   assert(oversized_cleanup >= 0);
   uint8_t oversized_bytes[2048] = {0};
   assert(write(oversized_cleanup, oversized_bytes, sizeof(oversized_bytes)) ==
          (ssize_t)sizeof(oversized_bytes));
   assert(close(oversized_cleanup) == 0);
   assert(kb_management_cert_storage_cleanup_prepare_intent(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, "cleanup", 0) == 0);

   assert(symlinkat("/dev/null", storage.dir_fd, "pending") == 0);
   assert(kb_management_cert_storage_pending_publish(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(kb_management_cert_storage_pending_clear_exact(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, "pending", 0) == 0);
   assert(mkfifoat(storage.dir_fd, "pending", 0600) == 0);
   assert(kb_management_cert_storage_pending_publish(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, "pending", 0) == 0);
   int planted_fd = openat(storage.dir_fd, "pending", O_WRONLY | O_CREAT | O_EXCL, 0600);
   assert(planted_fd >= 0);
   const uint8_t malformed_pending[] = {0x61, 0x62, 0x63};
   assert(write(planted_fd, malformed_pending, sizeof(malformed_pending)) ==
          (ssize_t)sizeof(malformed_pending));
   assert(close(planted_fd) == 0);
   assert(kb_management_cert_storage_pending_publish(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(kb_management_cert_storage_pending_clear_exact(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, "pending", 0) == 0);
   planted_fd = openat(storage.dir_fd, "pending", O_WRONLY | O_CREAT | O_EXCL, 0600);
   assert(planted_fd >= 0);
   assert(write(planted_fd, pending_record, pending_record_len) == (ssize_t)pending_record_len);
   assert(close(planted_fd) == 0);
   assert(linkat(storage.dir_fd, "pending", storage.dir_fd, "pending.extra", 0) == 0);
   assert(kb_management_cert_storage_pending_read(&storage, readback, sizeof(readback),
                                                  &readback_len) ==
          KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(kb_management_cert_storage_pending_publish(
              &storage, pending_record, pending_record_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, "pending.extra", 0) == 0);
   assert(unlinkat(storage.dir_fd, "pending", 0) == 0);

   char planted_operation[65];
   fill_hex(planted_operation, 64, 'c');
   uint8_t planted_cipher[64], planted_record[4096];
   base_candidate(&staged_candidate, planted_operation, planted_cipher, 3);
   size_t planted_record_len = 0;
   assert(kb_management_cert_candidate_encode(&staged_candidate, planted_record,
                                              sizeof(planted_record), &planted_record_len) == 0);
   const char planted[] =
       "candidate.cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
   assert(symlinkat("/dev/null", storage.dir_fd, planted) == 0);
   assert(kb_management_cert_storage_stage(&storage, "candidate", planted_operation, planted_record,
                                           planted_record_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, planted, 0) == 0);
   assert(mkfifoat(storage.dir_fd, planted, 0600) == 0);
   assert(kb_management_cert_storage_stage(&storage, "candidate", planted_operation, planted_record,
                                           planted_record_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, planted, 0) == 0);

   char malformed_operation[65];
   fill_hex(malformed_operation, 64, 'e');
   uint8_t malformed_cipher[64], valid_replay[4096];
   base_candidate(&staged_candidate, malformed_operation, malformed_cipher, 4);
   size_t valid_replay_len = 0;
   assert(kb_management_cert_candidate_encode(&staged_candidate, valid_replay, sizeof(valid_replay),
                                              &valid_replay_len) == 0);
   const char malformed_name[] =
       "candidate.eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
   int malformed_fd = openat(storage.dir_fd, malformed_name, O_WRONLY | O_CREAT | O_EXCL, 0600);
   assert(malformed_fd >= 0);
   const uint8_t malformed_record[] = {1, 2, 3};
   assert(write(malformed_fd, malformed_record, sizeof(malformed_record)) ==
          (ssize_t)sizeof(malformed_record));
   assert(close(malformed_fd) == 0);
   assert(kb_management_cert_storage_stage(&storage, "candidate", malformed_operation, valid_replay,
                                           valid_replay_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, malformed_name, 0) == 0);

   char linked_operation[65];
   fill_hex(linked_operation, 64, 'f');
   uint8_t linked_cipher[64], linked_record[4096];
   base_candidate(&staged_candidate, linked_operation, linked_cipher, 5);
   size_t linked_record_len = 0;
   assert(kb_management_cert_candidate_encode(&staged_candidate, linked_record,
                                              sizeof(linked_record), &linked_record_len) == 0);
   const char linked_name[] =
       "candidate.ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
   int linked_fd = openat(storage.dir_fd, linked_name, O_WRONLY | O_CREAT | O_EXCL, 0600);
   assert(linked_fd >= 0);
   assert(write(linked_fd, linked_record, linked_record_len) == (ssize_t)linked_record_len);
   assert(close(linked_fd) == 0);
   assert(linkat(storage.dir_fd, linked_name, storage.dir_fd, "candidate.linked", 0) == 0);
   assert(kb_management_cert_storage_stage(&storage, "candidate", linked_operation, linked_record,
                                           linked_record_len) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, "candidate.linked", 0) == 0);
   assert(unlinkat(storage.dir_fd, linked_name, 0) == 0);

   assert(unlinkat(storage.dir_fd, "current", 0) == 0);
   assert(unlinkat(storage.dir_fd,
                   "candidate.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                   0) == 0);
   assert(unlinkat(storage.dir_fd,
                   "intent.dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
                   0) == 0);
   kb_management_cert_storage_close(&storage);
   assert(rmdir(path) == 0);
}

int main(void)
{
   test_plaintext_codecs();
   test_record_codecs();
   test_binding_transcripts();
   test_key_and_csr();
   test_lifecycle_constructor_guards();
#ifdef AIMEE_MANAGEMENT_CERT_TESTING
   test_active_identity_canonicalization();
   test_noncanonical_custodied_hex();
   test_lifecycle_orchestration();
#endif
   test_management_leaf_profile();
   test_storage_rejects_oversize_and_fifo();
   test_storage_open_and_protocol();
   puts("test_kb_management_cert_lifecycle: ok");
   return 0;
}
