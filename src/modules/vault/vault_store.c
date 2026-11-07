#include <aimee/core/codec/base64.h>
#include <limits.h>
/* vault_store.c: the 0600 per-principal file substrate for the credential vault
 * (WP-C.1). Stores only ciphertext (envelope per vault_crypto). The filename is
 * the URL-safe base64 of the principal so an attacker-influenced webuser name
 * cannot traverse the path or collide with another principal. See vault_store.h.
 */
#include "vault_store.h"
#include "vault_internal.h" /* vault_store_backend_t seam */
#include "vault_crypto.h"
#include "vault_kek_check.h"
#include "vault_service.h" /* reserved instance principal */
#include "config.h"        /* config_default_dir */
#include "platform_path.h" /* platform_mkdir_p */
#include "cJSON.h"
#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <dirent.h>
#include <errno.h>
#include <sys/file.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Serializes the load->mutate->write critical section so concurrent set/set or
 * set/delete for the same principal cannot lose an update (the file is a single
 * read-modify-write per op). One global lock is sufficient at the vault's low
 * write rate; reads rely on the atomic rename for a consistent snapshot. */
static pthread_mutex_t g_vault_write_mu = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int g_fenced_use_callback;
int vault_fenced_use_callback_active(void)
{
   return g_fenced_use_callback;
}

#define VAULT_FILE_VERSION 1
#define VAULT_SECRET_MAX   (64 * 1024) /* opaque OAuth docs and SSH credentials */
#define VAULT_AAD_MAX      320         /* "principal|agent|cred" (160 + 64 + 64 + seps) */

/* Deployment compatibility: v2 includes permanent value-free retirement. */
__attribute__((used, externally_visible, retain))
const unsigned aimee_vault_immutable_custody_contract_v2 = 2;

#define IMMUTABLE_MARKER  "immutable_generation_v1"
#define RETIRED_MARKER    "immutable_retired_v1"
#define IMMUTABLE_AAD_MAX (VAULT_AAD_MAX + 80)
static int generation_valid(const char *s)
{
   if (!s || strnlen(s, VAULT_IMMUTABLE_GENERATION_CAP) != 32)
      return 0;
   int nonzero = 0;
   for (size_t i = 0; i < 32; i++)
   {
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
      nonzero |= s[i] != '0';
   }
   return nonzero;
}
/* 0 legacy, 1 live immutable, 2 value-free retirement, -1 malformed marker. */
static int immutable_state(const cJSON *entry)
{
   const cJSON *generation = cJSON_GetObjectItemCaseSensitive(entry, IMMUTABLE_MARKER);
   const cJSON *retired = cJSON_GetObjectItemCaseSensitive(entry, RETIRED_MARKER);
   if (!generation)
      return retired ? -1 : 0;
   if (!generation_valid(cJSON_GetStringValue(generation)))
      return -1;
   if (!retired)
      return 1;
   if (!cJSON_IsTrue(retired) || cJSON_GetArraySize(entry) != 4 ||
       !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(entry, "agent")) ||
       !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(entry, "cred")))
      return -1;
   return 2;
}
/* Detach + free the (agent,cred) entry from the creds array if present. */
static void remove_cred(cJSON *creds, cJSON *entry)
{
   if (!creds || !entry)
      return;
   int idx = 0;
   cJSON *e = NULL;
   cJSON_ArrayForEach(e, creds)
   {
      if (e == entry)
      {
         cJSON_DeleteItemFromArray(creds, idx);
         return;
      }
      idx++;
   }
}

/* Vault records keep their historical unpadded URL decoding profile. */
static char *b64url_encode(const uint8_t *in, size_t len)
{
   size_t capacity = aimee_codec_base64_capacity(len, AIMEE_BASE64_URL), written = 0;
   if (!capacity)
      return NULL;
   char *out = malloc(capacity);
   if (!out)
      return NULL;
   if (!aimee_codec_base64_encode(in, len, out, capacity, AIMEE_BASE64_URL, &written))
   {
      free(out);
      return NULL;
   }
   return out;
}

static int b64url_decode(const char *in, uint8_t *out, size_t out_cap)
{
   size_t written = 0;
   if (!in ||
       !aimee_codec_base64_decode(in, strlen(in), out, out_cap, AIMEE_BASE64_URL_COMPAT,
                                  &written) ||
       written > INT_MAX)
      return -1;
   return (int)written;
}

/* ── Vault file path ──────────────────────────────────────────────────────── */
static int vault_dir(char *out, size_t cap)
{
   const char *base = config_default_dir();
   if (!base || !base[0])
      return -1;
   if ((size_t)snprintf(out, cap, "%s/.vault", base) >= cap)
      return -1;
   return 0;
}

static int vault_file_path(const char *principal, char *out, size_t cap)
{
   char dir[1024];
   if (vault_dir(dir, sizeof(dir)) != 0)
      return -1;
   char *enc = b64url_encode((const uint8_t *)principal, strlen(principal));
   if (!enc)
      return -1;
   int rc = (size_t)snprintf(out, cap, "%s/%s.json", dir, enc) >= cap ? -1 : 0;
   free(enc);
   return rc;
}

/* Serialize mutations across independently executing core resource helpers as
 * well as threads. Atomic rename alone prevents torn reads, not lost updates. */
static int vault_write_lock(void)
{
   if (g_fenced_use_callback)
      return -1;
   pthread_mutex_lock(&g_vault_write_mu);
   char dir[1024], path[1280];
   if (vault_dir(dir, sizeof(dir)) != 0 || platform_mkdir_p(dir, 0700) != 0 ||
       (size_t)snprintf(path, sizeof(path), "%s/.write.lock", dir) >= sizeof(path))
   {
      pthread_mutex_unlock(&g_vault_write_mu);
      return -1;
   }
   int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
   struct stat st;
   if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
       (st.st_mode & 0077) != 0 || flock(fd, LOCK_EX) != 0)
   {
      if (fd >= 0)
         close(fd);
      pthread_mutex_unlock(&g_vault_write_mu);
      return -1;
   }
   return fd;
}

static void vault_write_unlock(int fd)
{
   close(fd);
   pthread_mutex_unlock(&g_vault_write_mu);
}

/* A corrupt, unreadable or non-regular file is never an absent credential. */
static int vault_file_missing(const char *principal)
{
   char path[1280];
   struct stat st;
   return vault_file_path(principal, path, sizeof(path)) == 0 && lstat(path, &st) != 0 &&
          errno == ENOENT;
}

/* ── File I/O ─────────────────────────────────────────────────────────────── */
#define VAULT_FILE_BYTES_MAX (8u * 1024u * 1024u)
static char *read_whole_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long sz = ftell(f);
   if (sz < 0 || (unsigned long)sz > VAULT_FILE_BYTES_MAX)
   {
      fclose(f);
      return NULL;
   }
   rewind(f);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[n] = '\0';
   return buf;
}

/* Persist the .vault directory entry in the configured existing home as well
 * as the vault file's entry in .vault. The service owner supplies a durable
 * configured home, like the parent directory of any other database. */
static int sync_vault_parent(void)
{
   const char *base = config_default_dir();
   if (!base || !base[0])
      return -1;
   int fd = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   if (fd < 0)
      return -1;
   int rc = fsync(fd);
   if (close(fd) != 0)
      rc = -1;
   return rc == 0 ? 0 : -1;
}

/* Atomically write `json` to the principal's vault file at 0600. */
static int write_vault_file(const char *principal, cJSON *root)
{
   char dir[1024], path[1280], tmp[1408];
   if (vault_dir(dir, sizeof(dir)) != 0 || vault_file_path(principal, path, sizeof(path)) != 0)
      return -1;
   if (platform_mkdir_p(dir, 0700) != 0)
      return -1;
   /* Unique per writer: pid + a process-wide atomic counter, so two threads of
    * the same process writing the same principal never share one tmp path (in
    * addition to the g_vault_write_mu serialization). */
   static atomic_uint s_tmp_seq = 0;
   unsigned seq = atomic_fetch_add(&s_tmp_seq, 1);
   if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%u", path, (int)getpid(), seq) >= sizeof(tmp))
      return -1;

   char *txt = cJSON_PrintUnformatted(root);
   if (!txt)
      return -1;
   size_t len = strlen(txt);
   if (len > VAULT_FILE_BYTES_MAX)
   {
      OPENSSL_cleanse(txt, len);
      free(txt);
      return -1;
   }

   int rc = -1;
   int fd = open(tmp, O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (fd >= 0)
   {
      ssize_t w = write(fd, txt, len);
      if (w == (ssize_t)len && fsync(fd) == 0)
         rc = 0;
      close(fd);
      if (rc == 0 && rename(tmp, path) != 0)
         rc = -1;
      if (rc == 0)
      {
         int dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
         if (dir_fd < 0 || fsync(dir_fd) != 0)
            rc = -1;
         if (dir_fd >= 0)
            close(dir_fd);
         if (rc == 0 && sync_vault_parent() != 0)
            rc = -1;
      }
      if (rc != 0)
         unlink(tmp);
   }
   /* The serialized JSON carries no plaintext/keys (only ciphertext), but cleanse
    * anyway — defense in depth for the heap buffer. */
   OPENSSL_cleanse(txt, strlen(txt));
   free(txt);
   return rc;
}

/* Load the principal's vault JSON, or NULL if absent/corrupt. */
static cJSON *load_vault(const char *principal)
{
   char path[1280];
   if (vault_file_path(principal, path, sizeof(path)) != 0)
      return NULL;
   char *txt = read_whole_file(path);
   if (!txt)
      return NULL;
   cJSON *root = cJSON_Parse(txt);
   OPENSSL_cleanse(txt, strlen(txt));
   free(txt);
   return root;
}

static cJSON *creds_array(cJSON *root)
{
   cJSON *creds = cJSON_GetObjectItemCaseSensitive(root, "creds");
   return cJSON_IsArray(creds) ? creds : NULL;
}

static cJSON *find_cred(cJSON *root, const char *agent, const char *cred)
{
   cJSON *creds = creds_array(root);
   if (!creds)
      return NULL;
   cJSON *e = NULL;
   cJSON_ArrayForEach(e, creds)
   {
      cJSON *ja = cJSON_GetObjectItemCaseSensitive(e, "agent");
      cJSON *jc = cJSON_GetObjectItemCaseSensitive(e, "cred");
      if (cJSON_IsString(ja) && cJSON_IsString(jc) && strcmp(ja->valuestring, agent) == 0 &&
          strcmp(jc->valuestring, cred) == 0)
         return e;
   }
   return NULL;
}

/* Build the AEAD AAD "principal|agent|cred" into out; returns its length or -1.
 *
 * REFUSES a component containing the '|' delimiter. Without that check two
 * different slots can produce the same AAD — agent "claude" + cred "api_key|x"
 * and agent "claude|api_key" + cred "x" both yield "<p>|claude|api_key|x" — and
 * the AAD then no longer binds a ciphertext to its (agent, cred) slot, which is
 * the entire property it exists to provide. vault_aad_build_v1_safe (the
 * Postgres backend's builder) rejects '|' for exactly this reason; this is the
 * file backend's half of the same rule.
 *
 * Refusing the input rather than escaping or length-prefixing is deliberate: it
 * leaves the AAD byte-identical for every valid entry, so nothing already
 * encrypted at rest has to be re-wrapped. */
static int build_aad(const char *principal, const char *agent, const char *cred, char *out,
                     size_t cap)
{
   if (!principal || !agent || !cred || strchr(principal, '|') || strchr(agent, '|') ||
       strchr(cred, '|'))
      return -1;
   int n = snprintf(out, cap, "%s|%s|%s", principal, agent, cred);
   return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

static int generation_aad(const char *principal, const char *agent, const char *cred,
                          const char *generation, char *out, size_t cap)
{
   char legacy[VAULT_AAD_MAX];
   if (build_aad(principal, agent, cred, legacy, sizeof(legacy)) < 0)
      return -1;
   if (!generation)
      return build_aad(principal, agent, cred, out, cap);
   if (!generation_valid(generation))
      return -1;
   int n = snprintf(out, cap, "vault-immutable-v1|%s|%s", generation, legacy);
   return n >= 0 && (size_t)n < cap ? n : -1;
}

static int jsonfile_get_or_create_salt(void *ctx, const char *principal,
                                       uint8_t salt[VAULT_SALT_LEN])
{
   if (!principal || !principal[0] || !salt)
      return -1;

   /* Held across the load->(maybe create+write) so two concurrent unlocks of the
    * same new principal cannot each create a file with a different salt (the
    * second would orphan the first's cached KEK). */
   int lock_fd = vault_write_lock();
   if (lock_fd < 0)
      return -1;
   int rc = -1;
   cJSON *root = load_vault(principal);
   if (root)
   {
      cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "salt");
      rc = (cJSON_IsString(js) &&
            b64url_decode(js->valuestring, salt, VAULT_SALT_LEN) == VAULT_SALT_LEN)
               ? 0
               : -1;
      goto done;
   }

   if (!vault_file_missing(principal))
      goto done;

   /* No file yet: create one with a fresh random salt + empty creds list. */
   if (vault_crypto_random(salt, VAULT_SALT_LEN) != 0)
      goto done;
   root = cJSON_CreateObject();
   if (!root)
      goto done;
   {
      char *salt_b64 = b64url_encode(salt, VAULT_SALT_LEN);
      if (!salt_b64)
         goto done;
      cJSON_AddNumberToObject(root, "version", VAULT_FILE_VERSION);
      cJSON_AddStringToObject(root, "principal", principal);
      /* Record which KDF unlock will use: webuser: principals derive the KEK from
       * the login password via scrypt; uid: principals use HKDF over a root key. */
      cJSON_AddStringToObject(root, "kdf_version",
                              strncmp(principal, "webuser:", 8) == 0 ? VAULT_KDF_VERSION_SCRYPT
                                                                     : VAULT_KDF_VERSION);
      cJSON_AddStringToObject(root, "salt", salt_b64);
      cJSON_AddArrayToObject(root, "creds");
      free(salt_b64);
      rc = write_vault_file(principal, root);
   }

done:
   cJSON_Delete(root);
   vault_write_unlock(lock_fd);
   if (rc != 0)
      OPENSSL_cleanse(salt, VAULT_SALT_LEN);
   return rc;
}

/* Add a base64'd binary field to a cred object. */
static int add_b64_field(cJSON *obj, const char *name, const uint8_t *bin, size_t len)
{
   char *enc = b64url_encode(bin, len);
   if (!enc)
      return -1;
   int rc = cJSON_AddStringToObject(obj, name, enc) ? 0 : -1;
   free(enc);
   return rc;
}

/* Shared set implementation. `kek` (user wrap) and `server_kek` (server wrap,
 * "wrapped_dek_server", WP-C.4 dual-access) are each optional, but at least one
 * must be present: kek-only = vault_store_set, both = _set_dual, server_kek-only
 * = _set_server (a server-readable cred set with no user KEK / unlock). */
static int vault_stage_credential(cJSON *root, const char *principal, const uint8_t *kek,
                                  const uint8_t *server_kek, const char *agent, const char *cred,
                                  const char *secret, const char *generation)
{
   /* No normal mutable API may replace opt-in immutable content. */
   cJSON *existing = find_cred(root, agent, cred);
   if (existing && immutable_state(existing) != 0)
      return -1;
   size_t pt_len = strlen(secret);
   int rc = -1;
   uint8_t dek[VAULT_DEK_LEN] = {0};
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN], wrapped_server[VAULT_WRAPPED_DEK_LEN],
       nonce[VAULT_GCM_NONCE_LEN], tag[VAULT_GCM_TAG_LEN];
   uint8_t *ct = malloc(pt_len ? pt_len : 1);
   char aad[IMMUTABLE_AAD_MAX];
   if (!ct)
      goto out;
   if (generation_aad(principal, agent, cred, generation, aad, sizeof(aad)) < 0)
      goto out;
   if (vault_crypto_random(dek, sizeof(dek)) != 0)
      goto out;
   if (vault_secret_encrypt(dek, (const uint8_t *)aad, strlen(aad), (const uint8_t *)secret, pt_len,
                            nonce, ct, tag) != 0)
      goto out;
   /* User wrap: optional (a server-only entry has no wrapped_dek — it is read
    * back exclusively via the server wrap). Fail-closed on a wrap error. */
   if (kek && vault_dek_wrap(kek, dek, wrapped) != 0)
      goto out;
   /* Dual-access: a second wrap of the same DEK under the server KEK. Fail-closed
    * — if requested but it can't be produced, store nothing (never a credential
    * the server cannot later read). */
   if (server_kek && vault_dek_wrap(server_kek, dek, wrapped_server) != 0)
      goto out;

   /* Build the cred object, replacing any existing entry for (agent,cred). */
   {
      cJSON *creds = creds_array(root);
      if (!creds)
         goto out;
      remove_cred(creds, find_cred(root, agent, cred)); /* replace any existing entry */
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
         goto out;
      if ((generation && !cJSON_AddStringToObject(obj, IMMUTABLE_MARKER, generation)) ||
          !cJSON_AddStringToObject(obj, "agent", agent) ||
          !cJSON_AddStringToObject(obj, "cred", cred) ||
          (kek && add_b64_field(obj, "wrapped_dek", wrapped, sizeof(wrapped)) != 0) ||
          add_b64_field(obj, "nonce", nonce, sizeof(nonce)) != 0 ||
          add_b64_field(obj, "ciphertext", ct, pt_len) != 0 ||
          add_b64_field(obj, "tag", tag, sizeof(tag)) != 0 ||
          (server_kek &&
           add_b64_field(obj, "wrapped_dek_server", wrapped_server, sizeof(wrapped_server)) != 0))
      {
         cJSON_Delete(obj);
         goto out;
      }
      if (!cJSON_AddItemToArray(creds, obj))
      {
         cJSON_Delete(obj);
         goto out;
      }
   }
   rc = 0;

out:
   OPENSSL_cleanse(dek, sizeof(dek));
   if (ct)
   {
      if (pt_len)
         OPENSSL_cleanse(ct, pt_len);
      free(ct);
   }
   return rc;
}

static int vault_store_set_impl(const char *principal, const uint8_t *kek,
                                const uint8_t *server_kek, const char *agent, const char *cred,
                                const char *secret, int only_if_absent)
{
   if (!principal || (!kek && !server_kek) || !agent || !agent[0] || !cred || !cred[0] || !secret)
      return -1;
   if (strlen(secret) > VAULT_SECRET_MAX)
      return -1;

   int lock_fd = vault_write_lock();
   if (lock_fd < 0)
      return -1;
   cJSON *root = load_vault(principal);
   if (!root) /* must be unlocked/created first */
   {
      vault_write_unlock(lock_fd);
      return -1;
   }

   if (only_if_absent && find_cred(root, agent, cred))
   {
      cJSON_Delete(root);
      vault_write_unlock(lock_fd);
      return -1;
   }

   int rc = vault_stage_credential(root, principal, kek, server_kek, agent, cred, secret, NULL);
   if (!rc)
      rc = write_vault_file(principal, root);
   cJSON_Delete(root);
   vault_write_unlock(lock_fd);
   return rc;
}

static int jsonfile_set(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                        const char *agent, const char *cred, const char *secret)
{
   return vault_store_set_impl(principal, kek, NULL, agent, cred, secret, 0);
}

static int jsonfile_set_dual(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                             const uint8_t server_kek[VAULT_KEK_LEN], const char *agent,
                             const char *cred, const char *secret)
{
   if (!server_kek)
      return -1;
   return vault_store_set_impl(principal, kek, server_kek, agent, cred, secret, 0);
}

static int jsonfile_set_server(void *ctx, const char *principal,
                               const uint8_t server_kek[VAULT_KEK_LEN], const char *agent,
                               const char *cred, const char *secret)
{
   if (!server_kek)
      return -1;
   /* No user KEK: the entry carries only a server wrap, so it can be set without
    * an unlocked user vault and read back via vault_store_get_server. */
   return vault_store_set_impl(principal, NULL, server_kek, agent, cred, secret, 0);
}

static int jsonfile_get_from_snapshot(cJSON *root, const char *principal,
                                      const uint8_t kek[VAULT_KEK_LEN], const char *agent,
                                      const char *cred, char *out, size_t out_len)
{
   int rc = -1;
   uint8_t dek[VAULT_DEK_LEN] = {0};
   uint8_t *pt = NULL;
   size_t pt_alloc = 0;
   cJSON *e = find_cred(root, agent, cred);
   if (e && immutable_state(e) != 0)
      goto out;
   if (!e)
   {
      rc = VAULT_STORE_NO_ENTRY;
      goto out;
   }
   {
      cJSON *jw = cJSON_GetObjectItemCaseSensitive(e, "wrapped_dek");
      cJSON *jn = cJSON_GetObjectItemCaseSensitive(e, "nonce");
      cJSON *jc = cJSON_GetObjectItemCaseSensitive(e, "ciphertext");
      cJSON *jt = cJSON_GetObjectItemCaseSensitive(e, "tag");
      if (!cJSON_IsString(jw) || !cJSON_IsString(jn) || !cJSON_IsString(jc) || !cJSON_IsString(jt))
         goto out;
      uint8_t wrapped[VAULT_WRAPPED_DEK_LEN], nonce[VAULT_GCM_NONCE_LEN], tag[VAULT_GCM_TAG_LEN];
      if (b64url_decode(jw->valuestring, wrapped, sizeof(wrapped)) != VAULT_WRAPPED_DEK_LEN ||
          b64url_decode(jn->valuestring, nonce, sizeof(nonce)) != VAULT_GCM_NONCE_LEN ||
          b64url_decode(jt->valuestring, tag, sizeof(tag)) != VAULT_GCM_TAG_LEN)
         goto out;
      /* ciphertext length is variable. */
      size_t ct_cap = strlen(jc->valuestring); /* >= decoded length */
      pt = malloc(ct_cap + 1);
      pt_alloc = ct_cap + 1;
      uint8_t *ctbuf = malloc(ct_cap + 1);
      if (!pt || !ctbuf)
      {
         free(ctbuf);
         goto out;
      }
      int ct_len = b64url_decode(jc->valuestring, ctbuf, ct_cap + 1);
      char aad[VAULT_AAD_MAX];
      if (ct_len < 0 || build_aad(principal, agent, cred, aad, sizeof(aad)) < 0 ||
          (size_t)ct_len >= out_len)
      {
         free(ctbuf);
         goto out;
      }
      if (vault_dek_unwrap(kek, wrapped, dek) != 0 ||
          vault_secret_decrypt(dek, (const uint8_t *)aad, strlen(aad), nonce, ctbuf, (size_t)ct_len,
                               tag, pt) != 0)
      {
         OPENSSL_cleanse(ctbuf, ct_cap + 1);
         free(ctbuf);
         goto out; /* fail-closed: wrong KEK / tamper / AAD mismatch */
      }
      memcpy(out, pt, (size_t)ct_len);
      out[ct_len] = '\0';
      OPENSSL_cleanse(ctbuf, ct_cap + 1);
      free(ctbuf);
      rc = 0;
   }

out:
   OPENSSL_cleanse(dek, sizeof(dek));
   if (pt)
   {
      OPENSSL_cleanse(pt, pt_alloc);
      free(pt);
   }
   if (rc != 0 && out && out_len)
      OPENSSL_cleanse(out, out_len);
   return rc;
}

static int jsonfile_get(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                        const char *agent, const char *cred, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!principal || !kek || !agent || !cred || !out || !out_len)
      return -1;

   cJSON *root = load_vault(principal);
   if (!root)
      return vault_file_missing(principal) ? VAULT_STORE_NO_ENTRY : -1;

   int rc = jsonfile_get_from_snapshot(root, principal, kek, agent, cred, out, out_len);
   cJSON_Delete(root);
   return rc;
}

static int jsonfile_get_server_snapshot(cJSON *root, const char *principal,
                                        const uint8_t server_kek[VAULT_KEK_LEN], const char *agent,
                                        const char *cred, char *out, size_t out_len,
                                        size_t *plain_length)
{
   if (plain_length)
      *plain_length = 0;
   if (out && out_len)
      out[0] = '\0';
   int rc = -1;
   uint8_t dek[VAULT_DEK_LEN] = {0};
   uint8_t *pt = NULL;
   size_t pt_alloc = 0;
   cJSON *e = find_cred(root, agent, cred);
   if (!e)
   {
      rc = VAULT_STORE_NO_ENTRY;
      goto out;
   }
   {
      cJSON *jw = cJSON_GetObjectItemCaseSensitive(e, "wrapped_dek_server");
      cJSON *jn = cJSON_GetObjectItemCaseSensitive(e, "nonce");
      cJSON *jc = cJSON_GetObjectItemCaseSensitive(e, "ciphertext");
      cJSON *jt = cJSON_GetObjectItemCaseSensitive(e, "tag");
      /* A legacy entry (pre-dual-wrap) has no server wrap: report NO_ENTRY so the
       * caller falls back / backfills at the next user unlock, never an error. */
      if (!cJSON_IsString(jw))
      {
         rc = VAULT_STORE_NO_ENTRY;
         goto out;
      }
      if (!cJSON_IsString(jn) || !cJSON_IsString(jc) || !cJSON_IsString(jt))
         goto out;
      uint8_t wrapped[VAULT_WRAPPED_DEK_LEN], nonce[VAULT_GCM_NONCE_LEN], tag[VAULT_GCM_TAG_LEN];
      if (b64url_decode(jw->valuestring, wrapped, sizeof(wrapped)) != VAULT_WRAPPED_DEK_LEN ||
          b64url_decode(jn->valuestring, nonce, sizeof(nonce)) != VAULT_GCM_NONCE_LEN ||
          b64url_decode(jt->valuestring, tag, sizeof(tag)) != VAULT_GCM_TAG_LEN)
         goto out;
      size_t ct_cap = strlen(jc->valuestring); /* >= decoded length */
      pt = malloc(ct_cap + 1);
      pt_alloc = ct_cap + 1;
      uint8_t *ctbuf = malloc(ct_cap + 1);
      if (!pt || !ctbuf)
      {
         free(ctbuf);
         goto out;
      }
      int ct_len = b64url_decode(jc->valuestring, ctbuf, ct_cap + 1);
      char aad[IMMUTABLE_AAD_MAX];
      const cJSON *marker = cJSON_GetObjectItemCaseSensitive(e, IMMUTABLE_MARKER);
      const char *generation = marker ? cJSON_GetStringValue(marker) : NULL;
      if (marker && !generation)
      {
         free(ctbuf);
         goto out;
      }
      if (ct_len < 0 || generation_aad(principal, agent, cred, generation, aad, sizeof(aad)) < 0 ||
          (size_t)ct_len >= out_len)
      {
         free(ctbuf);
         goto out;
      }
      if (vault_dek_unwrap(server_kek, wrapped, dek) != 0 ||
          vault_secret_decrypt(dek, (const uint8_t *)aad, strlen(aad), nonce, ctbuf, (size_t)ct_len,
                               tag, pt) != 0)
      {
         OPENSSL_cleanse(ctbuf, ct_cap + 1);
         free(ctbuf);
         goto out; /* fail-closed: wrong server KEK / tamper / AAD mismatch */
      }
      memcpy(out, pt, (size_t)ct_len);
      out[ct_len] = '\0';
      if (plain_length)
         *plain_length = (size_t)ct_len;
      OPENSSL_cleanse(ctbuf, ct_cap + 1);
      free(ctbuf);
      rc = 0;
   }

out:
   OPENSSL_cleanse(dek, sizeof(dek));
   if (pt)
   {
      OPENSSL_cleanse(pt, pt_alloc);
      free(pt);
   }
   if (rc != 0 && out && out_len)
      OPENSSL_cleanse(out, out_len);
   return rc;
}

static int jsonfile_get_server(void *ctx, const char *principal,
                               const uint8_t server_kek[VAULT_KEK_LEN], const char *agent,
                               const char *cred, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!principal || !server_kek || !agent || !cred || !out || !out_len)
      return -1;
   cJSON *root = load_vault(principal);
   if (!root)
      return vault_file_missing(principal) ? VAULT_STORE_NO_ENTRY : -1;
   cJSON *entry = find_cred(root, agent, cred);
   if (entry && immutable_state(entry) != 0)
   {
      OPENSSL_cleanse(out, out_len);
      cJSON_Delete(root);
      return -1;
   }
   int rc =
       jsonfile_get_server_snapshot(root, principal, server_kek, agent, cred, out, out_len, NULL);
   cJSON_Delete(root);
   return rc;
}

static int jsonfile_get_server_generation(void *ctx, const char *principal,
                                          const uint8_t server_kek[VAULT_KEK_LEN],
                                          const char *agent, const char *cred,
                                          const char *expected_generation, char *out,
                                          size_t out_len)
{
   (void)ctx;
   cJSON *root = load_vault(principal);
   if (!root)
      return vault_file_missing(principal) ? VAULT_STORE_NO_ENTRY : -1;
   cJSON *entry = find_cred(root, agent, cred);
   int state = entry ? immutable_state(entry) : 0;
   const char *generation =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, IMMUTABLE_MARKER));
   int rc = !entry ? VAULT_STORE_NO_ENTRY : state < 0 ? -1 : VAULT_STORE_GENERATION_MISMATCH;
   if (state > 0 && !CRYPTO_memcmp(generation, expected_generation, 32))
      rc = state == 2 ? VAULT_STORE_RETIRED
                      : jsonfile_get_server_snapshot(root, principal, server_kek, agent, cred, out,
                                                     out_len, NULL);
   cJSON_Delete(root);
   return rc;
}

static int jsonfile_use_server_generation_fenced(void *ctx, const char *principal,
                                                 const uint8_t server_kek[VAULT_KEK_LEN],
                                                 const char *agent, const char *cred,
                                                 const char *generation, char *out, size_t out_len,
                                                 vault_store_secret_use_fn use, void *use_context)
{
   int lock = vault_write_lock();
   if (lock < 0)
      return -1;
   int rc = jsonfile_get_server_generation(ctx, principal, server_kek, agent, cred, generation, out,
                                           out_len);
   if (!rc)
   {
      size_t length = strnlen(out, out_len);
      rc = length == out_len ? -1 : use(out, length, use_context) ? VAULT_STORE_USE_FAILED : 0;
   }
   vault_write_unlock(lock);
   return rc;
}

static int jsonfile_add_server_wraps(void *ctx, const char *principal,
                                     const uint8_t user_kek[VAULT_KEK_LEN],
                                     const uint8_t server_kek[VAULT_KEK_LEN])
{
   if (!principal || !user_kek || !server_kek)
      return -1;
   int lock_fd = vault_write_lock();
   if (lock_fd < 0)
      return -1;
   cJSON *root = load_vault(principal);
   if (!root)
   {
      vault_write_unlock(lock_fd);
      return 0; /* no vault -> nothing to backfill */
   }

   int rc = 0;
   int changed = 0;
   cJSON *creds = creds_array(root);
   cJSON *e = NULL;
   if (creds)
   {
      cJSON_ArrayForEach(e, creds)
      {
         int state = immutable_state(e);
         if (state < 0)
         {
            rc = -1;
            break;
         }
         if (state == 2)
            continue;

         if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(e, "wrapped_dek_server")))
            continue; /* already dual-wrapped */
         cJSON *jw = cJSON_GetObjectItemCaseSensitive(e, "wrapped_dek");
         uint8_t wrapped[VAULT_WRAPPED_DEK_LEN], dek[VAULT_DEK_LEN],
             rewrapped[VAULT_WRAPPED_DEK_LEN];
         if (!cJSON_IsString(jw) ||
             b64url_decode(jw->valuestring, wrapped, sizeof(wrapped)) != VAULT_WRAPPED_DEK_LEN ||
             vault_dek_unwrap(user_kek, wrapped, dek) != 0 ||
             vault_dek_wrap(server_kek, dek, rewrapped) != 0)
         {
            OPENSSL_cleanse(dek, sizeof(dek));
            rc = -1; /* wrong user KEK / tamper -> abort, write nothing */
            break;
         }
         OPENSSL_cleanse(dek, sizeof(dek));
         if (add_b64_field(e, "wrapped_dek_server", rewrapped, sizeof(rewrapped)) != 0)
         {
            rc = -1;
            break;
         }
         changed = 1;
      }
   }
   if (rc == 0 && changed)
      rc = write_vault_file(principal, root);
   cJSON_Delete(root);
   vault_write_unlock(lock_fd);
   return rc;
}

static int jsonfile_rekey_field(void *ctx, const char *principal, const char *field,
                                const uint8_t old_kek[VAULT_KEK_LEN],
                                const uint8_t new_kek[VAULT_KEK_LEN])
{
   if (!principal || !field || !field[0] || !old_kek || !new_kek)
      return -1;
   int lock_fd = vault_write_lock();
   if (lock_fd < 0)
      return -1;
   cJSON *root = load_vault(principal);
   if (!root)
   {
      vault_write_unlock(lock_fd);
      return 0; /* no vault for this principal -> nothing to re-wrap */
   }

   int rc = 0;
   int rewrapped_count = 0;
   cJSON *creds = creds_array(root);
   cJSON *e = NULL;
   if (creds)
   {
      cJSON_ArrayForEach(e, creds)
      {
         int state = immutable_state(e);
         if (state < 0)
         {
            rc = -1;
            break;
         }
         if (state == 2)
            continue;

         cJSON *jw = cJSON_GetObjectItemCaseSensitive(e, field);
         if (!cJSON_IsString(jw))
            continue; /* this cred has no such wrap — left untouched */
         uint8_t wrapped[VAULT_WRAPPED_DEK_LEN], dek[VAULT_DEK_LEN],
             rewrapped[VAULT_WRAPPED_DEK_LEN];
         if (b64url_decode(jw->valuestring, wrapped, sizeof(wrapped)) != VAULT_WRAPPED_DEK_LEN ||
             vault_dek_unwrap(old_kek, wrapped, dek) != 0 ||
             vault_dek_wrap(new_kek, dek, rewrapped) != 0)
         {
            OPENSSL_cleanse(dek, sizeof(dek));
            rc = -1; /* wrong old KEK / tamper -> abort, write nothing */
            break;
         }
         char *enc = b64url_encode(rewrapped, sizeof(rewrapped));
         OPENSSL_cleanse(dek, sizeof(dek));
         if (!enc)
         {
            rc = -1;
            break;
         }
         cJSON_SetValuestring(jw, enc); /* overwrite in place */
         free(enc);
         rewrapped_count++;
      }
   }
   if (rc == 0 && rewrapped_count > 0)
      rc = write_vault_file(principal, root);
   cJSON_Delete(root);
   vault_write_unlock(lock_fd);
   return rc == 0 ? rewrapped_count : -1;
}

static int jsonfile_list_principals(void *ctx, char (*out)[VAULT_PRINCIPAL_MAX], int max)
{
   if (!out || max <= 0)
      return -1;
   char dir[1024];
   if (vault_dir(dir, sizeof(dir)) != 0)
      return -1;
   DIR *d = opendir(dir);
   if (!d)
      return 0; /* no .vault dir yet -> no principals */
   int count = 0;
   struct dirent *de;
   while (count < max && (de = readdir(d)) != NULL)
   {
      const char *name = de->d_name;
      size_t len = strlen(name);
      /* vault files are "<b64url(principal)>.json"; skip ., .., and the
       * special master-key / non-JSON entries. */
      if (len <= 5 || strcmp(name + len - 5, ".json") != 0)
         continue;
      char b64[256];
      if (len - 5 >= sizeof(b64))
         continue;
      memcpy(b64, name, len - 5);
      b64[len - 5] = '\0';
      uint8_t decoded[VAULT_PRINCIPAL_MAX];
      int n = b64url_decode(b64, decoded, sizeof(decoded) - 1);
      if (n <= 0 || (size_t)n >= sizeof(decoded))
         continue; /* not a principal-named vault file */
      memcpy(out[count], decoded, (size_t)n);
      out[count][n] = '\0';
      count++;
   }
   closedir(d);
   return count;
}

static int jsonfile_salt_readonly(void *ctx, const char *principal, uint8_t salt[VAULT_SALT_LEN])
{
   if (!principal || !principal[0] || !salt)
      return -1;
   cJSON *root = load_vault(principal);
   if (!root)
      return -1;
   cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "salt");
   int ok =
       cJSON_IsString(js) && b64url_decode(js->valuestring, salt, VAULT_SALT_LEN) == VAULT_SALT_LEN;
   cJSON_Delete(root);
   if (!ok)
      OPENSSL_cleanse(salt, VAULT_SALT_LEN);
   return ok ? 0 : -1;
}

/* True if `kek` unwraps the stored kek_check verifier to the sentinel. */
static int kek_check_matches(cJSON *root, const uint8_t kek[VAULT_KEK_LEN])
{
   cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "kek_check");
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
   int matches =
       cJSON_IsString(jc) &&
       b64url_decode(jc->valuestring, wrapped, sizeof(wrapped)) == VAULT_WRAPPED_DEK_LEN &&
       vault_kek_check_verify(kek, wrapped) == 0;
   OPENSSL_cleanse(wrapped, sizeof(wrapped));
   return matches;
}

/* Wrap the sentinel under `kek` into the root's kek_check field. 0 on success. */
static int kek_check_set(cJSON *root, const uint8_t kek[VAULT_KEK_LEN])
{
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
   if (vault_kek_check_wrap(kek, wrapped) != 0)
      return -1;
   char *enc = b64url_encode(wrapped, sizeof(wrapped));
   if (!enc)
      return -1;
   cJSON *existing = cJSON_GetObjectItemCaseSensitive(root, "kek_check");
   if (cJSON_IsString(existing))
      cJSON_SetValuestring(existing, enc);
   else
      cJSON_AddStringToObject(root, "kek_check", enc);
   free(enc);
   return 0;
}

static int jsonfile_unlock_check(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN])
{
   if (!principal || !kek)
      return -1;
   int lock_fd = vault_write_lock();
   if (lock_fd < 0)
      return -1;
   cJSON *root = load_vault(principal);
   if (!root)
   {
      vault_write_unlock(lock_fd);
      return -1;
   }
   int rc;
   if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "kek_check")))
   {
      /* Established already: this KEK is valid only if it unwraps the verifier. */
      rc = kek_check_matches(root, kek) ? 0 : -1;
   }
   else
   {
      /* First unlock: establish the verifier from this KEK (write-back). */
      rc = (kek_check_set(root, kek) == 0 && write_vault_file(principal, root) == 0) ? 0 : -1;
   }
   cJSON_Delete(root);
   vault_write_unlock(lock_fd);
   return rc;
}

static int jsonfile_rekey(void *ctx, const char *principal, const uint8_t old_kek[VAULT_KEK_LEN],
                          const uint8_t new_kek[VAULT_KEK_LEN])
{
   if (!principal || !old_kek || !new_kek)
      return -1;
   int lock_fd = vault_write_lock();
   if (lock_fd < 0)
      return -1;
   cJSON *root = load_vault(principal);
   if (!root)
   {
      vault_write_unlock(lock_fd);
      return -1; /* no vault to rekey */
   }

   /* Validate the OLD KEK against the key-check verifier FIRST — independent of
    * credential count, so an empty vault cannot be rekeyed with a wrong old
    * password. A vault with no verifier was never unlocked: refuse. */
   if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "kek_check")) ||
       !kek_check_matches(root, old_kek))
   {
      cJSON_Delete(root);
      vault_write_unlock(lock_fd);
      return -1;
   }

   int rc = kek_check_set(root, new_kek); /* re-wrap the verifier under the new KEK */
   cJSON *creds = creds_array(root);
   cJSON *e = NULL;
   if (creds)
   {
      cJSON_ArrayForEach(e, creds)
      {
         int state = immutable_state(e);
         if (state < 0)
         {
            rc = -1;
            break;
         }
         if (state == 2)
            continue;

         cJSON *jw = cJSON_GetObjectItemCaseSensitive(e, "wrapped_dek");
         /* The verified user key cannot rewrap a server-only entry. Keep its
          * exact encrypted content and immutable generation unchanged. */
         if (!jw)
         {
            cJSON *server_wrap = cJSON_GetObjectItemCaseSensitive(e, "wrapped_dek_server");
            uint8_t server_wrapped[VAULT_WRAPPED_DEK_LEN];
            int valid_server = cJSON_IsString(server_wrap) &&
                               b64url_decode(server_wrap->valuestring, server_wrapped,
                                             sizeof(server_wrapped)) == VAULT_WRAPPED_DEK_LEN;
            OPENSSL_cleanse(server_wrapped, sizeof(server_wrapped));
            if (!valid_server)
            {
               rc = -1;
               break;
            }
            continue;
         }
         uint8_t wrapped[VAULT_WRAPPED_DEK_LEN], dek[VAULT_DEK_LEN],
             rewrapped[VAULT_WRAPPED_DEK_LEN];
         if (!cJSON_IsString(jw) ||
             b64url_decode(jw->valuestring, wrapped, sizeof(wrapped)) != VAULT_WRAPPED_DEK_LEN ||
             vault_dek_unwrap(old_kek, wrapped, dek) != 0 ||
             vault_dek_wrap(new_kek, dek, rewrapped) != 0)
         {
            OPENSSL_cleanse(dek, sizeof(dek));
            rc = -1; /* wrong old KEK / tamper -> abort, write nothing */
            break;
         }
         char *enc = b64url_encode(rewrapped, sizeof(rewrapped));
         OPENSSL_cleanse(dek, sizeof(dek));
         if (!enc)
         {
            rc = -1;
            break;
         }
         cJSON_SetValuestring(jw, enc);
         free(enc);
      }
   }
   if (rc == 0)
      rc = write_vault_file(principal, root);
   cJSON_Delete(root);
   vault_write_unlock(lock_fd);
   return rc;
}

static int jsonfile_has_entry(void *ctx, const char *principal, const char *agent, const char *cred)
{
   if (!principal || !agent || !cred)
      return 0;
   cJSON *root = load_vault(principal);
   if (!root)
      return 0;
   cJSON *entry = find_cred(root, agent, cred);
   int state = entry ? immutable_state(entry) : 0;
   int has = state < 0 ? -1 : entry && state != 2;
   cJSON_Delete(root);
   return has;
}

static int jsonfile_list(void *ctx, const char *principal, vault_store_entry_t *out, int max)
{
   if (!principal || (max > 0 && !out))
      return -1;
   cJSON *root = load_vault(principal);
   if (!root)
      return 0; /* no file -> empty list */
   cJSON *creds = creds_array(root);
   int n = 0;
   cJSON *e = NULL;
   if (creds)
   {
      cJSON_ArrayForEach(e, creds)
      {
         int state = immutable_state(e);
         if (state < 0)
         {
            cJSON_Delete(root);
            return -1;
         }
         if (state == 2)
            continue;

         if (n >= max)
            break;
         cJSON *ja = cJSON_GetObjectItemCaseSensitive(e, "agent");
         cJSON *jc = cJSON_GetObjectItemCaseSensitive(e, "cred");
         if (cJSON_IsString(ja) && cJSON_IsString(jc))
         {
            snprintf(out[n].agent, sizeof(out[n].agent), "%s", ja->valuestring);
            snprintf(out[n].cred, sizeof(out[n].cred), "%s", jc->valuestring);
            n++;
         }
      }
   }
   cJSON_Delete(root);
   return n;
}

static int retire_or_remove(cJSON *creds, cJSON *entry)
{
   if (!entry)
      return 0;
   int state = immutable_state(entry);
   if (state < 0)
      return -1;
   if (state == 2)
      return 0;
   if (!state)
   {
      remove_cred(creds, entry);
      return 1;
   }
   cJSON *retired = cJSON_CreateObject();
   if (!retired ||
       !cJSON_AddStringToObject(
           retired, "agent",
           cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "agent"))) ||
       !cJSON_AddStringToObject(
           retired, "cred",
           cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "cred"))) ||
       !cJSON_AddStringToObject(
           retired, IMMUTABLE_MARKER,
           cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, IMMUTABLE_MARKER))) ||
       !cJSON_AddTrueToObject(retired, RETIRED_MARKER) ||
       !cJSON_ReplaceItemViaPointer(creds, entry, retired))
   {
      cJSON_Delete(retired);
      return -1;
   }
   return 1;
}

/* Re-establish durability for a tombstone left visible by an earlier rename
 * whose directory fsync failed. Called while holding the provider write lock. */
static int sync_visible_vault(const char *principal)
{
   char dir[1024], path[1280];
   if (vault_dir(dir, sizeof(dir)) != 0 || vault_file_path(principal, path, sizeof(path)) != 0)
      return -1;
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return -1;
   struct stat st;
   int rc = -1;
   if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && fsync(fd) == 0)
      rc = 0;
   if (close(fd) != 0)
      rc = -1;
   if (rc != 0)
      return -1;
   int dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
   if (dir_fd < 0)
      return -1;
   rc = fsync(dir_fd);
   if (close(dir_fd) != 0)
      rc = -1;
   return rc == 0 ? sync_vault_parent() : -1;
}

static int jsonfile_delete(void *ctx, const char *principal, const char *agent, const char *cred)
{
   if (!principal || !agent || !cred)
      return -1;
   int lock_fd = vault_write_lock();
   if (lock_fd < 0)
      return -1;
   cJSON *root = load_vault(principal);
   if (!root)
   {
      vault_write_unlock(lock_fd);
      return vault_file_missing(principal) ? 0 : -1; /* corrupt is not absence */
   }
   cJSON *creds = creds_array(root);
   int rc = creds ? 0 : -1;
   if (creds)
   {
      cJSON *entry = find_cred(root, agent, cred);
      int changed = retire_or_remove(creds, entry);
      if (changed < 0)
         rc = -1;
      else if (changed)
         rc = write_vault_file(principal, root);
      else if (entry && immutable_state(entry) == 2)
         rc = sync_visible_vault(principal);
      else
         rc = 0;
   }
   cJSON_Delete(root);
   vault_write_unlock(lock_fd);
   return rc;
}

static int create_identifier(const char *s, size_t cap)
{
   if (!s || !s[0] || strnlen(s, cap) >= cap)
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
      if (*p < 32 || *p == 127 || *p == '|')
         return 0;
   return 1;
}
static vault_create_once_result_t jsonfile_create_server_once(
    void *ctx, const char *principal, const uint8_t server_kek[VAULT_KEK_LEN], const char *agent,
    const char *cred, const char *secret, char *generation, size_t generation_capacity)
{
   (void)ctx;
   (void)generation_capacity;
   char selected_generation[VAULT_IMMUTABLE_GENERATION_CAP] = {0};
   int lock_fd = vault_write_lock();
   if (lock_fd < 0)
      return VAULT_CREATE_ONCE_IO;
   vault_create_once_result_t result = VAULT_CREATE_ONCE_IO;
   cJSON *root = load_vault(principal);
   uint8_t salt[VAULT_SALT_LEN] = {0};
   char *prior = NULL;
   if (!root)
   {
      if (!vault_file_missing(principal))
         goto done;
      if (vault_crypto_random(salt, sizeof(salt)))
      {
         result = VAULT_CREATE_ONCE_CRYPTO;
         goto done;
      }
      root = cJSON_CreateObject();
      char *encoded = b64url_encode(salt, sizeof(salt));
      int ok = root && encoded && cJSON_AddNumberToObject(root, "version", VAULT_FILE_VERSION) &&
               cJSON_AddStringToObject(root, "principal", principal) &&
               cJSON_AddStringToObject(root, "kdf_version",
                                       !strncmp(principal, "webuser:", 8) ? VAULT_KDF_VERSION_SCRYPT
                                                                          : VAULT_KDF_VERSION) &&
               cJSON_AddStringToObject(root, "salt", encoded) &&
               cJSON_AddArrayToObject(root, "creds");
      free(encoded);
      if (!ok)
         goto done;
   }
   cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "salt");
   cJSON *owner = cJSON_GetObjectItemCaseSensitive(root, "principal");
   if (!cJSON_IsObject(root) || !cJSON_IsString(owner) || strcmp(owner->valuestring, principal) ||
       cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root, "version")) !=
           VAULT_FILE_VERSION ||
       !cJSON_IsString(js) ||
       b64url_decode(js->valuestring, salt, sizeof(salt)) != (int)sizeof(salt) ||
       !creds_array(root))
      goto done;
   cJSON *entry = find_cred(root, agent, cred);
   if (entry)
   {
      int state = immutable_state(entry);
      if (state < 0)
      {
         result = VAULT_CREATE_ONCE_CRYPTO;
         goto done;
      }
      if (state == 2)
      {
         result = VAULT_CREATE_ONCE_RETIRED;
         goto done;
      }

      const cJSON *marker = cJSON_GetObjectItemCaseSensitive(entry, IMMUTABLE_MARKER);
      const char *retained = cJSON_GetStringValue(marker);
      if (!marker)
      {
         result = VAULT_CREATE_ONCE_CONFLICT;
         goto done;
      }
      if (!generation_valid(retained))
      {
         result = VAULT_CREATE_ONCE_CRYPTO;
         goto done;
      }
      memcpy(selected_generation, retained, sizeof(selected_generation));
      prior = calloc(1, VAULT_SECRET_MAX + 1);
      if (!prior)
         goto done;
      size_t prior_length = 0;
      if (jsonfile_get_server_snapshot(root, principal, server_kek, agent, cred, prior,
                                       VAULT_SECRET_MAX + 1, &prior_length))
      {
         result = VAULT_CREATE_ONCE_CRYPTO;
         goto done;
      }
      size_t length = strlen(secret);
      if (prior_length == length && !CRYPTO_memcmp(prior, secret, length))
         result =
             sync_visible_vault(principal) == 0 ? VAULT_CREATE_ONCE_REPLAY : VAULT_CREATE_ONCE_IO;
      else
         result = VAULT_CREATE_ONCE_CONFLICT;
      goto done;
   }
   uint8_t random_generation[16];
   if (vault_crypto_random(random_generation, sizeof(random_generation)))
   {
      result = VAULT_CREATE_ONCE_CRYPTO;
      goto done;
   }
   static const char hex[] = "0123456789abcdef";
   for (size_t i = 0; i < sizeof(random_generation); i++)
   {
      selected_generation[2 * i] = hex[random_generation[i] >> 4];
      selected_generation[2 * i + 1] = hex[random_generation[i] & 15];
   }
   if (!generation_valid(selected_generation) ||
       vault_stage_credential(root, principal, NULL, server_kek, agent, cred, secret,
                              selected_generation))
   {
      result = VAULT_CREATE_ONCE_CRYPTO;
      goto done;
   }
   if (!write_vault_file(principal, root))
      result = VAULT_CREATE_ONCE_CREATED;
done:
   if (prior)
   {
      OPENSSL_cleanse(prior, VAULT_SECRET_MAX + 1);
      free(prior);
   }
   OPENSSL_cleanse(salt, sizeof(salt));
   if (result == VAULT_CREATE_ONCE_CREATED || result == VAULT_CREATE_ONCE_REPLAY)
      memcpy(generation, selected_generation, sizeof(selected_generation));
   cJSON_Delete(root);
   vault_write_unlock(lock_fd);
   return result;
}

static vault_retire_generation_result_t
jsonfile_retire_server_generation(void *ctx, const char *principal, const char *agent,
                                  const char *cred, const char *expected)
{
   (void)ctx;
   int lock = vault_write_lock();
   if (lock < 0)
      return VAULT_RETIRE_GENERATION_IO;
   vault_retire_generation_result_t result = VAULT_RETIRE_GENERATION_IO;
   cJSON *root = load_vault(principal);
   if (!root)
   {
      if (vault_file_missing(principal))
         result = VAULT_RETIRE_GENERATION_MISSING;
      goto done;
   }
   const char *owner = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "principal"));
   if (!owner || strcmp(owner, principal) || !creds_array(root) ||
       cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root, "version")) !=
           VAULT_FILE_VERSION)
      goto done;
   cJSON *entry = find_cred(root, agent, cred);
   if (!entry)
   {
      result = VAULT_RETIRE_GENERATION_MISSING;
      goto done;
   }
   int state = immutable_state(entry);
   const char *generation =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, IMMUTABLE_MARKER));
   if (state < 0)
      goto done;
   if (!state || !generation || CRYPTO_memcmp(generation, expected, 32))
   {
      result = VAULT_RETIRE_GENERATION_MISMATCH;
      goto done;
   }
   if (state == 1)
   {
      if (retire_or_remove(creds_array(root), entry) != 1 || write_vault_file(principal, root))
         goto done;
   }
   else if (sync_visible_vault(principal))
      goto done;
   result = VAULT_RETIRE_GENERATION_CONFIRMED;
done:
   cJSON_Delete(root);
   vault_write_unlock(lock);
   return result;
}

/* Explicit slot closure; share the create-once provider lock in both race orders. */
static vault_retire_generation_result_t
jsonfile_close_server_slot_once(void *ctx, const char *principal, const char *agent,
                                const char *cred, char *closed_generation,
                                size_t generation_capacity)
{
   (void)ctx;
   (void)generation_capacity;
   struct stat base;
   const char *home = config_default_dir();
   if (!home || stat(home, &base) || !S_ISDIR(base.st_mode))
      return VAULT_RETIRE_GENERATION_IO;
   int lock = vault_write_lock();
   if (lock < 0)
      return VAULT_RETIRE_GENERATION_IO;
   vault_retire_generation_result_t result = VAULT_RETIRE_GENERATION_IO;
   uint8_t salt[VAULT_SALT_LEN] = {0}, random_generation[16] = {0};
   char selected[VAULT_IMMUTABLE_GENERATION_CAP] = {0};
   cJSON *root = load_vault(principal);
   if (!root)
   {
      if (!vault_file_missing(principal) || vault_crypto_random(salt, sizeof salt))
         goto done;
      root = cJSON_CreateObject();
      char *encoded = b64url_encode(salt, sizeof salt);
      int built =
          root && encoded && cJSON_AddNumberToObject(root, "version", VAULT_FILE_VERSION) &&
          cJSON_AddStringToObject(root, "principal", principal) &&
          cJSON_AddStringToObject(root, "kdf_version",
                                  !strncmp(principal, "webuser:", 8) ? VAULT_KDF_VERSION_SCRYPT
                                                                     : VAULT_KDF_VERSION) &&
          cJSON_AddStringToObject(root, "salt", encoded) && cJSON_AddArrayToObject(root, "creds");
      free(encoded);
      if (!built)
         goto done;
   }
   const char *owner = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "principal"));
   const char *encoded = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "salt"));
   if (!cJSON_IsObject(root) || !owner || strcmp(owner, principal) || !encoded ||
       b64url_decode(encoded, salt, sizeof salt) != (int)sizeof salt || !creds_array(root) ||
       cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root, "version")) !=
           VAULT_FILE_VERSION)
      goto done;
   cJSON *entry = find_cred(root, agent, cred);
   if (entry)
   {
      int state = immutable_state(entry);
      if (state < 0)
         goto done;
      if (!state)
      {
         result = VAULT_RETIRE_GENERATION_MISMATCH;
         goto done;
      }
      memcpy(selected,
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, IMMUTABLE_MARKER)),
             sizeof selected);
      if (state == 2)
      {
         if (sync_visible_vault(principal))
            goto done;
      }
      else if (retire_or_remove(creds_array(root), entry) != 1 || write_vault_file(principal, root))
         goto done;
   }
   else
   {
      if (vault_crypto_random(random_generation, sizeof random_generation))
         goto done;
      static const char hex[] = "0123456789abcdef";
      for (size_t i = 0; i < sizeof random_generation; ++i)
      {
         selected[2 * i] = hex[random_generation[i] >> 4];
         selected[2 * i + 1] = hex[random_generation[i] & 15];
      }
      if (!generation_valid(selected))
         goto done;
      cJSON *retired = cJSON_CreateObject();
      if (!retired || !cJSON_AddStringToObject(retired, "agent", agent) ||
          !cJSON_AddStringToObject(retired, "cred", cred) ||
          !cJSON_AddStringToObject(retired, IMMUTABLE_MARKER, selected) ||
          !cJSON_AddTrueToObject(retired, RETIRED_MARKER) ||
          !cJSON_AddItemToArray(creds_array(root), retired))
      {
         cJSON_Delete(retired);
         goto done;
      }
      if (write_vault_file(principal, root))
         goto done;
   }
   memcpy(closed_generation, selected, sizeof selected);
   result = VAULT_RETIRE_GENERATION_CONFIRMED;
done:
   OPENSSL_cleanse(salt, sizeof salt);
   OPENSSL_cleanse(random_generation, sizeof random_generation);
   cJSON_Delete(root);
   vault_write_unlock(lock);
   return result;
}

/* ── Backend seam ─────────────────────────────────────────────────────────────
 * The default "jsonfile" backend: the 0600 per-principal JSON file store above.
 * Stateless (ctx==NULL) — every op resolves its path from config_default_dir().
 * Each public vault_store_<op>() below is a thin forwarder that dispatches
 * through g_store_backend, passing g_store_backend->ctx as the first argument so
 * a future stateful backend needs no globals. The public signatures in
 * vault_store.h are UNCHANGED. */
static const vault_store_backend_t jsonfile_backend = {
    .name = "jsonfile",
    .ctx = NULL,
    .get_or_create_salt = jsonfile_get_or_create_salt,
    .salt_readonly = jsonfile_salt_readonly,
    .unlock_check = jsonfile_unlock_check,
    .set = jsonfile_set,
    .set_dual = jsonfile_set_dual,
    .set_server = jsonfile_set_server,
    .get_server = jsonfile_get_server,
    .create_server_once = jsonfile_create_server_once,
    .get_server_generation = jsonfile_get_server_generation,
    .use_server_generation_fenced = jsonfile_use_server_generation_fenced,
    .retire_server_generation = jsonfile_retire_server_generation,
    .close_server_slot_once = jsonfile_close_server_slot_once,
    .add_server_wraps = jsonfile_add_server_wraps,
    .get = jsonfile_get,
    .has_entry = jsonfile_has_entry,
    .list = jsonfile_list,
    .delete = jsonfile_delete,
    .rekey = jsonfile_rekey,
    .rekey_field = jsonfile_rekey_field,
    .list_principals = jsonfile_list_principals,
};

static const vault_store_backend_t *g_store_backend = &jsonfile_backend;
static int g_instance_local;

static const vault_store_backend_t *store_for(const char *principal)
{
   return g_instance_local && principal && strcmp(principal, VAULT_SERVER_PRINCIPAL) == 0
              ? &jsonfile_backend
              : g_store_backend;
}

/* Construction-time migration: each old instance credential is decrypted,
 * durably sealed into local Vault, then removed from the tenant backend. Partial
 * migration is resumable. Conflicting values fail closed without overwriting
 * either copy. No listener may run until this function succeeds. */
int vault_store_bind_tenant_backend(const vault_store_backend_t *backend,
                                    const uint8_t instance_kek[VAULT_KEK_LEN])
{
   if (g_fenced_use_callback)
      return -1;
   if (!backend || !instance_kek || !backend->list || !backend->get || !backend->delete)
      return -1;
   enum
   {
      MAX_ENTRIES = 4096
   };
   vault_store_entry_t *entries = calloc(MAX_ENTRIES + 1, sizeof(*entries));
   char *old = calloc(1, VAULT_SECRET_MAX + 1), *local = calloc(1, VAULT_SECRET_MAX + 1);
   int rc = -1;
   if (!entries || !old || !local)
      goto done;
   int n = backend->list(backend->ctx, VAULT_SERVER_PRINCIPAL, entries, MAX_ENTRIES + 1);
   if (n < 0 || n > MAX_ENTRIES)
      goto done;
   for (int i = 0; i < n; i++)
   {
      if (backend->get(backend->ctx, VAULT_SERVER_PRINCIPAL, instance_kek, entries[i].agent,
                       entries[i].cred, old, VAULT_SECRET_MAX + 1) != 0)
         goto done;
      int found = jsonfile_get(NULL, VAULT_SERVER_PRINCIPAL, instance_kek, entries[i].agent,
                               entries[i].cred, local, VAULT_SECRET_MAX + 1);
      if (found == VAULT_STORE_NO_ENTRY)
      {
         uint8_t salt[VAULT_SALT_LEN];
         if (jsonfile_get_or_create_salt(NULL, VAULT_SERVER_PRINCIPAL, salt) != 0 ||
             vault_store_set_impl(VAULT_SERVER_PRINCIPAL, instance_kek, NULL, entries[i].agent,
                                  entries[i].cred, old, 1) != 0)
            goto done;
      }
      else if (found != 0 || strlen(old) != strlen(local) ||
               CRYPTO_memcmp(old, local, strlen(old)) != 0)
         goto done;
      if (backend->delete(backend->ctx, VAULT_SERVER_PRINCIPAL, entries[i].agent,
                          entries[i].cred) != 0)
         goto done;
      OPENSSL_cleanse(old, VAULT_SECRET_MAX + 1);
      OPENSSL_cleanse(local, VAULT_SECRET_MAX + 1);
   }
   g_store_backend = backend;
   g_instance_local = 1;
   rc = 0;
done:
   if (old)
      OPENSSL_cleanse(old, VAULT_SECRET_MAX + 1);
   if (local)
      OPENSSL_cleanse(local, VAULT_SECRET_MAX + 1);
   free(old);
   free(local);
   free(entries);
   return rc;
}

/* Rebind the active storage backend (P7 profile composition / tests). NULL
 * restores the built-in jsonfile backend. See vault_internal.h. */
void vault_store_set_backend(const vault_store_backend_t *backend)
{
   if (g_fenced_use_callback)
      return;
   g_store_backend = backend ? backend : &jsonfile_backend;
   g_instance_local = 0;
}

int vault_store_get_or_create_salt(const char *principal, uint8_t salt[VAULT_SALT_LEN])
{
   if (g_fenced_use_callback)
      return -1;
   const vault_store_backend_t *backend = store_for(principal);
   return backend->get_or_create_salt(backend->ctx, principal, salt);
}

int vault_store_salt_readonly(const char *principal, uint8_t salt[VAULT_SALT_LEN])
{
   if (g_fenced_use_callback)
      return -1;
   const vault_store_backend_t *backend = store_for(principal);
   return backend->salt_readonly(backend->ctx, principal, salt);
}

int vault_store_unlock_check(const char *principal, const uint8_t kek[VAULT_KEK_LEN])
{
   if (g_fenced_use_callback)
      return -1;
   const vault_store_backend_t *backend = store_for(principal);
   return backend->unlock_check(backend->ctx, principal, kek);
}

int vault_store_set(const char *principal, const uint8_t kek[VAULT_KEK_LEN], const char *agent,
                    const char *cred, const char *secret)
{
   if (g_fenced_use_callback)
      return -1;
   const vault_store_backend_t *backend = store_for(principal);
   return backend->set(backend->ctx, principal, kek, agent, cred, secret);
}

int vault_store_set_dual(const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                         const uint8_t server_kek[VAULT_KEK_LEN], const char *agent,
                         const char *cred, const char *secret)
{
   if (g_fenced_use_callback)
      return -1;
   const vault_store_backend_t *backend = store_for(principal);
   return backend->set_dual(backend->ctx, principal, kek, server_kek, agent, cred, secret);
}

int vault_store_set_server(const char *principal, const uint8_t server_kek[VAULT_KEK_LEN],
                           const char *agent, const char *cred, const char *secret)
{
   if (g_fenced_use_callback)
      return -1;
   const vault_store_backend_t *backend = store_for(principal);
   return backend->set_server(backend->ctx, principal, server_kek, agent, cred, secret);
}

vault_create_once_result_t vault_store_create_server_once(const char *principal,
                                                          const uint8_t server_kek[VAULT_KEK_LEN],
                                                          const char *agent, const char *cred,
                                                          const char *secret, char *generation,
                                                          size_t generation_capacity)
{
   if (generation && generation_capacity)
      OPENSSL_cleanse(generation, generation_capacity);
   if (g_fenced_use_callback)
      return VAULT_CREATE_ONCE_BADARG;
   if (!generation || generation_capacity < VAULT_IMMUTABLE_GENERATION_CAP || !server_kek ||
       !create_identifier(principal, VAULT_PRINCIPAL_MAX) ||
       !create_identifier(agent, sizeof(((vault_store_entry_t *)0)->agent)) ||
       !create_identifier(cred, sizeof(((vault_store_entry_t *)0)->cred)) || !secret ||
       strnlen(secret, VAULT_SECRET_MAX + 1) > VAULT_SECRET_MAX)
      return VAULT_CREATE_ONCE_BADARG;
   /* Instance credentials use a primary-wrap contract, not this per-principal
    * server-wrap contract. Do not create an incarnation rotation cannot retain. */
   if (!strcmp(principal, VAULT_SERVER_PRINCIPAL))
      return VAULT_CREATE_ONCE_UNSUPPORTED;
   const vault_store_backend_t *backend = store_for(principal);
   if (!backend->create_server_once)
      return VAULT_CREATE_ONCE_UNSUPPORTED;
   vault_create_once_result_t result = backend->create_server_once(
       backend->ctx, principal, server_kek, agent, cred, secret, generation, generation_capacity);
   if ((result == VAULT_CREATE_ONCE_CREATED || result == VAULT_CREATE_ONCE_REPLAY) &&
       !generation_valid(generation))
      result = VAULT_CREATE_ONCE_ERROR;
   if (result != VAULT_CREATE_ONCE_CREATED && result != VAULT_CREATE_ONCE_REPLAY)
      OPENSSL_cleanse(generation, generation_capacity);
   switch (result)
   {
   case VAULT_CREATE_ONCE_CREATED:
   case VAULT_CREATE_ONCE_REPLAY:
   case VAULT_CREATE_ONCE_CONFLICT:
   case VAULT_CREATE_ONCE_RETIRED:
   case VAULT_CREATE_ONCE_ERROR:
   case VAULT_CREATE_ONCE_UNSUPPORTED:
   case VAULT_CREATE_ONCE_BADARG:
   case VAULT_CREATE_ONCE_CRYPTO:
   case VAULT_CREATE_ONCE_IO:
      return result;
   }
   return VAULT_CREATE_ONCE_ERROR;
}

int vault_store_get_server_generation(const char *principal,
                                      const uint8_t server_kek[VAULT_KEK_LEN], const char *agent,
                                      const char *cred, const char *expected_generation, char *out,
                                      size_t out_len)
{
   if (out && out_len)
      OPENSSL_cleanse(out, out_len);
   if (g_fenced_use_callback)
      return -1;
   if (!principal || !server_kek || !agent || !cred || !generation_valid(expected_generation) ||
       !out || !out_len)
      return -1;
   if (!strcmp(principal, VAULT_SERVER_PRINCIPAL))
      return VAULT_STORE_GENERATION_UNSUPPORTED;
   const vault_store_backend_t *backend = store_for(principal);
   if (!backend->get_server_generation)
      return VAULT_STORE_GENERATION_UNSUPPORTED;
   int rc = backend->get_server_generation(backend->ctx, principal, server_kek, agent, cred,
                                           expected_generation, out, out_len);
   if (!rc && !memchr(out, '\0', out_len))
      rc = -1;
   if (rc)
      OPENSSL_cleanse(out, out_len);
   return rc;
}

typedef struct
{
   char *buffer;
   size_t capacity;
   vault_store_secret_use_fn use;
   void *context;
   int entered, ran, invalid, result;
} fenced_use_call;

static int fenced_use_checked(const char *secret, size_t length, void *opaque)
{
   fenced_use_call *call = opaque;
   if (call->entered)
   {
      call->invalid = 1;
      return -1;
   }
   call->entered = 1;
   if (secret != call->buffer || length >= call->capacity ||
       memchr(call->buffer, '\0', call->capacity) != call->buffer + length)
   {
      call->invalid = 1;
      return -1;
   }
   call->ran = 1;
   g_fenced_use_callback = 1;
   call->result = call->use(secret, length, call->context);
   g_fenced_use_callback = 0;
   return call->result;
}

__attribute__((used, externally_visible, retain))
const unsigned aimee_vault_generation_fenced_use_contract_v1 = 1;

/* Deliver a pending cancellation only after locks and secret buffers are gone.
 * Restore enabled callers in deferred mode before testing, then restore their
 * original type on normal return. In particular, glibc 2.39 async-enable can
 * terminate with a NULL join result instead of PTHREAD_CANCELED; an explicit
 * deferred delivery preserves both cancellation status and the no-success-ACK
 * boundary. Previously disabled callers remain disabled with their type intact. */
static void restore_vault_cancellation(int previous_state)
{
   int previous_type = PTHREAD_CANCEL_DEFERRED;
   (void)pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &previous_type);
   (void)pthread_setcancelstate(previous_state, NULL);
   if (previous_state == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   (void)pthread_setcanceltype(previous_type, NULL);
}

int vault_store_use_server_generation_fenced(const char *principal,
                                             const uint8_t server_kek[VAULT_KEK_LEN],
                                             const char *agent, const char *cred,
                                             const char *generation, vault_store_secret_use_fn use,
                                             void *context)
{
   if (g_fenced_use_callback || !server_kek || !create_identifier(principal, VAULT_PRINCIPAL_MAX) ||
       !create_identifier(agent, sizeof(((vault_store_entry_t *)0)->agent)) ||
       !create_identifier(cred, sizeof(((vault_store_entry_t *)0)->cred)) ||
       !generation_valid(generation) || !use)
      return -1;
   if (!strcmp(principal, VAULT_SERVER_PRINCIPAL))
      return VAULT_STORE_GENERATION_UNSUPPORTED;
   const vault_store_backend_t *backend = store_for(principal);
   if (!backend->use_server_generation_fenced)
      return VAULT_STORE_GENERATION_UNSUPPORTED;
   int previous_cancel;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous_cancel))
      return -1;
   enum
   {
      CAPACITY = VAULT_SECRET_MAX + 1
   };
   char *buffer = calloc(1, CAPACITY);
   int rc = -1;
   if (buffer)
   {
      fenced_use_call call = {
          .buffer = buffer, .capacity = CAPACITY, .use = use, .context = context};
      rc = backend->use_server_generation_fenced(backend->ctx, principal, server_kek, agent, cred,
                                                 generation, buffer, CAPACITY, fenced_use_checked,
                                                 &call);
      if (call.ran)
         rc = rc || call.invalid || call.result ? VAULT_STORE_USE_FAILED : 0;
      else if (call.invalid ||
               (rc != VAULT_STORE_GENERATION_UNSUPPORTED && rc != VAULT_STORE_RETIRED &&
                rc != VAULT_STORE_GENERATION_MISMATCH && rc != VAULT_STORE_NO_ENTRY))
         rc = -1;
      OPENSSL_cleanse(buffer, CAPACITY);
      free(buffer);
   }
   restore_vault_cancellation(previous_cancel);
   return rc;
}

int vault_store_get_server(const char *principal, const uint8_t server_kek[VAULT_KEK_LEN],
                           const char *agent, const char *cred, char *out, size_t out_len)
{
   if (g_fenced_use_callback)
   {
      if (out && out_len)
         OPENSSL_cleanse(out, out_len);
      return -1;
   }
   const vault_store_backend_t *backend = store_for(principal);
   return backend->get_server(backend->ctx, principal, server_kek, agent, cred, out, out_len);
}

int vault_store_add_server_wraps(const char *principal, const uint8_t user_kek[VAULT_KEK_LEN],
                                 const uint8_t server_kek[VAULT_KEK_LEN])
{
   if (g_fenced_use_callback)
      return -1;
   const vault_store_backend_t *backend = store_for(principal);
   return backend->add_server_wraps(backend->ctx, principal, user_kek, server_kek);
}

int vault_store_write_batch(const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                            const char *agent, const vault_store_write_t *writes, size_t count)
{
   if (g_fenced_use_callback)
      return -1;
   if (!principal || !principal[0] || !kek || !agent || !agent[0] || !writes || !count ||
       count > 16)
      return -1;
   for (size_t i = 0; i < count; i++)
   {
      if (!writes[i].credential || !writes[i].credential[0] ||
          (writes[i].secret && strlen(writes[i].secret) > VAULT_SECRET_MAX))
         return -1;
      for (size_t j = 0; j < i; j++)
         if (!strcmp(writes[i].credential, writes[j].credential))
            return -1;
   }
   if (store_for(principal) != &jsonfile_backend)
      return VAULT_STORE_BATCH_UNSUPPORTED;
   int lock_fd = vault_write_lock();
   if (lock_fd < 0)
      return -1;
   cJSON *root = load_vault(principal);
   int rc = -1;
   if (!root)
      goto done;
   int changed = 0;
   int needs_sync = 0;
   for (size_t i = 0; i < count; i++)
   {
      if (writes[i].secret)
      {
         if (vault_stage_credential(root, principal, kek, NULL, agent, writes[i].credential,
                                    writes[i].secret, NULL))
            goto done;
         changed = 1;
      }
      else
      {
         cJSON *creds = creds_array(root);
         if (!creds)
            goto done;
         cJSON *entry = find_cred(root, agent, writes[i].credential);
         if (entry && immutable_state(entry) == 2)
            needs_sync = 1;
         int change = retire_or_remove(creds, entry);
         if (change < 0)
            goto done;
         changed |= change;
      }
   }
   if (changed)
      rc = write_vault_file(principal, root);
   else if (needs_sync)
      rc = sync_visible_vault(principal);
   else
      rc = 0;
done:
   cJSON_Delete(root);
   vault_write_unlock(lock_fd);
   return rc;
}

int vault_store_read_snapshot(const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                              const char *agent, vault_store_read_t *reads, size_t count)
{
   if (!reads || !count || count > 16)
      return -1;
   int valid = !g_fenced_use_callback && principal && kek && agent;
   for (size_t i = 0; i < count; i++)
   {
      reads[i].present = 0;
      if (reads[i].value && reads[i].capacity)
         OPENSSL_cleanse(reads[i].value, reads[i].capacity);
      else
         valid = 0;
      if (!reads[i].credential || !reads[i].credential[0])
         valid = 0;
   }
   if (!valid)
      return -1;
   /* Server instance credentials use this local file owner even when tenant
    * secrets use another backend. Never emulate atomicity with separate reads. */
   if (store_for(principal) != &jsonfile_backend)
      return VAULT_STORE_SNAPSHOT_UNSUPPORTED;
   cJSON *root = load_vault(principal);
   if (!root)
      return vault_file_missing(principal) ? 0 : -1;
   int rc = 0;
   for (size_t i = 0; i < count; i++)
   {
      int read_rc = jsonfile_get_from_snapshot(root, principal, kek, agent, reads[i].credential,
                                               reads[i].value, reads[i].capacity);
      if (read_rc == 0)
         reads[i].present = 1;
      else if (read_rc != VAULT_STORE_NO_ENTRY)
      {
         rc = -1;
         break;
      }
   }
   cJSON_Delete(root);
   if (rc)
      for (size_t i = 0; i < count; i++)
      {
         OPENSSL_cleanse(reads[i].value, reads[i].capacity);
         reads[i].present = 0;
      }
   return rc;
}

int vault_store_get(const char *principal, const uint8_t kek[VAULT_KEK_LEN], const char *agent,
                    const char *cred, char *out, size_t out_len)
{
   if (g_fenced_use_callback)
   {
      if (out && out_len)
         OPENSSL_cleanse(out, out_len);
      return -1;
   }
   const vault_store_backend_t *backend = store_for(principal);
   return backend->get(backend->ctx, principal, kek, agent, cred, out, out_len);
}

int vault_store_has_entry(const char *principal, const char *agent, const char *cred)
{
   if (g_fenced_use_callback)
      return -1;
   const vault_store_backend_t *backend = store_for(principal);
   return backend->has_entry(backend->ctx, principal, agent, cred);
}

int vault_store_list(const char *principal, vault_store_entry_t *out, int max)
{
   if (g_fenced_use_callback)
      return -1;
   const vault_store_backend_t *backend = store_for(principal);
   return backend->list(backend->ctx, principal, out, max);
}

int vault_store_delete(const char *principal, const char *agent, const char *cred)
{
   if (g_fenced_use_callback)
      return -1;
   const vault_store_backend_t *backend = store_for(principal);
   return backend->delete(backend->ctx, principal, agent, cred);
}

int vault_store_rekey(const char *principal, const uint8_t old_kek[VAULT_KEK_LEN],
                      const uint8_t new_kek[VAULT_KEK_LEN])
{
   if (g_fenced_use_callback)
      return -1;
   const vault_store_backend_t *backend = store_for(principal);
   return backend->rekey(backend->ctx, principal, old_kek, new_kek);
}

int vault_store_rekey_field(const char *principal, const char *field,
                            const uint8_t old_kek[VAULT_KEK_LEN],
                            const uint8_t new_kek[VAULT_KEK_LEN])
{
   if (g_fenced_use_callback)
      return -1;
   const vault_store_backend_t *backend = store_for(principal);
   return backend->rekey_field(backend->ctx, principal, field, old_kek, new_kek);
}

int vault_store_list_principals(char (*out)[VAULT_PRINCIPAL_MAX], int max)
{
   if (g_fenced_use_callback)
      return -1;
   if (!g_instance_local)
      return g_store_backend->list_principals(g_store_backend->ctx, out, max);
   if (!out || max < 1)
      return -1;
   snprintf(out[0], VAULT_PRINCIPAL_MAX, "%s", VAULT_SERVER_PRINCIPAL);
   int n = g_store_backend->list_principals(g_store_backend->ctx, out + 1, max - 1);
   if (n < 0 || n >= max)
      return -1;
   int used = 1;
   for (int i = 1; i <= n; i++)
      if (strcmp(out[i], VAULT_SERVER_PRINCIPAL) != 0)
         memmove(out[used++], out[i], VAULT_PRINCIPAL_MAX);
   return used;
}

__attribute__((used, externally_visible, retain))
const unsigned aimee_vault_generation_retirement_contract_v1 = 1;
__attribute__((used, externally_visible, retain))
const unsigned aimee_vault_slot_closure_contract_v1 = 1;
__attribute__((used, externally_visible, retain)) vault_retire_generation_result_t
vault_store_close_server_slot_once(const char *principal, const char *agent, const char *cred,
                                   char *closed_generation, size_t generation_capacity)
{
   if (closed_generation && generation_capacity)
      closed_generation[0] = 0;
   if (g_fenced_use_callback || !create_identifier(principal, VAULT_PRINCIPAL_MAX) ||
       !create_identifier(agent, sizeof(((vault_store_entry_t *)0)->agent)) ||
       !create_identifier(cred, sizeof(((vault_store_entry_t *)0)->cred)) || !closed_generation ||
       generation_capacity < VAULT_IMMUTABLE_GENERATION_CAP)
      return VAULT_RETIRE_GENERATION_BADARG;
   if (!strcmp(principal, VAULT_SERVER_PRINCIPAL))
      return VAULT_RETIRE_GENERATION_UNSUPPORTED;
   const vault_store_backend_t *backend = store_for(principal);
   if (!backend->close_server_slot_once)
      return VAULT_RETIRE_GENERATION_UNSUPPORTED;
   int previous_cancel;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous_cancel))
      return VAULT_RETIRE_GENERATION_IO;
   char selected[VAULT_IMMUTABLE_GENERATION_CAP] = {0};
   vault_retire_generation_result_t result = backend->close_server_slot_once(
       backend->ctx, principal, agent, cred, selected, sizeof selected);
   if (result < VAULT_RETIRE_GENERATION_CONFIRMED || result > VAULT_RETIRE_GENERATION_IO ||
       result == VAULT_RETIRE_GENERATION_MISSING ||
       (result == VAULT_RETIRE_GENERATION_CONFIRMED && !generation_valid(selected)))
      result = VAULT_RETIRE_GENERATION_IO;
   if (result == VAULT_RETIRE_GENERATION_CONFIRMED)
      memcpy(closed_generation, selected, sizeof selected);
   restore_vault_cancellation(previous_cancel);
   return result;
}
__attribute__((used, externally_visible, retain)) vault_retire_generation_result_t
vault_store_retire_server_generation(const char *principal, const char *agent, const char *cred,
                                     const char *expected)
{
   if (g_fenced_use_callback || !create_identifier(principal, VAULT_PRINCIPAL_MAX) ||
       !create_identifier(agent, sizeof(((vault_store_entry_t *)0)->agent)) ||
       !create_identifier(cred, sizeof(((vault_store_entry_t *)0)->cred)) ||
       !generation_valid(expected))
      return VAULT_RETIRE_GENERATION_BADARG;
   if (!strcmp(principal, VAULT_SERVER_PRINCIPAL))
      return VAULT_RETIRE_GENERATION_UNSUPPORTED;
   const vault_store_backend_t *backend = store_for(principal);
   if (!backend->retire_server_generation)
      return VAULT_RETIRE_GENERATION_UNSUPPORTED;
   int previous_cancel;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous_cancel))
      return VAULT_RETIRE_GENERATION_IO;
   vault_retire_generation_result_t r =
       backend->retire_server_generation(backend->ctx, principal, agent, cred, expected);
   if (r < VAULT_RETIRE_GENERATION_CONFIRMED || r > VAULT_RETIRE_GENERATION_IO)
      r = VAULT_RETIRE_GENERATION_IO;
   /* Every provider lock and temporary buffer must be released before a pending
    * cancellation can be delivered. No canceled caller receives a success ACK. */
   restore_vault_cancellation(previous_cancel);
   return r;
}
