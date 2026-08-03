/* kb_sidecar_identity.c: issue the mTLS identities for the kb's sidecar hops.
 *
 * Every kb -> sidecar hop is mTLS, like every other container hop here. The kb is
 * the CLIENT on each, and the kb already owns the CA (kb_pki_ca_load_or_create_
 * custodied, the same one kb_mtls_start uses), so the kb is what must issue both
 * halves:
 *
 *   server.pem / server.key  the sidecar presents these; CN is its DNS name
 *   client.pem / client.key  the kb presents these when calling synthesis
 *   ca.pem                   what each side verifies the other against
 *
 * WHY AT KB STARTUP rather than on demand. The deployment order is server, wizard,
 * kb, then whichever sidecars were selected. Issuing here means the material
 * exists before anything could ask for it, with no new route, no bootstrap
 * one-shot, and no ordering to coordinate: the sidecar cannot start earlier than
 * the kb that deploys it. The sidecar treats a missing identity as a hard error
 * precisely because this guarantees it is present.
 *
 * Idempotent. Re-issuing on every boot would hand the sidecar a new certificate
 * while it holds the old one, and a restart of one container would break the hop
 * until the other restarted too.
 */
#include "kb_sidecar_identity.h"
#include "kb_pki.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A year, matching the kb's own server certificate. Long enough that a deployment
 * is not woken by expiry, short enough that a leaked key is not permanent. */
#define SIDECAR_CERT_VALID_SECS (60L * 60 * 24 * 365)

static int write_private(const char *path, const char *pem)
{
   /* 0600 before any bytes land: creating world-readable and chmod-ing after leaves
    * a window in which the key is readable, and on a shared volume that window is
    * the whole point of an attacker being on the box. */
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
   {
      LOG_WARN("kb_sidecar_identity", "cannot create %s: %s", path, strerror(errno));
      return -1;
   }
   size_t len = strlen(pem);
   ssize_t written = write(fd, pem, len);
   int rc = (written == (ssize_t)len) ? 0 : -1;
   if (rc != 0)
      LOG_WARN("kb_sidecar_identity", "short write to %s", path);
   if (close(fd) != 0)
      rc = -1;
   return rc;
}

static int write_public(const char *path, const char *pem)
{
   FILE *f = fopen(path, "w");
   if (!f)
   {
      LOG_WARN("kb_sidecar_identity", "cannot create %s: %s", path, strerror(errno));
      return -1;
   }
   int rc = (fputs(pem, f) >= 0) ? 0 : -1;
   if (fclose(f) != 0)
      rc = -1;
   return rc;
}

static int exists(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0 && st.st_size > 0;
}

int kb_sidecar_identity_ensure(const char *data_dir, const char *subdir, const char *sidecar_host,
                               const char *client_cn)
{
   if (!data_dir || !data_dir[0] || !sidecar_host || !sidecar_host[0])
      return -1;
   if (!subdir || !subdir[0] || !client_cn || !client_cn[0])
      return -1;

   char dir[1024];
   if (snprintf(dir, sizeof(dir), "%s/%s", data_dir, subdir) >= (int)sizeof(dir))
      return -1;
   if (mkdir(dir, 0700) != 0 && errno != EEXIST)
   {
      LOG_WARN("kb_sidecar_identity", "cannot create %s: %s", dir, strerror(errno));
      return -1;
   }

   char ca_path[1152], scert[1152], skey[1152], ccert[1152], ckey[1152];
   snprintf(ca_path, sizeof(ca_path), "%s/ca.pem", dir);
   snprintf(scert, sizeof(scert), "%s/server.pem", dir);
   snprintf(skey, sizeof(skey), "%s/server.key", dir);
   snprintf(ccert, sizeof(ccert), "%s/client.pem", dir);
   snprintf(ckey, sizeof(ckey), "%s/client.key", dir);

   if (exists(ca_path) && exists(scert) && exists(skey) && exists(ccert) && exists(ckey))
      return 0;

   /* The SAME CA the kb's own mTLS listener uses. A second CA here would mean two
    * trust roots to rotate and revoke, for one deployment. */
   char ca_dir[1152];
   if (snprintf(ca_dir, sizeof(ca_dir), "%s/kb-ca", data_dir) >= (int)sizeof(ca_dir))
      return -1;
   kb_pki_ca_t ca;
   if (kb_pki_ca_load_or_create_custodied(ca_dir, &ca, NULL) != 0)
   {
      LOG_WARN("kb_sidecar_identity", "cannot load the kb CA from %s", ca_dir);
      return -1;
   }

   int rc = -1;
   char cert_pem[KB_PKI_CERT_PEM_MAX], key_pem[KB_PKI_KEY_PEM_MAX];

   /* The sidecar's server identity. CN is the name the kb will connect to, because
    * that is what the kb's TLS stack checks the certificate against. */
   if (kb_pki_issue_server_cert(&ca, sidecar_host, SIDECAR_CERT_VALID_SECS, cert_pem,
                                sizeof(cert_pem), key_pem, sizeof(key_pem)) != 0)
   {
      LOG_WARN("kb_sidecar_identity", "cannot issue a server certificate for %s", sidecar_host);
      goto done;
   }
   if (write_public(scert, cert_pem) != 0 || write_private(skey, key_pem) != 0)
      goto done;
   OPENSSL_cleanse(key_pem, sizeof(key_pem));

   /* The kb's own client identity for this hop. Separate from any other client cert
    * the kb holds, and separate PER HOP: the subject says which of the kb's callers
    * this is, and a distinct subject is what lets the sidecar's logs name who
    * connected. */
   if (kb_pki_issue_client_cert(&ca, client_cn, SIDECAR_CERT_VALID_SECS, cert_pem, sizeof(cert_pem),
                                key_pem, sizeof(key_pem)) != 0)
   {
      LOG_WARN("kb_sidecar_identity", "cannot issue the kb client certificate");
      goto done;
   }
   if (write_public(ccert, cert_pem) != 0 || write_private(ckey, key_pem) != 0)
      goto done;
   OPENSSL_cleanse(key_pem, sizeof(key_pem));

   if (write_public(ca_path, ca.cert_pem) != 0)
      goto done;

   LOG_INFO("kb_sidecar_identity",
            "issued sidecar mTLS identities in %s (server CN=%s, client CN=%s)", dir, sidecar_host,
            client_cn);
   rc = 0;

done:
   OPENSSL_cleanse(&ca, sizeof(ca));
   OPENSSL_cleanse(key_pem, sizeof(key_pem));
   return rc;
}

/* The synthesis hop. The directory name and the client subject are load-bearing
 * deployment contract, not parameters a caller should choose: the sidecar's
 * entrypoint reads this exact path, so it lives here rather than at each call site. */
int kb_synthesis_identity_ensure(const char *data_dir, const char *sidecar_host)
{
   return kb_sidecar_identity_ensure(data_dir, "synthesis-tls", sidecar_host, "aimee-kb-synthesis");
}
