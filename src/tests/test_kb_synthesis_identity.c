/* test_kb_synthesis_identity.c: the synthesis sidecar's mTLS identities.
 *
 * This code writes private keys onto a volume two containers share, and the sidecar
 * refuses to start without what it produces. Three properties therefore matter more
 * than the happy path:
 *
 *   - keys are never world- or group-readable, not even briefly;
 *   - it is idempotent, because reissuing on a kb restart would hand the sidecar a
 *     certificate its running peer does not know about and break the hop until both
 *     containers restarted;
 *   - both halves verify against the SAME CA, which is the entire point.
 */
#include "kb_synthesis_identity.h"
#include "kb_pki.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static int is_pem(const char *path, const char *marker)
{
   char *s = slurp(path);
   int ok = s && strstr(s, marker) != NULL;
   free(s);
   return ok;
}

int main(void)
{
   char tmpl[] = "/tmp/aimee-synth-idXXXXXX";
   const char *dir = mkdtemp(tmpl);
   assert(dir && "need a scratch dir");

   char sdir[512], ca[512], scert[512], skey[512], ccert[512], ckey[512];
   snprintf(sdir, sizeof(sdir), "%s/synthesis-tls", dir);
   snprintf(ca, sizeof(ca), "%s/ca.pem", sdir);
   snprintf(scert, sizeof(scert), "%s/server.pem", sdir);
   snprintf(skey, sizeof(skey), "%s/server.key", sdir);
   snprintf(ccert, sizeof(ccert), "%s/client.pem", sdir);
   snprintf(ckey, sizeof(ckey), "%s/client.key", sdir);

   printf("issuing\n");
   check(kb_synthesis_identity_ensure(dir, "aimee-llm") == 0, "ensure() succeeds");
   check(is_pem(ca, "BEGIN CERTIFICATE"), "ca.pem is a certificate");
   check(is_pem(scert, "BEGIN CERTIFICATE"), "server.pem is a certificate");
   check(is_pem(ccert, "BEGIN CERTIFICATE"), "client.pem is a certificate");
   check(is_pem(skey, "PRIVATE KEY"), "server.key is a private key");
   check(is_pem(ckey, "PRIVATE KEY"), "client.key is a private key");

   printf("key permissions\n");
   /* The sidecar reads these off a shared volume. Group or other read on a private
    * key is the whole exposure, and chmod-after-create leaves a window. */
   check(mode_of(skey) == 0600, "server.key is 0600");
   check(mode_of(ckey) == 0600, "client.key is 0600");
   check((mode_of(sdir) & 077) == 0, "synthesis-tls is not group/world accessible");

   printf("idempotence\n");
   char *server_before = slurp(scert);
   char *client_before = slurp(ccert);
   assert(server_before && client_before);
   check(kb_synthesis_identity_ensure(dir, "aimee-llm") == 0, "second ensure() succeeds");
   char *server_after = slurp(scert);
   char *client_after = slurp(ccert);
   check(server_after && strcmp(server_before, server_after) == 0, "server cert is NOT reissued");
   check(client_after && strcmp(client_before, client_after) == 0, "client cert is NOT reissued");
   free(server_after);
   free(client_after);

   printf("both halves share one trust root\n");
   /* If these did not verify against the same CA the hop could never complete, and
    * the symptom would be a handshake failure at first synthesis rather than here. */
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

   printf("rejects nonsense\n");
   check(kb_synthesis_identity_ensure(NULL, "aimee-llm") == -1, "NULL data dir is refused");
   check(kb_synthesis_identity_ensure(dir, "") == -1, "empty sidecar host is refused");

   free(server_before);
   free(client_before);

   if (failures)
   {
      printf("\nkb_synthesis_identity: %d check(s) failed\n", failures);
      return 1;
   }
   printf("\nkb_synthesis_identity: ok\n");
   return 0;
}
