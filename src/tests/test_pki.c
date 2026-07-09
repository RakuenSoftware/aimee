/* test_pki.c: aimee's client-cert CA — CA ensure, issuance (cert chains to the
 * CA), revocation snapshot, and CN-spoofing refusal (mtls-client-identity slice 2). */
#include "pki.h"
#include "vault_principal.h"
#include "config.h" /* config_default_dir */
#include "db1.h"    /* db1_init */
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

   /* End-to-end: an existing cert is AUTHORITATIVE. Once provisioned it is kept
    * verbatim across boots — and, critically, even when AIMEE_TLS_EXTRA_SAN or the
    * hostname later changes. The server never recreates it, so a container recreate
    * (fresh gethostname()) cannot rotate the cert out from under a TOFU-pinned
    * client. A fresh cert is minted ONLY when none exists. */
   char crtp[512], keyp[512];
   snprintf(crtp, sizeof(crtp), "%s/tls/server.crt", home);
   snprintf(keyp, sizeof(keyp), "%s/tls/server.key", home);
   unsetenv("AIMEE_TLS_CN");
   setenv("AIMEE_TLS_EXTRA_SAN", "192.168.1.254,smoothnas", 1);
   assert(pki_ensure_self_signed_server_cert(crtp, keyp) == 0);
   char c1[8192];
   long n1 = slurp(crtp, c1, sizeof(c1));
   assert(n1 > 0);
   assert(pki_ensure_self_signed_server_cert(crtp, keyp) == 0); /* second boot, same identity */
   char c2[8192];
   long n2 = slurp(crtp, c2, sizeof(c2));
   assert(n1 == n2 && memcmp(c1, c2, (size_t)n1) == 0); /* kept verbatim */
   /* An identity change — as a container recreate or an operator SAN edit would
    * present — must NOT rotate an existing cert. This is the core guarantee. */
   setenv("AIMEE_TLS_EXTRA_SAN", "10.0.0.9,othername", 1);
   setenv("AIMEE_TLS_CN", "some-other-host", 1);
   assert(pki_ensure_self_signed_server_cert(crtp, keyp) == 0);
   char c3[8192];
   long n3 = slurp(crtp, c3, sizeof(c3));
   assert(n1 == n3 && memcmp(c1, c3, (size_t)n1) == 0); /* STILL kept: never recreated */
   /* Only an absent cert triggers provisioning: delete it and a fresh one appears. */
   assert(unlink(crtp) == 0 && unlink(keyp) == 0);
   assert(pki_ensure_self_signed_server_cert(crtp, keyp) == 0);
   char c4[8192];
   long n4 = slurp(crtp, c4, sizeof(c4));
   assert(n4 > 0 && !(n1 == n4 && memcmp(c1, c4, (size_t)n1) == 0)); /* minted when absent */
   unsetenv("AIMEE_TLS_EXTRA_SAN");
   unsetenv("AIMEE_TLS_CN");
   printf("pki: existing server cert authoritative (kept across identity change; minted only when "
          "absent) ok\n");

   snprintf(cmd, sizeof(cmd), "rm -rf %s", home);
   (void)system(cmd);
   printf("pki: all tests passed\n");
   return 0;
}
