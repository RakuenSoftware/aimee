/* enroll.c: zero-config enrollment primitives for aimee-kb remote auth — opaque
 * single-use enrollment tokens and the `aimee://` connection string. See
 * kb_enroll.h. (distributed-mode-auth proposal, Phase 1.)
 *
 * Tokens are 256-bit opaque random values, base64url-encoded. Only sha256(token)
 * enters the single-use registry, and that registry is encrypted in Vault; the
 * named path is an empty cross-process lock inode. Token checks are constant-time. */
#include "kb_enroll.h"
#include "vault_service.h"
#include "kb_pki.h"     /* internal CA: load-or-create + fingerprint */
#include "oauth_pkce.h" /* oauth_pkce_base64url_encode */
#include "platform_random.h"

#include <openssl/crypto.h> /* OPENSSL_cleanse — secure zero of transient secrets */
#include <openssl/sha.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h> /* flock */
#include <unistd.h>

/* --- token crypto --- */

int kb_enroll_token_generate(char *out, size_t cap)
{
   if (!out || cap < KB_ENROLL_TOKEN_MAX)
      return -1;
   unsigned char raw[KB_ENROLL_TOKEN_RAW_BYTES];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      return -1;
   /* oauth_pkce_base64url_encode returns 0 on success (NUL-terminating out),
    * -1 on error; report the token length to callers. */
   int enc = oauth_pkce_base64url_encode(raw, sizeof(raw), out, cap);
   OPENSSL_cleanse(raw, sizeof(raw)); /* wipe the entropy from the stack */
   if (enc != 0)
      return -1;
   return (int)strlen(out);
}

int kb_enroll_token_hash(const char *token, char *hex_out, size_t cap)
{
   if (!token || !hex_out || cap < KB_ENROLL_HASH_HEX)
      return -1;
   unsigned char digest[SHA256_DIGEST_LENGTH];
   SHA256((const unsigned char *)token, strlen(token), digest);
   for (size_t i = 0; i < sizeof(digest); i++)
      snprintf(hex_out + i * 2, 3, "%02x", digest[i]);
   hex_out[SHA256_DIGEST_LENGTH * 2] = '\0';
   return 0;
}

/* Constant-time compare of two NUL-terminated strings of equal expected length.
 * Does not short-circuit on the first mismatch and folds the length check into
 * the accumulator so the timing does not leak how far the strings matched. */
static int ct_streq(const char *a, const char *b)
{
   size_t alen = a ? strlen(a) : 0;
   size_t blen = b ? strlen(b) : 0;
   size_t n = alen > blen ? alen : blen;
   unsigned char diff = (unsigned char)(alen ^ blen);
   for (size_t i = 0; i < n; i++)
   {
      unsigned char x = i < alen ? (unsigned char)a[i] : 0;
      unsigned char y = i < blen ? (unsigned char)b[i] : 0;
      diff |= (unsigned char)(x ^ y);
   }
   return diff == 0;
}

int kb_enroll_token_verify(const char *presented, const char *stored_hex)
{
   if (!presented || !stored_hex || !stored_hex[0])
      return 0;
   char computed[KB_ENROLL_HASH_HEX];
   if (kb_enroll_token_hash(presented, computed, sizeof(computed)) != 0)
      return 0;
   return ct_streq(computed, stored_hex);
}

/* --- connection-string codec --- */

int kb_enroll_conn_string_build(const char *host, int port, const char *ca_sha256,
                                const char *enroll_token, char *out, size_t cap)
{
   if (!host || !host[0] || port <= 0 || port > 65535 || !ca_sha256 || !ca_sha256[0] ||
       !enroll_token || !enroll_token[0] || !out || cap == 0)
      return -1;
   int n = snprintf(out, cap, "aimee://%s:%d?ca=sha256:%s&enroll=%s", host, port, ca_sha256,
                    enroll_token);
   if (n < 0 || (size_t)n >= cap)
      return -1;
   return n;
}

/* Copy [start,end) into dst[cap] (NUL-terminated). Returns 0/-1 (too long). */
static int copy_span(char *dst, size_t cap, const char *start, const char *end)
{
   if (end < start)
      return -1;
   size_t len = (size_t)(end - start);
   if (len + 1 > cap)
      return -1;
   memcpy(dst, start, len);
   dst[len] = '\0';
   return 0;
}

int kb_enroll_conn_string_parse(const char *s, kb_enroll_conn_t *out)
{
   if (!s || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   static const char SCHEME[] = "aimee://";
   if (strncmp(s, SCHEME, sizeof(SCHEME) - 1) != 0)
      return -1;
   const char *p = s + sizeof(SCHEME) - 1;

   /* authority = host:port, up to '?' */
   const char *q = strchr(p, '?');
   if (!q)
      return -1;
   const char *colon = NULL;
   for (const char *c = p; c < q; c++)
      if (*c == ':')
         colon = c; /* last colon before '?' splits host:port */
   if (!colon || colon == p)
      return -1;
   if (copy_span(out->host, sizeof(out->host), p, colon) != 0)
      return -1;
   char port_buf[16];
   if (copy_span(port_buf, sizeof(port_buf), colon + 1, q) != 0)
      return -1;
   out->port = atoi(port_buf);
   if (out->port <= 0 || out->port > 65535)
      return -1;

   /* query = &-separated key=value pairs; accept ca + enroll in any order. */
   int have_ca = 0, have_enroll = 0;
   const char *kv = q + 1;
   while (*kv)
   {
      const char *amp = strchr(kv, '&');
      const char *kv_end = amp ? amp : kv + strlen(kv);
      const char *eq = NULL;
      for (const char *c = kv; c < kv_end; c++)
         if (*c == '=')
         {
            eq = c;
            break;
         }
      if (eq)
      {
         size_t klen = (size_t)(eq - kv);
         const char *val = eq + 1;
         if (klen == 2 && strncmp(kv, "ca", 2) == 0)
         {
            static const char PFX[] = "sha256:";
            size_t vlen = (size_t)(kv_end - val);
            if (vlen > sizeof(PFX) - 1 && strncmp(val, PFX, sizeof(PFX) - 1) == 0)
            {
               if (copy_span(out->ca_sha256, sizeof(out->ca_sha256), val + sizeof(PFX) - 1,
                             kv_end) == 0)
                  have_ca = 1;
            }
         }
         else if (klen == 6 && strncmp(kv, "enroll", 6) == 0)
         {
            if (copy_span(out->enroll_token, sizeof(out->enroll_token), val, kv_end) == 0)
               have_enroll = 1;
         }
      }
      if (!amp)
         break;
      kv = amp + 1;
   }

   if (!have_ca || !have_enroll || !out->ca_sha256[0] || !out->enroll_token[0])
      return -1;
   return 0;
}

/* --- single-use enrollment-token registry (in-memory) --- */

#define KB_ENROLL_REGISTRY_MAX 256

typedef struct
{
   int in_use;
   int consumed;
   char hash[KB_ENROLL_HASH_HEX];
   char scope[KB_ENROLL_SCOPE_MAX];
} enroll_entry_t;

static enroll_entry_t g_registry[KB_ENROLL_REGISTRY_MAX];
static pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;

int kb_enroll_registry_issue(const char *scope, char *out, size_t cap)
{
   if (!out || cap < KB_ENROLL_TOKEN_MAX)
      return -1;
   char token[KB_ENROLL_TOKEN_MAX];
   if (kb_enroll_token_generate(token, sizeof(token)) < 0)
      return -1;
   char hash[KB_ENROLL_HASH_HEX];
   if (kb_enroll_token_hash(token, hash, sizeof(hash)) != 0)
      return -1;

   pthread_mutex_lock(&g_registry_lock);
   int slot = -1;
   for (int i = 0; i < KB_ENROLL_REGISTRY_MAX; i++)
      if (!g_registry[i].in_use)
      {
         slot = i;
         break;
      }
   if (slot < 0)
   {
      pthread_mutex_unlock(&g_registry_lock);
      OPENSSL_cleanse(token, sizeof(token)); /* wipe cleartext on the full path too */
      return -1;                             /* registry full */
   }
   g_registry[slot].in_use = 1;
   g_registry[slot].consumed = 0;
   snprintf(g_registry[slot].hash, sizeof(g_registry[slot].hash), "%s", hash);
   snprintf(g_registry[slot].scope, sizeof(g_registry[slot].scope), "%s", scope ? scope : "");
   pthread_mutex_unlock(&g_registry_lock);

   snprintf(out, cap, "%s", token);
   OPENSSL_cleanse(token, sizeof(token)); /* the caller now owns the only copy */
   return 0;
}

int kb_enroll_registry_consume(const char *token, char *scope_out, size_t scope_cap)
{
   if (!token)
      return 0;
   char hash[KB_ENROLL_HASH_HEX];
   if (kb_enroll_token_hash(token, hash, sizeof(hash)) != 0)
      return 0;

   int ok = 0;
   pthread_mutex_lock(&g_registry_lock);
   for (int i = 0; i < KB_ENROLL_REGISTRY_MAX; i++)
   {
      if (g_registry[i].in_use && !g_registry[i].consumed && ct_streq(g_registry[i].hash, hash))
      {
         g_registry[i].consumed = 1; /* single-use: atomic under the lock */
         if (scope_out && scope_cap)
            snprintf(scope_out, scope_cap, "%s", g_registry[i].scope);
         ok = 1;
         break;
      }
   }
   pthread_mutex_unlock(&g_registry_lock);
   return ok;
}

void kb_enroll_registry_reset(void)
{
   pthread_mutex_lock(&g_registry_lock);
   memset(g_registry, 0, sizeof(g_registry));
   pthread_mutex_unlock(&g_registry_lock);
}

/* --- Vault-backed single-use enrollment-token store ---
 * Each encrypted Vault record contains lines of
 * "<sha256hex>\t<consumed 0|1>\t<scope>\n". `path` now names only an empty flock
 * coordination inode; hashing it namespaces deployments in the shared server
 * Vault. An older plaintext hash file is ingested under that lock and securely
 * truncated before the operation succeeds. A token verifier is credential
 * material, so even its one-way hash must never persist outside Vault. */

#define KB_ENROLL_VAULT_AGENT "kb-enrollment"
#define KB_ENROLL_VAULT_REGISTRY_MAX (1024 * 1024)

static int enroll_vault_cred(const char *path, char out[80])
{
   char hash[KB_ENROLL_HASH_HEX];
   if (kb_enroll_token_hash(path, hash, sizeof(hash)) != 0)
      return -1;
   int n = snprintf(out, 80, "registry_%s", hash);
   return n > 0 && n < 80 ? 0 : -1;
}

static int clear_legacy_store(int fd, off_t size)
{
   if (size <= 0)
      return 0;
   unsigned char zeros[4096] = {0};
   off_t off = 0;
   while (off < size)
   {
      size_t n = (size - off) < (off_t)sizeof(zeros) ? (size_t)(size - off) : sizeof(zeros);
      ssize_t wrote = pwrite(fd, zeros, n, off);
      if (wrote <= 0)
         return -1;
      off += wrote;
   }
   if (fsync(fd) != 0 || ftruncate(fd, 0) != 0 || fsync(fd) != 0)
      return -1;
   return 0;
}

static int load_enroll_registry(int fd, const char *cred, char **out, size_t *len,
                                off_t *legacy_size)
{
   *out = calloc(1, KB_ENROLL_VAULT_REGISTRY_MAX + 1);
   if (!*out)
      return -1;
   vault_status_t st = vault_service_get_server_principal(
       KB_ENROLL_VAULT_AGENT, cred, *out, KB_ENROLL_VAULT_REGISTRY_MAX + 1);
   if (st == VAULT_OK)
   {
      *len = strlen(*out);
      *legacy_size = lseek(fd, 0, SEEK_END);
      if (*legacy_size < 0)
         return -1;
      return 0;
   }
   if (st != VAULT_NO_ENTRY)
      return -1;

   off_t size = lseek(fd, 0, SEEK_END);
   if (size < 0 || size > KB_ENROLL_VAULT_REGISTRY_MAX || lseek(fd, 0, SEEK_SET) != 0)
      return -1;
   *legacy_size = size;
   size_t got = 0;
   while (got < (size_t)size)
   {
      ssize_t n = read(fd, *out + got, (size_t)size - got);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return -1;
      got += (size_t)n;
   }
   (*out)[got] = '\0';
   *len = got;
   return 0;
}

static int save_enroll_registry(int fd, const char *cred, char *registry, size_t len,
                                off_t legacy_size)
{
   if (len >= KB_ENROLL_VAULT_REGISTRY_MAX || registry[len] != '\0')
      return -1;
   if (vault_service_set_server(KB_ENROLL_VAULT_AGENT, cred, registry) != VAULT_OK)
      return -1;
   return clear_legacy_store(fd, legacy_size);
}

int kb_enroll_store_migrate(const char *path)
{
   if (!path || !path[0])
      return -1;
   int fd = open(path, O_RDWR);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   int rc = -1;
   if (flock(fd, LOCK_EX) == 0)
   {
      char cred[80], *registry = NULL;
      size_t registry_len = 0;
      off_t legacy_size = 0;
      if (enroll_vault_cred(path, cred) == 0 &&
          load_enroll_registry(fd, cred, &registry, &registry_len, &legacy_size) == 0)
      {
         if (legacy_size == 0 ||
             (registry_len > 0 &&
              save_enroll_registry(fd, cred, registry, registry_len, legacy_size) == 0))
            rc = 0;
      }
      if (registry)
      {
         OPENSSL_cleanse(registry, KB_ENROLL_VAULT_REGISTRY_MAX + 1);
         free(registry);
      }
      flock(fd, LOCK_UN);
   }
   close(fd);
   return rc;
}

int kb_enroll_store_issue(const char *path, const char *scope, char *out, size_t cap)
{
   if (!path || !path[0] || !out || cap < KB_ENROLL_TOKEN_MAX)
      return -1;
   if (!scope)
      scope = "";
   /* The record is tab/newline delimited, so a scope carrying either would
    * corrupt the format; reject it (and an over-long scope). */
   if (strlen(scope) >= KB_ENROLL_SCOPE_MAX || strchr(scope, '\t') || strchr(scope, '\n'))
      return -1;

   char tok[KB_ENROLL_TOKEN_MAX], hash[KB_ENROLL_HASH_HEX], cred[80];
   if (kb_enroll_token_generate(tok, sizeof(tok)) < 0)
      return -1;
   if (kb_enroll_token_hash(tok, hash, sizeof(hash)) != 0)
      return -1;
   if (enroll_vault_cred(path, cred) != 0)
      return -1;

   int fd = open(path, O_RDWR | O_CREAT, 0600);
   if (fd < 0)
      return -1;
   int rc = -1;
   if (flock(fd, LOCK_EX) == 0)
   {
      char *registry = NULL;
      size_t registry_len = 0;
      off_t legacy_size = 0;
      char line[64 + 4 + KB_ENROLL_SCOPE_MAX];
      int n = snprintf(line, sizeof(line), "%s\t0\t%s\n", hash, scope);
      if (n > 0 && (size_t)n < sizeof(line) &&
          load_enroll_registry(fd, cred, &registry, &registry_len, &legacy_size) == 0 &&
          registry_len + (size_t)n < KB_ENROLL_VAULT_REGISTRY_MAX)
      {
         memcpy(registry + registry_len, line, (size_t)n + 1);
         registry_len += (size_t)n;
         if (save_enroll_registry(fd, cred, registry, registry_len, legacy_size) == 0)
            rc = 0;
      }
      if (registry)
      {
         OPENSSL_cleanse(registry, KB_ENROLL_VAULT_REGISTRY_MAX + 1);
         free(registry);
      }
      flock(fd, LOCK_UN);
   }
   close(fd);
   if (rc == 0)
   {
      snprintf(out, cap, "%s", tok);
   }
   else
   {
      out[0] = '\0';
   }
   OPENSSL_cleanse(tok, sizeof(tok)); /* don't leave the cleartext on the stack */
   return rc;
}

int kb_enroll_store_consume(const char *path, const char *token, char *scope_out, size_t scope_cap)
{
   if (!path || !path[0] || !token)
      return 0;
   char hash[KB_ENROLL_HASH_HEX], cred[80];
   if (kb_enroll_token_hash(token, hash, sizeof(hash)) != 0)
      return 0;
   if (enroll_vault_cred(path, cred) != 0)
      return 0;

   int fd = open(path, O_RDWR | O_CREAT, 0600);
   if (fd < 0)
      return 0; /* no store -> nothing to consume */

   int result = 0;
   char *buf = NULL;
   if (flock(fd, LOCK_EX) != 0)
   {
      close(fd);
      return 0;
   }

   size_t registry_len = 0;
   off_t legacy_size = 0;
   if (load_enroll_registry(fd, cred, &buf, &registry_len, &legacy_size) != 0)
      goto done;

   /* Find the first matching, unconsumed record; flip its consumed flag. The
    * line layout is fixed: 64 hex chars, '\t', the consumed flag, '\t', scope. */
   for (char *line = buf; line && *line;)
   {
      char *nl = strchr(line, '\n');
      size_t llen = nl ? (size_t)(nl - line) : strlen(line);
      /* Need at least "<64hex>\t<flag>\t" = 67 chars before the scope. */
      if (llen >= 67 && line[64] == '\t' && line[66] == '\t')
      {
         char linehash[KB_ENROLL_HASH_HEX];
         memcpy(linehash, line, 64);
         linehash[64] = '\0';
         char flag = line[65];
         if (flag == '0' && ct_streq(linehash, hash))
         {
            /* copy scope (between the 2nd tab and end-of-line) */
            const char *scope = line + 67;
            size_t slen = llen - 67;
            if (scope_out && scope_cap)
            {
               size_t copy = slen < scope_cap - 1 ? slen : scope_cap - 1;
               memcpy(scope_out, scope, copy);
               scope_out[copy] = '\0';
            }
            line[65] = '1';
            if (save_enroll_registry(fd, cred, buf, registry_len, legacy_size) == 0)
               result = 1;
            break;
         }
      }
      line = nl ? nl + 1 : NULL;
   }
   if (!result && legacy_size > 0)
      (void)save_enroll_registry(fd, cred, buf, registry_len, legacy_size);

done:
   if (buf)
   {
      OPENSSL_cleanse(buf, KB_ENROLL_VAULT_REGISTRY_MAX + 1);
      free(buf);
   }
   flock(fd, LOCK_UN);
   close(fd);
   return result;
}

/* --- one-shot enrollment mint --- */

int kb_enroll_mint(const char *data_dir, const char *host, int port, const char *scope, char *out,
                   size_t cap)
{
   if (!data_dir || !data_dir[0] || !host || !host[0] || port <= 0 || port > 65535 || !out)
      return -1;

   char ca_dir[1024], store_path[1024];
   int n1 = snprintf(ca_dir, sizeof(ca_dir), "%s/kb-ca", data_dir);
   int n2 = snprintf(store_path, sizeof(store_path), "%s/kb-enroll-tokens", data_dir);
   if (n1 <= 0 || (size_t)n1 >= sizeof(ca_dir) || n2 <= 0 || (size_t)n2 >= sizeof(store_path))
      return -1;

   /* Persistent CA (created once, reused on every later call/restart). */
   kb_pki_ca_t ca;
   if (kb_pki_ca_load_or_create_custodied(ca_dir, &ca, NULL) != 0)
      return -1;
   char fp[KB_PKI_FP_HEX];
   if (kb_pki_ca_fingerprint(ca.cert_pem, fp, sizeof(fp)) != 0)
   {
      OPENSSL_cleanse(&ca, sizeof(ca));
      return -1;
   }
   OPENSSL_cleanse(&ca, sizeof(ca)); /* the CA key is no longer needed here */

   /* Single-use enrollment token, persisted so the running server can redeem it. */
   char token[KB_ENROLL_TOKEN_MAX];
   if (kb_enroll_store_issue(store_path, scope ? scope : "", token, sizeof(token)) != 0)
      return -1;

   int rc = kb_enroll_conn_string_build(host, port, fp, token, out, cap);
   OPENSSL_cleanse(token, sizeof(token));
   return rc > 0 ? 0 : -1;
}

/* --- enrollment redemption (client side: token -> CA-signed client cert) --- */

int kb_enroll_redeem(const char *data_dir, const char *token, long valid_secs, char *scope_out,
                     size_t scope_cap, char *cert_pem_out, size_t cert_cap, char *key_pem_out,
                     size_t key_cap)
{
   if (!data_dir || !data_dir[0] || !token || !cert_pem_out || !key_pem_out)
      return -1;

   char ca_dir[1024], store_path[1024];
   int n1 = snprintf(ca_dir, sizeof(ca_dir), "%s/kb-ca", data_dir);
   int n2 = snprintf(store_path, sizeof(store_path), "%s/kb-enroll-tokens", data_dir);
   if (n1 <= 0 || (size_t)n1 >= sizeof(ca_dir) || n2 <= 0 || (size_t)n2 >= sizeof(store_path))
      return -1;

   /* Load the CA first, so a missing/broken CA does NOT burn the single-use
    * token (the token is only consumed once we know we can issue against it). */
   kb_pki_ca_t ca;
   if (kb_pki_ca_load_custodied(ca_dir, &ca) != 0)
      return -1;

   /* Validate + atomically consume the token; its scope becomes the cert subject. */
   char scope[KB_ENROLL_SCOPE_MAX];
   if (kb_enroll_store_consume(store_path, token, scope, sizeof(scope)) != 1)
   {
      OPENSSL_cleanse(&ca, sizeof(ca));
      return -1; /* unknown / replayed / already-consumed token */
   }

   const char *cn = scope[0] ? scope : "global";
   int rc =
       kb_pki_issue_client_cert(&ca, cn, valid_secs, cert_pem_out, cert_cap, key_pem_out, key_cap);
   OPENSSL_cleanse(&ca, sizeof(ca)); /* CA key is no longer needed here */
   if (rc != 0)
      return -1;

   if (scope_out && scope_cap)
      snprintf(scope_out, scope_cap, "%s", scope);
   return 0;
}

/* --- CSR-based redemption (client keeps its key; server signs the CSR) --- */

int kb_enroll_redeem_csr(const char *data_dir, const char *token, const char *csr_pem,
                         long valid_secs, char *scope_out, size_t scope_cap, char *cert_pem_out,
                         size_t cert_cap)
{
   if (!data_dir || !data_dir[0] || !token || !csr_pem || !cert_pem_out)
      return -1;

   char ca_dir[1024], store_path[1024];
   int n1 = snprintf(ca_dir, sizeof(ca_dir), "%s/kb-ca", data_dir);
   int n2 = snprintf(store_path, sizeof(store_path), "%s/kb-enroll-tokens", data_dir);
   if (n1 <= 0 || (size_t)n1 >= sizeof(ca_dir) || n2 <= 0 || (size_t)n2 >= sizeof(store_path))
      return -1;

   /* Validate the CSR FIRST so a malformed/weak CSR never burns the single-use
    * token (the client can retry with a good CSR and the same token). */
   if (kb_pki_csr_validate(csr_pem) != 0)
      return -1;

   /* Load the CA before consuming, so a missing CA also does not burn the token. */
   kb_pki_ca_t ca;
   if (kb_pki_ca_load_custodied(ca_dir, &ca) != 0)
      return -1;

   char scope[KB_ENROLL_SCOPE_MAX];
   if (kb_enroll_store_consume(store_path, token, scope, sizeof(scope)) != 1)
   {
      OPENSSL_cleanse(&ca, sizeof(ca));
      return -1; /* unknown / replayed / already-consumed token */
   }

   /* Bind the CSR's key to a cert whose subject is the token's scope. */
   const char *cn = scope[0] ? scope : "global";
   int rc = kb_pki_sign_csr(&ca, csr_pem, cn, valid_secs, cert_pem_out, cert_cap);
   OPENSSL_cleanse(&ca, sizeof(ca));
   if (rc != 0)
      return -1;

   if (scope_out && scope_cap)
      snprintf(scope_out, scope_cap, "%s", scope);
   return 0;
}
