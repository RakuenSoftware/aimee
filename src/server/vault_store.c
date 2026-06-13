/* vault_store.c: the 0600 per-principal file substrate for the credential vault
 * (WP-C.1). Stores only ciphertext (envelope per vault_crypto). The filename is
 * the URL-safe base64 of the principal so an attacker-influenced webuser name
 * cannot traverse the path or collide with another principal. See vault_store.h.
 */
#include "vault_store.h"
#include "vault_crypto.h"
#include "config.h"        /* config_default_dir */
#include "platform_path.h" /* platform_mkdir_p */
#include "cJSON.h"
#include <openssl/crypto.h> /* OPENSSL_cleanse */
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

#define VAULT_FILE_VERSION 1
#define VAULT_SECRET_MAX   4096 /* max credential plaintext length */
#define VAULT_AAD_MAX      320  /* "principal|agent|cred" (160 + 64 + 64 + seps) */

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

/* ── URL-safe base64 (no padding), binary-safe ───────────────────────────── */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Encode len bytes into a malloc'd NUL-terminated string (caller frees), or NULL
 * on OOM. */
static char *b64url_encode(const uint8_t *in, size_t len)
{
   size_t olen = (len + 2) / 3 * 4;
   char *out = malloc(olen + 1);
   if (!out)
      return NULL;
   size_t o = 0;
   for (size_t i = 0; i < len;)
   {
      uint32_t a = in[i++];
      uint32_t b = i < len ? in[i++] : 0;
      uint32_t c = i < len ? in[i++] : 0;
      uint32_t triple = (a << 16) | (b << 8) | c;
      out[o++] = B64[(triple >> 18) & 0x3f];
      out[o++] = B64[(triple >> 12) & 0x3f];
      out[o++] = B64[(triple >> 6) & 0x3f];
      out[o++] = B64[triple & 0x3f];
   }
   /* Trim the padding chars implied by the input length (no '=' in url-safe). */
   size_t rem = len % 3;
   if (rem == 1)
      o -= 2;
   else if (rem == 2)
      o -= 1;
   out[o] = '\0';
   return out;
}

static int b64_val(char c)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   if (c == '-')
      return 62;
   if (c == '_')
      return 63;
   return -1;
}

/* Decode into out (size out_cap). Returns the decoded byte count, or -1 on
 * malformed input / overflow. */
static int b64url_decode(const char *in, uint8_t *out, size_t out_cap)
{
   size_t ilen = strlen(in);
   size_t o = 0;
   uint32_t acc = 0;
   int bits = 0;
   for (size_t i = 0; i < ilen; i++)
   {
      int v = b64_val(in[i]);
      if (v < 0)
         return -1;
      acc = (acc << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         if (o >= out_cap)
            return -1;
         out[o++] = (uint8_t)((acc >> bits) & 0xff);
      }
   }
   return (int)o;
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

/* ── File I/O ─────────────────────────────────────────────────────────────── */
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
   if (sz < 0 || sz > 8 * 1024 * 1024)
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

   int rc = -1;
   int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0600);
   if (fd >= 0)
   {
      size_t len = strlen(txt);
      ssize_t w = write(fd, txt, len);
      if (w == (ssize_t)len && fsync(fd) == 0)
         rc = 0;
      close(fd);
      if (rc == 0 && rename(tmp, path) != 0)
         rc = -1;
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

/* Build the AEAD AAD "principal|agent|cred" into out; returns its length or -1. */
static int build_aad(const char *principal, const char *agent, const char *cred, char *out,
                     size_t cap)
{
   int n = snprintf(out, cap, "%s|%s|%s", principal, agent, cred);
   return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int vault_store_get_or_create_salt(const char *principal, uint8_t salt[VAULT_SALT_LEN])
{
   if (!principal || !principal[0] || !salt)
      return -1;

   /* Held across the load->(maybe create+write) so two concurrent unlocks of the
    * same new principal cannot each create a file with a different salt (the
    * second would orphan the first's cached KEK). */
   pthread_mutex_lock(&g_vault_write_mu);
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
   pthread_mutex_unlock(&g_vault_write_mu);
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
   cJSON_AddStringToObject(obj, name, enc);
   free(enc);
   return 0;
}

int vault_store_set(const char *principal, const uint8_t kek[VAULT_KEK_LEN], const char *agent,
                    const char *cred, const char *secret)
{
   if (!principal || !kek || !agent || !agent[0] || !cred || !cred[0] || !secret)
      return -1;
   size_t pt_len = strlen(secret);
   if (pt_len > VAULT_SECRET_MAX)
      return -1;

   pthread_mutex_lock(&g_vault_write_mu);
   cJSON *root = load_vault(principal);
   if (!root) /* must be unlocked/created first */
   {
      pthread_mutex_unlock(&g_vault_write_mu);
      return -1;
   }

   int rc = -1;
   uint8_t dek[VAULT_DEK_LEN] = {0};
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN], nonce[VAULT_GCM_NONCE_LEN], tag[VAULT_GCM_TAG_LEN];
   uint8_t *ct = malloc(pt_len ? pt_len : 1);
   char aad[VAULT_AAD_MAX];
   if (!ct)
      goto out;
   if (build_aad(principal, agent, cred, aad, sizeof(aad)) < 0)
      goto out;
   if (vault_crypto_random(dek, sizeof(dek)) != 0)
      goto out;
   if (vault_secret_encrypt(dek, (const uint8_t *)aad, strlen(aad), (const uint8_t *)secret, pt_len,
                            nonce, ct, tag) != 0)
      goto out;
   if (vault_dek_wrap(kek, dek, wrapped) != 0)
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
      cJSON_AddStringToObject(obj, "agent", agent);
      cJSON_AddStringToObject(obj, "cred", cred);
      if (add_b64_field(obj, "wrapped_dek", wrapped, sizeof(wrapped)) != 0 ||
          add_b64_field(obj, "nonce", nonce, sizeof(nonce)) != 0 ||
          add_b64_field(obj, "ciphertext", ct, pt_len) != 0 ||
          add_b64_field(obj, "tag", tag, sizeof(tag)) != 0)
      {
         cJSON_Delete(obj);
         goto out;
      }
      cJSON_AddItemToArray(creds, obj);
   }
   rc = write_vault_file(principal, root);

out:
   OPENSSL_cleanse(dek, sizeof(dek));
   if (ct)
   {
      if (pt_len)
         OPENSSL_cleanse(ct, pt_len);
      free(ct);
   }
   cJSON_Delete(root);
   pthread_mutex_unlock(&g_vault_write_mu);
   return rc;
}

int vault_store_get(const char *principal, const uint8_t kek[VAULT_KEK_LEN], const char *agent,
                    const char *cred, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!principal || !kek || !agent || !cred || !out || !out_len)
      return -1;

   cJSON *root = load_vault(principal);
   if (!root)
      return VAULT_STORE_NO_ENTRY; /* no file -> no entry, fall back */

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
   cJSON_Delete(root);
   return rc;
}

int vault_store_has_entry(const char *principal, const char *agent, const char *cred)
{
   if (!principal || !agent || !cred)
      return 0;
   cJSON *root = load_vault(principal);
   if (!root)
      return 0;
   int has = find_cred(root, agent, cred) != NULL;
   cJSON_Delete(root);
   return has;
}

int vault_store_list(const char *principal, vault_store_entry_t *out, int max)
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

int vault_store_delete(const char *principal, const char *agent, const char *cred)
{
   if (!principal || !agent || !cred)
      return -1;
   pthread_mutex_lock(&g_vault_write_mu);
   cJSON *root = load_vault(principal);
   if (!root)
   {
      pthread_mutex_unlock(&g_vault_write_mu);
      return 0; /* nothing to delete */
   }
   cJSON *creds = creds_array(root);
   int rc = 0;
   if (creds)
   {
      cJSON *existing = find_cred(root, agent, cred);
      if (existing)
      {
         remove_cred(creds, existing);
         rc = write_vault_file(principal, root);
      }
   }
   cJSON_Delete(root);
   pthread_mutex_unlock(&g_vault_write_mu);
   return rc;
}
