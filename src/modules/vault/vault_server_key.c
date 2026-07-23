/* vault_server_key.c: the server-sealed KEK for dual-access credentials (WP-C.4).
 * See vault_server_key.h. Reuses the vault_crypto primitives — this file only
 * manages the 0600 master-key file and caches the derived server KEK. */
#include "vault_server_key.h"
#include "vault_internal.h" /* vault_custody_provider_t seam */
#include "vault_crypto.h"
#include "vault_kek_cache.h" /* vault_kek_cache_clear (seal flushes the KEK cache) */
#include "vault_store.h"     /* vault_store_list_principals, _rekey_field (D13) */
#include "config.h"          /* config_default_dir */
#include "platform_path.h"   /* platform_mkdir_p */
#include "log.h"
#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif
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
static volatile sig_atomic_t g_fork_child_invalid;
static int ensure_atfork(void);

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

static int file_get_kek(void *ctx, uint8_t kek[VAULT_KEK_LEN])
{
   if (g_fork_child_invalid || ensure_atfork() != 0 || !kek)
   {
      if (kek)
         OPENSSL_cleanse(kek, VAULT_KEK_LEN);
      return -1;
   }
   pthread_mutex_lock(&g_kek_mu);
   int rc = g_kek_ready ? 0 : derive_and_cache();
   if (rc == 0)
      memcpy(kek, g_kek, VAULT_KEK_LEN);
   pthread_mutex_unlock(&g_kek_mu);
   if (rc != 0)
      OPENSSL_cleanse(kek, VAULT_KEK_LEN);
   return rc;
}

static void file_kek_clear(void)
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
   int in = open(src, O_RDONLY | O_NOFOLLOW); /* never follow a symlink source (F1) */
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
      /* lstat (NOT stat): never follow a symlink planted in .vault/ — a
       * `<x>.json -> /etc/shadow` would otherwise be read through and copied into
       * the backup, then written back on restore (F1). Copy only real files. */
      struct stat st;
      if (lstat(sp, &st) != 0 || !S_ISREG(st.st_mode))
         continue; /* skip symlinks, dirs, and other non-regular entries */
      if (rot_copy_file(sp, dp) != 0)
      {
         rc = -1;
         break;
      }
   }
   closedir(d);
   return rc;
}

static int file_rotate(void *ctx, const char *server_principal, int *out_principals, int *out_creds,
                       char *backup_path, size_t backup_path_len, char *errbuf, size_t errlen)
{
#define ROT_FAIL(msg)                                                                              \
   do                                                                                              \
   {                                                                                               \
      if (errbuf && errlen)                                                                        \
         snprintf(errbuf, errlen, "%s", (msg));                                                    \
      goto fail;                                                                                   \
   } while (0)

   if (g_fork_child_invalid || ensure_atfork() != 0)
      return -1;
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
   struct stat bst;
   if (lstat(bdir, &bst) == 0)
      ROT_FAIL("a backup directory from a prior/concurrent rotation already exists — remove it "
               "first (S3)");
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

   /* F3: prove the new server KEK actually decrypts a server-principal credential
    * end-to-end BEFORE committing the master. A field-name mismatch (server creds
    * not in "wrapped_dek") would re-wrap 0 entries and silently orphan every
    * server cred once the master is swapped — so if the server principal has any
    * credential, one MUST read back cleanly under the new KEK or we abort. */
   vault_store_entry_t probe;
   int sc = vault_store_list(server_principal, &probe, 1);
   if (sc > 0)
   {
      char vbuf[8192];
      int vrc =
          vault_store_get(server_principal, new_kek, probe.agent, probe.cred, vbuf, sizeof(vbuf));
      OPENSSL_cleanse(vbuf, sizeof(vbuf));
      if (vrc != 0)
         ROT_FAIL("post-rewrap verify failed: a server-principal credential does not decrypt under "
                  "the new key (field mismatch?) — refusing to swap master, vault restored");
   }

   /* Commit: swap in the new master key atomically. Only now is the on-disk
    * master consistent with the freshly re-wrapped server wraps. */
   if (master_key_write(path, new_master) != 0)
      ROT_FAIL("re-wrap succeeded but persisting the new master key failed — vault restored");

   /* New master is live: mark committed FIRST (so a signal here cannot trigger a
    * spurious restore), then drop the cached KEK so the next use re-derives it. */
   restore_on_fail = 0; /* committed — do not restore (F5) */
   OPENSSL_cleanse(g_kek, sizeof(g_kek));
   g_kek_ready = 0;

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
   {
      /* Revert to the pre-rotation state. The master key was NOT swapped on any
       * fail path, so the cached g_kek still matches the on-disk (old) master.
       * If the restore itself fails partway (EIO/ENOSPC), the vault may be left
       * mixed old/new-wrapped — say so explicitly so the operator restores by
       * hand from the intact backup rather than trusting a silent revert (F4). */
      if (rot_copy_dir_flat(bdir, vdir) != 0 && errbuf && errlen)
      {
         char base_err[200];
         snprintf(base_err, sizeof(base_err), "%s", errbuf);
         snprintf(errbuf, errlen,
                  "%s; AND automatic restore FAILED — manually copy %s/* back over %s/ before "
                  "restarting",
                  base_err, bdir, vdir);
      }
   }
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

/* ── Custody seam ─────────────────────────────────────────────────────────────
 * The default "file" custody provider: derives the server KEK from the 0600
 * .server-master.key file above and drives its rotation. Stateless (ctx==NULL) —
 * the cached KEK lives in the module globals (g_kek/g_kek_ready), so
 * vault_server_key_reset_for_test() keeps working unchanged. The public
 * vault_server_kek() / vault_server_key_rotate() below are thin forwarders that
 * dispatch through g_custody, passing g_custody->ctx as the first argument so a
 * future stateful provider needs no globals. Signatures in vault_server_key.h
 * are UNCHANGED. (hwm_read/hwm_cas are deferred to a later slice.) */
static const vault_custody_provider_t file_custody = {
    .name = "file", .ctx = NULL, .get_kek = file_get_kek, .rotate = file_rotate,
    /* Seal slots deliberately left NULL: file custody self-unseals from its 0600
     * master-key file and is ALWAYS unsealed (is_sealed=0, seal/unseal no-op). The
     * server profile runs this provider and never observes VAULT_ERR_SEALED. */
};

static const vault_custody_provider_t *g_custody = &file_custody;
static pthread_mutex_t g_hwm_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t g_use_lock = PTHREAD_RWLOCK_INITIALIZER;
static uint64_t g_use_epoch = 1;
static uint64_t g_primary_seal_epoch;
static int g_primary_epoch_initialized;

typedef struct
{
   vault_maintenance_guard_t *token;
   pthread_t owner;
   int prior_cancel_state;
   int callback_active;
   uint8_t *arena;
   size_t arena_len;
} maintenance_registry_t;

static pthread_mutex_t g_maintenance_mu = PTHREAD_MUTEX_INITIALIZER;
static maintenance_registry_t g_maintenance;
static uintptr_t g_next_guard_token = 1;
static _Thread_local vault_maintenance_guard_t *g_owned_guard;
static _Thread_local int g_owned_use;
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;
static int g_atfork_status = -1;

static int custody_is_sealed_unlocked(void)
{
   return g_custody->is_sealed ? g_custody->is_sealed(g_custody->ctx) : 0;
}

static int custody_get_kek_unlocked(uint8_t kek[VAULT_KEK_LEN])
{
   return g_custody->get_kek ? g_custody->get_kek(g_custody->ctx, kek) : -1;
}

static int custody_unseal_unlocked(const void *params, size_t len)
{
   return g_custody->unseal ? g_custody->unseal(g_custody->ctx, params, len) : 0;
}

static vault_custody_auth_result_t custody_unseal_typed_unlocked(const void *params, size_t len)
{
   if (!g_custody->typed_unseal)
      return VAULT_CUSTODY_AUTH_UNSUPPORTED;
   int rc = g_custody->typed_unseal(g_custody->ctx, params, len);
   return rc >= VAULT_CUSTODY_AUTHORIZED && rc <= VAULT_CUSTODY_AUTH_UNSUPPORTED
              ? (vault_custody_auth_result_t)rc
              : VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
}

/* Always clear local key material and epoch synchronization, even when the
 * provider reports a seal failure. Caller holds g_use_lock exclusively. */
static int custody_seal_failclosed_unlocked(void)
{
   int epoch_ok = g_use_epoch != UINT64_MAX;
   if (epoch_ok)
      g_use_epoch++;
   g_primary_seal_epoch = 0;
   g_primary_epoch_initialized = 0;
   vault_kek_cache_clear();
   file_kek_clear();
   int rc = g_custody->seal ? g_custody->seal(g_custody->ctx) : 0;
   return rc == 0 && epoch_ok ? 0 : -1;
}

static void maintenance_child_atfork(void)
{
   /* The child is permanently custody-invalid until exec. It must never touch
    * inherited pthread/ESYS/provider state, which may have been owned by a
    * vanished parent thread. Wipe with volatile stores before publishing the
    * invalid flag; do not reinitialize an already-initialized pthread object. */
   g_fork_child_invalid = 1;
   if (g_custody->after_fork_child)
      g_custody->after_fork_child(g_custody->ctx);
   vault_kek_cache_after_fork_child();
   /* MADV_WIPEONFORK has already zeroed a live arena in the child. Never
    * dereference or unmap its inherited registry pointer here: fork may have
    * interrupted publication, and a callback can still be returning through it. */
   g_maintenance.token = NULL;
   g_maintenance.callback_active = 0;
   g_maintenance.arena = NULL;
   g_maintenance.arena_len = 0;
   g_owned_guard = NULL;
   g_owned_use = 0;
   g_primary_seal_epoch = 0;
   g_primary_epoch_initialized = 0;
   if (g_use_epoch != UINT64_MAX)
      g_use_epoch++;
   volatile uint8_t *p = g_kek;
   for (size_t i = 0; i < sizeof(g_kek); i++)
      p[i] = 0;
   g_kek_ready = 0;
}

static void register_atfork_once(void)
{
   g_atfork_status = pthread_atfork(NULL, NULL, maintenance_child_atfork);
}

static int ensure_atfork(void)
{
   return pthread_once(&g_atfork_once, register_atfork_once) == 0 && g_atfork_status == 0 ? 0 : -1;
}

static int maintenance_arena_new(uint8_t **arena, size_t *mapped)
{
#if defined(__linux__) && defined(MADV_DONTDUMP) && defined(MADV_WIPEONFORK)
   long page = sysconf(_SC_PAGESIZE);
   if (page <= 0)
      return -1;
   size_t n = ((size_t)VAULT_KEK_LEN + (size_t)page - 1) & ~((size_t)page - 1);
   void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (p == MAP_FAILED)
      return -1;
   if (mlock(p, n) != 0 || madvise(p, n, MADV_DONTDUMP) != 0 || madvise(p, n, MADV_WIPEONFORK) != 0)
   {
      OPENSSL_cleanse(p, n);
      (void)munlock(p, n);
      (void)munmap(p, n);
      return -1;
   }
   *arena = p;
   *mapped = n;
   return 0;
#else
   (void)arena;
   (void)mapped;
   return -1;
#endif
}

static void maintenance_arena_free(uint8_t *arena, size_t mapped)
{
   if (!arena)
      return;
   OPENSSL_cleanse(arena, mapped);
#if defined(__linux__)
   (void)munlock(arena, mapped);
   (void)munmap(arena, mapped);
#else
   (void)mapped;
#endif
}

/* Validate identity before dereferencing any caller-controlled handle. */
static int maintenance_validate(vault_maintenance_guard_t *guard)
{
   if (g_fork_child_invalid)
      return VAULT_MAINTENANCE_INVALID;
   int rc = VAULT_MAINTENANCE_INVALID;
   pthread_mutex_lock(&g_maintenance_mu);
   if (guard && guard == g_maintenance.token)
      rc = !pthread_equal(pthread_self(), g_maintenance.owner) ? VAULT_MAINTENANCE_WRONG_OWNER
           : g_maintenance.callback_active                     ? VAULT_MAINTENANCE_BUSY
                                                               : VAULT_MAINTENANCE_OK;
   pthread_mutex_unlock(&g_maintenance_mu);
   return rc;
}

/* Rebind the active custody provider (P7 profile composition / tests). NULL
 * restores the built-in file provider. See vault_internal.h. */
void vault_custody_set_provider(const vault_custody_provider_t *provider)
{
   if (g_fork_child_invalid || g_owned_guard || g_owned_use)
      return;
   if (ensure_atfork() != 0)
      return;
   pthread_rwlock_wrlock(&g_use_lock);
   pthread_mutex_lock(&g_hwm_mu);
   g_custody = provider ? provider : &file_custody;
   pthread_mutex_unlock(&g_hwm_mu);
   if (g_use_epoch != UINT64_MAX)
      g_use_epoch++;
   g_primary_seal_epoch = 0;
   g_primary_epoch_initialized = 0;
   pthread_rwlock_unlock(&g_use_lock);
}

void vault_server_key_reset_for_test(void)
{
   if (g_fork_child_invalid || g_owned_guard || g_owned_use)
      return;
   if (ensure_atfork() != 0)
      return;
   pthread_rwlock_wrlock(&g_use_lock);
   file_kek_clear();
   if (g_use_epoch != UINT64_MAX)
      g_use_epoch++;
   g_primary_seal_epoch = 0;
   g_primary_epoch_initialized = 0;
   pthread_rwlock_unlock(&g_use_lock);
}

int vault_server_kek(uint8_t kek[VAULT_KEK_LEN])
{
   if (g_fork_child_invalid || g_owned_guard || ensure_atfork() != 0)
   {
      if (kek)
         OPENSSL_cleanse(kek, VAULT_KEK_LEN);
      return -1;
   }
   return g_custody->get_kek(g_custody->ctx, kek);
}

int vault_server_key_rotate(const char *server_principal, int *out_principals, int *out_creds,
                            char *backup_path, size_t backup_path_len, char *errbuf, size_t errlen)
{
   if (g_fork_child_invalid || g_owned_guard || g_owned_use || ensure_atfork() != 0)
      return -1;
   return g_custody->rotate(g_custody->ctx, server_principal, out_principals, out_creds,
                            backup_path, backup_path_len, errbuf, errlen);
}

/* ── Seal barrier (P7 §3) ─────────────────────────────────────────────────────
 * These dispatch to the bound provider's OPTIONAL seal slots. A NULL slot means
 * "always unsealed / no-op", which is the file provider — so the SERVER profile
 * (file custody) never seals and vault_is_sealed() is always 0 for it. */
int vault_is_sealed(void)
{
   if (g_fork_child_invalid || ensure_atfork() != 0)
      return 1;
   return custody_is_sealed_unlocked();
}

vault_custody_local_status_t vault_custody_selected_local_status(void)
{
   if (g_fork_child_invalid || ensure_atfork() != 0)
      return VAULT_CUSTODY_LOCAL_UNAVAILABLE;

   /* Provider binding is startup-only.  g_hwm_mu nevertheless makes the
    * pointer snapshot race-free for tests and rejects a concurrent rebind
    * instead of allowing an operator status request to block behind it. */
   if (pthread_mutex_trylock(&g_hwm_mu) != 0)
      return VAULT_CUSTODY_LOCAL_UNAVAILABLE;
   const vault_custody_provider_t *provider = g_custody;
   int (*status_fn)(void *, unsigned) = provider ? provider->local_status : NULL;
   void *ctx = provider ? provider->ctx : NULL;
   pthread_mutex_unlock(&g_hwm_mu);

   if (!status_fn)
      return VAULT_CUSTODY_LOCAL_UNAVAILABLE;
   int status = status_fn(ctx, 50);
   return status >= VAULT_CUSTODY_LOCAL_AVAILABLE_SEALED && status <= VAULT_CUSTODY_LOCAL_MALFORMED
              ? (vault_custody_local_status_t)status
              : VAULT_CUSTODY_LOCAL_MALFORMED;
}

vault_custody_auth_result_t
vault_custody_selected_authorization_preflight(const void *secret, size_t secret_len,
                                               uint64_t expected_generation)
{
   if (g_fork_child_invalid || ensure_atfork() != 0 || g_owned_guard || g_owned_use || !secret ||
       secret_len == 0 || secret_len > VAULT_CUSTODY_AUTH_SECRET_MAX || expected_generation == 0 ||
       expected_generation > INT64_MAX)
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;

   pthread_mutex_lock(&g_hwm_mu);
   const vault_custody_provider_t *provider = g_custody;
   int (*preflight)(void *, const void *, size_t, uint64_t) =
       provider ? provider->authorization_preflight : NULL;
   void *ctx = provider ? provider->ctx : NULL;
   pthread_mutex_unlock(&g_hwm_mu);
   int rc = preflight ? preflight(ctx, secret, secret_len, expected_generation)
                      : VAULT_CUSTODY_AUTH_UNSUPPORTED;
   return rc >= VAULT_CUSTODY_AUTHORIZED && rc <= VAULT_CUSTODY_AUTH_UNSUPPORTED
              ? (vault_custody_auth_result_t)rc
              : VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
}

vault_custody_auth_result_t
vault_custody_selected_authorization_preflight_current(const void *secret, size_t secret_len,
                                                       uint64_t *generation)
{
   if (generation)
      *generation = 0;
   if (!generation || g_fork_child_invalid || ensure_atfork() != 0 || g_owned_guard ||
       g_owned_use || !secret || secret_len == 0 || secret_len > VAULT_CUSTODY_AUTH_SECRET_MAX)
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;

   pthread_mutex_lock(&g_hwm_mu);
   const vault_custody_provider_t *provider = g_custody;
   int (*preflight)(void *, const void *, size_t, uint64_t *) =
       provider ? provider->authorization_preflight_current : NULL;
   void *ctx = provider ? provider->ctx : NULL;
   pthread_mutex_unlock(&g_hwm_mu);
   int rc =
       preflight ? preflight(ctx, secret, secret_len, generation) : VAULT_CUSTODY_AUTH_UNSUPPORTED;
   if (rc < VAULT_CUSTODY_AUTHORIZED || rc > VAULT_CUSTODY_AUTH_UNSUPPORTED)
      rc = VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   if (rc != VAULT_CUSTODY_AUTHORIZED || *generation == 0 || *generation > INT64_MAX)
   {
      *generation = 0;
      return rc == VAULT_CUSTODY_AUTHORIZED ? VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE
                                            : (vault_custody_auth_result_t)rc;
   }
   return VAULT_CUSTODY_AUTHORIZED;
}

int vault_unseal(const void *params, size_t len)
{
   if (g_fork_child_invalid || g_owned_guard || g_owned_use || ensure_atfork() != 0)
      return -1;
   pthread_rwlock_wrlock(&g_use_lock);
   int rc = custody_unseal_unlocked(params, len);
   pthread_rwlock_unlock(&g_use_lock);
   return rc;
}

int vault_seal(void)
{
   if (g_fork_child_invalid || g_owned_guard || g_owned_use || ensure_atfork() != 0)
      return -1;
   pthread_rwlock_wrlock(&g_use_lock);
   int rc = custody_seal_failclosed_unlocked();
   pthread_rwlock_unlock(&g_use_lock);
   return rc;
}

uint64_t vault_use_epoch_snapshot(void)
{
   if (g_fork_child_invalid || g_owned_guard || g_owned_use || ensure_atfork() != 0)
      return 0;
   pthread_rwlock_rdlock(&g_use_lock);
   uint64_t epoch = g_use_epoch;
   pthread_rwlock_unlock(&g_use_lock);
   return epoch;
}

int vault_use_begin(uint64_t expected_epoch, uint64_t admitted_primary_epoch,
                    uint8_t kek[VAULT_KEK_LEN])
{
   if (!kek)
      return -1;
   /* Every rejection path leaves caller-owned key storage clean, including
    * malformed epochs rejected before custody is consulted. */
   OPENSSL_cleanse(kek, VAULT_KEK_LEN);
   if (g_fork_child_invalid || g_owned_guard || g_owned_use || ensure_atfork() != 0 ||
       !expected_epoch || expected_epoch == UINT64_MAX || !admitted_primary_epoch ||
       admitted_primary_epoch > INT64_MAX || pthread_rwlock_rdlock(&g_use_lock) != 0)
      return -1;
   if (g_use_epoch != expected_epoch || !g_primary_epoch_initialized ||
       g_primary_seal_epoch != admitted_primary_epoch || custody_is_sealed_unlocked() ||
       custody_get_kek_unlocked(kek) != 0)
   {
      OPENSSL_cleanse(kek, VAULT_KEK_LEN);
      pthread_rwlock_unlock(&g_use_lock);
      return -1;
   }
   g_owned_use = 1;
   return 0;
}

void vault_use_end(void)
{
   if (!g_owned_use)
      return;
   g_owned_use = 0;
   pthread_rwlock_unlock(&g_use_lock);
}

int vault_primary_epoch_initialize(uint64_t primary_epoch)
{
   if (g_fork_child_invalid || g_owned_guard || g_owned_use)
      return VAULT_MAINTENANCE_BUSY;
   if (ensure_atfork() != 0 || !primary_epoch || primary_epoch > INT64_MAX ||
       pthread_rwlock_wrlock(&g_use_lock) != 0)
      return VAULT_MAINTENANCE_INVALID;
   int rc = VAULT_MAINTENANCE_ERROR;
   pthread_mutex_lock(&g_maintenance_mu);
   int guard_active = g_maintenance.token != NULL;
   pthread_mutex_unlock(&g_maintenance_mu);
   if (!guard_active)
   {
      if (!g_primary_epoch_initialized)
      {
         g_primary_seal_epoch = primary_epoch;
         g_primary_epoch_initialized = 1;
         rc = VAULT_MAINTENANCE_OK;
      }
      else if (g_primary_seal_epoch == primary_epoch)
         rc = VAULT_MAINTENANCE_OK;
      else
         rc = VAULT_MAINTENANCE_EPOCH;
   }
   pthread_rwlock_unlock(&g_use_lock);
   return rc;
}

int vault_maintenance_guard_begin(vault_maintenance_guard_t **guard)
{
   if (g_fork_child_invalid || !guard || *guard || g_owned_use || ensure_atfork() != 0)
      return VAULT_MAINTENANCE_INVALID;
   if (g_owned_guard)
      return VAULT_MAINTENANCE_BUSY;

   int prior = PTHREAD_CANCEL_ENABLE;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &prior) != 0)
      return VAULT_MAINTENANCE_ERROR;
   uint8_t *arena = NULL;
   size_t mapped = 0;
   if (maintenance_arena_new(&arena, &mapped) != 0 || pthread_rwlock_wrlock(&g_use_lock) != 0)
   {
      maintenance_arena_free(arena, mapped);
      (void)pthread_setcancelstate(prior, NULL);
      return VAULT_MAINTENANCE_ERROR;
   }

   pthread_mutex_lock(&g_maintenance_mu);
   if (g_next_guard_token > (UINTPTR_MAX - 1) / 2)
   {
      pthread_mutex_unlock(&g_maintenance_mu);
      pthread_rwlock_unlock(&g_use_lock);
      maintenance_arena_free(arena, mapped);
      (void)pthread_setcancelstate(prior, NULL);
      return VAULT_MAINTENANCE_ERROR;
   }
   uintptr_t token_value = (g_next_guard_token++ << 1) | 1;
   vault_maintenance_guard_t *token = (vault_maintenance_guard_t *)token_value;
   g_maintenance.token = token;
   g_maintenance.owner = pthread_self();
   g_maintenance.prior_cancel_state = prior;
   g_maintenance.arena = arena;
   g_maintenance.arena_len = mapped;
   g_owned_guard = token;
   *guard = token;
   pthread_mutex_unlock(&g_maintenance_mu);
   return VAULT_MAINTENANCE_OK;
}

int vault_maintenance_guard_sync_primary_epoch(vault_maintenance_guard_t *guard,
                                               uint64_t primary_epoch)
{
   int valid = maintenance_validate(guard);
   if (valid != VAULT_MAINTENANCE_OK)
      return valid;
   if (!primary_epoch || primary_epoch > INT64_MAX)
      return VAULT_MAINTENANCE_EPOCH;
   if (g_primary_epoch_initialized && primary_epoch < g_primary_seal_epoch)
      return VAULT_MAINTENANCE_EPOCH;
   if (g_primary_epoch_initialized && primary_epoch == g_primary_seal_epoch)
      return VAULT_MAINTENANCE_OK;
   if (g_use_epoch == UINT64_MAX)
      return VAULT_MAINTENANCE_EPOCH;
   g_primary_seal_epoch = primary_epoch;
   g_primary_epoch_initialized = 1;
   g_use_epoch++;
   return VAULT_MAINTENANCE_OK;
}

int vault_maintenance_guard_with_active_kek(vault_maintenance_guard_t *guard,
                                            vault_maintenance_kek_fn callback, void *ctx)
{
   int valid = maintenance_validate(guard);
   if (valid != VAULT_MAINTENANCE_OK || !callback)
      return valid == VAULT_MAINTENANCE_OK ? VAULT_MAINTENANCE_INVALID : valid;
   if (custody_is_sealed_unlocked())
      return VAULT_MAINTENANCE_SEALED;

   uint8_t *arena;
   size_t mapped;
   pthread_mutex_lock(&g_maintenance_mu);
   g_maintenance.callback_active = 1;
   arena = g_maintenance.arena;
   mapped = g_maintenance.arena_len;
   pthread_mutex_unlock(&g_maintenance_mu);
   OPENSSL_cleanse(arena, mapped);
   if (custody_get_kek_unlocked(arena) != 0)
   {
      OPENSSL_cleanse(arena, mapped);
      pthread_mutex_lock(&g_maintenance_mu);
      g_maintenance.callback_active = 0;
      pthread_mutex_unlock(&g_maintenance_mu);
      return VAULT_MAINTENANCE_ERROR;
   }
   int rc = callback(arena, ctx);
   if (g_fork_child_invalid)
   {
      /* The kernel wiped this mapping before the child handler ran. Do not
       * touch inherited mutexes or attempt to release the parent's guard. */
      return VAULT_MAINTENANCE_INVALID;
   }
   OPENSSL_cleanse(arena, mapped);
   pthread_mutex_lock(&g_maintenance_mu);
   g_maintenance.callback_active = 0;
   pthread_mutex_unlock(&g_maintenance_mu);
   return rc;
}

int vault_maintenance_guard_unseal(vault_maintenance_guard_t *guard, const void *params, size_t len)
{
   int valid = maintenance_validate(guard);
   return valid == VAULT_MAINTENANCE_OK ? custody_unseal_unlocked(params, len) : valid;
}

vault_custody_auth_result_t vault_maintenance_guard_unseal_typed(vault_maintenance_guard_t *guard,
                                                                 const void *params, size_t len)
{
   int valid = maintenance_validate(guard);
   if (valid != VAULT_MAINTENANCE_OK || !params || len == 0 || len > VAULT_CUSTODY_AUTH_SECRET_MAX)
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   return custody_unseal_typed_unlocked(params, len);
}

int vault_maintenance_guard_seal(vault_maintenance_guard_t *guard)
{
   int valid = maintenance_validate(guard);
   return valid == VAULT_MAINTENANCE_OK ? custody_seal_failclosed_unlocked() : valid;
}

int vault_maintenance_guard_end(vault_maintenance_guard_t **guard)
{
   if (!guard)
      return VAULT_MAINTENANCE_INVALID;
   if (g_fork_child_invalid)
   {
      *guard = NULL;
      return VAULT_MAINTENANCE_INVALID;
   }
   int valid = maintenance_validate(*guard);
   if (valid != VAULT_MAINTENANCE_OK)
      return valid;

   int seal_rc = custody_seal_failclosed_unlocked();
   uint8_t *arena;
   size_t mapped;
   int prior;
   pthread_mutex_lock(&g_maintenance_mu);
   arena = g_maintenance.arena;
   mapped = g_maintenance.arena_len;
   prior = g_maintenance.prior_cancel_state;
   memset(&g_maintenance, 0, sizeof(g_maintenance));
   g_owned_guard = NULL;
   *guard = NULL;
   pthread_mutex_unlock(&g_maintenance_mu);
   maintenance_arena_free(arena, mapped);
   pthread_rwlock_unlock(&g_use_lock);
   (void)pthread_setcancelstate(prior, NULL);
   return seal_rc == 0 ? VAULT_MAINTENANCE_OK : VAULT_MAINTENANCE_ERROR;
}

int vault_maintenance_guard_end_operational(vault_maintenance_guard_t **guard,
                                            uint64_t committed_primary_epoch)
{
   if (!guard)
      return VAULT_MAINTENANCE_INVALID;
   int valid = maintenance_validate(*guard);
   if (valid != VAULT_MAINTENANCE_OK)
      return valid;

   int failure = VAULT_MAINTENANCE_OK;
   if (!committed_primary_epoch || committed_primary_epoch > INT64_MAX ||
       !g_primary_epoch_initialized || g_primary_seal_epoch != committed_primary_epoch ||
       g_use_epoch == UINT64_MAX)
      failure = VAULT_MAINTENANCE_EPOCH;
   else if (custody_is_sealed_unlocked())
      failure = VAULT_MAINTENANCE_SEALED;
   else
   {
      uint8_t *arena;
      size_t mapped;
      pthread_mutex_lock(&g_maintenance_mu);
      arena = g_maintenance.arena;
      mapped = g_maintenance.arena_len;
      pthread_mutex_unlock(&g_maintenance_mu);
      OPENSSL_cleanse(arena, mapped);
      if (custody_get_kek_unlocked(arena) != 0 || custody_is_sealed_unlocked())
         failure = VAULT_MAINTENANCE_ERROR;
      OPENSSL_cleanse(arena, mapped);
   }

   if (failure != VAULT_MAINTENANCE_OK)
   {
      int end_rc = vault_maintenance_guard_end(guard);
      return end_rc == VAULT_MAINTENANCE_OK ? failure : VAULT_MAINTENANCE_ERROR;
   }

   /* Publish a fresh local admission generation immediately before releasing
    * the exclusive writer.  Readers can only observe the increment after the
    * rwlock acquire/release edge and must also present the committed epoch. */
   g_use_epoch++;
   uint8_t *arena;
   size_t mapped;
   int prior;
   pthread_mutex_lock(&g_maintenance_mu);
   arena = g_maintenance.arena;
   mapped = g_maintenance.arena_len;
   prior = g_maintenance.prior_cancel_state;
   memset(&g_maintenance, 0, sizeof(g_maintenance));
   g_owned_guard = NULL;
   *guard = NULL;
   pthread_mutex_unlock(&g_maintenance_mu);
   maintenance_arena_free(arena, mapped);
   pthread_rwlock_unlock(&g_use_lock);
   (void)pthread_setcancelstate(prior, NULL);
   return VAULT_MAINTENANCE_OK;
}

int vault_hwm_read(const char *key_id, uint64_t *version, uint8_t *att, size_t att_cap,
                   size_t *att_len)
{
   if (version)
      *version = 0;
   if (att_len)
      *att_len = 0;
   if (g_fork_child_invalid || ensure_atfork() != 0 || !key_id || !key_id[0] || !version || !att ||
       att_cap == 0 || !att_len)
   {
      if (att && att_cap)
         OPENSSL_cleanse(att, att_cap);
      return -1;
   }
   pthread_mutex_lock(&g_hwm_mu);
   int rc = (!g_custody->hwm_read || !g_custody->hwm_cas)
                ? -1
                : g_custody->hwm_read(g_custody->ctx, key_id, version, att, att_cap, att_len);
   pthread_mutex_unlock(&g_hwm_mu);
   if (rc != 0 || *att_len == 0 || *att_len > att_cap)
   {
      OPENSSL_cleanse(att, att_cap);
      *version = 0;
      *att_len = 0;
      return -1;
   }
   return 0;
}

int vault_hwm_cas(const char *key_id, uint64_t expected, uint64_t next, uint8_t *att,
                  size_t att_cap, size_t *att_len)
{
   if (att_len)
      *att_len = 0;
   if (g_fork_child_invalid || ensure_atfork() != 0 || !key_id || !key_id[0] ||
       expected == UINT64_MAX || next != expected + 1 || !att || att_cap == 0 || !att_len)
   {
      if (att && att_cap)
         OPENSSL_cleanse(att, att_cap);
      return -1;
   }
   pthread_mutex_lock(&g_hwm_mu);
   int rc = (!g_custody->hwm_read || !g_custody->hwm_cas)
                ? -1
                : g_custody->hwm_cas(g_custody->ctx, key_id, expected, next, att, att_cap, att_len);
   pthread_mutex_unlock(&g_hwm_mu);
   if (rc != 0 || *att_len == 0 || *att_len > att_cap)
   {
      OPENSSL_cleanse(att, att_cap);
      *att_len = 0;
      return -1;
   }
   return 0;
}

int vault_hwm_verify(const char *key_id, uint64_t version, const uint8_t *att, size_t att_len)
{
   if (g_fork_child_invalid || ensure_atfork() != 0 || !key_id || !key_id[0] || !version || !att ||
       !att_len)
      return -1;
   pthread_mutex_lock(&g_hwm_mu);
   int rc = (!g_custody->hwm_read || !g_custody->hwm_cas || !g_custody->hwm_verify)
                ? -1
                : g_custody->hwm_verify(g_custody->ctx, key_id, version, att, att_len);
   pthread_mutex_unlock(&g_hwm_mu);
   return rc == 0 ? 0 : -1;
}
