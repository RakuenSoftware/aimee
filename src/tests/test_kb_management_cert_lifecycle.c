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
   assert(kb_management_cert_intent_decode(encoded, n - 1, &decoded) != 0);
   assert(zeroed(&decoded, sizeof(decoded)));
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
   strcpy(candidate.serial_norm, "01abcdef");
   memset(candidate.fingerprint, 13, 32);
   memset(candidate.spki_digest, 14, 32);
   candidate.not_before_epoch = 1000;
   candidate.not_after_epoch = 4600;
   candidate.ciphertext = cipher;
   candidate.ciphertext_len = sizeof(cipher);
   assert(kb_management_cert_candidate_encode(&candidate, encoded, sizeof(encoded), &n) == 0);
   kb_management_cert_candidate_view_t candidate_out;
   assert(kb_management_cert_candidate_decode(encoded, n, &candidate_out) == 0);
   assert(!strcmp(candidate_out.authority_id, candidate.authority_id) &&
          candidate_out.provider_kind == candidate.provider_kind &&
          !strcmp(candidate_out.issuer, candidate.issuer));
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

   kb_management_cert_bundle_t secret;
   memset(&secret, 0xa5, sizeof(secret));
   kb_management_cert_bundle_clear(&secret);
   assert(zeroed(&secret, sizeof(secret)));
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
   assert(kb_management_cert_bundle_encode(material.key_der, material.key_der_len,
                                           verified.leaf_der, verified.leaf_der_len,
                                           verified.ca_der, verified.ca_der_len, plain,
                                           sizeof(plain), &plain_len) == 0);
   kb_management_cert_bundle_view_t view;
   assert(kb_management_cert_bundle_decode(plain, plain_len, &view) == 0);
   kb_management_cert_bundle_t pem;
   assert(kb_management_cert_bundle_to_pem(view.key_der, view.key_der_len, view.leaf_der,
                                           view.leaf_der_len, view.ca_der, view.ca_der_len,
                                           &pem) == 0);
   assert(strstr(pem.key_pem, "BEGIN PRIVATE KEY") && strstr(pem.leaf_pem, "BEGIN CERTIFICATE"));
   kb_management_cert_bundle_clear(&pem);
   OPENSSL_cleanse(&ca, sizeof(ca));
   OPENSSL_cleanse(plain, sizeof(plain));
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
   assert(faccessat(storage.dir_fd,
                    "candidate.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    F_OK, 0) != 0);

   const char fifo[] =
       "intent.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
   assert(mkfifoat(storage.dir_fd, fifo, 0600) == 0);
   uint8_t output[32];
   size_t output_len = 9;
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
   const uint8_t record[] = {1, 2, 3, 4, 5};
   assert(kb_management_cert_storage_stage(&storage, "candidate", operation, record,
                                           sizeof(record)) == KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_stage(&storage, "candidate", operation, record,
                                           sizeof(record)) == KB_MANAGEMENT_STORAGE_OK);
   const uint8_t different[] = {1, 2, 3, 4, 6};
   assert(kb_management_cert_storage_stage(&storage, "candidate", operation, different,
                                           sizeof(different)) == KB_MANAGEMENT_STORAGE_CONFLICT);
   uint8_t readback[32];
   size_t readback_len = 0;
   assert(kb_management_cert_storage_read(&storage, "candidate", operation, readback,
                                          sizeof(readback), &readback_len) ==
          KB_MANAGEMENT_STORAGE_OK);
   assert(readback_len == sizeof(record) && !memcmp(readback, record, sizeof(record)));

   const uint8_t manifest[] = {9, 8, 7, 6};
   assert(kb_management_cert_storage_promote(&storage, manifest, sizeof(manifest)) ==
          KB_MANAGEMENT_STORAGE_OK);
   assert(kb_management_cert_storage_current(&storage, readback, sizeof(readback),
                                             &readback_len) == KB_MANAGEMENT_STORAGE_OK);
   assert(readback_len == sizeof(manifest) && !memcmp(readback, manifest, sizeof(manifest)));

   char planted_operation[65];
   fill_hex(planted_operation, 64, 'c');
   const char planted[] =
       "candidate.cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
   assert(symlinkat("/dev/null", storage.dir_fd, planted) == 0);
   assert(kb_management_cert_storage_stage(&storage, "candidate", planted_operation, record,
                                           sizeof(record)) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, planted, 0) == 0);
   assert(mkfifoat(storage.dir_fd, planted, 0600) == 0);
   assert(kb_management_cert_storage_stage(&storage, "candidate", planted_operation, record,
                                           sizeof(record)) == KB_MANAGEMENT_STORAGE_INTEGRITY);
   assert(unlinkat(storage.dir_fd, planted, 0) == 0);

   assert(unlinkat(storage.dir_fd, "current", 0) == 0);
   assert(unlinkat(storage.dir_fd,
                   "candidate.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                   0) == 0);
   kb_management_cert_storage_close(&storage);
   assert(rmdir(path) == 0);
}

int main(void)
{
   test_plaintext_codecs();
   test_record_codecs();
   test_key_and_csr();
   test_management_leaf_profile();
   test_storage_rejects_oversize_and_fifo();
   test_storage_open_and_protocol();
   puts("test_kb_management_cert_lifecycle: ok");
   return 0;
}
