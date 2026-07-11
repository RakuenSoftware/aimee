/* cmd_self_update.c: thin-client self-update. See cmd_self_update.h.
 *
 * The client learns the server version from GET /v1/version (the server already
 * serves it). Because the client<->server relationship is 1:1, the server's
 * version is the exact target: an update fetches the matching release binary
 * from GitHub and atomically swaps this executable, so client and server stay
 * lockstep. This first cut is conservative: session-start only NOTIFIES on
 * drift; the swap happens only when the user runs `aimee self-update`.
 *
 * Integrity: the download is over cert-verified HTTPS, and the fetched binary is
 * executed (`<tmp> version`) and required to report the exact target version
 * before it is swapped in -- this catches truncation, wrong-arch, and
 * wrong-version artifacts. A cryptographic signature check is a recommended
 * follow-up (the release does not yet publish per-asset signatures). */

#include "headers/cmd_self_update.h"

#include "cli_client.h"
#include "cJSON.h"
#include "headers/aimee_version.h"
#include "headers/util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Pure helpers (aimee_version_compare / aimee_version_is_safe /
 * aimee_self_update_asset) live in self_update_util.c. */

#define AIMEE_RELEASE_URL_BASE "https://github.com/RakuenSoftware/aimee/releases/download"

/* Version strings may or may not carry a leading 'v' (AIMEE_VERSION is stamped
 * "v0.2.182" by release builds; /v1/version may report either form). Strip it
 * for display so we never print "vv0.2.182". */
static const char *vnum(const char *s)
{
   return (s && (s[0] == 'v' || s[0] == 'V')) ? s + 1 : s;
}

int aimee_fetch_server_version(char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!out || cap == 0)
      return -1;
   char *endpoint = cli_v1_client_endpoint();
   if (!endpoint)
      return -1;
   char *bearer = cli_v1_client_bearer();
   int status = 0;
   cJSON *resp = cli_http_request(endpoint, "GET", "/v1/version", NULL, bearer, 5000, &status);
   free(endpoint);
   free(bearer);
   if (!resp)
      return -1;
   int rc = -1;
   cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "version");
   if (status == 200 && cJSON_IsString(v) && v->valuestring[0])
   {
      snprintf(out, cap, "%s", v->valuestring);
      rc = 0;
   }
   cJSON_Delete(resp);
   return rc;
}

int aimee_self_update_notice(char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!out || cap == 0)
      return 0;
   /* Drift is only meaningful for a thin client talking to a separate server;
    * a co-located session shares this very binary and cannot drift. */
   if (!cli_v1_has_remote_endpoint())
      return 0;
   char server_ver[64];
   if (aimee_fetch_server_version(server_ver, sizeof server_ver) != 0)
      return 0;
   if (aimee_version_compare(server_ver, AIMEE_VERSION) <= 0)
      return 0;
   snprintf(out, cap,
            "aimee-server is v%s but this client is v%s. Run `aimee self-update` to "
            "catch up (keeps client and server in lockstep).",
            vnum(server_ver), vnum(AIMEE_VERSION));
   return 1;
}

/* readlink /proc/self/exe -> `out`. 0 on success. Linux-only; the swap path is
 * gated on this so other platforms get a clear "not supported here" message. */
static int resolve_self_path(char *out, size_t cap)
{
#ifdef __linux__
   ssize_t n = readlink("/proc/self/exe", out, cap - 1);
   if (n <= 0)
      return -1;
   out[n] = '\0';
   return 0;
#else
   (void)out;
   (void)cap;
   return -1;
#endif
}

static int path_is_shell_safe(const char *p)
{
   /* We single-quote paths into a shell command; a literal single quote would
    * break out of the quoting. System executable paths never contain one, so
    * refuse rather than attempt to escape. */
   return p && !strchr(p, '\'');
}

int cmd_self_update(int argc, char **argv)
{
   int check_only = 0, assume_yes = 0;
   const char *forced_version = NULL;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--check") == 0)
         check_only = 1;
      else if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0)
         assume_yes = 1;
      else if (strcmp(argv[i], "--version") == 0 && i + 1 < argc)
         forced_version = argv[++i];
      else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      {
         printf("Usage: aimee self-update [--check] [--version vX.Y.Z] [--yes]\n"
                "  Update this thin-client binary to match the aimee-server.\n"
                "  --check          Report whether an update is available; do not download.\n"
                "  --version vX.Y.Z Target a specific version instead of the server's.\n"
                "  --yes            Do not prompt before swapping the binary.\n");
         return 0;
      }
   }

   /* Determine the target version: an explicit --version, else the server's. */
   char target[64];
   if (forced_version)
   {
      snprintf(target, sizeof target, "%s", forced_version);
   }
   else if (aimee_fetch_server_version(target, sizeof target) != 0)
   {
      fprintf(stderr, "aimee self-update: could not read the server version (no remote "
                      "endpoint configured, or the server is unreachable).\n");
      return 1;
   }
   /* Normalise a leading 'v' out of the compare/URL handling below. */
   const char *tnorm = (target[0] == 'v' || target[0] == 'V') ? target + 1 : target;
   if (!aimee_version_is_safe(tnorm))
   {
      fprintf(stderr, "aimee self-update: refusing to act on an implausible version '%s'.\n",
              target);
      return 1;
   }

   int cmp = aimee_version_compare(tnorm, AIMEE_VERSION);
   /* Only prefix 'v' for semver-looking targets; a non-semver server string
    * (e.g. a dev "testing-<sha>" build) is shown verbatim. */
   const char *tpfx = (tnorm[0] >= '0' && tnorm[0] <= '9') ? "v" : "";
   printf("client v%s, target %s%s\n", vnum(AIMEE_VERSION), tpfx, tnorm);
   if (cmp < 0)
   {
      printf("Client is ahead of the target; nothing to do (self-update never downgrades).\n");
      return 0;
   }
   if (cmp == 0)
   {
      printf("Already up to date.\n");
      return 0;
   }
   if (check_only)
   {
      printf("Update available: run `aimee self-update` to install v%s.\n", tnorm);
      return 0;
   }

   const char *asset = aimee_self_update_asset();
   if (!asset)
   {
      fprintf(stderr, "aimee self-update: no release asset for this platform.\n");
      return 1;
   }
   char self[PATH_MAX];
   if (resolve_self_path(self, sizeof self) != 0)
   {
      fprintf(stderr, "aimee self-update: could not resolve this executable's path "
                      "(auto-swap is supported on Linux only).\n");
      return 1;
   }
   if (!path_is_shell_safe(self))
   {
      fprintf(stderr, "aimee self-update: executable path is not safe to script over.\n");
      return 1;
   }
   if (access(self, W_OK) != 0)
   {
      fprintf(stderr,
              "aimee self-update: %s is not writable by you; re-run with the right "
              "privileges (or reinstall).\n",
              self);
      return 1;
   }

   if (!assume_yes)
      printf("Downloading v%s (%s) and replacing %s ...\n", tnorm, asset, self);

   /* Download to a sibling temp file (same directory -> same filesystem, so the
    * final rename() is atomic and running processes keep the old inode). */
   char tmp[PATH_MAX];
   if (snprintf(tmp, sizeof tmp, "%s.update.%ld", self, (long)getpid()) >= (int)sizeof tmp)
   {
      fprintf(stderr, "aimee self-update: path too long.\n");
      return 1;
   }
   if (!path_is_shell_safe(tmp))
   {
      fprintf(stderr, "aimee self-update: temp path is not safe to script over.\n");
      return 1;
   }

   char url[512];
   snprintf(url, sizeof url, "%s/v%s/%s", AIMEE_RELEASE_URL_BASE, tnorm, asset);

   char cmd[1200];
   snprintf(cmd, sizeof cmd,
            "curl -fSL --proto '=https' --tlsv1.2 --connect-timeout 15 --max-time 300 "
            "-o '%s' '%s' 2>&1",
            tmp, url);
   int rc = 0;
   char *dl = run_cmd(cmd, &rc);
   struct stat st;
   if (rc != 0 || stat(tmp, &st) != 0 || st.st_size <= 0)
   {
      fprintf(stderr, "aimee self-update: download failed (%s).\n  url: %s\n",
              dl && dl[0] ? dl : "curl error", url);
      free(dl);
      unlink(tmp);
      return 1;
   }
   free(dl);
   if (chmod(tmp, 0755) != 0)
   {
      fprintf(stderr, "aimee self-update: could not chmod the downloaded binary.\n");
      unlink(tmp);
      return 1;
   }

   /* Integrity/correctness gate: the downloaded binary must run and report the
    * exact target version before we trust it enough to swap it in. */
   char verify_cmd[PATH_MAX + 32];
   snprintf(verify_cmd, sizeof verify_cmd, "'%s' version 2>/dev/null", tmp);
   int vrc = 0;
   char *vout = run_cmd(verify_cmd, &vrc);
   int version_ok = 0;
   if (vrc == 0 && vout)
   {
      /* `aimee version` prints "<prog> <version>" (and <prog> here is the temp
       * filename). Trim trailing whitespace first, THEN take the last space/tab
       * token, and compare by semver to the target. */
      char got[128];
      snprintf(got, sizeof got, "%s", vout);
      size_t L = strlen(got);
      while (L > 0 &&
             (got[L - 1] == '\n' || got[L - 1] == '\r' || got[L - 1] == ' ' || got[L - 1] == '\t'))
         got[--L] = '\0';
      const char *tok = got;
      for (const char *p = got; *p; p++)
         if (*p == ' ' || *p == '\t')
            tok = p + 1;
      if (tok[0] && aimee_version_compare(tok, tnorm) == 0)
         version_ok = 1;
   }
   if (!version_ok)
   {
      fprintf(stderr,
              "aimee self-update: downloaded binary failed verification (expected v%s, got "
              "'%s'). Not swapping.\n",
              tnorm, vout ? vout : "<no output>");
      free(vout);
      unlink(tmp);
      return 1;
   }
   free(vout);

   /* Back up the current binary for rollback, then atomically swap. */
   char bak[PATH_MAX];
   if (snprintf(bak, sizeof bak, "%s.bak", self) < (int)sizeof bak)
   {
      char cpcmd[PATH_MAX * 2 + 32];
      snprintf(cpcmd, sizeof cpcmd, "cp -p '%s' '%s' 2>/dev/null", self, bak);
      int crc = 0;
      char *co = run_cmd(cpcmd, &crc);
      free(co);
   }
   if (rename(tmp, self) != 0)
   {
      fprintf(stderr,
              "aimee self-update: atomic replace failed; the current binary is "
              "unchanged. Downloaded file left at %s\n",
              tmp);
      return 1;
   }

   printf("Updated to v%s. Backup at %s.bak\n", tnorm, self);
   return 0;
}
