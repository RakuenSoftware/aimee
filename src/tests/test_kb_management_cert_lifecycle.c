#include "kb/kb_management_cert_binding.h"
#include "kb/kb_management_cert_codec.h"
#include "kb/kb_management_cert_crypto.h"
#include "kb/kb_management_cert_storage.h"

#include <openssl/crypto.h>

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
   char path[] = "/tmp/aimee-p5b2c-storage.XXXXXX";
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
   char unsafe[] = "/tmp/aimee-p5b2c-open.XXXXXX";
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
   /* Crash-safe ordering: the immutable intent is durable and discoverable
    * before the single no-replace pending coordinate can exist. */
   assert(kb_management_cert_storage_stage(&storage, "intent", pending_operation, intent_record,
                                           intent_record_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_pending_publish(
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
   test_management_leaf_profile();
   test_storage_rejects_oversize_and_fifo();
   test_storage_open_and_protocol();
   puts("test_kb_management_cert_lifecycle: ok");
   return 0;
}
