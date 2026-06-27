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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define AIMEE_MKDIR(p) _mkdir(p)
/* _putenv_s(k, "") removes the variable on MSVC. */
#define AIMEE_UNSETENV(k)  _putenv_s((k), "")
#define AIMEE_SETENV(k, v) _putenv_s((k), (v))
#else
#include <sys/stat.h>
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

void cli_remote_load_persisted(void)
{
   /* A --server flag or AIMEE_SERVER_URL already wins; don't override it. */
   if (aimee_client_remote_active(NULL, 0))
      return;
   char url[512], token[256];
   if (read_remote_conf(url, sizeof(url), token, sizeof(token)) && url[0])
      aimee_client_set_remote(url, token[0] ? token : NULL);
}

static void remote_ca_path(char *out, size_t out_sz)
{
   snprintf(out, out_sz, "%s/remote-ca.pem", aimee_home());
}

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
   FILE *f = fopen(path, "w");
   if (!f)
   {
      fprintf(stderr, "aimee: cannot write %s\n", path);
      return 1;
   }
   fprintf(f, "%s\n%s\n", url, token ? token : "");
   fclose(f);

   /* For an https remote, establish trust now so later commands need no
    * AIMEE_TLS_INSECURE flag. If it already verifies (publicly-trusted CA, or a
    * previously pinned cert) we leave it alone; otherwise pin its cert (TOFU).
    * Verification is forced strict (env var suspended) so a self-signed/private
    * server is always pinned even if AIMEE_TLS_INSECURE happens to be set. */
   int is_https = (strncmp(url, "https://", 8) == 0);
   int pinned = 0, verified = 0;
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
      tls_insecure_restore(saved_insecure);
   }

   if (json_output)
      printf("{\"ok\":true,\"url\":\"%s\",\"token\":%s,\"pinned\":%s,\"verified\":%s}\n", url,
             token && *token ? "true" : "false", pinned ? "true" : "false",
             verified ? "true" : "false");
   else
   {
      printf("Remote server set to %s%s\n", url, token && *token ? " (with token)" : "");
      if (is_https && verified)
         printf(pinned ? "  TLS: verified against the pinned certificate.\n"
                       : "  TLS: verified against the system trust store.\n");
      else if (is_https)
         printf(
             "  TLS: could not establish trust (server unreachable, or cert pinning is not "
             "available on this platform).\n"
             "       Start the server and re-run `aimee remote set`, or `aimee remote trust`.\n");
   }
   return 0;
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
   tls_insecure_restore(saved_insecure);
   if (json_output)
      printf("{\"ok\":true,\"pinned\":true,\"verified\":%s}\n", verified ? "true" : "false");
   else
      printf(verified ? "  TLS: verified against the pinned certificate.\n"
                      : "  TLS: pinned, but the server still does not verify — check it.\n");
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
   free(body);

   if (json_output)
   {
      printf("{\"remote\":%s,\"target\":\"%s\",\"reachable\":%s,\"status\":%d}\n",
             active ? "true" : "false", active ? desc : "local-uds", reachable ? "true" : "false",
             st);
   }
   else
   {
      if (active)
         printf("Transport: remote TCP -> %s\n", desc);
      else
         printf("Transport: local Unix socket (no remote configured)\n");
      printf("Reachable: %s (GET /v1/health -> %d)\n", reachable ? "yes" : "no", st);
   }
   return reachable ? 0 : 1;
}

int cli_remote_cmd(int argc, char **argv, int json_output)
{
   const char *sub = argc > 0 ? argv[0] : "status";
   if (strcmp(sub, "set") == 0)
      return remote_set(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL, json_output);
   if (strcmp(sub, "clear") == 0)
      return remote_clear(json_output);
   if (strcmp(sub, "trust") == 0)
      return remote_trust(json_output);
   if (strcmp(sub, "status") == 0)
      return remote_status(json_output);
   fprintf(stderr, "usage: aimee remote <set <url> [token] | trust | status | clear>\n");
   return 2;
}
