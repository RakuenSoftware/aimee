#include "vault_custody_kms.h"
#include "vault_crypto.h"
#include "vault_hwm.h"
#include <openssl/crypto.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

/* The helper is the cloud-KMS adapter: it receives the operation name and key
 * id via argv/env and must emit exactly one 32-byte decrypted root to stdout.
 * No shell is used, output is bounded, and any short/extra output fails closed. */
typedef struct
{
   int sealed;
} kms_ctx;
static uint64_t g_hwm_version;
static int g_hwm_ready;
static int hwm_call(const char *op, const char *key, uint64_t expected, uint64_t next,
                    uint64_t *ver, uint8_t *att, size_t *alen)
{
   const char *helper = getenv("AIMEE_VAULT_KMS_HELPER");
   if (!helper || !key || !att || !alen)
      return -1;
   struct stat st;
   if (stat(helper, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 022) ||
       access(helper, X_OK) != 0)
      return -1;
   int p[2];
   if (pipe(p) != 0)
      return -1;
   pid_t pid = fork();
   if (pid < 0)
   {
      close(p[0]);
      close(p[1]);
      return -1;
   }
   if (pid == 0)
   {
      char e[32], n[32];
      snprintf(e, sizeof(e), "%llu", (unsigned long long)expected);
      snprintf(n, sizeof(n), "%llu", (unsigned long long)next);
      dup2(p[1], 1);
      close(p[0]);
      close(p[1]);
      execl(helper, helper, op, key, e, n, (char *)NULL);
      _exit(127);
   }
   close(p[1]);
   uint8_t b[128];
   size_t z = 0;
   while (z < sizeof(b))
   {
      ssize_t r = read(p[0], b + z, sizeof(b) - z);
      if (r < 0 && errno == EINTR)
         continue;
      if (r <= 0)
         break;
      z += (size_t)r;
   }
   close(p[0]);
   int ws = 0;
   waitpid(pid, &ws, 0);
   if (!WIFEXITED(ws) || WEXITSTATUS(ws) != 0 || z < 66 || z > 85)
      return -1;
   char *nl = (char *)memchr(b, '\n', z);
   size_t off = nl ? (size_t)(nl - (char *)b) : z;
   if (!nl || off > 20)
      return -1;
   b[off] = 0;
   char *q = NULL;
   unsigned long long v = strtoull((char *)b, &q, 10);
   if (!q || q != nl)
      return -1;
   size_t sig = z - off - 1;
   if (sig != 64)
      return -1;
   memcpy(att, nl + 1, 64);
   if (vault_hwm_attest_verify(key, (uint64_t)v, att, 64) != 0)
      return -1;
   *ver = (uint64_t)v;
   *alen = 64;
   return 0;
}
static int hwm_read(void *v, const char *k, uint64_t *ver, uint8_t *a, size_t c, size_t *n)
{
   (void)v;
   if (c < 64 || hwm_call("hwm-read", k, 0, 0, ver, a, n) != 0)
      return -1;
   g_hwm_version = *ver;
   return 0;
}
static int hwm_cas(void *v, const char *k, uint64_t e, uint64_t x, uint8_t *a, size_t c, size_t *n)
{
   (void)v;
   if (c < 64 || e != g_hwm_version || x != e + 1)
      return -1;
   uint64_t got = 0;
   if (hwm_call("hwm-cas", k, e, x, &got, a, n) != 0 || got != x)
      return -1;
   g_hwm_version = got;
   return 0;
}
static kms_ctx g = {1};
static int get_kek(void *v, uint8_t out[VAULT_KEK_LEN])
{
   kms_ctx *c = v;
   const char *helper = getenv("AIMEE_VAULT_KMS_HELPER");
   const char *key_id = getenv("AIMEE_VAULT_KMS_KEY_ID");
   if (!helper || !*helper || !key_id || !*key_id)
      return -1;
   struct stat st;
   if (stat(helper, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 022) ||
       access(helper, X_OK) != 0)
      return -1;
   int p[2];
   if (pipe(p) != 0)
      return -1;
   pid_t pid = fork();
   if (pid < 0)
   {
      close(p[0]);
      close(p[1]);
      return -1;
   }
   if (pid == 0)
   {
      dup2(p[1], STDOUT_FILENO);
      close(p[0]);
      close(p[1]);
      execl(helper, helper, "decrypt", key_id, (char *)NULL);
      _exit(127);
   }
   close(p[1]);
   size_t n = 0;
   int bad = 0;
   while (n < VAULT_KEK_LEN)
   {
      ssize_t r = read(p[0], out + n, VAULT_KEK_LEN - n);
      if (r < 0 && errno == EINTR)
         continue;
      if (r <= 0)
      {
         bad = 1;
         break;
      }
      n += (size_t)r;
   }
   uint8_t extra;
   if (!bad && read(p[0], &extra, 1) > 0)
      bad = 1;
   close(p[0]);
   int ws = 0;
   waitpid(pid, &ws, 0);
   if (bad || n != VAULT_KEK_LEN || !WIFEXITED(ws) || WEXITSTATUS(ws) != 0)
   {
      OPENSSL_cleanse(out, VAULT_KEK_LEN);
      return -1;
   }
   c->sealed = 0;
   return 0;
}
static int sealed(void *v)
{
   return ((kms_ctx *)v)->sealed;
}
static int unseal(void *v, const void *p, size_t n)
{
   (void)p;
   (void)n;
   uint8_t k[VAULT_KEK_LEN];
   int r = get_kek(v, k);
   OPENSSL_cleanse(k, sizeof(k));
   return r;
}
static int seal(void *v)
{
   ((kms_ctx *)v)->sealed = 1;
   g_hwm_ready = 0;
   return 0;
}
static int hwm_verify(void *v, const char *k, uint64_t ver, const uint8_t *a, size_t n)
{
   (void)v;
   return vault_hwm_attest_verify(k, ver, a, n);
}
static int rotate(void *v, const char *a, int *b, int *c, char *d, size_t e, char *f, size_t g)
{
   (void)v;
   (void)a;
   (void)b;
   (void)c;
   (void)d;
   (void)e;
   (void)f;
   (void)g;
   return -1;
}
static const vault_custody_provider_t p = {"kms",  &g,   get_kek,  rotate,  sealed,
                                           unseal, seal, hwm_read, hwm_cas, hwm_verify};
const vault_custody_provider_t *vault_custody_kms_provider(void)
{
   return &p;
}
int vault_custody_kms_hwm_refresh(void)
{
   const char *key = getenv("AIMEE_VAULT_KMS_KEY_ID");
   uint64_t version = 0;
   uint8_t att[64];
   size_t n = 0;
   g_hwm_ready = 0;
   if (!key || hwm_read(&g, key, &version, att, sizeof(att), &n) != 0)
      return -1;
   g_hwm_ready = 1;
   return 0;
}
int vault_custody_kms_hwm_ready(void)
{
   return g_hwm_ready;
}
