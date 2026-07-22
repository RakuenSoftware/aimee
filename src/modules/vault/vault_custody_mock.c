/* vault_custody_mock.c: a TEST/DEV-ONLY anchor custody provider. See
 * vault_custody_mock.h. OpenSSL-only (no file I/O, no globals beyond the
 * singleton ctx), so it links cleanly into both the kb image and the seal test. */
#include "vault_custody_mock.h"
#include "vault_crypto.h" /* vault_kek_derive, VAULT_ROOT_KEY_LEN */
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <stddef.h>
#include <string.h>

/* Fixed, non-secret HKDF salt for the mock's derived KEK (domain-separated from
 * the file provider's SERVER_KEK_SALT and the per-principal user salts). */
static const uint8_t MOCK_KEK_SALT[VAULT_SALT_LEN] = {
    0x61, 0x69, 0x6d, 0x65, 0x65, 0x2d, 0x6d, 0x6f, 0x63, 0x6b, 0x2d, 0x76, 0x31, 0x00, 0x00, 0x00};

static int mock_get_kek(void *vctx, uint8_t kek[VAULT_KEK_LEN])
{
   vault_custody_mock_ctx_t *ctx = vctx;
   if (!kek || !ctx)
      return -1;
   pthread_mutex_lock(&ctx->mu);
   int rc = 0;
   if (ctx->sealed || !ctx->kek_ready)
   {
      OPENSSL_cleanse(kek, VAULT_KEK_LEN); /* sealed anchor yields no key (P7 §3) */
      rc = -1;
   }
   else
      memcpy(kek, ctx->kek, VAULT_KEK_LEN);
   pthread_mutex_unlock(&ctx->mu);
   return rc;
}

/* The mock doesn't rotate — that rides with the real anchor + CA-key slice. */
static int mock_rotate(void *vctx, const char *server_principal, int *out_principals,
                       int *out_creds, char *backup_path, size_t backup_path_len, char *errbuf,
                       size_t errlen)
{
   (void)vctx;
   (void)server_principal;
   (void)out_principals;
   (void)out_creds;
   (void)backup_path;
   (void)backup_path_len;
   if (errbuf && errlen)
   {
      strncpy(errbuf, "mock custody does not support rotation", errlen - 1);
      errbuf[errlen - 1] = '\0'; /* strncpy does not NUL-terminate on truncation */
   }
   return -1;
}

static int mock_is_sealed(void *vctx)
{
   vault_custody_mock_ctx_t *ctx = vctx;
   if (!ctx)
      return 1;
   pthread_mutex_lock(&ctx->mu);
   int s = ctx->sealed;
   pthread_mutex_unlock(&ctx->mu);
   return s;
}

/* Model an anchor's Decrypt: condense the opaque unseal secret to a 32-byte root
 * (SHA-256), then HKDF-derive the KEK from it. Deterministic, so the same secret
 * always unseals to the same KEK (a stable KEK the tests can assert on). */
static int mock_unseal(void *vctx, const void *params, size_t len)
{
   vault_custody_mock_ctx_t *ctx = vctx;
   if (!ctx || (!params && len))
      return -1;

   uint8_t root[VAULT_ROOT_KEY_LEN];
   SHA256((const unsigned char *)params, len, root); /* secret -> 32-byte root */
   pthread_mutex_lock(&ctx->mu);
   int rc = vault_kek_derive(root, sizeof(root), MOCK_KEK_SALT, sizeof(MOCK_KEK_SALT), ctx->kek);
   OPENSSL_cleanse(root, sizeof(root));
   if (rc != 0)
   {
      /* Fail CLOSED: derive failed -> no usable KEK AND re-seal, so is_sealed()
       * stays consistent with get_kek() (never unsealed-without-KEK). */
      OPENSSL_cleanse(ctx->kek, sizeof(ctx->kek));
      ctx->kek_ready = 0;
      ctx->sealed = 1;
      pthread_mutex_unlock(&ctx->mu);
      return -1;
   }
   ctx->kek_ready = 1;
   ctx->sealed = 0;
   pthread_mutex_unlock(&ctx->mu);
   return 0;
}

static int mock_seal(void *vctx)
{
   vault_custody_mock_ctx_t *ctx = vctx;
   if (!ctx)
      return -1;
   pthread_mutex_lock(&ctx->mu);
   OPENSSL_cleanse(ctx->kek, sizeof(ctx->kek)); /* flush the derived KEK */
   ctx->kek_ready = 0;
   ctx->sealed = 1;
   pthread_mutex_unlock(&ctx->mu);
   return 0;
}

/* The singleton mock ctx starts SEALED (get_kek fails until unseal). */
static vault_custody_mock_ctx_t g_mock_ctx = {
    .mu = PTHREAD_MUTEX_INITIALIZER, .sealed = 1, .kek_ready = 0};

static const vault_custody_provider_t g_mock_provider = {
    .name = "mock",
    .ctx = &g_mock_ctx,
    .get_kek = mock_get_kek,
    .rotate = mock_rotate,
    .is_sealed = mock_is_sealed,
    .unseal = mock_unseal,
    .seal = mock_seal,
};

const vault_custody_provider_t *vault_custody_mock_provider(void)
{
   return &g_mock_provider;
}

void vault_custody_mock_reset(void)
{
   pthread_mutex_lock(&g_mock_ctx.mu);
   OPENSSL_cleanse(g_mock_ctx.kek, sizeof(g_mock_ctx.kek));
   g_mock_ctx.kek_ready = 0;
   g_mock_ctx.sealed = 1;
   pthread_mutex_unlock(&g_mock_ctx.mu);
}
