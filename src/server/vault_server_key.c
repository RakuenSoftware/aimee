/* vault_server_key.c: the server-sealed KEK for dual-access credentials (WP-C.4).
 * See vault_server_key.h. Reuses the vault_crypto primitives — this file only
 * manages the 0600 master-key file and caches the derived server KEK. */
#include "vault_server_key.h"
#include "vault_crypto.h"
#include "vault_store.h"   /* vault_store_list_principals, _rekey_field (D13) */
#include "config.h"        /* config_default_dir */
#include "platform_path.h" /* platform_mkdir_p */
#include "log.h"
#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Fixed, non-secret HKDF salt for the server KEK. Distinct from the random
 * per-principal salts the user vaults use, so the server KEK lives in its own
 * derivation space (domain separation). Changing it would orphan every existing
 * server wrap, so it is frozen. */
static const uint8_t SERVER_KEK_SALT[VAULT_SALT_LEN] = {
    0x61, 0x69, 0x6d, 0x65, 0x65, 0x2d, 0x73, 0x76, 0x72, 0x6b, 0x65, 0x6b, 0x2d, 0x76, 0x31, 0x00};

static pthread_mutex_t g_kek_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_kek[VAULT_KEK_LEN];
static int g_kek_ready; /* 0 until the KEK is derived + cached */

/* Compose <config_default_dir>/.vault/.server-master.key. Returns 0 on success. */
static int master_key_path(char *out, size_t cap)
{
   const char *base = config_default_dir();
   if (!base || !base[0])
      return -1;
   return (size_t)snprintf(out, cap, "%s/.vault/.server-master.key", base) >= cap ? -1 : 0;
}

/* Outcome of reading the master-key file. The caller must NOT mint a new key on
 * MK_READ_BAD — a present-but-unreadable file (transient EACCES/EIO, wrong size)
 * must fail closed, never be overwritten, or one transient error would orphan
 * every existing server wrap. Only MK_READ_ABSENT (genuinely no file) may mint. */
typedef enum
{
   MK_READ_OK = 0,
   MK_READ_ABSENT = 1, /* ENOENT: no key file yet — safe to mint */
   MK_READ_BAD = -1,   /* exists but unreadable/wrong size — fail closed, do NOT mint */
} mk_read_t;

/* Read exactly VAULT_ROOT_KEY_LEN bytes from `path` into `key`. Returns MK_READ_*
 * (key cleansed on any non-OK). A file of any size other than exactly 32 bytes is
 * BAD, not absent — we never overwrite it. */
static mk_read_t master_key_read(const char *path, uint8_t key[VAULT_ROOT_KEY_LEN])
{
   int fd = open(path, O_RDONLY);
   if (fd < 0)
   {
      OPENSSL_cleanse(key, VAULT_ROOT_KEY_LEN);
      return errno == ENOENT ? MK_READ_ABSENT : MK_READ_BAD;
   }
   uint8_t buf[VAULT_ROOT_KEY_LEN + 1];
   ssize_t n = read(fd, buf, sizeof(buf));
   close(fd);
   int ok = (n == (ssize_t)VAULT_ROOT_KEY_LEN);
   if (ok)
      memcpy(key, buf, VAULT_ROOT_KEY_LEN);
   OPENSSL_cleanse(buf, sizeof(buf));
   if (!ok)
      OPENSSL_cleanse(key, VAULT_ROOT_KEY_LEN);
   return ok ? MK_READ_OK : MK_READ_BAD;
}

/* fsync the directory holding `path` so a rename into it survives a crash. */
static void fsync_parent_dir(const char *path)
{
   char dir[1280];
   snprintf(dir, sizeof(dir), "%s", path);
   char *slash = strrchr(dir, '/');
   if (!slash)
      return;
   *slash = '\0';
   int fd = open(dir, O_RDONLY);
   if (fd >= 0)
   {
      (void)fsync(fd);
      close(fd);
   }
}

/* Atomically create the master-key file with `key` at 0600 (tmp + rename +
 * fsync). 0 on success, -1 on error. */
static int master_key_write(const char *path, const uint8_t key[VAULT_ROOT_KEY_LEN])
{
   char dir[1024], tmp[1280];
   const char *base = config_default_dir();
   if (!base || !base[0] || (size_t)snprintf(dir, sizeof(dir), "%s/.vault", base) >= sizeof(dir))
      return -1;
   if (platform_mkdir_p(dir, 0700) != 0)
      return -1;
   static atomic_uint s_seq = 0;
   unsigned seq = atomic_fetch_add(&s_seq, 1);
   if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%u", path, (int)getpid(), seq) >= sizeof(tmp))
      return -1;

   int rc = -1;
   int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC | O_EXCL, 0600);
   if (fd >= 0)
   {
      ssize_t w = write(fd, key, VAULT_ROOT_KEY_LEN);
      if (w == (ssize_t)VAULT_ROOT_KEY_LEN && fsync(fd) == 0)
         rc = 0;
      close(fd);
      if (rc == 0 && rename(tmp, path) != 0)
         rc = -1;
      if (rc != 0)
         unlink(tmp);
   }
   if (rc == 0)
      fsync_parent_dir(path); /* make the rename durable across a crash */
   return rc;
}

/* Load-or-create the master key, derive the server KEK, cache it. Caller holds
 * g_kek_mu. */
static int derive_and_cache(void)
{
   char path[1280];
   if (master_key_path(path, sizeof(path)) != 0)
      return -1;

   uint8_t master[VAULT_ROOT_KEY_LEN];
   mk_read_t r = master_key_read(path, master);
   if (r == MK_READ_BAD)
      return -1; /* present but unreadable — fail closed, NEVER overwrite (F1) */
   if (r == MK_READ_ABSENT)
   {
      /* Genuinely no key file: mint a fresh master and persist it. A concurrent
       * creator would lose the O_EXCL race; fall back to reading the winner's
       * file (which must now be readable, else fail closed). */
      if (vault_crypto_random(master, sizeof(master)) != 0)
         return -1;
      if (master_key_write(path, master) != 0)
      {
         OPENSSL_cleanse(master, sizeof(master));
         if (master_key_read(path, master) != MK_READ_OK)
         {
            OPENSSL_cleanse(master, sizeof(master)); /* read-back failed — cleanse (F2) */
            return -1;
         }
      }
   }

   int rc = vault_kek_derive(master, sizeof(master), SERVER_KEK_SALT, sizeof(SERVER_KEK_SALT),
                             g_kek) == 0
                ? 0
                : -1;
   OPENSSL_cleanse(master, sizeof(master));
   if (rc == 0)
      g_kek_ready = 1;
   else
      OPENSSL_cleanse(g_kek, sizeof(g_kek));
   return rc;
}

int vault_server_kek(uint8_t kek[VAULT_KEK_LEN])
{
   if (!kek)
      return -1;
   pthread_mutex_lock(&g_kek_mu);
   int rc = g_kek_ready ? 0 : derive_and_cache();
   if (rc == 0)
      memcpy(kek, g_kek, VAULT_KEK_LEN);
   pthread_mutex_unlock(&g_kek_mu);
   if (rc != 0)
      OPENSSL_cleanse(kek, VAULT_KEK_LEN);
   return rc;
}

void vault_server_key_reset_for_test(void)
{
   pthread_mutex_lock(&g_kek_mu);
   OPENSSL_cleanse(g_kek, sizeof(g_kek));
   g_kek_ready = 0;
   pthread_mutex_unlock(&g_kek_mu);
}

/* ── Master-key rotation (D13) ────────────────────────────────────────────── */

/* Copy one regular file src→dst, preserving 0600 for sensitive vault content. */
static int rot_copy_file(const char *src, const char *dst)
{
   int rc = -1;
   int in = open(src, O_RDONLY);
   if (in < 0)
      return -1;
   int out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0600);
   if (out < 0)
   {
      close(in);
      return -1;
   }
   char buf[8192];
   ssize_t n;
   rc = 0;
   while ((n = read(in, buf, sizeof(buf))) > 0)
   {
      ssize_t off = 0;
      while (off < n)
      {
         ssize_t w = write(out, buf + off, (size_t)(n - off));
         if (w <= 0)
         {
            rc = -1;
            break;
         }
         off += w;
      }
      if (rc != 0)
         break;
   }
   if (n < 0)
      rc = -1;
   if (rc == 0 && fsync(out) != 0)
      rc = -1;
   close(in);
   close(out);
   OPENSSL_cleanse(buf, sizeof(buf));
   return rc;
}

/* Copy every regular file in src_dir into dst_dir (created 0700). Flat copy — the
 * .vault directory has no subdirectories. 0 on success, -1 on any error. */
static int rot_copy_dir_flat(const char *src_dir, const char *dst_dir)
{
   if (platform_mkdir_p(dst_dir, 0700) != 0)
      return -1;
   DIR *d = opendir(src_dir);
   if (!d)
      return -1;
   int rc = 0;
   struct dirent *de;
   while ((de = readdir(d)) != NULL)
   {
      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
         continue;
      char sp[1408], dp[1408];
      if ((size_t)snprintf(sp, sizeof(sp), "%s/%s", src_dir, de->d_name) >= sizeof(sp) ||
          (size_t)snprintf(dp, sizeof(dp), "%s/%s", dst_dir, de->d_name) >= sizeof(dp))
      {
         rc = -1;
         break;
      }
      struct stat st;
      if (stat(sp, &st) != 0 || !S_ISREG(st.st_mode))
         continue; /* skip non-regular entries (no subdirs expected) */
      if (rot_copy_file(sp, dp) != 0)
      {
         rc = -1;
         break;
      }
   }
   closedir(d);
   return rc;
}

int vault_server_key_rotate(const char *server_principal, int *out_principals, int *out_creds,
                            char *backup_path, size_t backup_path_len, char *errbuf, size_t errlen)
{
#define ROT_FAIL(msg)                                                                              \
   do                                                                                              \
   {                                                                                               \
      if (errbuf && errlen)                                                                        \
         snprintf(errbuf, errlen, "%s", (msg));                                                    \
      goto fail;                                                                                   \
   } while (0)

   int ret = -1;
   uint8_t old_master[VAULT_ROOT_KEY_LEN] = {0}, new_master[VAULT_ROOT_KEY_LEN] = {0};
   uint8_t old_kek[VAULT_KEK_LEN] = {0}, new_kek[VAULT_KEK_LEN] = {0};
   char(*principals)[VAULT_PRINCIPAL_MAX] = NULL;
   int restore_on_fail = 0;
   char vdir[1024] = "", bdir[1280] = "";

   if (!server_principal || !server_principal[0])
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "internal: server principal not provided");
      return -1;
   }

   pthread_mutex_lock(&g_kek_mu);

   char path[1280];
   if (master_key_path(path, sizeof(path)) != 0)
      ROT_FAIL("cannot resolve master-key path (AIMEE_HOME unset?)");

   mk_read_t r = master_key_read(path, old_master);
   if (r == MK_READ_ABSENT)
   {
      /* No master key yet — nothing to rotate (it is minted on first write). */
      if (out_principals)
         *out_principals = 0;
      if (out_creds)
         *out_creds = 0;
      if (backup_path && backup_path_len)
         backup_path[0] = '\0';
      ret = 0;
      goto done;
   }
   if (r != MK_READ_OK)
      ROT_FAIL("master key present but unreadable/wrong size — fail closed (not overwritten)");

   if (vault_kek_derive(old_master, sizeof(old_master), SERVER_KEK_SALT, sizeof(SERVER_KEK_SALT),
                        old_kek) != 0)
      ROT_FAIL("could not derive the current server KEK");

   if (vault_crypto_random(new_master, sizeof(new_master)) != 0)
      ROT_FAIL("could not generate a new master key (entropy failure)");
   if (vault_kek_derive(new_master, sizeof(new_master), SERVER_KEK_SALT, sizeof(SERVER_KEK_SALT),
                        new_kek) != 0)
      ROT_FAIL("could not derive the new server KEK");

   /* Back up the whole .vault directory BEFORE mutating anything. */
   const char *base = config_default_dir();
   if (!base || !base[0] || (size_t)snprintf(vdir, sizeof(vdir), "%s/.vault", base) >= sizeof(vdir))
      ROT_FAIL("cannot resolve the .vault directory");
   if ((size_t)snprintf(bdir, sizeof(bdir), "%s.rotate-bak.%d", vdir, (int)getpid()) >=
       sizeof(bdir))
      ROT_FAIL("backup path too long");
   if (rot_copy_dir_flat(vdir, bdir) != 0)
      ROT_FAIL("could not create the pre-rotation backup");
   restore_on_fail = 1;

   /* Re-wrap every principal's server wrap old→new. */
   int max_principals = 256;
   principals = malloc((size_t)max_principals * sizeof(*principals));
   if (!principals)
      ROT_FAIL("out of memory");
   int np = vault_store_list_principals(principals, max_principals);
   if (np < 0)
      ROT_FAIL("could not enumerate vault principals");

   int total_creds = 0;
   for (int i = 0; i < np; i++)
   {
      /* The server principal holds the server KEK as its PRIMARY wrap
       * ("wrapped_dek"); every other principal holds it only in the dual-access
       * "wrapped_dek_server" field. Re-wrap exactly the server-KEK-held field. */
      const char *field =
          strcmp(principals[i], server_principal) == 0 ? "wrapped_dek" : "wrapped_dek_server";
      int c = vault_store_rekey_field(principals[i], field, old_kek, new_kek);
      if (c < 0)
         ROT_FAIL("a principal's server-wrap re-wrap failed (wrong key / tamper) — vault restored");
      total_creds += c;
   }

   /* Commit: swap in the new master key atomically. Only now is the on-disk
    * master consistent with the freshly re-wrapped server wraps. */
   if (master_key_write(path, new_master) != 0)
      ROT_FAIL("re-wrap succeeded but persisting the new master key failed — vault restored");

   /* New master is live; drop the cached KEK so the next use re-derives it. */
   OPENSSL_cleanse(g_kek, sizeof(g_kek));
   g_kek_ready = 0;
   restore_on_fail = 0; /* committed — do not restore */

   if (out_principals)
      *out_principals = np;
   if (out_creds)
      *out_creds = total_creds;
   if (backup_path && backup_path_len)
      snprintf(backup_path, backup_path_len, "%s", bdir);
   ret = 0;
   goto done;

fail:
   if (restore_on_fail && vdir[0] && bdir[0])
      (void)rot_copy_dir_flat(bdir, vdir); /* best-effort revert to the pre-rotation state */
done:
   OPENSSL_cleanse(old_master, sizeof(old_master));
   OPENSSL_cleanse(new_master, sizeof(new_master));
   OPENSSL_cleanse(old_kek, sizeof(old_kek));
   OPENSSL_cleanse(new_kek, sizeof(new_kek));
   free(principals);
   pthread_mutex_unlock(&g_kek_mu);
   return ret;
#undef ROT_FAIL
}
