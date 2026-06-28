/* cli_agent_keys.c: see cli_agent_keys.h.
 *
 * The legacy client-held credential PUSH (cli_session_creds_prime ->
 * /v1/session/credentials, and cli_agent_add_localize_key) was retired in P4b:
 * the server credential vault is the single, permanent store. What remains here
 * is the local agent-keys.json reader used by `aimee agent key import` to MIGRATE
 * any leftover client-held keys into the vault. */
#include "cli_agent_keys.h"
#include "cli_client.h" /* cli_v1_dispatch for the import migration */
#include "aimee_home.h" /* aimee_home */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* chmod */
#include <time.h>     /* time() for the backup suffix */
#ifndef _WIN32
#include <errno.h>  /* EEXIST (import lock) */
#include <fcntl.h>  /* open, O_CREAT|O_EXCL (import lock) */
#include <unistd.h> /* fsync, fileno, write, close */
#endif

#define AGENT_KEYS_FILE "agent-keys.json"

static int aimee_file_path(const char *name, char *out, size_t out_len)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;
   snprintf(out, out_len, "%s/%s", home, name);
   return 0;
}

static char *read_text(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > (1 << 20))
   {
      fclose(f);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t got = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[got] = '\0';
   return buf;
}

/* Load the local keyring object {name:key}; always returns an object. */
static cJSON *load_keys(void)
{
   char path[1024];
   if (aimee_file_path(AGENT_KEYS_FILE, path, sizeof(path)) != 0)
      return cJSON_CreateObject();
   char *buf = read_text(path);
   cJSON *root = buf ? cJSON_Parse(buf) : NULL;
   free(buf);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      root = cJSON_CreateObject();
   }
   return root;
}

cJSON *cli_agent_keys_load(void)
{
   return load_keys();
}

int cli_agent_key_set(const char *agent_name, const char *api_key)
{
   if (!agent_name || !agent_name[0])
      return -1;
   char path[1024];
   if (aimee_file_path(AGENT_KEYS_FILE, path, sizeof(path)) != 0)
      return -1;

   cJSON *root = load_keys();
   cJSON_DeleteItemFromObjectCaseSensitive(root, agent_name);
   if (api_key && api_key[0])
      cJSON_AddStringToObject(root, agent_name, api_key);

   char *json = cJSON_Print(root);
   cJSON_Delete(root);
   if (!json)
      return -1;

   /* Atomic write: a partial/failed rewrite must never truncate the keyring and
    * lose an entry that is NOT yet in the vault (scrub-on-import runs this per
    * vaulted key). Write a sibling temp file, flush (fsync on POSIX), chmod 0600,
    * then rename over the target; on any error remove the temp and leave the
    * original intact. Portable ISO-C stdio so the thin client builds on Windows
    * too. The caller MUST observe the return value (a failed scrub leaves
    * plaintext, which is the safe direction, but must be surfaced). */
   char tmp[1100];
   if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp))
   {
      free(json);
      return -1;
   }
   FILE *f = fopen(tmp, "wb");
   if (!f)
   {
      free(json);
      return -1;
   }
   size_t len = strlen(json);
   int ok = (fwrite(json, 1, len, f) == len) && (fflush(f) == 0);
   free(json);
#ifndef _WIN32
   if (ok && fsync(fileno(f)) != 0)
      ok = 0;
#endif
   if (fclose(f) != 0)
      ok = 0;
   if (ok)
      chmod(tmp, 0600);
#ifdef _WIN32
   /* Windows rename() will not replace an existing file. */
   if (ok)
      remove(path);
#endif
   if (!ok || rename(tmp, path) != 0)
   {
      remove(tmp);
      return -1;
   }
   return 0;
}

/* Copy the keyring file to a 0600 sibling backup before the destructive scrub
 * (D9 backup-before-scrub). The scrub empties agent-keys.json; if the vault copy
 * later becomes unreadable (e.g. a lost/corrupt master key — a failure mode the
 * server's atomic write does NOT cover), this backup is the only recoverable copy
 * of the plaintext keys. Writes the backup path into `out` (caller reports it so
 * the operator can verify delegates, then remove it). 0 on success, -1 on error
 * (incl. no keyring file to back up). */
static int backup_keyring(char *out, size_t out_len)
{
   char src[1024];
   if (aimee_file_path(AGENT_KEYS_FILE, src, sizeof(src)) != 0)
      return -1;
   char *buf = read_text(src);
   if (!buf)
      return -1; /* nothing to back up */
   char dst[1100];
   /* Unique, portable suffix (the lock already serializes concurrent runs on
    * POSIX; the timestamp keeps a prior, not-yet-removed backup from being
    * clobbered). */
   if ((size_t)snprintf(dst, sizeof(dst), "%s.pre-import.%lld", src, (long long)time(NULL)) >=
       sizeof(dst))
   {
      free(buf);
      return -1;
   }
   /* Create the backup 0600 ATOMICALLY (open with the mode, not fopen-then-chmod)
    * so the plaintext keys are never momentarily world-readable, and with
    * O_EXCL|O_NOFOLLOW so a pre-planted symlink at the predictable path can't trap
    * the write. On POSIX; the Windows thin client is not the import target. */
#ifndef _WIN32
   int fd = open(dst, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
   if (fd < 0)
   {
      free(buf);
      return -1;
   }
   FILE *f = fdopen(fd, "wb");
   if (!f)
   {
      close(fd);
      remove(dst);
      free(buf);
      return -1;
   }
#else
   FILE *f = fopen(dst, "wb");
   if (!f)
   {
      free(buf);
      return -1;
   }
#endif
   size_t len = strlen(buf);
   int ok = (fwrite(buf, 1, len, f) == len) && (fflush(f) == 0);
   free(buf);
#ifndef _WIN32
   if (ok && fsync(fileno(f)) != 0)
      ok = 0;
#endif
   if (fclose(f) != 0)
      ok = 0;
   if (!ok)
   {
      remove(dst);
      return -1;
   }
#ifdef _WIN32
   chmod(dst, 0600);
#endif
   snprintf(out, out_len, "%s", dst);
   return 0;
}

#ifndef _WIN32
/* Advisory exclusive lock so two concurrent `agent key import` runs (or a
 * concurrent `agent add` write) cannot interleave the read-modify-write of the
 * keyring (D9 per-run lock). POSIX-only; the import is an operator command run on
 * the server host. Returns the lock fd (>=0), -1 on a non-contention error, or -2
 * if the lock is already held. */
static int acquire_import_lock(char *lockpath, size_t lp_len)
{
   if (aimee_file_path(AGENT_KEYS_FILE ".import.lock", lockpath, lp_len) != 0)
      return -1;
   int fd = open(lockpath, O_CREAT | O_EXCL | O_WRONLY, 0600);
   if (fd < 0)
      return errno == EEXIST ? -2 : -1;
   char pid[32];
   int n = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
   (void)(write(fd, pid, (size_t)n) >= 0);
   return fd;
}

static void release_import_lock(int fd, const char *lockpath)
{
   /* Only remove the lock file if it is still OUR lock: if an operator manually
    * cleared a "stale" lock and another run re-created it, the path now names a
    * different inode and a blind remove() would delete the live run's lock. */
   if (fd >= 0 && lockpath && lockpath[0])
   {
      struct stat fd_st, path_st;
      if (fstat(fd, &fd_st) == 0 && stat(lockpath, &path_st) == 0 &&
          fd_st.st_ino == path_st.st_ino && fd_st.st_dev == path_st.st_dev)
         remove(lockpath);
   }
   if (fd >= 0)
      close(fd);
}
#endif

/* `aimee agent key import [--keep] [--dry-run]` (P3): migrate client-held
 * agent-keys.json entries into the server vault under the server principal
 * (vault.set_server). The vault is the single permanent credential store, so the
 * migration SCRUBS each plaintext entry by default once its vault store is
 * confirmed — leaving the secret in the local plaintext keyring after vaulting
 * defeats the purpose and is how stale plaintext keys linger. Pass --keep to
 * retain the local copy (e.g. a deliberate offline backup); --scrub is still
 * accepted as a no-op for backward compatibility. Reports per agent; idempotent
 * (re-runnable). Over a plaintext-TCP remote the server refuses (P2a/D2b) and the
 * entry is NOT vaulted and NOT scrubbed — provision over an attested
 * (UDS/webchat) connection holding the vault:write:server capability. */
int cli_agent_key_import(int argc, char **argv, int json_output)
{
   (void)json_output;
   int scrub = 1, dry = 0; /* scrub-by-default: keys belong only in the vault */
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--keep") == 0 || strcmp(argv[i], "--no-scrub") == 0)
         scrub = 0;
      else if (strcmp(argv[i], "--scrub") == 0)
         scrub = 1; /* accepted no-op (now the default) */
      else if (strcmp(argv[i], "--dry-run") == 0)
         dry = 1;
   }

   /* Serialize against a concurrent import / `agent add` so two writers cannot
    * interleave the keyring read-modify-write (D9). A dry-run mutates nothing, so
    * it needs no lock. POSIX-only; the import is a server-host operator command. */
#ifndef _WIN32
   int lock_fd = -1;
   char lockpath[1100] = "";
   if (!dry)
   {
      lock_fd = acquire_import_lock(lockpath, sizeof(lockpath));
      if (lock_fd == -2)
      {
         printf("agent key import: another import appears to be running (lock %s). Re-run after "
                "it finishes, or remove a stale lock.\n",
                lockpath);
         return 1;
      }
      /* lock_fd == -1 (a non-contention error, e.g. read-only home) is non-fatal:
       * proceed unlocked rather than block a legitimate single-run migration. */
   }
#endif

   /* Backup-before-scrub (D9): take ONE recoverable 0600 snapshot of the keyring
    * before the first destructive scrub. If it can't be written we must not scrub
    * (keep the plaintext — the safe direction) and say so loudly. */
   char backup_path[1100] = "";
   int backup_done = 0;
   if (scrub && !dry)
   {
      if (backup_keyring(backup_path, sizeof(backup_path)) == 0)
         backup_done = 1;
      else
      {
         scrub = 0;
         printf("WARNING: could not write a pre-import backup of the keyring — NOT scrubbing "
                "(plaintext kept). Fix the home directory permissions and re-run to scrub.\n");
      }
   }

   cJSON *keys = cli_agent_keys_load();
   int total = 0, vaulted = 0, refused = 0, errors = 0;
   cJSON *entry = NULL;
   cJSON_ArrayForEach(entry, keys)
   {
      const char *agent = entry->string;
      if (!agent || !agent[0] || !cJSON_IsString(entry) || !entry->valuestring[0])
         continue;
      total++;
      if (dry)
      {
         /* Preview only — never send the secret or mutate the keyring. */
         printf("  %-16s would vault (dry-run)\n", agent);
         continue;
      }
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "method", "vault.set_server");
      cJSON_AddStringToObject(req, "agent", agent);
      cJSON_AddStringToObject(req, "cred", "api_key");
      cJSON_AddStringToObject(req, "secret", entry->valuestring);
      cJSON *resp = cli_v1_dispatch(req, 30000);
      cJSON_Delete(req);

      cJSON *st = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
      int ok = st && cJSON_IsString(st) && strcmp(st->valuestring, "ok") == 0;
      if (ok)
      {
         vaulted++;
         if (scrub && cli_agent_key_set(agent, NULL) != 0)
            printf("  %-16s vaulted (WARNING: could not scrub plaintext copy — remove it "
                   "manually)\n",
                   agent);
         else
            printf("  %-16s vaulted%s\n", agent, scrub ? " + scrubbed" : "");
      }
      else
      {
         cJSON *msg = resp ? cJSON_GetObjectItemCaseSensitive(resp, "message") : NULL;
         const char *m = (msg && cJSON_IsString(msg)) ? msg->valuestring : "no server response";
         if (strstr(m, "attested") || strstr(m, "capability"))
            refused++;
         else
            errors++;
         printf("  %-16s SKIPPED (%s)\n", agent, m);
      }
      cJSON_Delete(resp);
   }
   cJSON_Delete(keys);

   if (dry)
   {
#ifndef _WIN32
      release_import_lock(lock_fd, lockpath);
#endif
      printf("agent key import (dry-run): %d entr%s would be sent; nothing changed\n", total,
             total == 1 ? "y" : "ies");
      return 0;
   }

   /* A backup that scrubbed nothing is a redundant extra plaintext copy — drop it.
    * Otherwise keep it and tell the operator to remove it once delegates verify. */
   if (backup_done && vaulted == 0)
   {
      remove(backup_path);
      backup_done = 0;
   }

   const char *suffix = "";
   if (vaulted > 0)
      suffix = scrub ? "; vaulted entries scrubbed from the local plaintext keyring"
                     : "; vaulted entries KEPT in the local plaintext keyring (--keep) — still "
                       "readable on disk";
   printf("agent key import: %d vaulted, %d refused, %d error (of %d)%s\n", vaulted, refused,
          errors, total, suffix);
   if (backup_done)
      printf("pre-import backup written to %s (0600) — verify delegates authenticate from the "
             "vault, then remove it (it holds the plaintext keys).\n",
             backup_path);
   if (refused > 0)
      printf("note: server-principal writes need an attested (UDS/webchat) connection holding the "
             "vault:write:server capability — run this on the server host over UDS, or grant the "
             "capability.\n");
#ifndef _WIN32
   release_import_lock(lock_fd, lockpath);
#endif
   return errors > 0 ? 1 : 0;
}
