/* cli_remote.c: `aimee remote` subcommand + persisted remote-server config.
 *
 * Thin-client only. The remote target lives in <aimee_home>/remote.conf as two
 * lines: the URL, then an optional bearer token. This keeps the thin client
 * free of config.c (which it does not link) while letting users point at a
 * remote aimee-server persistently.
 */
#include "cli_remote.h"
#include "aimee_client.h"
#include "aimee_home.h"
#include "aimee_tls.h"
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#endif
#ifdef _WIN32
#include <direct.h>
#define AIMEE_MKDIR(p) _mkdir(p)
/* _putenv_s(k, "") removes the variable on MSVC. */
#define AIMEE_UNSETENV(k)  _putenv_s((k), "")
#define AIMEE_SETENV(k, v) _putenv_s((k), (v))
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define AIMEE_MKDIR(p)     mkdir((p), 0700)
#define AIMEE_UNSETENV(k)  unsetenv(k)
#define AIMEE_SETENV(k, v) setenv((k), (v), 1)
#endif

/* Suspend AIMEE_TLS_INSECURE for the duration of trust establishment, returning the
 * prior value (caller frees via tls_insecure_restore). Trust pinning must verify the
 * server cert STRICTLY: with the env var set, the trust probe would "succeed"
 * insecurely and skip pinning, leaving the client dependent on AIMEE_TLS_INSECURE
 * forever instead of pinning the cert once. Forcing strict verification here makes a
 * self-signed/private server get pinned (TOFU) regardless of a stray env var, so
 * normal commands then need no flag. Returns NULL when it was unset/empty. */
static char *tls_insecure_suspend(void)
{
   const char *v = getenv("AIMEE_TLS_INSECURE");
   char *saved = (v && *v) ? strdup(v) : NULL;
   AIMEE_UNSETENV("AIMEE_TLS_INSECURE");
   return saved;
}

static void tls_insecure_restore(char *saved)
{
   if (saved)
   {
      AIMEE_SETENV("AIMEE_TLS_INSECURE", saved);
      free(saved);
   }
}

static void remote_conf_path(char *out, size_t out_sz)
{
   snprintf(out, out_sz, "%s/remote.conf", aimee_home());
}

/* remote.conf contains the bearer token, so never let the caller's umask make
 * it group/world-readable.  Opening without O_TRUNC lets us repair the mode of
 * an existing file before replacing its contents. */
static FILE *open_remote_conf_write(const char *path)
{
#ifdef _WIN32
   return fopen(path, "w");
#else
   int flags = O_WRONLY | O_CREAT;
#ifdef O_CLOEXEC
   flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
   flags |= O_NOFOLLOW;
#endif
   int fd = open(path, flags, S_IRUSR | S_IWUSR);
   if (fd < 0)
      return NULL;
   if (fchmod(fd, S_IRUSR | S_IWUSR) != 0 || ftruncate(fd, 0) != 0)
   {
      close(fd);
      return NULL;
   }
   FILE *f = fdopen(fd, "w");
   if (!f)
      close(fd);
   return f;
#endif
}

static int write_remote_conf(const char *path, const char *url, const char *token)
{
   FILE *f = open_remote_conf_write(path);
   if (!f)
      return -1;
   int failed = fprintf(f, "%s\n%s\n", url, token ? token : "") < 0;
   if (fclose(f) != 0)
      failed = 1;
   return failed ? -1 : 0;
}

/* Create |dir| and any missing parents (best effort, ignores existing/errors).
 * A thin-client-only host (the README's recommended deployment) has nothing
 * else that creates <aimee_home> (~/.config/aimee), so `aimee remote set` must
 * make it before writing remote.conf or the first call fails with ENOENT. */
static void ensure_dir_p(const char *dir)
{
   if (!dir || !dir[0])
      return;
   char tmp[512];
   snprintf(tmp, sizeof(tmp), "%s", dir);
   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         AIMEE_MKDIR(tmp);
         *p = '/';
      }
   }
   AIMEE_MKDIR(tmp);
}

/* Read the persisted URL (line 1) and token (line 2, optional) into the given
 * buffers. Returns 1 if a non-empty URL was read, else 0. */
static int read_remote_conf(char *url, size_t url_sz, char *token, size_t token_sz)
{
   if (url_sz)
      url[0] = '\0';
   if (token_sz)
      token[0] = '\0';
   char path[512];
   remote_conf_path(path, sizeof(path));
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   if (fgets(url, (int)url_sz, f))
      url[strcspn(url, "\r\n")] = '\0';
   if (fgets(token, (int)token_sz, f))
      token[strcspn(token, "\r\n")] = '\0';
   fclose(f);
   return url[0] != '\0';
}

/* remote_enroll is defined below; forward-declare for the load-time auto-enroll. */
static int remote_enroll(const char *url, const char *current_token, char *out, size_t out_sz,
                         int json_output);
static int remote_enroll_client_cert(int quiet);
static int g_remote_mtls_enrolled;

void cli_remote_load_persisted(void)
{
   /* A --server flag or AIMEE_SERVER_URL already wins; don't override it. */
   if (aimee_client_remote_active(NULL, 0))
      return;
   char url[512], token[256];
   if (!read_remote_conf(url, sizeof(url), token, sizeof(token)) || !url[0])
      return;
   aimee_client_set_remote(url, token[0] ? token : NULL);

   /* Certificate enrollment is idempotent. Individual bearer enrollment is an
    * explicit `aimee remote enroll` operation; there is no published bootstrap
    * credential whose presence should trigger a rotation. */
   if (token[0])
      remote_enroll_client_cert(1 /* quiet; idempotent when already installed */);
}

static void remote_ca_path(char *out, size_t out_sz)
{
   snprintf(out, out_sz, "%s/remote-ca.pem", aimee_home());
}

/* Move a refused client identity out of the active path, into
 * <aimee_home>/tls/refused-<UTC stamp>/, and report that directory in |dir_out|.
 * Never deletes: the certificate is the only record of what this client was
 * previously paired to, and an operator reconstructing a re-provisioning needs
 * it. Everything present moves, or the function fails: leaving part of a refused
 * identity behind would keep failing the connection closed on partial material.
 * Portable (no OpenSSL), so Windows can clear the same wedge even where minting a
 * replacement is unavailable. Returns 0 on success. */
static int remote_archive_client_cert(char *dir_out, size_t dir_n)
{
   char tls_dir[600];
   snprintf(tls_dir, sizeof(tls_dir), "%s/tls", aimee_home());
   time_t now = time(NULL);
   struct tm utc;
#ifdef _WIN32
   struct tm *utcp = gmtime(&now); /* MinGW has no gmtime_r; this path is single-threaded */
   if (!utcp)
      return -1;
   utc = *utcp;
#else
   if (!gmtime_r(&now, &utc))
      return -1;
#endif
   char stamp[32];
   if (strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &utc) == 0)
      return -1;
   snprintf(dir_out, dir_n, "%s/refused-%s", tls_dir, stamp);
   if (AIMEE_MKDIR(dir_out) != 0)
      return -1;
   static const char *const names[] = {"client.crt", "client.key"};
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
   {
      char from[1400], to[1400];
      snprintf(from, sizeof(from), "%s/%s", tls_dir, names[i]);
      snprintf(to, sizeof(to), "%s/%s", dir_out, names[i]);
      /* A half-written identity (only one of the pair) is one of the states worth
       * recovering from, so a missing file is success, not failure. Any other
       * rename error is real: leaving part of a refused identity in the active
       * path would keep failing the connection closed. */
      if (rename(from, to) != 0 && errno != ENOENT)
         return -1;
   }
   return 0;
}

#ifdef __linux__
static int write_sync_file(const char *path, const char *data, size_t len, mode_t mode)
{
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
   if (fd < 0)
      return -1;
   size_t off = 0;
   while (off < len)
   {
      ssize_t n = write(fd, data + off, len - off);
      if (n <= 0)
      {
         close(fd);
         unlink(path);
         return -1;
      }
      off += (size_t)n;
   }
   int sync_rc = fsync(fd);
   int close_rc = close(fd);
   int rc = sync_rc == 0 && close_rc == 0 ? 0 : -1;
   if (rc != 0)
      unlink(path);
   return rc;
}

/* Generate the thin-client key locally, submit only its signed CSR, then install
 * the key/cert with owner-only temp files and atomic renames. */
static int remote_enroll_client_cert(int quiet)
{
   int is_https = 0;
   if (!aimee_client_remote_active_scheme(NULL, 0, &is_https) || !is_https)
      return -1;
   char tls_dir[600], key_tmp[700], cert_tmp[700], key_path[700], cert_path[700];
   snprintf(tls_dir, sizeof(tls_dir), "%s/tls", aimee_home());
   ensure_dir_p(tls_dir);
   unsigned char rnd[8];
   if (RAND_bytes(rnd, sizeof(rnd)) != 1)
      return -1;
   char cn[128];
   snprintf(cn, sizeof(cn), "thin-%02x%02x%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3],
            rnd[4], rnd[5], rnd[6], rnd[7]);
   snprintf(key_tmp, sizeof(key_tmp), "%s/.client.key.%ld.tmp", tls_dir, (long)getpid());
   snprintf(cert_tmp, sizeof(cert_tmp), "%s/.client.crt.%ld.tmp", tls_dir, (long)getpid());
   snprintf(key_path, sizeof(key_path), "%s/client.key", tls_dir);
   snprintf(cert_path, sizeof(cert_path), "%s/client.crt", tls_dir);
   char existing_cert[700], existing_key[700];
   int eligible = aimee_tls_client_cert_eligible(aimee_home(), existing_cert, sizeof(existing_cert),
                                                 existing_key, sizeof(existing_key));
   if (eligible == 1)
      return 0;
   struct stat key_st, cert_st;
   int have_key = lstat(key_path, &key_st) == 0;
   int have_cert = lstat(cert_path, &cert_st) == 0;
   if (have_key || have_cert)
      return -1; /* preserve partial evidence; connection path fails closed */

   EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
   EVP_PKEY *key = NULL;
   X509_REQ *req = NULL;
   BIO *kb = NULL, *rb = NULL;
   char *key_pem = NULL, *csr_pem = NULL;
   int rc = -1;
   if (!kctx || EVP_PKEY_keygen_init(kctx) != 1 ||
       EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1) != 1 ||
       EVP_PKEY_keygen(kctx, &key) != 1 || !(req = X509_REQ_new()) ||
       X509_REQ_set_version(req, 0) != 1)
      goto done;
   X509_NAME *name = X509_REQ_get_subject_name(req);
   if (!name ||
       X509_NAME_add_entry_by_NID(name, NID_commonName, MBSTRING_ASC, (const unsigned char *)cn, -1,
                                  -1, 0) != 1 ||
       X509_REQ_set_pubkey(req, key) != 1 || X509_REQ_sign(req, key, EVP_sha256()) <= 0 ||
       !(kb = BIO_new(BIO_s_mem())) || !(rb = BIO_new(BIO_s_mem())) ||
       PEM_write_bio_PrivateKey(kb, key, NULL, NULL, 0, NULL, NULL) != 1 ||
       PEM_write_bio_X509_REQ(rb, req) != 1)
      goto done;
   BUF_MEM *km = NULL, *rm = NULL;
   BIO_get_mem_ptr(kb, &km);
   BIO_get_mem_ptr(rb, &rm);
   if (!km || !rm || !(key_pem = strndup(km->data, km->length)) ||
       !(csr_pem = strndup(rm->data, rm->length)) ||
       write_sync_file(key_tmp, key_pem, km->length, 0600) != 0)
      goto done;

   cJSON *request = cJSON_CreateObject();
   if (!request)
      goto done;
   cJSON_AddStringToObject(request, "cn", cn);
   cJSON_AddStringToObject(request, "csr", csr_pem);
   char *body = cJSON_PrintUnformatted(request);
   cJSON_Delete(request);
   int status = 0;
   if (quiet)
      aimee_client_suppress_conn_errors(1);
   char *response = body ? aimee_client_request("POST", "/v1/cert/sign", body, &status) : NULL;
   if (quiet)
      aimee_client_suppress_conn_errors(0);
   free(body);
   cJSON *root = response ? cJSON_Parse(response) : NULL;
   free(response);
   cJSON *cert = root ? cJSON_GetObjectItemCaseSensitive(root, "cert") : NULL;
   BIO *cert_bio = cert && cJSON_IsString(cert) ? BIO_new_mem_buf(cert->valuestring, -1) : NULL;
   X509 *leaf = cert_bio ? PEM_read_bio_X509(cert_bio, NULL, NULL, NULL) : NULL;
   int key_matches = leaf && X509_check_private_key(leaf, key) == 1;
   X509_free(leaf);
   BIO_free(cert_bio);
   if (status != 200 || !cJSON_IsString(cert) || !cert->valuestring || !key_matches ||
       write_sync_file(cert_tmp, cert->valuestring, strlen(cert->valuestring), 0600) != 0)
   {
      cJSON_Delete(root);
      goto done;
   }
   cJSON_Delete(root);
   if (rename(cert_tmp, cert_path) != 0)
      goto done;
   if (rename(key_tmp, key_path) != 0)
   {
      unlink(cert_path);
      goto done;
   }
   int dirfd = open(tls_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   if (dirfd < 0 || fsync(dirfd) != 0)
   {
      if (dirfd >= 0)
         close(dirfd);
      unlink(key_path);
      unlink(cert_path);
      goto done;
   }
   close(dirfd);
   if (!quiet)
      printf("  mTLS: enrolled individual client certificate %s\n", cn);
   rc = 0;
done:
   if (rc != 0)
   {
      unlink(key_tmp);
      unlink(cert_tmp);
   }
   if (key_pem)
      OPENSSL_cleanse(key_pem, strlen(key_pem));
   free(key_pem);
   free(csr_pem);
   BIO_free(kb);
   BIO_free(rb);
   X509_REQ_free(req);
   EVP_PKEY_free(key);
   EVP_PKEY_CTX_free(kctx);
   return rc;
}

#else
static int remote_enroll_client_cert(int quiet)
{
   (void)quiet;
   return -1;
}

#endif

/* Probe GET /v1/health over the currently-active remote (verifying TLS). Returns
 * 1 when the server answers (a real HTTP status — so the chain + hostname/SAN
 * verified), else 0. Used to detect whether trust is already established. */
static int remote_health_ok(void)
{
   int st = 0;
   char *body = aimee_client_request("GET", "/v1/health", NULL, &st);
   int ok = (body != NULL && st >= 200 && st < 500);
   free(body);
   return ok;
}

/* Is a client identity stored at all? Deliberately does NOT reuse the TLS
 * backend's eligibility gate: that helper is declared in the shared header but
 * defined only by the OpenSSL backend, so calling it here fails to link on macOS
 * (Secure Transport) and Windows (Schannel). Duplicating the gate into each
 * backend would fork a security-relevant check three ways, so ask the simpler
 * portable question instead.
 *
 * PRESENCE, not validity, is what matters: a malformed, partial, or
 * loose-permission identity is precisely what the probe below needs to test.
 * Either file counts — a partial identity fails the connection closed just as a
 * refused one does, and is equally worth recovering. */
static int remote_client_cert_present(void)
{
   static const char *const names[] = {"client.crt", "client.key"};
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
   {
      char p[700];
      snprintf(p, sizeof(p), "%s/tls/%s", aimee_home(), names[i]);
      FILE *f = fopen(p, "rb"); /* fopen, not stat: portable without extra headers */
      if (f)
      {
         fclose(f);
         return 1;
      }
   }
   return 0;
}

/* Re-probe with the stored mTLS identity suppressed, to tell two failures apart
 * that are otherwise identical at the client (both surface as "no HTTP status"):
 *
 *   a) the server is genuinely unreachable, versus
 *   b) the server is fine but REFUSES this client's certificate — the usual cause
 *      being a re-provisioned server whose new client-CA did not issue it.
 *
 * TLS 1.3 makes (b) look like (a): client auth is verified only after the
 * client's handshake completes, so aimee_tls_connect() reports success and the
 * following read dies on the server's alert with no status to report.
 *
 * Returns 1 only when the server answers WITHOUT the identity — which is
 * positive proof of (b). Returns 0 when nothing is stored (so the failure cannot
 * be an identity problem) or when the probe still gets no answer. */
static int remote_probe_without_client_cert(void)
{
#ifdef WITH_TLS
   if (!remote_client_cert_present())
      return 0;
   aimee_tls_suppress_client_cert(1);
   aimee_client_suppress_conn_errors(1);
   int ok = remote_health_ok();
   aimee_client_suppress_conn_errors(0);
   aimee_tls_suppress_client_cert(0); /* scoped to the probe — never left set */
   return ok;
#else
   return 0;
#endif
}

/* Recover from a refused client identity: archive it, then mint a replacement.
 * Only called once remote_probe_without_client_cert() has PROVEN the stored cert
 * is the blocker, so this never discards an identity that was working. Returns 1
 * when the identity is out of the active path (|dir_out| names the archive), 0 if
 * it could not be moved. Re-enrollment is best effort: a server that cannot sign
 * a new cert still leaves a working bearer-only channel, which beats the wedged
 * state the refused cert caused. */
static int remote_recover_client_cert(char *dir_out, size_t dir_n, int json_output)
{
   if (remote_archive_client_cert(dir_out, dir_n) != 0)
      return 0;
   remote_enroll_client_cert(json_output /* quiet under --json */);
   return 1;
}

/* A TLS peer can be reachable and verified while still rejecting the supplied
 * bearer.  `remote set` is the quickstart's pairing command, so accepting a 401
 * here creates a false-success setup that fails on the very next command. */
static int remote_authorized_ok(void)
{
   int st = 0;
   char *body = aimee_client_request("GET", "/v1/health", NULL, &st);
   int ok = (body != NULL && st >= 200 && st < 400);
   free(body);
   return ok;
}

/* Trust-on-first-use pin: fetch |url|'s leaf cert (no verification), write it to
 * <aimee_home>/remote-ca.pem, and report its SHA-256 fingerprint so the operator
 * can confirm it out-of-band. aimee_tls_connect then verifies against it (chain +
 * hostname/SAN still enforced). Returns 0 on success, -1 on failure (unreachable,
 * not https, or pinning unsupported on this platform). */
static int remote_pin_cert(const char *url, int json_output)
{
   char *pem = NULL;
   char fp[200] = "";
   if (aimee_client_fetch_cert(url, &pem, fp, sizeof(fp)) != 0 || !pem)
   {
      free(pem);
      return -1;
   }
   ensure_dir_p(aimee_home());
   char ca[512];
   remote_ca_path(ca, sizeof(ca));
   FILE *f = fopen(ca, "w");
   if (!f)
   {
      free(pem);
      return -1;
   }
   fputs(pem, f);
   fclose(f);
   free(pem);
#ifndef _WIN32
   chmod(ca, 0600);
#endif
   if (!json_output)
      printf("  TLS: pinned server certificate (SHA-256 %s)\n       -> %s\n", fp, ca);
   return 0;
}

/* Enrollment over the already-pinned/verified remote adds an individual client
 * credential without revoking anyone else. Persist the result to remote.conf and
 * adopt it for this process. On failure the prior token stays in place. */
static int remote_enroll(const char *url, const char *current_token, char *out, size_t out_sz,
                         int json_output)
{
   g_remote_mtls_enrolled = 0;
   int st = 0;
   (void)current_token;
   char *body = aimee_client_request("POST", "/v1/api/enroll_bearer", "{}", &st);
   if (body && (st == 404 || st == 405))
   {
      /* Never turn a non-destructive enrollment into a revoke-all operation.
       * Besides violating the command's contract, retrying with rotation after
       * an ambiguous response can revoke everyone even when enrollment already
       * succeeded server-side. */
      if (!json_output)
         fprintf(stderr,
                 "  enroll: this server does not support additive enrollment; upgrade it before\n"
                 "          pairing another client. No existing credential was revoked.\n");
   }
   if (!body || st != 200)
   {
      if (st == 401 && !json_output)
         fprintf(stderr, "  enroll: the current client credential was rejected. Re-pair it with\n"
                         "          `aimee remote set <url> <token>` before enrolling again.\n");
      free(body);
      return -1;
   }
   cJSON *root = cJSON_Parse(body);
   free(body);
   cJSON *tok = root ? cJSON_GetObjectItemCaseSensitive(root, "bearer_token") : NULL;
   if (!cJSON_IsString(tok) || !tok->valuestring || !tok->valuestring[0])
   {
      cJSON_Delete(root);
      return -1;
   }
   snprintf(out, out_sz, "%s", tok->valuestring);
   cJSON_Delete(root);

   /* Persist url + the individual token and adopt it for subsequent requests. */
   char path[512];
   remote_conf_path(path, sizeof(path));
   if (write_remote_conf(path, url, out) != 0)
   {
      fprintf(stderr, "aimee: cannot securely write %s\n", path);
      return -1;
   }
   aimee_client_set_remote(url, out);
   int mtls_enrolled = remote_enroll_client_cert(json_output);
   g_remote_mtls_enrolled = mtls_enrolled == 0;
   if (!json_output)
      printf("  enrolled: added an individual client bearer\n"
             "            (existing paired clients remain valid).\n");
   if (mtls_enrolled != 0 && !json_output)
      fprintf(stderr, "  mTLS: client certificate enrollment was not completed\n");
   return 0;
}

static int remote_set(const char *url, const char *token, int json_output)
{
   if (!url || !*url)
   {
      fprintf(stderr, "usage: aimee remote set <url> [token]\n");
      return 2;
   }
   char path[512];
   remote_conf_path(path, sizeof(path));
   ensure_dir_p(aimee_home());
   if (write_remote_conf(path, url, token) != 0)
   {
      fprintf(stderr, "aimee: cannot securely write %s\n", path);
      return 1;
   }

   /* For an https remote, establish trust now so later commands need no
    * AIMEE_TLS_INSECURE flag. If it already verifies (publicly-trusted CA, or a
    * previously pinned cert) we leave it alone; otherwise pin its cert (TOFU).
    * Verification is forced strict (env var suspended) so a self-signed/private
    * server is always pinned even if AIMEE_TLS_INSECURE happens to be set. */
   int is_https = (strncmp(url, "https://", 8) == 0);
   int pinned = 0, verified = 0, refused_identity = 0, recovered_identity = 0;
   char archived_dir[700] = "";
   if (is_https)
   {
      char *saved_insecure = tls_insecure_suspend();
      aimee_client_set_remote(url, token && *token ? token : NULL);
      /* This first probe is expected to fail for a self-signed server (not pinned
       * yet); silence its diagnostic so a clean set doesn't look like an error. */
      aimee_client_suppress_conn_errors(1);
      int already = remote_health_ok();
      aimee_client_suppress_conn_errors(0);
      if (already)
         verified = 1; /* already trusted — nothing to pin */
      else if (remote_pin_cert(url, json_output) == 0)
      {
         pinned = 1;
         verified = remote_health_ok(); /* re-probe against the pinned cert */
      }
      /* Still no answer: before blaming the network, rule out the one local cause
       * that mimics an unreachable server — a client certificate this server
       * refuses. Re-pairing is exactly when that happens (the server was
       * re-provisioned), and the stored identity is ours to replace, so recover
       * in place rather than making the operator find and delete it by hand. */
      if (!verified)
      {
         refused_identity = remote_probe_without_client_cert();
         if (refused_identity)
         {
            recovered_identity =
                remote_recover_client_cert(archived_dir, sizeof(archived_dir), json_output);
            if (recovered_identity)
               verified = remote_health_ok();
         }
      }
      tls_insecure_restore(saved_insecure);
   }

   /* Pairing stores the operator-provided credential and attempts certificate
    * enrollment. Additive bearer enrollment remains an explicit command. */
   int enrolled = 0;
   int mtls_enrolled = 0;
   /* Persisting the target is useful for a later `remote trust`, but an https
    * setup that cannot prove the peer is not a successful pairing. In
    * particular, an mTLS-required server may reject the TLS handshake before
    * this new client can reach certificate enrollment. */
   int remote_set_failed = is_https && !verified;
   if (verified && token && token[0] && remote_enroll_client_cert(json_output) == 0)
      mtls_enrolled = 1;

   /* Trust establishment proves only that we reached the intended TLS peer.
    * When the caller supplied a credential, also prove that the resulting
    * bearer/certificate combination is authorized before reporting success.
    * This still permits platforms without automatic mTLS enrollment while the
    * server is optional-mTLS: a valid bearer makes the probe succeed. */
   if (verified && token && token[0] && !remote_authorized_ok())
   {
      remote_set_failed = 1;
      if (!json_output)
         fprintf(stderr, "  auth: the server rejected the supplied credential; remote setup is not "
                         "complete\n");
   }

   if (json_output)
      printf("{\"ok\":%s,\"url\":\"%s\",\"token\":%s,\"pinned\":%s,\"verified\":%s,"
             "\"enrolled\":%s,\"mtls_enrolled\":%s,\"client_cert_refused\":%s,"
             "\"client_cert_recovered\":%s}\n",
             remote_set_failed ? "false" : "true", url, token && *token ? "true" : "false",
             pinned ? "true" : "false", verified ? "true" : "false", enrolled ? "true" : "false",
             mtls_enrolled ? "true" : "false", refused_identity ? "true" : "false",
             recovered_identity ? "true" : "false");
   else
   {
      printf("Remote server set to %s%s\n", url, token && *token ? " (with token)" : "");
      if (is_https && verified)
      {
         printf(pinned ? "  TLS: verified against the pinned certificate.\n"
                       : "  TLS: verified against the system trust store.\n");
         if (recovered_identity)
            printf("  mTLS: this server refused the stored client certificate (it was issued by a "
                   "different\n"
                   "        server instance). Archived it to %s\n"
                   "        and enrolled a replacement.\n",
                   archived_dir);
      }
      else if (is_https && refused_identity && recovered_identity)
         printf("  mTLS: this server refused the stored client certificate; archived it to\n"
                "        %s. The server still does\n"
                "        not answer, so something beyond the certificate is wrong.\n",
                archived_dir);
      else if (is_https && refused_identity)
         printf("  mTLS: this server refused the stored client certificate, and it could not be "
                "moved aside.\n"
                "        Move %s/tls/client.crt and client.key out of the way, then re-run "
                "`aimee remote set`.\n",
                aimee_home());
      else if (is_https && pinned)
         printf("  TLS: pinned this server's certificate, but it did not answer over the pinned "
                "channel.\n"
                "       The server is reachable (its certificate was just fetched), so this is a "
                "rejected\n"
                "       connection rather than a network problem — check the server log for the "
                "reason.\n");
      else if (is_https)
         printf("  TLS: could not reach %s to fetch its certificate (server down, wrong "
                "address/port,\n"
                "       or cert pinning is not available on this platform).\n"
                "       Start the server and re-run `aimee remote set`, or `aimee remote trust`.\n",
                url);
   }
   return remote_set_failed ? 1 : 0;
}

/* Re-pin the cert of the already-configured remote (e.g. after the server's
 * self-signed cert was rotated). */
static int remote_trust(int json_output)
{
   char url[512], token[256];
   if (!read_remote_conf(url, sizeof(url), token, sizeof(token)) || !url[0])
   {
      fprintf(stderr, "aimee: no remote configured; run `aimee remote set <url> [token]` first\n");
      return 2;
   }
   if (strncmp(url, "https://", 8) != 0)
   {
      fprintf(stderr, "aimee: remote %s is not https:// — no certificate to pin\n", url);
      return 1;
   }
   char *saved_insecure = tls_insecure_suspend(); /* pin + verify strictly, see remote_set */
   if (remote_pin_cert(url, json_output) != 0)
   {
      tls_insecure_restore(saved_insecure);
      fprintf(stderr,
              "aimee: could not fetch the server certificate (is %s reachable? is pinning "
              "supported on this platform?)\n",
              url);
      return 1;
   }
   aimee_client_set_remote(url, token[0] ? token : NULL);
   int verified = remote_health_ok();
   /* `remote trust` is the command an operator reaches for precisely when a server
    * was re-provisioned — the same event that invalidates the client certificate.
    * Re-pinning alone would leave them stuck, so apply the identical recovery. */
   int refused_identity = 0, recovered_identity = 0;
   char archived_dir[700] = "";
   if (!verified)
   {
      refused_identity = remote_probe_without_client_cert();
      if (refused_identity)
      {
         recovered_identity =
             remote_recover_client_cert(archived_dir, sizeof(archived_dir), json_output);
         if (recovered_identity)
            verified = remote_health_ok();
      }
   }
   tls_insecure_restore(saved_insecure);
   if (json_output)
      printf("{\"ok\":true,\"pinned\":true,\"verified\":%s,\"client_cert_refused\":%s,"
             "\"client_cert_recovered\":%s}\n",
             verified ? "true" : "false", refused_identity ? "true" : "false",
             recovered_identity ? "true" : "false");
   else if (verified)
   {
      printf("  TLS: verified against the pinned certificate.\n");
      if (recovered_identity)
         printf("  mTLS: this server refused the stored client certificate (it was issued by a "
                "different\n"
                "        server instance). Archived it to %s\n"
                "        and enrolled a replacement.\n",
                archived_dir);
   }
   else if (refused_identity && recovered_identity)
      printf("  mTLS: this server refused the stored client certificate; archived it to\n"
             "        %s. The server still does\n"
             "        not answer, so something beyond the certificate is wrong.\n",
             archived_dir);
   else if (refused_identity)
      printf("  mTLS: this server refused the stored client certificate, and it could not be moved "
             "aside.\n"
             "        Move %s/tls/client.crt and client.key out of the way, then re-run "
             "`aimee remote trust`.\n",
             aimee_home());
   else
      printf("  TLS: pinned, but the server still does not verify — check it.\n");
   return verified ? 0 : 1;
}

static int remote_clear(int json_output)
{
   char path[512];
   remote_conf_path(path, sizeof(path));
   int removed = (remove(path) == 0);
   /* Also drop the pinned cert so a later remote can't accidentally inherit it. */
   char ca[512];
   remote_ca_path(ca, sizeof(ca));
   remove(ca);
   if (json_output)
      printf("{\"ok\":true,\"cleared\":%s}\n", removed ? "true" : "false");
   else
      printf(removed ? "Remote server config cleared.\n" : "No persisted remote server config.\n");
   return 0;
}

static int remote_status(int json_output)
{
   /* Apply persisted config so status reflects what a real command would use. */
   cli_remote_load_persisted();
   char desc[300] = {0};
   int active = aimee_client_remote_active(desc, sizeof(desc));

   /* Probe reachability over whichever transport is in effect. */
   int st = 0;
   char *body = aimee_client_request("GET", "/v1/health", NULL, &st);
   int reachable = (body != NULL && st >= 200 && st < 500);
   /* A 401/403 means the transport is fine but the stored token is not accepted —
    * the single most common setup failure. Reporting a bare "Reachable: yes" for it
    * (and exiting 0) told users their client was configured when no command would
    * work; say what is actually wrong and fail. */
   int unauthorized = (body != NULL && (st == 401 || st == 403));
   int certificate_rejected = 0;
   if (unauthorized)
   {
      cJSON *response = cJSON_Parse(body);
      cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
      cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
      const char *text = cJSON_IsString(message) ? message->valuestring
                         : cJSON_IsString(error) ? error->valuestring
                                                 : "";
      certificate_rejected = strstr(text, "client certificate") != NULL;
      cJSON_Delete(response);
   }
   int ok = (body != NULL && st >= 200 && st < 400);
   free(body);

   if (json_output)
   {
      printf("{\"remote\":%s,\"target\":\"%s\",\"reachable\":%s,\"authorized\":%s,\"status\":%d,"
             "\"certificate_rejected\":%s}\n",
             active ? "true" : "false", active ? desc : "local-uds", reachable ? "true" : "false",
             unauthorized ? "false" : (ok ? "true" : "false"), st,
             certificate_rejected ? "true" : "false");
   }
   else
   {
      if (active)
         printf("Transport: remote TCP -> %s\n", desc);
      else
         printf("Transport: local Unix socket (no remote configured)\n");
      if (unauthorized)
      {
         printf("Reachable: yes, but NOT authorized (GET /v1/health -> %d)\n", st);
         if (certificate_rejected)
            printf("  The server rejected the client certificate. Have the server operator check\n"
                   "  its enrollment/revocation state and provision a valid client identity.\n"
                   "  The existing certificate and key have been preserved.\n");
         else
            printf(
                "  The server answered but rejected the stored token or its permissions. Re-run\n"
                "  `aimee remote set <url> <token>` with this server's current primary bearer.\n"
                "  Then run `aimee remote enroll` to give this client an individual bearer.\n");
      }
      else
      {
         printf("Reachable: %s (GET /v1/health -> %d)\n", reachable ? "yes" : "no", st);
      }
   }
   return ok ? 0 : 1;
}

static int remote_help_flag(const char *arg)
{
   return arg && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0);
}

static void remote_usage(FILE *out)
{
   fprintf(out, "usage: aimee remote <set <url> [token] | enroll | trust | status | clear>\n");
}

/* Mint an additional bearer for the configured remote and adopt it without
 * invalidating other clients. */
static int remote_enroll_cmd(int json_output)
{
   char url[512], token[256];
   if (!read_remote_conf(url, sizeof(url), token, sizeof(token)) || !url[0])
   {
      fprintf(stderr, "aimee: no remote configured; run `aimee remote set <url> [token]` first\n");
      return 2;
   }
   aimee_client_set_remote(url, token[0] ? token : NULL);
   char strong[256] = "";
   if (remote_enroll(url, token, strong, sizeof(strong), json_output) != 0)
   {
      if (!json_output)
         fprintf(stderr, "aimee: enrollment failed (is the remote reachable and the current token "
                         "valid?)\n");
      return 1;
   }
   if (!json_output)
      printf("Enrollment complete.\n");
   return 0;
}

int cli_remote_cmd(int argc, char **argv, int json_output)
{
   if (argc == 0)
      /* Preserve the historical shorthand: bare `aimee remote` is status. */
      return remote_status(json_output);

   /* `remote` is dispatched before cli_main's generic help gate because it is
    * the command that configures that transport. Handle help here before any
    * subcommand can mutate remote.conf or contact a server. In particular,
    * `aimee remote set --help` used to persist "--help" as the remote URL. */
   if (strcmp(argv[0], "help") == 0)
   {
      remote_usage(stdout);
      return 0;
   }
   for (int i = 0; i < argc; i++)
   {
      if (remote_help_flag(argv[i]))
      {
         remote_usage(stdout);
         return 0;
      }
   }

   const char *sub = argv[0];
   if (strcmp(sub, "set") == 0)
   {
      if (argc < 2 || argc > 3)
      {
         remote_usage(stderr);
         return 2;
      }
      return remote_set(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL, json_output);
   }
   if (argc != 1)
   {
      remote_usage(stderr);
      return 2;
   }
   if (strcmp(sub, "clear") == 0)
      return remote_clear(json_output);
   if (strcmp(sub, "trust") == 0)
      return remote_trust(json_output);
   if (strcmp(sub, "enroll") == 0)
      return remote_enroll_cmd(json_output);
   if (strcmp(sub, "status") == 0)
      return remote_status(json_output);
   remote_usage(stderr);
   return 2;
}
