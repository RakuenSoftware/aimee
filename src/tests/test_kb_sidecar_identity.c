/* test_kb_sidecar_identity.c: the mTLS identities the kb issues for its sidecars.
 *
 * This code writes private keys onto a volume two containers share, and each sidecar
 * refuses to start without what it produces. Four properties therefore matter more
 * than the happy path:
 *
 *   - keys are never world- or group-readable, not even briefly;
 *   - it is idempotent, because reissuing on a kb restart would hand the sidecar a
 *     certificate its running peer does not know about and break the hop until both
 *     containers restarted;
 *   - both halves of one hop verify against the SAME CA, which is the entire point;
 *   - the hops are INDEPENDENT of each other. An install may run a bundled embedder
 *     against an external synthesis provider, or the reverse. Provisioning one must
 *     not provision, or depend on, the other, and the two client certificates must
 *     carry different subjects, because that subject is how a sidecar's logs name who
 *     connected.
 *
 * The first three run against both hops rather than against synthesis alone: they are
 * properties of the issuer, and testing them through one caller would leave the
 * other's arguments unexercised. The embedder hop is the one where a wrong directory
 * would go unnoticed, because the kb treats a failure to provision as non-fatal.
 */
#include "kb_sidecar_identity.h"
#include "kb_pki.h"
#include <assert.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static int failures = 0;

static void check(int ok, const char *what)
{
   if (ok)
   {
      printf("  ok    %s\n", what);
      return;
   }
   printf("  FAIL  %s\n", what);
   failures++;
}

/* Returns a heap copy: the trust-root check needs two PEMs live at once, and a
 * shared static buffer would have the second read clobber the first. */
static char *slurp(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   char *buf = malloc(65536);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t n = fread(buf, 1, 65535, f);
   fclose(f);
   buf[n] = '\0';
   return buf;
}

static int mode_of(const char *path)
{
   struct stat st;
   if (stat(path, &st) != 0)
      return -1;
   return (int)(st.st_mode & 07777);
}

static int exists(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0;
}

static int is_pem(const char *path, const char *marker)
{
   char *s = slurp(path);
   int ok = s && strstr(s, marker) != NULL;
   free(s);
   return ok;
}

/* The subject is what distinguishes the two hops on the wire, so the test reads it
 * rather than trusting that a distinct string was passed in. kb_pki exposes the issuer
 * and the serial but not the subject, hence going to OpenSSL directly here. */
static int subject_contains(const char *cert_path, const char *needle)
{
   char *pem = slurp(cert_path);
   if (!pem)
      return 0;
   BIO *bio = BIO_new_mem_buf(pem, -1);
   X509 *x = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
   int found = 0;
   if (x)
   {
      char subject[512] = {0};
      if (X509_NAME_oneline(X509_get_subject_name(x), subject, sizeof(subject)))
         found = strstr(subject, needle) != NULL;
      X509_free(x);
   }
   if (bio)
      BIO_free(bio);
   free(pem);
   return found;
}

typedef struct
{
   const char *label;     /* what to call this hop in the output */
   const char *subdir;    /* where the issuer is expected to write */
   const char *host;      /* the sidecar's DNS name, the server cert's CN */
   const char *client_cn; /* the kb's subject on this hop */
   int (*ensure)(const char *data_dir, const char *sidecar_host);
} hop_t;

/* Every property that should hold for any hop the kb issues for. Run per hop, in its
 * own data dir, so one hop's state cannot satisfy another's assertions. */
static void check_hop(const hop_t *hop)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee-sidecar-idXXXXXX", platform_tmpdir());
   const char *dir = mkdtemp(tmpl);
   assert(dir && "need a scratch dir");

   char sdir[512], ca[640], scert[640], skey[640], ccert[640], ckey[640];
   snprintf(sdir, sizeof(sdir), "%s/%s", dir, hop->subdir);
   snprintf(ca, sizeof(ca), "%s/ca.pem", sdir);
   snprintf(scert, sizeof(scert), "%s/server.pem", sdir);
   snprintf(skey, sizeof(skey), "%s/server.key", sdir);
   snprintf(ccert, sizeof(ccert), "%s/client.pem", sdir);
   snprintf(ckey, sizeof(ckey), "%s/client.key", sdir);

   printf("%s: issuing\n", hop->label);
   check(hop->ensure(dir, hop->host) == 0, "ensure() succeeds");
   check(is_pem(ca, "BEGIN CERTIFICATE"), "ca.pem is a certificate");
   check(is_pem(scert, "BEGIN CERTIFICATE"), "server.pem is a certificate");
   check(is_pem(ccert, "BEGIN CERTIFICATE"), "client.pem is a certificate");
   check(is_pem(skey, "PRIVATE KEY"), "server.key is a private key");
   check(is_pem(ckey, "PRIVATE KEY"), "client.key is a private key");

   printf("%s: names on the wire\n", hop->label);
   /* The sidecar entrypoint reads this exact subdirectory, and the kb treats a failure
    * to provision as non-fatal, so a wrong path here surfaces as a sidecar that
    * refuses to start rather than as anything pointing back at this code. */
   check(subject_contains(scert, hop->host), "server cert CN is the sidecar host");
   check(subject_contains(ccert, hop->client_cn), "client cert names this hop");

   printf("%s: key permissions\n", hop->label);
   /* The sidecar reads these off a shared volume. Group or other read on a private
    * key is the whole exposure, and chmod-after-create leaves a window. */
   check(mode_of(skey) == 0600, "server.key is 0600");
   check(mode_of(ckey) == 0600, "client.key is 0600");
   check((mode_of(sdir) & 077) == 0, "the identity dir is not group/world accessible");

   printf("%s: idempotence\n", hop->label);
   char *server_before = slurp(scert);
   char *client_before = slurp(ccert);
   assert(server_before && client_before);
   check(hop->ensure(dir, hop->host) == 0, "second ensure() succeeds");
   char *server_after = slurp(scert);
   char *client_after = slurp(ccert);
   check(server_after && strcmp(server_before, server_after) == 0, "server cert is NOT reissued");
   check(client_after && strcmp(client_before, client_after) == 0, "client cert is NOT reissued");
   free(server_after);
   free(client_after);
   free(server_before);
   free(client_before);

   printf("%s: both halves share one trust root\n", hop->label);
   /* If these did not verify against the same CA the hop could never complete, and
    * the symptom would be a handshake failure at first use rather than here. */
   /* kb_pki_verify_client_cert takes PEM CONTENTS, not paths. Passing paths made
    * this fail against material that was in fact correctly signed. */
   char *ca_pem = slurp(ca);
   char *client_pem = slurp(ccert);
   char *server_pem = slurp(scert);
   check(ca_pem && client_pem && kb_pki_verify_client_cert(ca_pem, client_pem) == 1,
         "the kb client cert chains to ca.pem");
   check(ca_pem && server_pem && kb_pki_verify_client_cert(ca_pem, server_pem) == 1,
         "the sidecar server cert chains to the same ca.pem");
   free(ca_pem);
   free(client_pem);
   free(server_pem);

   printf("%s: rejects nonsense\n", hop->label);
   check(hop->ensure(NULL, hop->host) == -1, "NULL data dir is refused");
   check(hop->ensure(dir, "") == -1, "empty sidecar host is refused");
}

/* The property that makes these two hops two hops. An install can deploy either
 * without the other, so issuing for one must neither create nor require the other's
 * material, and the subjects must differ so a sidecar can say who called it. */
static void check_independence(void)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee-sidecar-indepXXXXXX", platform_tmpdir());
   const char *dir = mkdtemp(tmpl);
   assert(dir && "need a scratch dir");

   char sdir[512], edir[512];
   snprintf(sdir, sizeof(sdir), "%s/synthesis-tls", dir);
   snprintf(edir, sizeof(edir), "%s/embedder-tls", dir);

   printf("the hops are independent\n");
   check(kb_embedder_identity_ensure(dir, "aimee-embedder-a25m") == 0, "embedder alone succeeds");
   check(exists(edir), "embedder-tls exists");
   check(!exists(sdir), "synthesis-tls was NOT created");

   check(kb_synthesis_identity_ensure(dir, "aimee-llm") == 0, "synthesis then succeeds");
   check(exists(sdir), "synthesis-tls now exists");

   /* Both come from the kb's single CA, one trust root to rotate as designed, but with
    * distinct client subjects, so revoking one hop's client does not revoke the
    * other's and each sidecar's logs name its actual caller. */
   char sca[640], eca[640], sccert[640], eccert[640];
   snprintf(sca, sizeof(sca), "%s/ca.pem", sdir);
   snprintf(eca, sizeof(eca), "%s/ca.pem", edir);
   snprintf(sccert, sizeof(sccert), "%s/client.pem", sdir);
   snprintf(eccert, sizeof(eccert), "%s/client.pem", edir);

   char *s_ca = slurp(sca);
   char *e_ca = slurp(eca);
   check(s_ca && e_ca && strcmp(s_ca, e_ca) == 0, "both hops trust the same single kb CA");
   free(s_ca);
   free(e_ca);

   char *s_client = slurp(sccert);
   char *e_client = slurp(eccert);
   check(s_client && e_client && strcmp(s_client, e_client) != 0,
         "the two client certs are different certificates");
   free(s_client);
   free(e_client);
   check(subject_contains(sccert, "aimee-kb-synthesis"), "synthesis client subject is its own");
   check(subject_contains(eccert, "aimee-kb-embedder"), "embedder client subject is its own");
   check(!subject_contains(eccert, "synthesis"), "the embedder does not present as synthesis");
}

/* The generic entry point rejects what the named wrappers cannot get wrong. */
static void check_generic_guards(void)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee-sidecar-genXXXXXX", platform_tmpdir());
   const char *dir = mkdtemp(tmpl);
   assert(dir && "need a scratch dir");

   printf("the generic issuer guards its own arguments\n");
   check(kb_sidecar_identity_ensure(dir, "", "aimee-llm", "cn") == -1, "empty subdir is refused");
   check(kb_sidecar_identity_ensure(dir, "x-tls", "aimee-llm", "") == -1,
         "empty client CN is refused");
   check(kb_sidecar_identity_ensure(dir, NULL, "aimee-llm", "cn") == -1, "NULL subdir is refused");
   check(kb_sidecar_identity_ensure(dir, "x-tls", "aimee-llm", NULL) == -1,
         "NULL client CN is refused");
}

int main(void)
{
   static const hop_t hops[] = {
       {"synthesis", "synthesis-tls", "aimee-llm", "aimee-kb-synthesis",
        kb_synthesis_identity_ensure},
       {"embedder", "embedder-tls", "aimee-embedder-a25m", "aimee-kb-embedder",
        kb_embedder_identity_ensure},
   };

   for (size_t i = 0; i < sizeof(hops) / sizeof(hops[0]); i++)
      check_hop(&hops[i]);
   check_independence();
   check_generic_guards();

   if (failures)
   {
      printf("\nkb_sidecar_identity: %d check(s) failed\n", failures);
      return 1;
   }
   printf("\nkb_sidecar_identity: ok\n");
   return 0;
}
