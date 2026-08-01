/* test_kb_enroll.c — unit tests for the enrollment-token + connection-string
 * foundation (src/kb/enroll.c): token generation, sha256 storage hash,
 * constant-time verify, the aimee:// connection-string codec, and the
 * single-use token registry. */
#include "kb_enroll.h"
#include "kb_pki.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- 1. token generation: random, base64url, length, buffer guard ---- */
static void test_token_generate(void)
{
   char a[KB_ENROLL_TOKEN_MAX], b[KB_ENROLL_TOKEN_MAX];
   assert(kb_enroll_token_generate(a, sizeof(a)) > 0);
   assert(kb_enroll_token_generate(b, sizeof(b)) > 0);
   assert(strcmp(a, b) != 0); /* two tokens differ (random) */
   assert(strlen(a) >= 40 && strlen(a) < KB_ENROLL_TOKEN_MAX);
   for (size_t i = 0; i < strlen(a); i++)
   {
      char c = a[i];
      assert((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
             c == '-' || c == '_'); /* base64url alphabet, no padding */
   }
   char tiny[8];
   assert(kb_enroll_token_generate(tiny, sizeof(tiny)) == -1);
   printf("  token_generate: ok\n");
}

/* ---- 2. token hash: deterministic, 64 hex, known SHA-256 vector ---- */
static void test_token_hash(void)
{
   char h1[KB_ENROLL_HASH_HEX], h2[KB_ENROLL_HASH_HEX], h3[KB_ENROLL_HASH_HEX];
   assert(kb_enroll_token_hash("hello", h1, sizeof(h1)) == 0);
   assert(strlen(h1) == 64);
   assert(kb_enroll_token_hash("hello", h2, sizeof(h2)) == 0);
   assert(strcmp(h1, h2) == 0); /* deterministic */
   assert(kb_enroll_token_hash("world", h3, sizeof(h3)) == 0);
   assert(strcmp(h1, h3) != 0); /* different input -> different hash */
   assert(strcmp(h1, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824") == 0);
   char tiny[8];
   assert(kb_enroll_token_hash("hello", tiny, sizeof(tiny)) == -1);
   printf("  token_hash: ok\n");
}

/* ---- 3. constant-time verify ---- */
static void test_token_verify(void)
{
   char tok[KB_ENROLL_TOKEN_MAX], hash[KB_ENROLL_HASH_HEX];
   assert(kb_enroll_token_generate(tok, sizeof(tok)) > 0);
   assert(kb_enroll_token_hash(tok, hash, sizeof(hash)) == 0);
   assert(kb_enroll_token_verify(tok, hash) == 1);           /* correct token verifies */
   assert(kb_enroll_token_verify("wrong-token", hash) == 0); /* wrong token rejected */
   char tampered[KB_ENROLL_HASH_HEX];
   snprintf(tampered, sizeof(tampered), "%s", hash);
   tampered[0] = (tampered[0] == 'a' ? 'b' : 'a');
   assert(kb_enroll_token_verify(tok, tampered) == 0); /* tampered stored hash rejected */
   assert(kb_enroll_token_verify(tok, "") == 0);       /* empty stored hash rejected */
   printf("  token_verify: ok\n");
}

/* ---- 4. connection-string build + parse roundtrip ---- */
static void test_conn_string_roundtrip(void)
{
   char s[512];
   int n =
       kb_enroll_conn_string_build("kb.example.com", 8443, "abc123def", "TOKEN-xyz", s, sizeof(s));
   assert(n > 0);
   assert(strcmp(s, "aimee://kb.example.com:8443?ca=sha256:abc123def&enroll=TOKEN-xyz") == 0);

   kb_enroll_conn_t c;
   assert(kb_enroll_conn_string_parse(s, &c) == 0);
   assert(strcmp(c.host, "kb.example.com") == 0);
   assert(c.port == 8443);
   assert(strcmp(c.ca_sha256, "abc123def") == 0);
   assert(strcmp(c.enroll_token, "TOKEN-xyz") == 0);

   /* query params in either order */
   kb_enroll_conn_t c2;
   assert(kb_enroll_conn_string_parse("aimee://h:9?enroll=TT&ca=sha256:FF", &c2) == 0);
   assert(strcmp(c2.host, "h") == 0 && c2.port == 9 && strcmp(c2.ca_sha256, "FF") == 0 &&
          strcmp(c2.enroll_token, "TT") == 0);

   /* build rejects bad args */
   assert(kb_enroll_conn_string_build("", 8443, "f", "t", s, sizeof(s)) == -1);
   assert(kb_enroll_conn_string_build("h", 0, "f", "t", s, sizeof(s)) == -1);
   printf("  conn_string_roundtrip: ok\n");
}

/* ---- 5. malformed connection strings rejected ---- */
static void test_conn_string_malformed(void)
{
   kb_enroll_conn_t c;
   assert(kb_enroll_conn_string_parse("http://h:8443?ca=sha256:f&enroll=t", &c) == -1); /* scheme */
   assert(kb_enroll_conn_string_parse("aimee://h:8443?enroll=t", &c) == -1);            /* no ca */
   assert(kb_enroll_conn_string_parse("aimee://h:8443?ca=sha256:f", &c) == -1); /* no enroll */
   assert(kb_enroll_conn_string_parse("aimee://h:8443?ca=md5:f&enroll=t", &c) ==
          -1);                                                                      /* not sha256 */
   assert(kb_enroll_conn_string_parse("aimee://h?ca=sha256:f&enroll=t", &c) == -1); /* no port */
   assert(kb_enroll_conn_string_parse("aimee://h:0?ca=sha256:f&enroll=t", &c) == -1); /* port=0 */
   assert(kb_enroll_conn_string_parse("aimee://h:99999?ca=sha256:f&enroll=t", &c) ==
          -1); /* >65535 */
   printf("  conn_string_malformed: ok\n");
}

/* ---- 6. single-use registry: issue / consume / replay / reset ---- */
static void test_registry_single_use(void)
{
   kb_enroll_registry_reset();
   char tok[KB_ENROLL_TOKEN_MAX], scope[128];
   assert(kb_enroll_registry_issue("project:X", tok, sizeof(tok)) == 0);
   assert(kb_enroll_registry_consume(tok, scope, sizeof(scope)) == 1); /* first use */
   assert(strcmp(scope, "project:X") == 0);                            /* scope returned */
   assert(kb_enroll_registry_consume(tok, scope, sizeof(scope)) == 0); /* replay rejected */
   assert(kb_enroll_registry_consume("never-issued", scope, sizeof(scope)) == 0); /* unknown */

   /* a second issued token is independent */
   char tok2[KB_ENROLL_TOKEN_MAX];
   assert(kb_enroll_registry_issue("global", tok2, sizeof(tok2)) == 0);
   assert(strcmp(tok, tok2) != 0);
   assert(kb_enroll_registry_consume(tok2, scope, sizeof(scope)) == 1);
   assert(strcmp(scope, "global") == 0);

   kb_enroll_registry_reset();
   assert(kb_enroll_registry_consume(tok2, scope, sizeof(scope)) == 0); /* gone after reset */
   printf("  registry_single_use: ok\n");
}

/* ---- 7. file-backed single-use store: issue / consume / replay / persist ---- */
static void test_store_single_use(void)
{
   char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee_enroll_store_%d.txt", (int)getpid());
   remove(path); /* start clean */

   char tok[KB_ENROLL_TOKEN_MAX], tok2[KB_ENROLL_TOKEN_MAX], scope[KB_ENROLL_SCOPE_MAX];

   /* issue two independent tokens to the file. */
   assert(kb_enroll_store_issue(path, "project:X", tok, sizeof(tok)) == 0);
   assert(kb_enroll_store_issue(path, "global", tok2, sizeof(tok2)) == 0);
   assert(strcmp(tok, tok2) != 0);
   struct stat lock_stat;
   assert(stat(path, &lock_stat) == 0 && lock_stat.st_size == 0); /* verifier is Vault-only */

   /* consume the first: returns its scope, single-use (replay rejected). */
   assert(kb_enroll_store_consume(path, tok, scope, sizeof(scope)) == 1);
   assert(strcmp(scope, "project:X") == 0);
   assert(kb_enroll_store_consume(path, tok, scope, sizeof(scope)) == 0); /* replay */

   /* the second token is still independently consumable. */
   assert(kb_enroll_store_consume(path, tok2, scope, sizeof(scope)) == 1);
   assert(strcmp(scope, "global") == 0);

   /* unknown token / missing store reject. */
   assert(kb_enroll_store_consume(path, "never-issued", scope, sizeof(scope)) == 0);
   assert(kb_enroll_store_consume("/no/such/store.txt", tok, scope, sizeof(scope)) == 0);

   /* persistence: a token issued to the file is consumable "fresh" (the store is
    * the file, not process memory) — issue, then consume with no shared state. */
   char tok3[KB_ENROLL_TOKEN_MAX];
   assert(kb_enroll_store_issue(path, "project:Y", tok3, sizeof(tok3)) == 0);
   assert(kb_enroll_store_consume(path, tok3, scope, sizeof(scope)) == 1);
   assert(strcmp(scope, "project:Y") == 0);

   /* a scope with a tab/newline is rejected (would corrupt the record format). */
   char bad[KB_ENROLL_TOKEN_MAX];
   assert(kb_enroll_store_issue(path, "a\tb", bad, sizeof(bad)) == -1);
   assert(kb_enroll_store_issue(path, "a\nb", bad, sizeof(bad)) == -1);

   /* Upgrade path: an existing hash registry is sealed and zero-truncated
    * before its token remains redeemable from Vault. */
   char legacy_path[256], legacy_hash[KB_ENROLL_HASH_HEX];
   snprintf(legacy_path, sizeof(legacy_path), "/tmp/aimee_enroll_legacy_%d.txt", (int)getpid());
   assert(kb_enroll_token_hash("legacy-token", legacy_hash, sizeof(legacy_hash)) == 0);
   FILE *legacy = fopen(legacy_path, "wb");
   assert(legacy != NULL);
   fprintf(legacy, "%s\t0\tlegacy-scope\n", legacy_hash);
   fclose(legacy);
   assert(kb_enroll_store_migrate(legacy_path) == 0);
   assert(stat(legacy_path, &lock_stat) == 0 && lock_stat.st_size == 0);
   assert(kb_enroll_store_consume(legacy_path, "legacy-token", scope, sizeof(scope)) == 1);
   assert(strcmp(scope, "legacy-scope") == 0);

   remove(path);
   remove(legacy_path);
   printf("  store_single_use: ok\n");
}

/* ---- 8. kb_enroll_mint: CA + token store + connection string, end-to-end ---- */
static void test_mint(void)
{
   char dir[256];
   snprintf(dir, sizeof(dir), "/tmp/aimee_enroll_mint_%d", (int)getpid());
   mkdir(dir, 0700);

   char conn[512];
   assert(kb_enroll_mint(dir, "kb.example.com", 8443, "project:alpha", conn, sizeof(conn)) == 0);

   /* The emitted string parses, and carries the right host/port + a token. */
   kb_enroll_conn_t c;
   assert(kb_enroll_conn_string_parse(conn, &c) == 0);
   assert(strcmp(c.host, "kb.example.com") == 0 && c.port == 8443);
   assert(c.ca_sha256[0] && c.enroll_token[0]);

   /* The ca fingerprint matches the persisted CA's. */
   char capath[300];
   snprintf(capath, sizeof(capath), "%s/kb-ca", dir);
   kb_pki_ca_t ca;
   assert(kb_pki_ca_load(capath, &ca) == 0);
   char fp[KB_PKI_FP_HEX];
   assert(kb_pki_ca_fingerprint(ca.cert_pem, fp, sizeof(fp)) == 0);
   assert(strcmp(fp, c.ca_sha256) == 0);

   /* The token is redeemable once from the persisted store, with its scope. */
   char store[300], scope[KB_ENROLL_SCOPE_MAX];
   snprintf(store, sizeof(store), "%s/kb-enroll-tokens", dir);
   assert(kb_enroll_store_consume(store, c.enroll_token, scope, sizeof(scope)) == 1);
   assert(strcmp(scope, "project:alpha") == 0);
   assert(kb_enroll_store_consume(store, c.enroll_token, scope, sizeof(scope)) ==
          0); /* single-use */

   /* A second mint reuses the SAME CA (fingerprint stable across calls). */
   char conn2[512];
   assert(kb_enroll_mint(dir, "kb.example.com", 8443, "global", conn2, sizeof(conn2)) == 0);
   kb_enroll_conn_t c2;
   assert(kb_enroll_conn_string_parse(conn2, &c2) == 0);
   assert(strcmp(c2.ca_sha256, c.ca_sha256) == 0); /* CA not re-rolled */
   assert(strcmp(c2.enroll_token, c.enroll_token) != 0);

   /* arg guards */
   assert(kb_enroll_mint(NULL, "h", 1, "s", conn, sizeof(conn)) == -1);
   assert(kb_enroll_mint(dir, "", 1, "s", conn, sizeof(conn)) == -1);
   assert(kb_enroll_mint(dir, "h", 0, "s", conn, sizeof(conn)) == -1);

   /* cleanup */
   char p[320];
   snprintf(p, sizeof(p), "%s/kb-ca/ca.pem", dir);
   remove(p);
   snprintf(p, sizeof(p), "%s/kb-ca/ca-key.pem", dir);
   remove(p);
   snprintf(p, sizeof(p), "%s/kb-ca", dir);
   rmdir(p);
   remove(store);
   rmdir(dir);
   printf("  mint: ok\n");
}

/* ---- 9. kb_enroll_redeem: token -> CA-signed client cert (single-use) ---- */
static void test_redeem(void)
{
   char dir[256];
   snprintf(dir, sizeof(dir), "/tmp/aimee_enroll_redeem_%d", (int)getpid());
   mkdir(dir, 0700);

   /* mint a token bound to a scope (creates the CA + store under dir). */
   char conn[512];
   assert(kb_enroll_mint(dir, "kb.example.com", 8443, "project:alpha", conn, sizeof(conn)) == 0);
   kb_enroll_conn_t c;
   assert(kb_enroll_conn_string_parse(conn, &c) == 0);

   /* redeem it: get a client cert + key + the granted scope. */
   char scope[KB_ENROLL_SCOPE_MAX];
   char cert[KB_PKI_CERT_PEM_MAX], key[KB_PKI_KEY_PEM_MAX];
   assert(kb_enroll_redeem(dir, c.enroll_token, 3600, scope, sizeof(scope), cert, sizeof(cert), key,
                           sizeof(key)) == 0);
   assert(strcmp(scope, "project:alpha") == 0);
   assert(strstr(cert, "-----BEGIN CERTIFICATE-----"));
   assert(strstr(key, "PRIVATE KEY-----"));

   /* the issued client cert chains to the persisted CA. */
   char capath[300];
   snprintf(capath, sizeof(capath), "%s/kb-ca", dir);
   kb_pki_ca_t ca;
   assert(kb_pki_ca_load(capath, &ca) == 0);
   assert(kb_pki_verify_client_cert(ca.cert_pem, cert) == 1);

   /* single-use: redeeming the same token again is rejected (no second cert). */
   assert(kb_enroll_redeem(dir, c.enroll_token, 3600, scope, sizeof(scope), cert, sizeof(cert), key,
                           sizeof(key)) == -1);
   /* unknown token rejected. */
   assert(kb_enroll_redeem(dir, "never-minted", 3600, scope, sizeof(scope), cert, sizeof(cert), key,
                           sizeof(key)) == -1);

   /* a dir with no CA cannot redeem (and must not burn the token). */
   char empty[256];
   snprintf(empty, sizeof(empty), "/tmp/aimee_enroll_noca_%d", (int)getpid());
   mkdir(empty, 0700);
   char tok[KB_ENROLL_TOKEN_MAX];
   char tokstore[300];
   snprintf(tokstore, sizeof(tokstore), "%s/kb-enroll-tokens", empty);
   assert(kb_enroll_store_issue(tokstore, "x", tok, sizeof(tok)) == 0);
   assert(kb_enroll_redeem(empty, tok, 3600, scope, sizeof(scope), cert, sizeof(cert), key,
                           sizeof(key)) == -1);
   /* the token was NOT consumed (CA load failed first) — it stays redeemable
    * once a CA exists. */
   char sc2[KB_ENROLL_SCOPE_MAX];
   assert(kb_enroll_store_consume(tokstore, tok, sc2, sizeof(sc2)) == 1);

   /* arg guards */
   assert(kb_enroll_redeem(NULL, "t", 1, scope, sizeof(scope), cert, sizeof(cert), key,
                           sizeof(key)) == -1);

   /* cleanup */
   char p[320];
   snprintf(p, sizeof(p), "%s/kb-ca/ca.pem", dir);
   remove(p);
   snprintf(p, sizeof(p), "%s/kb-ca/ca-key.pem", dir);
   remove(p);
   snprintf(p, sizeof(p), "%s/kb-ca", dir);
   rmdir(p);
   snprintf(p, sizeof(p), "%s/kb-enroll-tokens", dir);
   remove(p);
   rmdir(dir);
   remove(tokstore);
   rmdir(empty);
   printf("  redeem: ok\n");
}

/* Build a PEM CSR for a fresh RSA-`bits` key; returns the CSR PEM + (out) key. */
static char *make_client_csr(int bits, EVP_PKEY **key_out)
{
   EVP_PKEY *key = EVP_RSA_gen(bits);
   assert(key);
   X509_REQ *req = X509_REQ_new();
   assert(req && X509_REQ_set_version(req, 0) == 1);
   X509_NAME *n = X509_REQ_get_subject_name(req);
   X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (const unsigned char *)"client", -1, -1, 0);
   assert(X509_REQ_set_pubkey(req, key) == 1);
   assert(X509_REQ_sign(req, key, EVP_sha256()) > 0);
   BIO *bio = BIO_new(BIO_s_mem());
   assert(bio && PEM_write_bio_X509_REQ(bio, req) == 1);
   BUF_MEM *bm = NULL;
   BIO_get_mem_ptr(bio, &bm);
   char *out = malloc(bm->length + 1);
   memcpy(out, bm->data, bm->length);
   out[bm->length] = '\0';
   BIO_free(bio);
   X509_REQ_free(req);
   if (key_out)
      *key_out = key;
   else
      EVP_PKEY_free(key);
   return out;
}

/* ---- 10. kb_enroll_redeem_csr: token + client CSR -> CA-signed cert ---- */
static void test_redeem_csr(void)
{
   char dir[256];
   snprintf(dir, sizeof(dir), "/tmp/aimee_redeem_csr_%d", (int)getpid());
   mkdir(dir, 0700);

   /* mint a scoped token (creates CA + store). */
   char conn[512];
   assert(kb_enroll_mint(dir, "kb.example.com", 8443, "project:beta", conn, sizeof(conn)) == 0);
   kb_enroll_conn_t c;
   assert(kb_enroll_conn_string_parse(conn, &c) == 0);

   /* client builds its own keypair + CSR; redeem returns ONLY a cert. */
   EVP_PKEY *ckey = NULL;
   char *csr = make_client_csr(2048, &ckey);
   char scope[KB_ENROLL_SCOPE_MAX], cert[KB_PKI_CERT_PEM_MAX];
   assert(kb_enroll_redeem_csr(dir, c.enroll_token, csr, 3600, scope, sizeof(scope), cert,
                               sizeof(cert)) == 0);
   assert(strcmp(scope, "project:beta") == 0);
   /* cert chains to the CA. */
   char capath[300];
   snprintf(capath, sizeof(capath), "%s/kb-ca", dir);
   kb_pki_ca_t ca;
   assert(kb_pki_ca_load(capath, &ca) == 0);
   assert(kb_pki_verify_client_cert(ca.cert_pem, cert) == 1);

   /* single-use: a second redeem of the same token is rejected. */
   assert(kb_enroll_redeem_csr(dir, c.enroll_token, csr, 3600, scope, sizeof(scope), cert,
                               sizeof(cert)) == -1);

   /* a MALFORMED CSR must NOT burn a fresh token (validated before consume). */
   char conn2[512];
   assert(kb_enroll_mint(dir, "kb.example.com", 8443, "project:gamma", conn2, sizeof(conn2)) == 0);
   kb_enroll_conn_t c2;
   assert(kb_enroll_conn_string_parse(conn2, &c2) == 0);
   assert(kb_enroll_redeem_csr(dir, c2.enroll_token, "garbage-csr", 3600, scope, sizeof(scope),
                               cert, sizeof(cert)) == -1);
   /* the token survived the bad CSR — a good CSR now redeems it. */
   char *csr2 = make_client_csr(2048, NULL);
   assert(kb_enroll_redeem_csr(dir, c2.enroll_token, csr2, 3600, scope, sizeof(scope), cert,
                               sizeof(cert)) == 0);
   assert(strcmp(scope, "project:gamma") == 0);

   /* arg guards */
   assert(kb_enroll_redeem_csr(NULL, "t", csr, 1, scope, sizeof(scope), cert, sizeof(cert)) == -1);

   free(csr);
   free(csr2);
   EVP_PKEY_free(ckey);
   char p[320];
   snprintf(p, sizeof(p), "%s/kb-ca/ca.pem", dir);
   remove(p);
   snprintf(p, sizeof(p), "%s/kb-ca/ca-key.pem", dir);
   remove(p);
   snprintf(p, sizeof(p), "%s/kb-ca", dir);
   rmdir(p);
   snprintf(p, sizeof(p), "%s/kb-enroll-tokens", dir);
   remove(p);
   rmdir(dir);
   printf("  redeem_csr: ok\n");
}

int main(void)
{
   char vault_home[256];
   snprintf(vault_home, sizeof(vault_home), "/tmp/aimee_kb_enroll_vault_%d", (int)getpid());
   assert(mkdir(vault_home, 0700) == 0);
   assert(setenv("AIMEE_HOME", vault_home, 1) == 0);
   printf("kb_enroll:\n");
   test_token_generate();
   test_token_hash();
   test_token_verify();
   test_conn_string_roundtrip();
   test_conn_string_malformed();
   test_registry_single_use();
   test_store_single_use();
   test_mint();
   test_redeem();
   test_redeem_csr();
   char cleanup[320];
   snprintf(cleanup, sizeof(cleanup), "rm -rf -- '%s'", vault_home);
   assert(system(cleanup) == 0);
   printf("All kb_enroll tests passed.\n");
   return 0;
}
