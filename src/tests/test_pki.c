/* test_pki.c: aimee's client-cert CA — CA ensure, issuance (cert chains to the
 * CA), revocation snapshot, and CN-spoofing refusal (mtls-client-identity slice 2). */
#include "pki.h"
#include "vault_principal.h"
#include "config.h"       /* config_default_dir */
#include "db1.h"          /* db1_init / db1_shutdown */
#include "db1_internal.h" /* db1_conn — direct durable UPDATE for the P8a tests */
#include <sqlite3.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *make_csr(const char *subject_cn, int forge)
{
   EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
   EVP_PKEY *key = NULL;
   X509_REQ *req = NULL;
   BIO *bio = NULL;
   char *out = NULL;
   if (!kctx || EVP_PKEY_keygen_init(kctx) != 1 ||
       EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1) != 1 ||
       EVP_PKEY_keygen(kctx, &key) != 1 || !(req = X509_REQ_new()) ||
       X509_REQ_set_version(req, 0) != 1)
      goto done;
   X509_NAME *name = X509_REQ_get_subject_name(req);
   if (!name ||
       X509_NAME_add_entry_by_NID(name, NID_commonName, MBSTRING_ASC,
                                  (const unsigned char *)subject_cn, -1, -1, 0) != 1 ||
       X509_REQ_set_pubkey(req, key) != 1 || X509_REQ_sign(req, key, EVP_sha256()) <= 0)
      goto done;
   if (forge)
   {
      EVP_PKEY_CTX *other_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
      EVP_PKEY *other = NULL;
      int ok = other_ctx && EVP_PKEY_keygen_init(other_ctx) == 1 &&
               EVP_PKEY_CTX_set_ec_paramgen_curve_nid(other_ctx, NID_X9_62_prime256v1) == 1 &&
               EVP_PKEY_keygen(other_ctx, &other) == 1 && X509_REQ_set_pubkey(req, other) == 1;
      EVP_PKEY_free(other);
      EVP_PKEY_CTX_free(other_ctx);
      if (!ok)
         goto done;
   }
   if (!(bio = BIO_new(BIO_s_mem())) || PEM_write_bio_X509_REQ(bio, req) != 1)
      goto done;
   BUF_MEM *mem = NULL;
   BIO_get_mem_ptr(bio, &mem);
   if (mem)
   {
      out = malloc(mem->length + 1);
      if (out)
      {
         memcpy(out, mem->data, mem->length);
         out[mem->length] = '\0';
      }
   }
done:
   BIO_free(bio);
   X509_REQ_free(req);
   EVP_PKEY_free(key);
   EVP_PKEY_CTX_free(kctx);
   return out;
}

static int verify_chains_to_ca(const char *cert_pem, const char *ca_path)
{
   BIO *cb = BIO_new_mem_buf(cert_pem, -1);
   X509 *cert = cb ? PEM_read_bio_X509(cb, NULL, NULL, NULL) : NULL;
   if (cb)
      BIO_free(cb);
   FILE *f = fopen(ca_path, "r");
   X509 *ca = f ? PEM_read_X509(f, NULL, NULL, NULL) : NULL;
   if (f)
      fclose(f);
   int ok = 0;
   if (cert && ca)
   {
      X509_STORE *store = X509_STORE_new();
      X509_STORE_CTX *ctx = X509_STORE_CTX_new();
      if (store && ctx && X509_STORE_add_cert(store, ca) == 1 &&
          X509_STORE_CTX_init(ctx, store, cert, NULL) == 1)
         ok = (X509_verify_cert(ctx) == 1);
      if (ctx)
         X509_STORE_CTX_free(ctx);
      if (store)
         X509_STORE_free(store);
   }
   if (cert)
      X509_free(cert);
   if (ca)
      X509_free(ca);
   return ok;
}

/* Read a whole file into |buf|; returns byte count (0 on any error). */
static long slurp(const char *path, char *buf, size_t cap)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return 0;
   size_t n = fread(buf, 1, cap, f);
   fclose(f);
   return (long)n;
}

/* Rewrite a cert's DURABLE expires_at directly in DB1, bypassing pki.c — lets the
 * P8a tests drive the expiry boundary without waiting on wall-clock. */
static int pki_test_set_expires(const char *serial, long expires_at)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "UPDATE pki_certs SET expires_at=? WHERE serial=?", -1, &st, NULL) !=
       SQLITE_OK)
      return -1;
   sqlite3_bind_int64(st, 1, expires_at);
   sqlite3_bind_text(st, 2, serial, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

/* Revoke a cert DURABLY (the DB row) but bypass pki_revoke's snapshot_add, so the
 * in-memory g_revoked snapshot stays STALE — the setup for the load-bearing (e)
 * property: pki_is_revoked misses it, pki_cert_check (durable) catches it. */
static int pki_test_revoke_db_only(const char *serial)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "UPDATE pki_certs SET revoked=1 WHERE serial=?", -1, &st, NULL) !=
       SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, serial, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int main(void)
{
   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-pki-test-%d", (int)getpid());
   char cmd[600];
   snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", home, home);
   assert(system(cmd) == 0);
   setenv("AIMEE_HOME", home, 1);
   assert(db1_init(":memory:") == 0);
   pki_reset_for_test();

   /* CA ensure: creates the CA + writes the client-ca.crt the server verifies against. */
   assert(pki_ca_ensure() == 0);
   char ca_path[400];
   snprintf(ca_path, sizeof(ca_path), "%s/tls/client-ca.crt", config_default_dir());
   assert(access(ca_path, R_OK) == 0);

   /* Issue a client cert; it must be a real PEM with a serial, and chain to the CA. */
   char cert[8192] = "", key[4096] = "", serial[64] = "";
   assert(pki_issue("ci-runner-1", 30, cert, sizeof(cert), key, sizeof(key), serial,
                    sizeof(serial)) == 0);
   assert(strstr(cert, "BEGIN CERTIFICATE") != NULL);
   assert(strstr(key, "BEGIN") != NULL && strstr(key, "PRIVATE KEY") != NULL);
   assert(serial[0] != '\0');
   assert(verify_chains_to_ca(cert, ca_path) == 1);

   /* P8b: the client owns the key and sends only a signed CSR. The CSR subject is
    * ignored; the server-selected CN is authoritative and the roster is durable. */
   char *csr = make_csr("attacker-chosen-cn", 0);
   char csr_cert[8192] = "", csr_serial[64] = "";
   assert(csr != NULL);
   assert(pki_sign_csr("thin-client-1", 30, csr, csr_cert, sizeof(csr_cert), csr_serial,
                       sizeof(csr_serial)) == 0);
   assert(verify_chains_to_ca(csr_cert, ca_path) == 1);
   assert(pki_cert_check(csr_serial, (long)time(NULL)) == PKI_CERT_VALID);
   BIO *signed_bio = BIO_new_mem_buf(csr_cert, -1);
   X509 *signed_cert = signed_bio ? PEM_read_bio_X509(signed_bio, NULL, NULL, NULL) : NULL;
   char signed_cn[128] = "";
   assert(signed_cert != NULL);
   X509_NAME_get_text_by_NID(X509_get_subject_name(signed_cert), NID_commonName, signed_cn,
                             sizeof(signed_cn));
   assert(strcmp(signed_cn, "thin-client-1") == 0);
   EXTENDED_KEY_USAGE *eku = X509_get_ext_d2i(signed_cert, NID_ext_key_usage, NULL, NULL);
   int have_client = 0, have_server = 0;
   assert(eku != NULL);
   for (int i = 0; i < sk_ASN1_OBJECT_num(eku); i++)
   {
      int nid = OBJ_obj2nid(sk_ASN1_OBJECT_value(eku, i));
      have_client |= nid == NID_client_auth;
      have_server |= nid == NID_server_auth;
   }
   assert(have_client && !have_server);
   EXTENDED_KEY_USAGE_free(eku);
   X509_free(signed_cert);
   BIO_free(signed_bio);
   char bad_cert[8192], bad_serial[64];
   assert(pki_sign_csr("thin-client-bad", 30, "not a csr", bad_cert, sizeof(bad_cert), bad_serial,
                       sizeof(bad_serial)) == -1);
   char *forged_csr = make_csr("forged", 1);
   assert(forged_csr != NULL);
   assert(pki_sign_csr("thin-client-forged", 30, forged_csr, bad_cert, sizeof(bad_cert), bad_serial,
                       sizeof(bad_serial)) == -1);
   free(forged_csr);
   free(csr);

   /* Revocation: not revoked, then revoked, reflected in the snapshot. */
   assert(pki_is_revoked(serial) == 0);
   assert(pki_revoke(serial) == 0);
   assert(pki_is_revoked(serial) == 1);

   /* A second cert is independent (distinct serial, still valid). */
   char cert2[8192] = "", key2[4096] = "", serial2[64] = "";
   assert(pki_issue("ci-runner-2", 30, cert2, sizeof(cert2), key2, sizeof(key2), serial2,
                    sizeof(serial2)) == 0);
   assert(strcmp(serial, serial2) != 0);
   assert(pki_is_revoked(serial2) == 0);

   /* A spoofing CN (would alias uid:0) is refused at issuance. */
   char tmp[8192], tkey[4096], tserial[64];
   assert(pki_issue("uid:0", 30, tmp, sizeof(tmp), tkey, sizeof(tkey), tserial, sizeof(tserial)) ==
          -1);

   /* Revoke serial2, then drop all caches and reload: the CA survives (sealed key
    * + cert file) and the revocation snapshot reloads from DB1. */
   assert(pki_revoke(serial2) == 0);
   pki_reset_for_test();
   assert(pki_ca_ensure() == 0);
   assert(pki_is_revoked(serial2) == 1); /* persisted across reload */

   /* === P8a: per-request DURABLE revocation/expiry re-check (invariant #5). The
    * durable read (pki_cert_check) is the per-request authority; the g_revoked
    * snapshot (pki_is_revoked) is the fast handshake-path check. === */
   long p8_now = (long)time(NULL);

   /* (a) A freshly issued cert is durably VALID. */
   char pc[8192] = "", pk[4096] = "", ps[64] = "";
   assert(pki_issue("p8a-fresh", 30, pc, sizeof(pc), pk, sizeof(pk), ps, sizeof(ps)) == 0);
   assert(pki_cert_check(ps, p8_now) == PKI_CERT_VALID);

   /* (b) After pki_revoke -> durably REVOKED. */
   assert(pki_revoke(ps) == 0);
   assert(pki_cert_check(ps, p8_now) == PKI_CERT_REVOKED);

   /* (d) A serial this server's CA never issued -> UNKNOWN (fail-closed at the
    * caller), and DISTINCT from ERROR (an unavailable authority). */
   assert(pki_cert_check("deadbeef-not-issued", p8_now) == PKI_CERT_UNKNOWN);
   assert(strcmp(pki_cert_status_str(PKI_CERT_VALID), "VALID") == 0);
   assert(strcmp(pki_cert_status_str(PKI_CERT_REVOKED), "REVOKED") == 0);
   assert(strcmp(pki_cert_status_str(PKI_CERT_EXPIRED), "EXPIRED") == 0);
   assert(strcmp(pki_cert_status_str(PKI_CERT_UNKNOWN), "UNKNOWN") == 0);
   assert(strcmp(pki_cert_status_str(PKI_CERT_ERROR), "ERROR") == 0);

   /* (c)+(f) Expiry boundary: expires_at>0 AND expires_at<=now -> EXPIRED. Issue a
    * fresh (VALID) cert, then rewrite expires_at durably to walk the boundary. */
   char ec[8192] = "", ek[4096] = "", es[64] = "";
   assert(pki_issue("p8a-expiring", 30, ec, sizeof(ec), ek, sizeof(ek), es, sizeof(es)) == 0);
   assert(pki_cert_check(es, p8_now) == PKI_CERT_VALID);
   assert(pki_test_set_expires(es, p8_now - 1) == 0); /* (c) past */
   assert(pki_cert_check(es, p8_now) == PKI_CERT_EXPIRED);
   assert(pki_test_set_expires(es, p8_now) == 0); /* (f) boundary: == now is EXPIRED */
   assert(pki_cert_check(es, p8_now) == PKI_CERT_EXPIRED);
   assert(pki_test_set_expires(es, p8_now + 1) == 0); /* strictly future -> VALID */
   assert(pki_cert_check(es, p8_now) == PKI_CERT_VALID);
   assert(pki_test_set_expires(es, 0) == 0); /* unset (0) is NOT treated as expired */
   assert(pki_cert_check(es, p8_now) == PKI_CERT_VALID);

   /* (e) THE LOAD-BEARING PROPERTY: the durable read beats a STALE snapshot. Issue
    * a fresh cert (so the snapshot has no entry for it), then revoke the row
    * DIRECTLY in the DB WITHOUT snapshot_add. pki_is_revoked reads the snapshot and
    * misses it (0); pki_cert_check re-reads the durable row and catches it
    * (REVOKED) — proving a cert revoked AFTER its handshake stops authorizing on
    * the very next request even though the in-memory snapshot never learned of it. */
   char sc[8192] = "", sk[4096] = "", ss[64] = "";
   assert(pki_issue("p8a-stale", 30, sc, sizeof(sc), sk, sizeof(sk), ss, sizeof(ss)) == 0);
   assert(pki_is_revoked(ss) == 0);
   assert(pki_cert_check(ss, p8_now) == PKI_CERT_VALID);
   assert(pki_test_revoke_db_only(ss) == 0);               /* durable revoke; snapshot untouched */
   assert(pki_is_revoked(ss) == 0);                        /* snapshot is STALE — misses it */
   assert(pki_cert_check(ss, p8_now) == PKI_CERT_REVOKED); /* durable read is authoritative */
   printf("pki: P8a durable per-request revocation/expiry re-check ok "
          "(durable beats stale snapshot)\n");

   /* Server-cert SAN builder: base set always present; AIMEE_TLS_EXTRA_SAN entries
    * appended with auto-typing (IP vs DNS) and pass-through of pre-typed entries. */
   char san[1024];
   pki_build_server_san("myhost", NULL, san, sizeof(san));
   assert(strstr(san, "DNS:myhost") && strstr(san, "DNS:localhost") &&
          strstr(san, "IP:127.0.0.1") && !strstr(san, "192")); /* no extras when NULL */
   pki_build_server_san("h", "192.168.1.254", san, sizeof(san));
   assert(strstr(san, ",IP:192.168.1.254") && !strstr(san, "IP:IP:")); /* bare IPv4 -> IP: */
   pki_build_server_san("h", "nas.local", san, sizeof(san));
   assert(strstr(san, ",DNS:nas.local")); /* bare hostname -> DNS: */
   pki_build_server_san("h", "IP:10.0.0.5", san, sizeof(san));
   assert(strstr(san, ",IP:10.0.0.5") && !strstr(san, "IP:IP:")); /* pre-typed kept as-is */
   pki_build_server_san("h", "192.168.1.254, nas.local", san, sizeof(san));
   assert(strstr(san, ",IP:192.168.1.254") && strstr(san, ",DNS:nas.local")); /* multi */
   pki_build_server_san("h", "fd00::1", san, sizeof(san));
   assert(strstr(san, ",IP:fd00::1")); /* IPv6 -> IP: */
   /* A leading CN that is an IP literal must be typed IP:, never DNS:<ip>. */
   pki_build_server_san("192.168.1.254", NULL, san, sizeof(san));
   assert(strstr(san, "IP:192.168.1.254") && !strstr(san, "DNS:192.168.1.254"));
   pki_build_server_san("smoothnas", NULL, san, sizeof(san));
   assert(strstr(san, "DNS:smoothnas")); /* hostname CN stays DNS: */
   printf("pki: SAN builder ok\n");

   /* Stable-CN resolution: the operator identity must win over the OS hostname so a
    * container recreate (fresh per-container gethostname()) cannot rotate the cert
    * and break TOFU-pinned clients. */
   char cn[256];
   setenv("AIMEE_TLS_CN", "explicit.example", 1);
   setenv("AIMEE_TLS_EXTRA_SAN", "192.168.1.254,smoothnas", 1);
   pki_resolve_server_cn(cn, sizeof(cn));
   assert(strcmp(cn, "explicit.example") == 0); /* 1. AIMEE_TLS_CN wins */
   unsetenv("AIMEE_TLS_CN");
   pki_resolve_server_cn(cn, sizeof(cn));
   assert(strcmp(cn, "smoothnas") == 0); /* 2. first NON-IP token (skips the IP) */
   setenv("AIMEE_TLS_EXTRA_SAN", "IP:192.168.1.254,DNS:nas.local", 1);
   pki_resolve_server_cn(cn, sizeof(cn));
   assert(strcmp(cn, "nas.local") == 0); /* pre-typed prefixes stripped */
   setenv("AIMEE_TLS_EXTRA_SAN", "192.168.1.254, 10.0.0.5", 1);
   pki_resolve_server_cn(cn, sizeof(cn));
   assert(strcmp(cn, "192.168.1.254") == 0); /* 3. all-IP list -> first IP */
   unsetenv("AIMEE_TLS_EXTRA_SAN");
   pki_resolve_server_cn(cn, sizeof(cn));
   assert(cn[0] != '\0'); /* 4. no operator identity -> gethostname() fallback */
   printf("pki: stable CN resolution ok\n");

   /* End-to-end: the public cert is durable while its private key exists only in
    * Vault. Existing plaintext key files are one-way migration inputs and are
    * securely removed after their key pair is verified. */
   char crtp[512], keyp[512];
   snprintf(crtp, sizeof(crtp), "%s/tls/server.crt", home);
   snprintf(keyp, sizeof(keyp), "%s/tls/server.key", home);
   unsetenv("AIMEE_TLS_CN");
   setenv("AIMEE_TLS_EXTRA_SAN", "192.168.1.254,smoothnas", 1);
   /* An orphaned legacy key has no certificate to authenticate it. It must be
    * erased before a new Vault identity is created, never carried forward. */
   FILE *orphan_key = fopen(keyp, "wb");
   assert(orphan_key);
   assert(fputs("orphaned-secret", orphan_key) >= 0 && fclose(orphan_key) == 0);
   assert(pki_ensure_self_signed_server_cert(crtp, keyp) == 0);
   assert(access(keyp, F_OK) != 0); /* fresh installs never emit a key file */
   char vault_server_key[4000];
   assert(pki_server_tls_key_load(vault_server_key, sizeof(vault_server_key)) == 0);
   assert(strstr(vault_server_key, "PRIVATE KEY") != NULL);
   char c1[8192];
   long n1 = slurp(crtp, c1, sizeof(c1));
   assert(n1 > 0);
   assert(pki_ensure_self_signed_server_cert(crtp, keyp) == 0); /* second boot, same identity */
   char c2[8192];
   long n2 = slurp(crtp, c2, sizeof(c2));
   assert(n1 == n2 && memcmp(c1, c2, (size_t)n1) == 0); /* kept verbatim */
   /* Simulate an old image reintroducing the same matching key file: the next
    * boot verifies it against both Vault and the cert, then erases it. */
   FILE *legacy_key = fopen(keyp, "wb");
   assert(legacy_key);
   assert(fputs(vault_server_key, legacy_key) >= 0 && fclose(legacy_key) == 0);
   assert(pki_ensure_self_signed_server_cert(crtp, keyp) == 0);
   assert(access(keyp, F_OK) != 0);
   OPENSSL_cleanse(vault_server_key, sizeof(vault_server_key));
   /* An identity change — as a container recreate or an operator SAN edit would
    * present — must NOT rotate an existing cert. This is the core guarantee. */
   setenv("AIMEE_TLS_EXTRA_SAN", "10.0.0.9,othername", 1);
   setenv("AIMEE_TLS_CN", "some-other-host", 1);
   assert(pki_ensure_self_signed_server_cert(crtp, keyp) == 0);
   char c3[8192];
   long n3 = slurp(crtp, c3, sizeof(c3));
   assert(n1 == n3 && memcmp(c1, c3, (size_t)n1) == 0); /* STILL kept: never recreated */
   /* Only an absent public cert triggers explicit identity rotation. */
   assert(unlink(crtp) == 0);
   assert(pki_ensure_self_signed_server_cert(crtp, keyp) == 0);
   char c4[8192];
   long n4 = slurp(crtp, c4, sizeof(c4));
   assert(n4 > 0 && !(n1 == n4 && memcmp(c1, c4, (size_t)n1) == 0)); /* minted when absent */
   unsetenv("AIMEE_TLS_EXTRA_SAN");
   unsetenv("AIMEE_TLS_CN");
   assert(access(keyp, F_OK) != 0);
   printf("pki: server TLS private key is Vault-only; legacy key migration + rotation ok\n");

   /* P8c durable optional->required ramp. Isolate the authoritative roster by
    * retiring all earlier test certs before initializing the singleton. */
   sqlite3 *rdb = db1_conn();
   assert(rdb);
   assert(sqlite3_exec(rdb, "UPDATE pki_certs SET revoked=1", NULL, NULL, NULL) == SQLITE_OK);
   assert(pki_mtls_ramp_init(1) == 1);
   int ramp_state = 0;
   char roster_hash[65] = "";
   long advanced_at = -1;
   assert(pki_mtls_ramp_get(&ramp_state, roster_hash, sizeof(roster_hash), &advanced_at) == 0);
   assert(ramp_state == 1 && strlen(roster_hash) == 64 && advanced_at == 0);
   assert(pki_mtls_ramp_ready(p8_now) == 0); /* empty rosters never advance */

   char r1c[8192] = "", r1k[4096] = "", r1s[64] = "";
   char r2c[8192] = "", r2k[4096] = "", r2s[64] = "";
   assert(pki_issue("ramp-client-1", 30, r1c, sizeof(r1c), r1k, sizeof(r1k), r1s, sizeof(r1s)) ==
          0);
   assert(pki_issue("ramp-client-2", 30, r2c, sizeof(r2c), r2k, sizeof(r2k), r2s, sizeof(r2s)) ==
          0);
   long ramp_now = (long)time(NULL);
   assert(pki_mtls_ramp_ready(ramp_now) == 0);
   assert(pki_mtls_note_presentation(r1s, ramp_now) == 0);
   assert(pki_mtls_ramp_ready(ramp_now) == 0); /* partial roster holds */
   assert(pki_mtls_note_presentation(r2s, ramp_now) == 0);
   assert(pki_mtls_ramp_ready(ramp_now) == 1);

   /* A new enrollment after preflight changes the transactional roster hash and
    * invalidates the stale readiness result until that client presents. */
   char r3c[8192] = "", r3k[4096] = "", r3s[64] = "";
   assert(pki_issue("ramp-client-3", 30, r3c, sizeof(r3c), r3k, sizeof(r3k), r3s, sizeof(r3s)) ==
          0);
   ramp_now = (long)time(NULL);
   assert(pki_mtls_ramp_ready(ramp_now) == 0);
   assert(pki_mtls_note_presentation(r3s, ramp_now) == 0);
   assert(pki_mtls_ramp_ready(ramp_now) == 1);
   assert(pki_mtls_ramp_advance(ramp_now) == 1);
   assert(pki_mtls_ramp_advance(ramp_now + 1) == 0); /* idempotent + monotonic */
   assert(pki_mtls_ramp_get(&ramp_state, roster_hash, sizeof(roster_hash), &advanced_at) == 0);
   assert(ramp_state == 2 && advanced_at == ramp_now);
   assert(pki_mtls_ramp_init(1) == 2); /* restart preserves required posture */
   printf("pki: P8c durable mTLS ramp + stale-roster race gate ok\n");

   /* (g) Authority-down path: with DB1 shut down, db1_conn() is NULL, so the
    * durable read cannot answer -> PKI_CERT_ERROR (fail-closed, NOT a misleading
    * UNKNOWN). Done last, as it tears down the shared :memory: DB. The caller
    * (handle_conn) refuses 403 on ERROR just as on REVOKED/EXPIRED/UNKNOWN. */
   db1_shutdown();
   assert(pki_cert_check("any-serial", (long)time(NULL)) == PKI_CERT_ERROR);
   assert(pki_mtls_ramp_init(1) == -1); /* never fall back to optional on DB failure */
   printf("pki: P8a authority-down -> ERROR (fail-closed) ok\n");

   /* (h) Identity-separation comparison semantics.
    *
    * server_tls.c proves the listener identity is distinct from the KB client
    * identity via EVP_PKEY_eq. Its return values are NOT a boolean:
    *   1 = same key, 0 = different key, -1 = different key TYPES, -2 = unsupported.
    * A test of `== 0` therefore folds -1 ("provably different algorithms") into
    * "collides". That is the exact field failure this guards: the server issues
    * an EC identity (gen_ec_key) while a managed KB enrollment issues RSA, so
    * the pair is maximally separated -- and was rejected as a reused key, which
    * disabled the TLS listener permanently with no configuration-level fix.
    *
    * Pin the three cases the separation gate depends on. */
   {
      EVP_PKEY *ec1 = NULL, *ec2 = NULL, *rsa = NULL;
      EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
      assert(c && EVP_PKEY_keygen_init(c) == 1 &&
             EVP_PKEY_CTX_set_ec_paramgen_curve_nid(c, NID_X9_62_prime256v1) == 1 &&
             EVP_PKEY_keygen(c, &ec1) == 1);
      EVP_PKEY_CTX_free(c);
      c = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
      assert(c && EVP_PKEY_keygen_init(c) == 1 &&
             EVP_PKEY_CTX_set_ec_paramgen_curve_nid(c, NID_X9_62_prime256v1) == 1 &&
             EVP_PKEY_keygen(c, &ec2) == 1);
      EVP_PKEY_CTX_free(c);
      c = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
      assert(c && EVP_PKEY_keygen_init(c) == 1 && EVP_PKEY_CTX_set_rsa_keygen_bits(c, 2048) == 1 &&
             EVP_PKEY_keygen(c, &rsa) == 1);
      EVP_PKEY_CTX_free(c);

      assert(EVP_PKEY_eq(ec1, ec1) == 1);  /* same key -> collision */
      assert(EVP_PKEY_eq(ec1, ec2) == 0);  /* different EC keys -> distinct */
      assert(EVP_PKEY_eq(ec1, rsa) == -1); /* EC vs RSA -> distinct, and NOT 0 */

      /* The mapping server_tls.c must implement: distinct unless provably equal,
       * with -2/unknown staying fail-closed. */
      assert((EVP_PKEY_eq(ec1, ec1) == 0 || EVP_PKEY_eq(ec1, ec1) == -1) == 0);
      assert((EVP_PKEY_eq(ec1, ec2) == 0 || EVP_PKEY_eq(ec1, ec2) == -1) == 1);
      assert((EVP_PKEY_eq(ec1, rsa) == 0 || EVP_PKEY_eq(ec1, rsa) == -1) == 1);

      EVP_PKEY_free(ec1);
      EVP_PKEY_free(ec2);
      EVP_PKEY_free(rsa);
      printf("pki: identity-separation key comparison semantics ok\n");
   }

   snprintf(cmd, sizeof(cmd), "rm -rf %s", home);
   (void)system(cmd);
   printf("pki: all tests passed\n");
   return 0;
}
