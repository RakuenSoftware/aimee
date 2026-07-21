/* test_kb_pki.c — unit tests for aimee-kb's internal CA (src/kb/pki.c):
 * CA generation, the sha256 CA fingerprint, client-cert issuance, and full
 * X509 chain verification (accept own-CA-issued, reject other-CA / garbage). */
#include "kb_pki.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_ca_generate(kb_pki_ca_t *ca)
{
   assert(kb_pki_ca_generate(ca) == 0);
   assert(strstr(ca->cert_pem, "-----BEGIN CERTIFICATE-----"));
   assert(strstr(ca->cert_pem, "-----END CERTIFICATE-----"));
   assert(strstr(ca->key_pem, "PRIVATE KEY-----"));
   assert(kb_pki_ca_generate(NULL) == -1);
   printf("  ca_generate: ok\n");
}

static void test_fingerprint(const kb_pki_ca_t *ca)
{
   char fp1[KB_PKI_FP_HEX], fp2[KB_PKI_FP_HEX];
   assert(kb_pki_ca_fingerprint(ca->cert_pem, fp1, sizeof(fp1)) == 0);
   assert(strlen(fp1) == 64);
   for (int i = 0; i < 64; i++)
   {
      char c = fp1[i];
      assert((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')); /* lowercase hex */
   }
   /* deterministic for the same cert */
   assert(kb_pki_ca_fingerprint(ca->cert_pem, fp2, sizeof(fp2)) == 0);
   assert(strcmp(fp1, fp2) == 0);

   /* a different CA yields a different fingerprint */
   kb_pki_ca_t other;
   assert(kb_pki_ca_generate(&other) == 0);
   char fp3[KB_PKI_FP_HEX];
   assert(kb_pki_ca_fingerprint(other.cert_pem, fp3, sizeof(fp3)) == 0);
   assert(strcmp(fp1, fp3) != 0);

   /* error paths */
   char tiny[8];
   assert(kb_pki_ca_fingerprint(ca->cert_pem, tiny, sizeof(tiny)) == -1);
   assert(kb_pki_ca_fingerprint("not a certificate", fp1, sizeof(fp1)) == -1);
   assert(kb_pki_ca_fingerprint("", fp1, sizeof(fp1)) == -1);
   printf("  fingerprint: ok\n");
}

/* Parse the issued cert and confirm its subject CN. */
static void assert_cert_cn(const char *cert_pem, const char *expect_cn)
{
   BIO *bio = BIO_new_mem_buf(cert_pem, -1);
   assert(bio);
   X509 *c = PEM_read_bio_X509(bio, NULL, NULL, NULL);
   BIO_free(bio);
   assert(c);
   char cn[128] = "";
   X509_NAME_get_text_by_NID(X509_get_subject_name(c), NID_commonName, cn, sizeof(cn));
   assert(strcmp(cn, expect_cn) == 0);
   X509_free(c);
}

/* True if the cert PEM carries an extension identified by `nid`. */
static int cert_has_ext(const char *cert_pem, int nid)
{
   BIO *bio = BIO_new_mem_buf(cert_pem, -1);
   X509 *c = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
   BIO_free(bio);
   if (!c)
      return 0;
   int idx = X509_get_ext_by_NID(c, nid, -1);
   X509_free(c);
   return idx >= 0;
}

static void assert_exact_eku(const char *cert_pem, int expected_nid)
{
   BIO *bio = BIO_new_mem_buf(cert_pem, -1);
   X509 *cert = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
   BIO_free(bio);
   assert(cert);
   EXTENDED_KEY_USAGE *eku = X509_get_ext_d2i(cert, NID_ext_key_usage, NULL, NULL);
   assert(eku && sk_ASN1_OBJECT_num(eku) == 1);
   assert(OBJ_obj2nid(sk_ASN1_OBJECT_value(eku, 0)) == expected_nid);
   EXTENDED_KEY_USAGE_free(eku);
   X509_free(cert);
}

/* RFC 5280 conformance: the CA and every issued (client/server) cert carry both
 * subjectKeyIdentifier and authorityKeyIdentifier, so strict X.509 verifiers
 * (OpenSSL 3.5+ default; python ssl) accept the chain rather than rejecting it
 * with "Missing Authority Key Identifier". */
static void test_akid_present(const kb_pki_ca_t *ca)
{
   assert(cert_has_ext(ca->cert_pem, NID_subject_key_identifier));
   assert(cert_has_ext(ca->cert_pem, NID_authority_key_identifier));

   char cert[KB_PKI_CERT_PEM_MAX], key[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_client_cert(ca, "c:akid", 3600, cert, sizeof(cert), key, sizeof(key)) == 0);
   assert(cert_has_ext(cert, NID_subject_key_identifier));
   assert(cert_has_ext(cert, NID_authority_key_identifier));

   char scert[KB_PKI_CERT_PEM_MAX], skey[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_server_cert(ca, "kb.example.com", 3600, scert, sizeof(scert), skey,
                                   sizeof(skey)) == 0);
   assert(cert_has_ext(scert, NID_subject_key_identifier));
   assert(cert_has_ext(scert, NID_authority_key_identifier));
   printf("  akid_present: ok\n");
}

/* The point of the AKI fix: a STRICT X.509 verifier (the OpenSSL 3.5+ / python
 * ssl default) accepts our chain. Without AKI this fails with
 * X509_V_ERR_MISSING_AUTHORITY_KEY_IDENTIFIER. */
static void test_strict_verify(const kb_pki_ca_t *ca)
{
   char cert[KB_PKI_CERT_PEM_MAX], key[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_client_cert(ca, "strict:client", 3600, cert, sizeof(cert), key,
                                   sizeof(key)) == 0);
   BIO *lb = BIO_new_mem_buf(cert, -1);
   X509 *leaf = PEM_read_bio_X509(lb, NULL, NULL, NULL);
   BIO_free(lb);
   BIO *cb = BIO_new_mem_buf(ca->cert_pem, -1);
   X509 *cacert = PEM_read_bio_X509(cb, NULL, NULL, NULL);
   BIO_free(cb);
   assert(leaf && cacert);

   X509_STORE *store = X509_STORE_new();
   assert(X509_STORE_add_cert(store, cacert) == 1);
   X509_STORE_set_flags(store, X509_V_FLAG_X509_STRICT);
   X509_STORE_CTX *ctx = X509_STORE_CTX_new();
   assert(X509_STORE_CTX_init(ctx, store, leaf, NULL) == 1);
   int rc = X509_verify_cert(ctx);
   if (rc != 1)
      fprintf(stderr, "strict verify failed: %s\n",
              X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx)));
   assert(rc == 1); /* strict verification passes with AKI present */

   X509_STORE_CTX_free(ctx);
   X509_STORE_free(store);
   X509_free(leaf);
   X509_free(cacert);
   printf("  strict_verify: ok\n");
}

static void test_issue_and_verify(const kb_pki_ca_t *ca)
{
   char cert[KB_PKI_CERT_PEM_MAX], key[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_client_cert(ca, "client:project:alpha", 3600, cert, sizeof(cert), key,
                                   sizeof(key)) == 0);
   assert(strstr(cert, "-----BEGIN CERTIFICATE-----"));
   assert(strstr(key, "PRIVATE KEY-----"));
   assert_cert_cn(cert, "client:project:alpha");

   char issuer[256], serial[128];
   assert(kb_pki_cert_metadata(cert, issuer, sizeof(issuer), serial, sizeof(serial)) == 0);
   assert(strstr(issuer, "CN=aimee-kb-ca") != NULL && serial[0] != '\0');
   for (const char *p = serial; *p; p++)
      assert((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F'));
   assert(kb_pki_cert_metadata("garbage", issuer, sizeof(issuer), serial, sizeof(serial)) == -1);
   assert(kb_pki_cert_metadata(cert, issuer, 1, serial, sizeof(serial)) == -1);

   /* The issued cert chains to its CA. */
   assert(kb_pki_verify_client_cert(ca->cert_pem, cert) == 1);

   /* It does NOT chain to a different CA. */
   kb_pki_ca_t other;
   assert(kb_pki_ca_generate(&other) == 0);
   assert(kb_pki_verify_client_cert(other.cert_pem, cert) == 0);

   /* A rogue self-signed CA cert (basicConstraints CA:TRUE) is NOT trusted by
    * our CA — a foreign trust anchor cannot pass as a peer of ours. */
   assert(kb_pki_verify_client_cert(ca->cert_pem, other.cert_pem) == 0);

   /* A cert issued by `other` does not verify against `ca` either. */
   char ocert[KB_PKI_CERT_PEM_MAX], okey[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_client_cert(&other, "c2", 3600, ocert, sizeof(ocert), okey, sizeof(okey)) ==
          0);
   assert(kb_pki_verify_client_cert(ca->cert_pem, ocert) == 0);
   assert(kb_pki_verify_client_cert(other.cert_pem, ocert) == 1);

   /* Garbage / empty inputs are not trusted. */
   assert(kb_pki_verify_client_cert(ca->cert_pem, "garbage") == 0);
   assert(kb_pki_verify_client_cert("garbage", cert) == 0);
   assert(kb_pki_verify_client_cert("", "") == 0);

   /* bad issuance args */
   assert(kb_pki_issue_client_cert(NULL, "x", 3600, cert, sizeof(cert), key, sizeof(key)) == -1);
   assert(kb_pki_issue_client_cert(ca, "", 3600, cert, sizeof(cert), key, sizeof(key)) == -1);
   assert(kb_pki_issue_client_cert(ca, "x", 0, cert, sizeof(cert), key, sizeof(key)) == -1);
   printf("  issue_and_verify: ok\n");
}

/* Recursively remove a test directory's CA files + the dir. */
static void cleanup_dir(const char *dir)
{
   char p[512];
   snprintf(p, sizeof(p), "%s/ca.pem", dir);
   remove(p);
   snprintf(p, sizeof(p), "%s/ca-key.pem", dir);
   remove(p);
   rmdir(dir);
}

static void test_persistence(const kb_pki_ca_t *ca)
{
   char dir[256];
   snprintf(dir, sizeof(dir), "/tmp/aimee_ca_test_%d", (int)getpid());
   cleanup_dir(dir); /* start clean */

   /* save creates the dir + both files; the key file is owner-only (0600). */
   assert(kb_pki_ca_save(dir, ca) == 0);
   char keypath[512];
   snprintf(keypath, sizeof(keypath), "%s/ca-key.pem", dir);
   struct stat st;
   assert(stat(keypath, &st) == 0);
   assert((st.st_mode & 077) == 0); /* no group/other permissions on the key */

   /* load round-trips: the reloaded CA has the same cert + fingerprint. */
   kb_pki_ca_t loaded;
   assert(kb_pki_ca_load(dir, &loaded) == 0);
   assert(strcmp(loaded.cert_pem, ca->cert_pem) == 0);
   char fp_orig[KB_PKI_FP_HEX], fp_loaded[KB_PKI_FP_HEX];
   assert(kb_pki_ca_fingerprint(ca->cert_pem, fp_orig, sizeof(fp_orig)) == 0);
   assert(kb_pki_ca_fingerprint(loaded.cert_pem, fp_loaded, sizeof(fp_loaded)) == 0);
   assert(strcmp(fp_orig, fp_loaded) == 0);
   /* the reloaded CA can still issue a verifiable client cert. */
   char ccert[KB_PKI_CERT_PEM_MAX], ckey[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_client_cert(&loaded, "c", 3600, ccert, sizeof(ccert), ckey, sizeof(ckey)) ==
          0);
   assert(kb_pki_verify_client_cert(loaded.cert_pem, ccert) == 1);

   /* load from a dir with no CA fails. */
   cleanup_dir(dir);
   assert(kb_pki_ca_load(dir, &loaded) == -1);

   /* an oversized ca.pem (beyond the PEM buffer) is rejected, not truncated. */
   assert(mkdir(dir, 0700) == 0);
   char cpath[512];
   snprintf(cpath, sizeof(cpath), "%s/ca.pem", dir);
   FILE *cf = fopen(cpath, "wb");
   assert(cf);
   for (int i = 0; i < KB_PKI_CERT_PEM_MAX + 100; i++)
      fputc('x', cf);
   fclose(cf);
   assert(kb_pki_ca_load(dir, &loaded) == -1);
   cleanup_dir(dir);
   printf("  persistence: ok\n");
}

static void test_load_or_create(void)
{
   char dir[256];
   snprintf(dir, sizeof(dir), "/tmp/aimee_ca_loc_%d", (int)getpid());
   cleanup_dir(dir);

   /* first call generates + persists (created == 1). */
   kb_pki_ca_t a;
   int created = -1;
   assert(kb_pki_ca_load_or_create(dir, &a, &created) == 0);
   assert(created == 1);

   /* second call loads the SAME CA (created == 0, same fingerprint) — it does
    * NOT re-roll the CA, which is the whole point of invariant 3. */
   kb_pki_ca_t b;
   created = -1;
   assert(kb_pki_ca_load_or_create(dir, &b, &created) == 0);
   assert(created == 0);
   char fa[KB_PKI_FP_HEX], fb[KB_PKI_FP_HEX];
   assert(kb_pki_ca_fingerprint(a.cert_pem, fa, sizeof(fa)) == 0);
   assert(kb_pki_ca_fingerprint(b.cert_pem, fb, sizeof(fb)) == 0);
   assert(strcmp(fa, fb) == 0);

   /* `created` may be NULL. */
   kb_pki_ca_t c;
   assert(kb_pki_ca_load_or_create(dir, &c, NULL) == 0);

   cleanup_dir(dir);
   printf("  load_or_create: ok\n");
}

/* Build a PEM PKCS#10 CSR for `key` with subject CN `cn`. Caller frees. */
static char *make_csr(EVP_PKEY *key, const char *cn)
{
   X509_REQ *req = X509_REQ_new();
   assert(req);
   assert(X509_REQ_set_version(req, 0) == 1);
   X509_NAME *n = X509_REQ_get_subject_name(req);
   assert(X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (const unsigned char *)cn, -1, -1, 0) ==
          1);
   assert(X509_REQ_set_pubkey(req, key) == 1);
   assert(X509_REQ_sign(req, key, EVP_sha256()) > 0);

   BIO *bio = BIO_new(BIO_s_mem());
   assert(bio && PEM_write_bio_X509_REQ(bio, req) == 1);
   BUF_MEM *bm = NULL;
   BIO_get_mem_ptr(bio, &bm);
   char *out = malloc(bm->length + 1);
   assert(out);
   memcpy(out, bm->data, bm->length);
   out[bm->length] = '\0';
   BIO_free(bio);
   X509_REQ_free(req);
   return out;
}

static void test_sign_csr(const kb_pki_ca_t *ca)
{
   /* Client generates its OWN keypair and a CSR (key never leaves the client). */
   EVP_PKEY *client = EVP_RSA_gen(2048);
   assert(client);
   /* The CSR asserts a bogus identity — the server must ignore it. */
   char *csr = make_csr(client, "client-asserted-admin");

   char cert[KB_PKI_CERT_PEM_MAX];
   assert(kb_pki_sign_csr(ca, csr, "project:alpha", 3600, cert, sizeof(cert)) == 0);
   assert(strstr(cert, "-----BEGIN CERTIFICATE-----"));

   /* The issued cert chains to the CA. */
   assert(kb_pki_verify_client_cert(ca->cert_pem, cert) == 1);
   /* Subject is the SERVER-controlled value, NOT the CSR's asserted CN. */
   assert_cert_cn(cert, "project:alpha");

   /* The cert carries the CLIENT's public key (so the client's private key
    * matches the cert — it can authenticate with it). */
   {
      BIO *b = BIO_new_mem_buf(cert, -1);
      X509 *c = PEM_read_bio_X509(b, NULL, NULL, NULL);
      BIO_free(b);
      assert(c);
      EVP_PKEY *cert_pub = X509_get_pubkey(c);
      assert(cert_pub && EVP_PKEY_eq(cert_pub, client) == 1);
      EVP_PKEY_free(cert_pub);
      X509_free(c);
   }

   /* A corrupted CSR (broken self-signature) is rejected. */
   {
      char *bad = malloc(strlen(csr) + 1);
      strcpy(bad, csr);
      /* flip a char in the base64 body to break the signature */
      char *body = strstr(bad, "REQUEST-----\n");
      assert(body);
      body += strlen("REQUEST-----\n");
      *body = (*body == 'A' ? 'B' : 'A');
      char c2[KB_PKI_CERT_PEM_MAX];
      assert(kb_pki_sign_csr(ca, bad, "project:alpha", 3600, c2, sizeof(c2)) == -1);
      free(bad);
   }

   /* A CSR carrying a weak (1024-bit RSA) key is rejected — clients cannot
    * enroll a crackable key. */
   {
      EVP_PKEY *weak = EVP_RSA_gen(1024);
      assert(weak);
      char *weak_csr = make_csr(weak, "whoever");
      char wc[KB_PKI_CERT_PEM_MAX];
      assert(kb_pki_sign_csr(ca, weak_csr, "project:alpha", 3600, wc, sizeof(wc)) == -1);
      free(weak_csr);
      EVP_PKEY_free(weak);
   }

   /* arg guards */
   char c3[KB_PKI_CERT_PEM_MAX];
   assert(kb_pki_sign_csr(NULL, csr, "s", 3600, c3, sizeof(c3)) == -1);
   assert(kb_pki_sign_csr(ca, "garbage", "s", 3600, c3, sizeof(c3)) == -1);
   assert(kb_pki_sign_csr(ca, csr, "", 3600, c3, sizeof(c3)) == -1);
   assert(kb_pki_sign_csr(ca, csr, "s", 0, c3, sizeof(c3)) == -1);

   /* kb_pki_csr_validate: accepts a good CSR, rejects garbage / weak keys. */
   assert(kb_pki_csr_validate(csr) == 0);
   assert(kb_pki_csr_validate("not a csr") == -1);
   assert(kb_pki_csr_validate("") == -1);
   assert(kb_pki_csr_validate(NULL) == -1);

   free(csr);
   EVP_PKEY_free(client);
   printf("  sign_csr: ok\n");
}

static void test_role_csr_profiles(const kb_pki_ca_t *ca)
{
   EVP_PKEY *client_key = EVP_RSA_gen(2048);
   EVP_PKEY *server_key = EVP_RSA_gen(2048);
   assert(client_key && server_key && EVP_PKEY_eq(client_key, server_key) == 0);
   char *client_csr = make_csr(client_key, "ignored-client");
   char *server_csr = make_csr(server_key, "ignored-server");
   char client_cert[KB_PKI_CERT_PEM_MAX], server_cert[KB_PKI_CERT_PEM_MAX];

   assert(kb_pki_sign_csr_profile(ca, client_csr, "server:p5-a", 3600, KB_PKI_CSR_CLIENT_AUTH,
                                  client_cert, sizeof(client_cert)) == 0);
   assert(kb_pki_sign_csr_profile(ca, server_csr, "p5-server.example", 3600, KB_PKI_CSR_SERVER_AUTH,
                                  server_cert, sizeof(server_cert)) == 0);
   assert_exact_eku(client_cert, NID_client_auth);
   assert_exact_eku(server_cert, NID_server_auth);
   assert_cert_cn(client_cert, "server:p5-a");
   assert_cert_cn(server_cert, "p5-server.example");

   BIO *bio = BIO_new_mem_buf(server_cert, -1);
   X509 *cert = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
   BIO_free(bio);
   assert(cert && X509_check_host(cert, "p5-server.example", 0, 0, NULL) == 1);
   X509_free(cert);

   assert(kb_pki_sign_csr_profile(ca, client_csr, "server:p5-a", 3600, (kb_pki_csr_profile_t)99,
                                  client_cert, sizeof(client_cert)) == -1);
   free(client_csr);
   free(server_csr);
   EVP_PKEY_free(client_key);
   EVP_PKEY_free(server_key);
   printf("  role_csr_profiles: ok\n");
}

static void test_server_cert(const kb_pki_ca_t *ca)
{
   char cert[KB_PKI_CERT_PEM_MAX], key[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_server_cert(ca, "kb.example.com", 3600, cert, sizeof(cert), key,
                                   sizeof(key)) == 0);
   assert(strstr(cert, "-----BEGIN CERTIFICATE-----") && strstr(key, "PRIVATE KEY-----"));
   assert(kb_pki_verify_client_cert(ca->cert_pem, cert) == 1); /* chains to the CA */
   assert_cert_cn(cert, "kb.example.com");

   /* SAN matches the host (TLS hostname verification would pass / reject). */
   {
      BIO *b = BIO_new_mem_buf(cert, -1);
      X509 *c = PEM_read_bio_X509(b, NULL, NULL, NULL);
      BIO_free(b);
      assert(c);
      assert(X509_check_host(c, "kb.example.com", 0, 0, NULL) == 1);
      assert(X509_check_host(c, "evil.example.com", 0, 0, NULL) != 1);
      X509_free(c);
   }

   /* a literal IP host gets an IP subjectAltName. */
   assert(kb_pki_issue_server_cert(ca, "10.0.0.5", 3600, cert, sizeof(cert), key, sizeof(key)) ==
          0);
   {
      BIO *b = BIO_new_mem_buf(cert, -1);
      X509 *c = PEM_read_bio_X509(b, NULL, NULL, NULL);
      BIO_free(b);
      assert(c);
      unsigned char ip[4] = {10, 0, 0, 5};
      assert(X509_check_ip(c, ip, 4, 0) == 1);
      X509_free(c);
   }

   /* arg guards */
   assert(kb_pki_issue_server_cert(NULL, "h", 1, cert, sizeof(cert), key, sizeof(key)) == -1);
   assert(kb_pki_issue_server_cert(ca, "", 1, cert, sizeof(cert), key, sizeof(key)) == -1);
   assert(kb_pki_issue_server_cert(ca, "h", 0, cert, sizeof(cert), key, sizeof(key)) == -1);
   printf("  server_cert: ok\n");
}

int main(void)
{
   printf("kb_pki:\n");
   kb_pki_ca_t ca;
   test_ca_generate(&ca);
   test_fingerprint(&ca);
   test_issue_and_verify(&ca);
   test_persistence(&ca);
   test_load_or_create();
   test_sign_csr(&ca);
   test_role_csr_profiles(&ca);
   test_server_cert(&ca);
   test_akid_present(&ca);
   test_strict_verify(&ca);
   printf("All kb_pki tests passed.\n");
   return 0;
}
