/* cmd_self_update.c: thin-client self-update. See cmd_self_update.h.
 *
 * The client learns the server version from GET /v1/version (the server already
 * serves it). Because the client<->server relationship is 1:1, the server's
 * version is the exact target: an update fetches the matching release binary
 * from GitHub and atomically swaps this executable, so client and server stay
 * lockstep. This first cut is conservative: session-start only NOTIFIES on
 * drift; the swap happens only when the user runs `aimee self-update`.
 *
 * Integrity, in order: (1) the download is over cert-verified HTTPS; (2) its
 * SHA-256 is checked against the digest GitHub publishes for the release asset
 * (keyless -- a confirmed mismatch is fatal; --require-verify makes a missing
 * digest fatal too); (3) the fetched binary is executed (`<tmp> version`) and
 * required to report the exact target version, catching truncation/wrong-arch/
 * wrong-version. A key-based signature (defending against a compromised GitHub
 * itself) remains a further follow-up -- the release publishes no per-asset
 * signatures yet, only the API digest used here. */

#include "headers/cmd_self_update.h"

#include "cli_client.h"
#include "cJSON.h"
#include "headers/aimee_home.h"
#include "headers/aimee_version.h"
#include "headers/util.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
   /* Only a comparable (semver) server that is strictly newer is actionable; a
    * dev/branch "testing-<sha>" server is not orderable, so stay silent. */
   if (!aimee_version_is_semver(server_ver))
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

static void lc_hex(char *s)
{
   for (; *s; s++)
      if (*s >= 'A' && *s <= 'F')
         *s += 32;
}

/* Compute the SHA-256 of `path` into `hex` (lowercase, 64 chars). The swap path
 * is Linux-only, so sha256sum (coreutils) is always present here. 0 on success. */
static int sha256_of_file(const char *path, char *hex, size_t cap)
{
   if (!path_is_shell_safe(path))
      return -1;
   char cmd[PATH_MAX + 32];
   snprintf(cmd, sizeof cmd, "sha256sum '%s' 2>/dev/null", path);
   int rc = 0;
   char *out = run_cmd(cmd, &rc);
   int ok = -1;
   if (rc == 0 && out)
   {
      /* Output: "<64-hex>  <path>". Take the leading hex token. */
      size_t n = 0;
      while (out[n] && ((out[n] >= '0' && out[n] <= '9') || (out[n] >= 'a' && out[n] <= 'f') ||
                        (out[n] >= 'A' && out[n] <= 'F')))
         n++;
      if (n == 64 && n < cap)
      {
         memcpy(hex, out, 64);
         hex[64] = '\0';
         lc_hex(hex);
         ok = 0;
      }
   }
   free(out);
   return ok;
}

/* Fetch the SHA-256 GitHub publishes for release asset `asset` of version
 * v`tnorm` (its API `digest` field, "sha256:<hex>"). Writes lowercase hex to
 * `hex`. Returns 0 on success, -1 if the API/asset/digest is unavailable. */
static int fetch_asset_sha256(const char *tnorm, const char *asset, char *hex, size_t cap)
{
   if (!aimee_version_is_safe(tnorm) || !asset)
      return -1;
   char url[256];
   snprintf(url, sizeof url, "https://api.github.com/repos/RakuenSoftware/aimee/releases/tags/v%s",
            tnorm);
   char cmd[400];
   snprintf(cmd, sizeof cmd,
            "curl -fsSL --proto '=https' --tlsv1.2 --connect-timeout 15 --max-time 60 "
            "-H 'User-Agent: aimee-self-update' '%s' 2>/dev/null",
            url);
   int rc = 0;
   char *body = run_cmd(cmd, &rc);
   if (rc != 0 || !body)
   {
      free(body);
      return -1;
   }
   cJSON *root = cJSON_Parse(body);
   free(body);
   if (!root)
      return -1;
   int found = -1;
   cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
   cJSON *a = NULL;
   cJSON_ArrayForEach(a, assets)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
      if (!cJSON_IsString(name) || strcmp(name->valuestring, asset) != 0)
         continue;
      cJSON *dig = cJSON_GetObjectItemCaseSensitive(a, "digest");
      if (cJSON_IsString(dig))
      {
         const char *d = dig->valuestring;
         if (strncmp(d, "sha256:", 7) == 0)
            d += 7;
         if (strlen(d) == 64 && (size_t)65 <= cap)
         {
            snprintf(hex, cap, "%s", d);
            lc_hex(hex);
            found = 0;
         }
      }
      break;
   }
   cJSON_Delete(root);
   return found;
}

/* Read `update_mode:` from <aimee_home>/aimee.yaml. Returns "off", "notify"
 * (default), or "apply". Minimal scalar read (mirrors the attention-guard's). */
static const char *read_update_mode(void)
{
   static char mode[16];
   snprintf(mode, sizeof mode, "notify");
   const char *home = aimee_home();
   if (!home || !home[0])
      return mode;
   char path[1024];
   snprintf(path, sizeof path, "%s/aimee.yaml", home);
   FILE *fp = fopen(path, "r");
   if (!fp)
      return mode;
   char line[256];
   while (fgets(line, sizeof line, fp))
   {
      const char *p = line;
      while (*p == ' ' || *p == '\t')
         p++;
      if (strncmp(p, "update_mode:", 12) != 0)
         continue;
      p += 12;
      while (*p == ' ' || *p == '\t')
         p++;
      char v[16];
      size_t n = 0;
      while (*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '#' && n < sizeof v - 1)
         v[n++] = *p++;
      v[n] = '\0';
      if (strcmp(v, "off") == 0 || strcmp(v, "apply") == 0 || strcmp(v, "notify") == 0)
         snprintf(mode, sizeof mode, "%s", v);
      break;
   }
   fclose(fp);
   return mode;
}

/* apply-mode auto-update: when `update_mode: apply` and the server is a strictly
 * newer semver, spawn a detached `self-update --yes --require-verify` (verified
 * swap only). Rate-limited to at most once/hour via a stamp file so it does not
 * fire every session. Best-effort and non-blocking: the caller's session is
 * never held up, and the atomic rename means the running session keeps its
 * binary while the NEXT one starts current. No-op unless mode is apply. */
void aimee_self_update_apply_async(void)
{
   if (strcmp(read_update_mode(), "apply") != 0)
      return;
   if (!cli_v1_has_remote_endpoint())
      return;

   const char *home = aimee_home();
   char stamp[1024] = "";
   if (home && home[0])
   {
      snprintf(stamp, sizeof stamp, "%s/.self-update-stamp", home);
      struct stat st;
      if (stat(stamp, &st) == 0 && (time(NULL) - st.st_mtime) < 3600)
         return; /* attempted within the last hour -> back off */
   }

   char server_ver[64];
   if (aimee_fetch_server_version(server_ver, sizeof server_ver) != 0)
      return;
   if (!aimee_version_is_semver(server_ver) ||
       aimee_version_compare(server_ver, AIMEE_VERSION) <= 0)
      return;

   char self[PATH_MAX];
   if (resolve_self_path(self, sizeof self) != 0)
      return;

   /* Record the attempt up front so a persistently-failing update still backs
    * off (avoids a download storm on every session). */
   if (stamp[0])
   {
      FILE *fp = fopen(stamp, "w");
      if (fp)
         fclose(fp);
   }

   pid_t pid = fork();
   if (pid != 0)
      return; /* parent (or fork failure): do not block the session */
   /* Child: detach and run the verified self-update. CRITICAL: this runs inside
    * the SessionStart hook, whose stdout IS the hook's JSON channel -- redirect
    * ALL of stdin/stdout/stderr away before the update writes anything, so its
    * output never corrupts the hook response. Prefer a log file; fall back to
    * /dev/null when there is no resolvable home. */
   setsid();
   int devnull = open("/dev/null", O_RDWR);
   if (devnull >= 0)
   {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
   }
   if (home && home[0])
   {
      char logp[1024];
      snprintf(logp, sizeof logp, "%s/self-update.log", home);
      int lf = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (lf >= 0)
      {
         dup2(lf, STDOUT_FILENO);
         dup2(lf, STDERR_FILENO);
         close(lf);
      }
   }
   if (devnull >= 0)
      close(devnull);
   execl(self, "aimee", "self-update", "--yes", "--require-verify", (char *)NULL);
   _exit(127);
}

/* After a successful swap, warn if PATH exposes other `aimee` binaries that now
 * differ -- the "two out-of-sync copies" smell. Advisory only; never modifies. */
static void warn_sibling_binaries(const char *self)
{
   int rc = 0;
   char *out = run_cmd("command -v -a aimee 2>/dev/null", &rc);
   if (!out)
      return;
   char *save = NULL;
   for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
   {
      if (!line[0] || strcmp(line, self) == 0 || !path_is_shell_safe(line))
         continue;
      char vcmd[PATH_MAX + 32], selfcmp[PATH_MAX + 32];
      snprintf(vcmd, sizeof vcmd, "'%s' version 2>/dev/null", line);
      snprintf(selfcmp, sizeof selfcmp, "'%s' version 2>/dev/null", self);
      int a = 0, b = 0;
      char *lv = run_cmd(vcmd, &a);
      char *sv = run_cmd(selfcmp, &b);
      if (lv && sv && strcmp(lv, sv) != 0)
         printf("note: another aimee on PATH differs: %s (%s). Update it too, or make it a "
                "symlink to %s so there is one canonical install.\n",
                line, lv[0] ? lv : "unknown", self);
      free(lv);
      free(sv);
   }
   free(out);
}

int cmd_self_update(int argc, char **argv)
{
   int check_only = 0, assume_yes = 0, require_verify = 0;
   const char *forced_version = NULL;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--check") == 0)
         check_only = 1;
      else if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0)
         assume_yes = 1;
      else if (strcmp(argv[i], "--require-verify") == 0)
         require_verify = 1; /* fail unless the published SHA-256 is confirmed */
      else if (strcmp(argv[i], "--version") == 0 && i + 1 < argc)
         forced_version = argv[++i];
      else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      {
         printf(
             "Usage: aimee self-update [--check] [--version vX.Y.Z] [--yes] "
             "[--require-verify]\n"
             "  Update this thin-client binary to match the aimee-server.\n"
             "  --check          Report whether an update is available; do not download.\n"
             "  --version vX.Y.Z Target a specific version instead of the server's.\n"
             "  --yes            Do not prompt before swapping the binary.\n"
             "  --require-verify Refuse to install unless the published SHA-256 is confirmed.\n");
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
   /* Releases are semver-tagged; a non-semver target has no release to fetch.
    * The common case is a dev/branch server (deliberately "testing-<sha>", per
    * publish-testing.yml) -- report that honestly rather than misclaiming the
    * client is ahead, and let the user target a specific release explicitly. */
   if (!aimee_version_is_semver(tnorm))
   {
      if (forced_version)
      {
         fprintf(stderr, "aimee self-update: '%s' is not a release version (expected vX.Y.Z).\n",
                 target);
         return 1;
      }
      printf("client v%s, server reports '%s'\n", vnum(AIMEE_VERSION), vnum(target));
      printf("The server reports a non-semver (dev/branch) version, so drift cannot be "
             "compared. Target a specific release with `aimee self-update --version vX.Y.Z`.\n");
      return 0;
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

   /* Strong integrity check: verify the download's SHA-256 against the digest
    * GitHub publishes for this release asset. A confirmed mismatch is fatal. If
    * the digest cannot be fetched (offline / API rate-limited), fall back to the
    * version self-check below for interactive updates -- but --require-verify
    * (used by auto-apply) refuses to proceed without a confirmed hash. */
   char want_sha[80] = "", got_sha[80] = "";
   if (fetch_asset_sha256(tnorm, asset, want_sha, sizeof want_sha) == 0)
   {
      if (sha256_of_file(tmp, got_sha, sizeof got_sha) != 0)
      {
         fprintf(stderr, "aimee self-update: could not hash the download.\n");
         unlink(tmp);
         return 1;
      }
      if (strcmp(want_sha, got_sha) != 0)
      {
         fprintf(stderr,
                 "aimee self-update: SHA-256 mismatch -- refusing to install.\n"
                 "  expected %s\n  got      %s\n",
                 want_sha, got_sha);
         unlink(tmp);
         return 1;
      }
      if (!assume_yes)
         printf("Verified SHA-256 %s\n", got_sha);
   }
   else if (require_verify)
   {
      fprintf(stderr, "aimee self-update: could not confirm the published SHA-256 (GitHub API "
                      "unavailable) and --require-verify is set; not installing.\n");
      unlink(tmp);
      return 1;
   }
   else
   {
      fprintf(stderr, "aimee self-update: warning: could not fetch the published SHA-256 "
                      "(GitHub API unavailable); relying on the version self-check.\n");
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
   warn_sibling_binaries(self);
   return 0;
}
